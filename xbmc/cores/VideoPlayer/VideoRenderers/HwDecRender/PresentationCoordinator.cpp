/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PresentationCoordinator.h"

#include "ServiceBroker.h"
#include "utils/StringUtils.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "filesystem/File.h"
#include "platform/linux/SysfsPath.h"
#include "utils/AMLUtils.h"
#include "utils/PresentationTimeline.h"
#include "utils/log.h"
#include "windowing/amlogic/GraphicsTick.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/ThreadRole.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace
{
constexpr int REPORT_TICKS = 1200;
constexpr int SHADOW_HOLD_SAMPLES = 24;
constexpr int64_t SYNTHETIC_TICK_NS = 50000000;
// a queued vblank event the CRTC never delivered (it went off): queue another
constexpr int64_t EVENT_LOST_NS = 100000000;
// a flip event that never came (CRTC off mid-flight): take the flip as done
constexpr int64_t FLIP_LOST_NS = 500000000;
// GT-5 (GT-D15): a timed buffer picked after its slot still lands on the next vblank only if
// committed this long before it (the commit worker's ioctl and the driver's latch)
constexpr int64_t LATE_PICK_MARGIN_NS = 3000000;
// GT-D15 / MC-29: how long after a flip a GUI frame waits for that vblank's tick (slot)
constexpr int64_t UI_SLOT_HOLD_NS = 3000000;
// GUNTIMED (m8's untimed metric): post_vbl/post_seq is the vblank interval the post fell in,
// counted back from the flip's vblank; flip/flipseq the DRM sequence of the flip that showed it
// (-1: none, the post was lost); by= the state that showed it
void LogUntimed(const SUntimedPost& post,
                const std::string& by,
                const char* what,
                int64_t sequence,
                int64_t flipNs,
                int64_t periodNs)
{
  int64_t vbl = -1;
  int64_t postSeq = -1;
  if (periodNs > 0 && sequence >= 0 && flipNs > 0)
  {
    vbl = (std::max<int64_t>(0, flipNs - post.postNs) + periodNs - 1) / periodNs;
    postSeq = sequence - vbl;
  }
  GRAPHICSTICK::GTLogF(LOGINFO,
            "GUNTIMED u={} route={} post_seq={} post_vbl={} post_ns={} flip={} flipseq={} "
            "flip_ns={} period_us={} vbl={} upd_us={} by={} what={} page={}",
            post.u, post.route, postSeq, postSeq, post.postNs, sequence, sequence, flipNs,
            periodNs > 0 ? periodNs / 1000 : -1, vbl, post.updUs, by, what,
            post.firstPage ? 1 : 0);
}

// 5.B2-7: a plane off C has not started by then runs inline (today's hold bound)
constexpr auto PLANE_OFF_BOUND = std::chrono::seconds(2);

std::atomic<CPresentationCoordinator*> s_instance{nullptr};
std::atomic<bool> s_untimedLog{false};
//! test only: special://profile/geomwake_always runs the wake tail while ticking too (the old
//! behaviour), so a run names the slow step by its GEOMWAKE slow line
std::atomic<bool> s_geomWakeAlways{false};

int64_t MonotonicNs()
{
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct SequenceEvent
{
  bool got = false;
  uint64_t seq = 0;
  uint64_t ns = 0;
  uint64_t epoch = 0;
};
thread_local SequenceEvent t_event;
thread_local bool t_flipped = false;
thread_local uint64_t t_flipTag = 0;
thread_local unsigned int t_flipSequence = 0;
thread_local int64_t t_flipNs = 0;

void OnSequence(int fd, uint64_t sequence, uint64_t ns, uint64_t userData)
{
  t_event.got = true;
  t_event.seq = sequence;
  t_event.ns = ns;
  t_event.epoch = userData;
}

void OnPageFlip(int fd,
                unsigned int sequence,
                unsigned int sec,
                unsigned int usec,
                unsigned int crtc,
                void* userData)
{
  t_flipped = true;
  t_flipTag = reinterpret_cast<uintptr_t>(userData);
  t_flipSequence = sequence;
  t_flipNs = static_cast<int64_t>(sec) * 1000000000 + static_cast<int64_t>(usec) * 1000;
}

int ReadKernelDrops()
{
  const auto value =
      CSysfsPath("/sys/module/aml_media/parameters/video_drop_vf_cnt").Get<std::string>();
  return value ? std::atoi(value->c_str()) : -1;
}

// 5.E2-6 (r2 L5): C's state, published for RM's acceptance line
std::atomic<int> s_publishedState{0};

const char* StateName(int state)
{
  static const char* const names[] = {"IDLE", "PLAYING", "PAUSED", "DISPLAY-LOST", "HELD"};
  return names[state];
}
} // namespace

CPresentationCoordinator::CPresentationCoordinator(int masterFd, int tickFd, uint32_t tickCrtc)
  : CThread("PresentCoord"), m_masterFd(masterFd), m_tickFd(tickFd), m_tickCrtc(tickCrtc)
{
}

CPresentationCoordinator::~CPresentationCoordinator()
{
  Stop();
}

CPresentationCoordinator* CPresentationCoordinator::Get()
{
  return s_instance.load();
}

bool CPresentationCoordinator::Start()
{
  auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem || m_masterFd < 0)
    return false;

  // Its own fd for the vblank events, so they never mix with the flip events
  // on the master fd the commits go through.
  char* path = drmGetDeviceNameFromFd2(m_masterFd);
  if (path)
  {
    m_vblankFd = open(path, O_RDWR | O_CLOEXEC);
    free(path);
  }
  m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  m_crtc = winSystem->GetDRMCrtcId();
  if (m_vblankFd < 0 || m_wakeFd < 0)
  {
    GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - no event fds (vblank:{} wake:{})",
              m_vblankFd, m_wakeFd);
    if (m_vblankFd >= 0)
      close(m_vblankFd);
    if (m_wakeFd >= 0)
      close(m_wakeFd);
    m_vblankFd = m_wakeFd = -1;
    return false;
  }

  m_report = Report();
  m_kernelDrops = ReadKernelDrops();
  s_untimedLog = XFILE::CFile::Exists("special://profile/guntimed_log");
  if (s_untimedLog)
    GRAPHICSTICK::GTLogF(LOGWARNING, "GUNTIMED logging on (guntimed_log)");
  s_geomWakeAlways = XFILE::CFile::Exists("special://profile/geomwake_always");
  if (s_geomWakeAlways)
    GRAPHICSTICK::GTLogF(LOGWARNING,
                         "GEOMWAKE test: the wake tail runs while ticking (geomwake_always)");
  aml_presenter_set_wake_fd(m_wakeFd);
  m_commitStop = false;
  m_commitThread = std::thread(&CPresentationCoordinator::CommitWorker, this);
  Create();
  s_instance = this;
  // GT-D15: the video path's log writer runs while the coordinator does
  GRAPHICSTICK::GTLogStart();
  GRAPHICSTICK::GTLogF(LOGINFO, "CPresentationCoordinator - started, commits the GUI plane (crtc {})", m_crtc.load());
  // the gate tooling (gx2d_summ_soc.py) keys its bound cell on this exact form,
  // never on a box's name
  GRAPHICSTICK::GTLogF(LOGINFO, "P5 SoC family={} validated={}", aml_get_cpufamily_name(),
            aml_presentation_validated() ? 1 : 0);
  return true;
}

void CPresentationCoordinator::Stop()
{
  if (m_wakeFd < 0)
    return;

  s_instance = nullptr;
  m_bStop = true;
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
  StopThread(true);
  aml_presenter_set_wake_fd(-1);
  {
    std::unique_lock lock(m_uiMutex);
    m_commitStop = true;
  }
  m_uiCond.notify_all();
  m_commitThread.join();

  std::vector<DumbBuffer> dumbs;
  {
    std::unique_lock lock(m_uiMutex);
    for (int plane = 0; plane < PLANE_COUNT; plane++)
      Drop(plane, m_planes[plane].ready);
    for (UiBuffer& buffer : m_timed)
      Drop(PLANE_GRAPHICS, buffer);
    m_timed.clear();
    if (m_cbufReq)
      drmModeAtomicFree(m_cbufReq);
    m_cbufReq = nullptr;
    if (m_cbuf.fb)
      dumbs.push_back(m_cbuf);
    m_cbuf = DumbBuffer();
    for (const Retired& retired : m_cbufRetired)
      dumbs.push_back(retired.buffer);
    m_cbufRetired.clear();
  }
  for (DumbBuffer& dumb : dumbs)
    FreeDumb(dumb);
  close(m_vblankFd);
  close(m_wakeFd);
  m_vblankFd = m_wakeFd = -1;
  LogReport();
  GRAPHICSTICK::GTLogF(LOGINFO, "CPresentationCoordinator - stopped");
  // every queued C/G line is written before the window system goes on with its teardown
  GRAPHICSTICK::GTLogStop();
}

void CPresentationCoordinator::SetTickSource(int tickFd, uint32_t tickCrtc)
{
  m_tickCrtc = tickCrtc;
  m_tickFd = tickFd;
}

bool CPresentationCoordinator::AttachVideo(CRenderManager* renderManager)
{
  {
    std::unique_lock lock(m_videoMutex);
    m_video = renderManager;
  }
  // the CRTC: under hotplugworker the published DisplayState's id (5.B3-4); under off HEAD's live
  // read (racy: this runs on VideoPlayer::OpenFile's caller via RM PreInit, or on V at a configure
  // after a failed attach, which can be during a restore)
  if (auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
    m_crtc = winSystem->GetDRMCrtcId();

  const int64_t start = MonotonicNs();
  const bool timeline = m_tickFd >= 0 && m_crtc == m_tickCrtc;
  if (m_tickFd >= 0 && !timeline)
    GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - crtc {} is not the timekeeper's ({}), "
                          "queueing its own vblank events", m_crtc.load(), m_tickCrtc.load());
  m_attachNs = start;
  m_useTimeline = timeline;
  m_videoAttached = true;
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));

  // presenting needs a vblank source that delivers: a vblank after this
  // point, not an event left queued by an earlier attach
  std::unique_lock lock(m_vblankMutex);
  const bool delivering = m_vblankCond.wait_for(lock, std::chrono::milliseconds(150),
                                                [&] { return m_lastVblankNs > start; });
  if (!delivering)
    GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - no vblank event from crtc {}, "
                          "video stays on the render thread", m_crtc.load());
  return delivering;
}

void CPresentationCoordinator::DetachVideo(CRenderManager* renderManager)
{
  std::unique_lock lock(m_videoMutex);
  if (m_video != renderManager)
    return;
  m_video = nullptr;
  m_videoAttached = false;
}

uint64_t CPresentationCoordinator::Submit(int plane,
                                          gbm_bo* bo,
                                          uint32_t fb,
                                          drmModeAtomicReqPtr req,
                                          int fence,
                                          double pts,
                                          uint64_t epoch)
{
  uint64_t seq;
  bool replacedCbuf = false;
  {
    std::unique_lock lock(m_uiMutex);
    // 5.B2-4 (SM4): a buffer of an old surface goes straight back
    if (plane == PLANE_UI && epoch < m_uiEpoch)
    {
      const int refused = ++m_uiRefused;
      const uint64_t current = m_uiEpoch;
      lock.unlock();
      if (req)
        drmModeAtomicFree(req);
      if (fence >= 0)
        close(fence);
      if (refused <= 3 || refused % 100 == 0)
        GRAPHICSTICK::GTLogF(LOGINFO, "UIEPOCH refused submit {} of epoch {} below {}", refused, epoch,
                  current);
      return 0;
    }
    UiBuffer& ready = m_planes[plane].ready;
    // a replaced buffer's posts are shown by this one
    std::vector<SUntimedPost> untimed;
    if (s_untimedLog)
    {
      untimed = std::move(ready.untimed);
      untimed.insert(untimed.end(), m_untimedNext[plane].begin(), m_untimedNext[plane].end());
      m_untimedNext[plane].clear();
    }
    if (ready.req)
    {
      replacedCbuf = ready.cOwned;
      Drop(plane, ready);
      if (plane == PLANE_UI)
        m_report.uiReplaced++;
      else
        m_report.gfxReplaced++;
    }
    ready.bo = bo;
    ready.fb = fb;
    ready.req = req;
    ready.fence = fence;
    ready.pts = pts;
    ready.epoch = epoch;
    ready.cOwned = false;
    ready.readyNs = fence < 0 ? MonotonicNs() : 0;
    ready.seq = seq = ++m_submitSeq;
    if (s_untimedLog)
      ready.untimed = std::move(untimed);
    if (plane == PLANE_UI)
      m_report.uiSubmits++;
    else
      m_report.gfxSubmits++;
  }
  if (replacedCbuf)
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF replaced by main before its commit");
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
  return seq;
}

bool CPresentationCoordinator::WaitTaken(uint64_t seq, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(m_uiMutex);
  return m_uiCond.wait_for(lock, timeout, [&] {
    for (const PlaneState& state : m_planes)
      if (state.ready.seq == seq)
        return false;
    return true;
  });
}

void CPresentationCoordinator::TakeReturned(int plane, std::vector<gbm_bo*>& returned)
{
  std::unique_lock lock(m_uiMutex);
  std::vector<Returned>& from = m_planes[plane].returned;
  for (const Returned& buffer : from)
    returned.push_back(buffer.bo);
  from.clear();
}

void CPresentationCoordinator::TakeReturned(std::vector<gbm_bo*>& returned,
                                            std::vector<uint64_t>& epochs)
{
  std::unique_lock lock(m_uiMutex);
  std::vector<Returned>& from = m_planes[PLANE_UI].returned;
  const auto cbuf = [this](uint32_t fb)
  {
    return fb == m_cbuf.fb || std::any_of(m_cbufRetired.begin(), m_cbufRetired.end(),
                                          [fb](const Retired& r) { return r.buffer.fb == fb; });
  };
  for (const Returned& buffer : from)
  {
    // never C's own buffers (5.B2-4)
    if (buffer.fb && cbuf(buffer.fb))
    {
      GRAPHICSTICK::GTLogF(LOGERROR, "UIEPOCH ASSERT a returned buffer carries C's fb {}", buffer.fb);
      continue;
    }
    returned.push_back(buffer.bo);
    epochs.push_back(buffer.epoch);
  }
  from.clear();
}

void CPresentationCoordinator::DetachSurface(int plane)
{
  // parked, and no flip in flight
  CAmlPresenterHold hold("detach surface");
  std::unique_lock lock(m_uiMutex);
  AbandonLostCommit(plane);
  ReclaimPlane(plane);
}

uint64_t CPresentationCoordinator::DetachUiSurfaceNewEpoch(uint32_t format)
{
  CAmlPresenterHold hold("detach surface");
  bool stale;
  uint64_t epoch;
  DumbBuffer shown;
  {
    std::unique_lock lock(m_uiMutex);
    // C's buffer up, or its commit's flip lost: it may be on the plane
    if (m_cbuf.fb && (m_cbufUp || m_planes[PLANE_UI].committedCOwned))
    {
      shown = m_cbuf;
      m_cbuf = DumbBuffer();
    }
    AbandonLostCommit(PLANE_UI);
    ReclaimPlane(PLANE_UI);
    m_uiFormat = format;
    epoch = ++m_uiEpoch;
    // a request not used by now was for the old epoch
    stale = m_cbufReq != nullptr;
    if (stale)
      drmModeAtomicFree(m_cbufReq);
    m_cbufReq = nullptr;
  }
  if (stale)
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF dropped an unused request at epoch {}", epoch);
  // Still parked, with nothing in C's books on it: its RmFB takes the GUI plane off before
  // commit 1, as the EGL destroy's RmFB of main's buffer does today. The next set makes a
  // new buffer.
  if (shown.fb)
  {
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF off at the detach: fb={} {}x{} freed", shown.fb, shown.width,
              shown.height);
    FreeDumb(shown);
  }
  return epoch;
}

bool CPresentationCoordinator::ReclaimUiForModeSet(uint64_t& newEpoch,
                                                   drmModeAtomicReqPtr off,
                                                   uint32_t format,
                                                   bool acked)
{
  const uint64_t requested = newEpoch;
  static int s_refused = 0;
  static int s_unbuilt = 0;
  // the caller's park: C acknowledged it, so no GUI flip is in flight (R4-M1); unacked twice,
  // the caller runs today's way on purpose (logged there)
  if (acked && !aml_presenter_parked())
    GRAPHICSTICK::GTLogF(LOGERROR, "UIEPOCH ASSERT the GUI plane reclaim runs with C not parked");
  // G's own switch-off (a refusal, ghold) would put the GUI plane's fb it read back up
  std::unique_lock planeOff(m_planeOffMutex);
  uint32_t shownFb;
  bool cbufShown;
  {
    std::unique_lock lock(m_uiMutex);
    AbandonLostCommit(PLANE_UI);
    shownFb = m_planes[PLANE_UI].onScreenFb;
    cbufShown = m_cbufUp;
  }
  // blocking, as DisableGraphicsPlane's: never under m_uiMutex
  bool done = false;
  int err = 0;
  int64_t tookUs = 0;
  if (off)
  {
    const int64_t start = MonotonicNs();
    done = drmModeAtomicCommit(m_masterFd, off, 0, nullptr) == 0;
    err = errno;
    tookUs = (MonotonicNs() - start) / 1000;
    drmModeAtomicFree(off);
  }
  uint64_t previous;
  bool stale;
  {
    std::unique_lock lock(m_uiMutex);
    // done or not, every buffer goes back and nothing shows in C's books (R4-M2, R4-M4); C's
    // own buffer is off the plane with the rest and goes up again with this set's request
    ReclaimPlane(PLANE_UI);
    if (format)
      m_uiFormat = format;
    previous = m_uiEpoch;
    m_uiEpoch = std::max(m_uiEpoch + 1, newEpoch);
    newEpoch = m_uiEpoch;
    stale = m_cbufReq != nullptr;
    if (stale)
      drmModeAtomicFree(m_cbufReq);
    m_cbufReq = nullptr;
  }
  // W is the one writer from 5.B2-5b: its next epoch is always the one taken here
  if (newEpoch != requested)
    GRAPHICSTICK::GTLogF(LOGERROR, "UIEPOCH ASSERT epoch {} asked for at epoch {}, {} taken", requested,
              previous, newEpoch);
  if (stale)
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF dropped an unused request at epoch {}", newEpoch);
  if (done)
    GRAPHICSTICK::GTLogF(LOGINFO, "PLANE ui off done epoch={} fb={} cbuf={} took_us={}{}", newEpoch, shownFb,
              cbufShown ? 1 : 0, tookUs, acked ? "" : " (unparked)");
  else if (!off)
    GRAPHICSTICK::GTLogF(LOGWARNING, "PLANE ui off not built (no plane or CRTC) epoch={} fb={} n={}",
              newEpoch, shownFb, ++s_unbuilt);
  else
    GRAPHICSTICK::GTLogF(LOGERROR, "PLANE ui off refused (SB3 state) epoch={} fb={} {} n={}", newEpoch,
              shownFb, strerror(err), ++s_refused);
  return done;
}

void CPresentationCoordinator::ForgetUiEpoch(uint64_t epoch)
{
  int forgotten = 0;
  {
    std::unique_lock lock(m_uiMutex);
    PlaneState& ui = m_planes[PLANE_UI];
    if (ui.ready.bo && ui.ready.epoch < epoch)
    {
      Drop(PLANE_UI, ui.ready);
      forgotten++;
    }
    if (ui.committedBo && ui.committedEpoch < epoch)
    {
      ui.returned.push_back({ui.committedBo, ui.committedFb, ui.committedEpoch});
      ui.inCommit = false;
      ui.committedBo = nullptr;
      ui.committedFb = 0;
      ui.committedEpoch = 0;
      ui.committedCOwned = false;
      forgotten++;
    }
    if (ui.onScreen && ui.onScreenEpoch < epoch)
    {
      ui.returned.push_back({ui.onScreen, ui.onScreenFb, ui.onScreenEpoch});
      ui.onScreen = nullptr;
      ui.onScreenFb = 0;
      ui.onScreenEpoch = 0;
      forgotten++;
    }
    // a commit still in its ioctl is never handed back
    ui.generation++;
  }
  GRAPHICSTICK::GTLogF(LOGERROR, "UIEPOCH ForgetUiEpoch below {}: {} buffers", epoch, forgotten);
}

uint64_t CPresentationCoordinator::UiEpoch()
{
  std::unique_lock lock(m_uiMutex);
  return m_uiEpoch;
}

bool CPresentationCoordinator::HoldsUiEpochBelow(uint64_t epoch)
{
  std::unique_lock lock(m_uiMutex);
  const PlaneState& ui = m_planes[PLANE_UI];
  return (ui.ready.bo && ui.ready.epoch < epoch) ||
         (ui.committedBo && ui.committedEpoch < epoch) || (ui.onScreen && ui.onScreenEpoch < epoch);
}

void CPresentationCoordinator::PrepareClearedUi(
    int width,
    int height,
    uint64_t epoch,
    const std::function<drmModeAtomicReqPtr(uint32_t, int&, int&)>& build)
{
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
  {
    GRAPHICSTICK::GTLogF(LOGERROR, "CBUF no buffer for a {}x{} GUI", width, height);
    return;
  }
  uint32_t format;
  uint32_t fb;
  bool reuse;
  {
    std::unique_lock lock(m_uiMutex);
    format = m_uiFormat ? m_uiFormat : DRM_FORMAT_ARGB8888;
    reuse = m_cbuf.fb && m_cbuf.width == width && m_cbuf.height == height;
    fb = m_cbuf.fb;
  }
  // C is parked: the allocation and the request stay off its thread and its lock (SL5,
  // R4-L1, SM5)
  DumbBuffer fresh;
  if (!reuse)
  {
    if (!CreateDumb(width, height, format, fresh))
      return;
    fb = fresh.fb;
  }
  int srcWidth = 0;
  int srcHeight = 0;
  drmModeAtomicReqPtr req = build(fb, srcWidth, srcHeight);
  bool stale;
  {
    std::unique_lock lock(m_uiMutex);
    if (!reuse)
    {
      if (m_cbuf.fb)
        m_cbufRetired.push_back({m_cbuf, m_uiFlips});
      m_cbuf = fresh;
    }
    if (m_cbufReq)
      drmModeAtomicFree(m_cbufReq);
    stale = epoch < m_uiEpoch;
    if (stale && req)
      drmModeAtomicFree(req);
    m_cbufReq = stale ? nullptr : req;
    m_cbufEpoch = epoch;
  }
  GRAPHICSTICK::GTLogF(LOGINFO, "CBUF armed epoch={} fb={} {}x{} src={}x{} made={} request={}{}", epoch, fb,
            width, height, srcWidth, srcHeight, reuse ? 0 : 1, req ? 1 : 0,
            stale ? " (stale epoch: dropped)" : "");
  if (req && (srcWidth != width || srcHeight != height))
    GRAPHICSTICK::GTLogF(LOGERROR, "CBUF request src {}x{} is not the buffer's {}x{}", srcWidth, srcHeight,
              width, height);
}

bool CPresentationCoordinator::CreateDumb(int width, int height, uint32_t format, DumbBuffer& dumb)
{
  drm_mode_create_dumb create = {};
  create.width = static_cast<uint32_t>(width);
  create.height = static_cast<uint32_t>(height);
  create.bpp = 32;
  if (drmIoctl(m_masterFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
  {
    GRAPHICSTICK::GTLogF(LOGERROR, "CBUF dumb buffer {}x{} failed: {}", width, height, strerror(errno));
    return false;
  }
  dumb.handle = create.handle;
  dumb.width = width;
  dumb.height = height;
  // every byte 0: transparent, plane alpha full
  drm_mode_map_dumb map = {};
  map.handle = create.handle;
  bool cleared = false;
  if (drmIoctl(m_masterFd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0)
  {
    void* pixels = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, m_masterFd,
                        static_cast<off_t>(map.offset));
    if (pixels != MAP_FAILED)
    {
      std::memset(pixels, 0, create.size);
      munmap(pixels, create.size);
      cleared = true;
    }
  }
  const uint32_t handles[4] = {create.handle};
  const uint32_t pitches[4] = {create.pitch};
  const uint32_t offsets[4] = {};
  if (!cleared || drmModeAddFB2(m_masterFd, create.width, create.height, format, handles,
                                pitches, offsets, &dumb.fb, 0) != 0)
  {
    GRAPHICSTICK::GTLogF(LOGERROR, "CBUF fb {}x{} failed (cleared {}): {}", width, height, cleared,
              strerror(errno));
    FreeDumb(dumb);
    return false;
  }
  GRAPHICSTICK::GTLogF(LOGINFO, "CBUF made fb={} {}x{} pitch={}", dumb.fb, width, height, create.pitch);
  return true;
}

void CPresentationCoordinator::FreeDumb(DumbBuffer& dumb)
{
  if (dumb.fb)
    drmModeRmFB(m_masterFd, dumb.fb);
  if (dumb.handle)
  {
    drm_mode_destroy_dumb destroy = {};
    destroy.handle = dumb.handle;
    drmIoctl(m_masterFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }
  dumb = DumbBuffer();
}

bool CPresentationCoordinator::DisableGraphicsPlane(
    const std::function<drmModeAtomicReqPtr(uint32_t)>& build)
{
  // parked, and no flip in flight: this thread owns the commit
  CAmlPresenterHold hold("graphics plane off");
  return GraphicsPlaneOff(build);
}

bool CPresentationCoordinator::GraphicsPlaneOff(
    const std::function<drmModeAtomicReqPtr(uint32_t)>& build)
{
  // 5.B2-5b: never between a GUI plane reclaim's snapshot and its books (uncontended off)
  std::unique_lock planeOff(m_planeOffMutex);
  uint32_t primaryFb;
  bool on;
  {
    std::unique_lock lock(m_uiMutex);
    AbandonLostCommit(PLANE_GRAPHICS);
    primaryFb = m_planes[PLANE_UI].onScreenFb;
    on = m_graphicsOn;
  }
  bool done = !on;
  if (on)
  {
    if (drmModeAtomicReqPtr req = build(primaryFb))
    {
      done = drmModeAtomicCommit(m_masterFd, req, 0, nullptr) == 0;
      if (!done)
        GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - graphics plane off failed: {}",
                  strerror(errno));
      drmModeAtomicFree(req);
    }
  }
  std::unique_lock lock(m_uiMutex);
  if (done)
    m_graphicsOn = false;
  ReclaimPlane(PLANE_GRAPHICS);
  return done;
}

bool CPresentationCoordinator::SwitchGraphicsOff(
    const std::function<drmModeAtomicReqPtr(uint32_t)>& build, const char* who)
{
  if (!OUTPUT::PlaneOffOnC())
    return DisableGraphicsPlane(build);
  auto op = std::make_shared<PlaneOffOp>();
  op->who = who;
  op->graphicsOff = build;
  if (RunByC({op}, std::chrono::steady_clock::now() + PLANE_OFF_BOUND))
    return op->result;
  // C not running (stop, teardown): inline at once, as today
  if (op->state == PlaneOffOp::State::QUEUED)
    GRAPHICSTICK::GTLogF(LOGERROR, "graphics plane off: C did not answer in 2 s, inline (who={})", who);
  return DisableGraphicsPlane(build);
}

bool CPresentationCoordinator::PlanesOffForModeSet(
    const std::function<drmModeAtomicReqPtr(uint32_t)>& graphicsOff,
    const std::function<drmModeAtomicReqPtr()>& uiOff,
    uint64_t& newEpoch,
    uint32_t format,
    bool parked)
{
  std::shared_ptr<PlaneOffOp> graphics;
  std::shared_ptr<PlaneOffOp> ui;
  if (OUTPUT::PlaneOffOnC() && parked)
  {
    std::vector<std::shared_ptr<PlaneOffOp>> ops;
    if (graphicsOff)
    {
      graphics = std::make_shared<PlaneOffOp>();
      graphics->who = "mode_set";
      graphics->graphicsOff = graphicsOff;
      ops.push_back(graphics);
    }
    ui = std::make_shared<PlaneOffOp>();
    ui->ui = true;
    ui->who = "mode_set";
    ui->uiOff = uiOff;
    ui->epoch = newEpoch;
    ui->format = format;
    ops.push_back(ui);
    if (!RunByC(ops, std::chrono::steady_clock::now() + PLANE_OFF_BOUND) &&
        ui->state == PlaneOffOp::State::QUEUED)
      GRAPHICSTICK::GTLogF(LOGERROR,
                "mode set planes off: C did not answer in 2 s, inline on W (graphics {}, ui {})",
                !graphics ? "-" : graphics->state == PlaneOffOp::State::DONE ? "done" : "inline",
                ui->state == PlaneOffOp::State::DONE ? "done" : "inline");
  }

  bool graphicsDone = graphics && graphics->state == PlaneOffOp::State::DONE;
  if (graphicsDone)
    graphicsDone = graphics->result;
  // unparked: B2-5c's calls, its own hold first; W's acknowledged park needs no other
  else if (graphicsOff)
    graphicsDone = graphics ? GraphicsPlaneOff(graphicsOff) : DisableGraphicsPlane(graphicsOff);
  if (graphicsOff && !graphicsDone)
    GRAPHICSTICK::GTLogF(LOGWARNING, "CGraphicsPlaneAML - plane left to go with its buffers");

  if (ui && ui->state == PlaneOffOp::State::DONE)
  {
    newEpoch = ui->epoch;
    return ui->result;
  }
  return ReclaimUiForModeSet(newEpoch, uiOff(), format, parked);
}

bool CPresentationCoordinator::RunByC(const std::vector<std::shared_ptr<PlaneOffOp>>& ops,
                                      std::chrono::steady_clock::time_point deadline)
{
  std::unique_lock lock(m_uiMutex);
  if (!m_planeOffOpen)
  {
    for (const auto& op : ops)
      op->state = PlaneOffOp::State::DROPPED;
    return false;
  }
  const int64_t now = MonotonicNs();
  for (const auto& op : ops)
  {
    op->postedNs = now;
    op->uiFlipAtPost = m_inFlight;
    m_planeOffs.push_back(op);
  }
  // under the lock: C has not left its loop, so the fd is open
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
  const auto state = [&ops](PlaneOffOp::State a, PlaneOffOp::State b)
  {
    return std::all_of(ops.begin(), ops.end(),
                       [a, b](const auto& op) { return op->state == a || op->state == b; });
  };
  if (m_uiCond.wait_until(lock, deadline, [&]
                          { return state(PlaneOffOp::State::DONE, PlaneOffOp::State::DROPPED); }))
    return state(PlaneOffOp::State::DONE, PlaneOffOp::State::DONE);
  // what C has not started is no longer C's; the one it runs ends with its blocking commit and
  // is never run again
  for (const auto& op : ops)
    if (op->state == PlaneOffOp::State::QUEUED)
      m_planeOffs.erase(std::remove(m_planeOffs.begin(), m_planeOffs.end(), op),
                        m_planeOffs.end());
  m_uiCond.wait(lock,
                [&ops]
                {
                  return std::none_of(ops.begin(), ops.end(), [](const auto& op)
                                      { return op->state == PlaneOffOp::State::RUNNING; });
                });
  return state(PlaneOffOp::State::DONE, PlaneOffOp::State::DONE);
}

void CPresentationCoordinator::RunPlaneOffs(bool held)
{
  while (!UiInFlight())
  {
    std::shared_ptr<PlaneOffOp> op;
    int deferred;
    {
      std::unique_lock lock(m_uiMutex);
      if (m_planeOffs.empty())
        return;
      op = m_planeOffs.front();
      m_planeOffs.pop_front();
      op->state = PlaneOffOp::State::RUNNING;
      deferred = std::exchange(m_uiDeferred, 0);
    }
    const int64_t start = MonotonicNs();
    uint64_t epoch = op->epoch;
    const bool result = op->ui ? ReclaimUiForModeSet(epoch, op->uiOff ? op->uiOff() : nullptr,
                                                     op->format)
                               : GraphicsPlaneOff(op->graphicsOff);
    const int64_t end = MonotonicNs();
    // how far it ran past the vblank of C's next tick (-1: no video ticks)
    const bool ticking = !held && m_videoAttached && m_tickVblankNs && m_tickPeriodNs;
    const int64_t lateUs =
        ticking ? std::max<int64_t>(0, end - (m_tickVblankNs + m_tickPeriodNs)) / 1000 : -1;
    GRAPHICSTICK::GTLogF(LOGINFO,
              "PLANEOFF by C n={} op={} who={} epoch={} uiepoch={} held={} result={} wait_us={} "
              "took_us={} tick_late_us={} uiflip_at_post={} ui_deferred={}",
              ++m_planeOffSeq, op->ui ? "ui" : "graphics", op->who, aml_presenter_epoch(),
              op->ui ? epoch : 0, held ? 1 : 0, result ? 1 : 0, (start - op->postedNs) / 1000,
              (end - start) / 1000, lateUs, op->uiFlipAtPost ? 1 : 0, deferred);
    m_planeOffTickOwed = ticking;
    {
      std::unique_lock lock(m_uiMutex);
      op->epoch = epoch;
      op->result = result;
      op->state = PlaneOffOp::State::DONE;
      m_diagUnblockedNs = MonotonicNs();
    }
    m_uiCond.notify_all();
  }
}

void CPresentationCoordinator::AbandonLostCommit(int detached)
{
  // The hold was acknowledged, so a commit still marked in flight lost its
  // flip (CRTC off mid-flight). Nothing else would ever clear it: the next
  // surface could never commit. The detached plane's buffer goes back with the
  // rest; the other plane's is taken as shown, since it may be on screen and
  // must not be drawn into.
  if (!m_inFlight)
    return;
  m_inFlight = false;
  m_inCommitTag = 0;
  m_diagUnblockedNs = MonotonicNs();
  // GT-1 r1 N1: no flip names what it showed
  if (s_untimedLog)
    LogUntimedLost(m_untimedInFlight, "flip lost");
  if (m_inFlightTimed.u)
    m_gtCarriedU = std::max(m_gtCarriedU, m_inFlightTimed.u);
  if (m_inFlightTimed.timed)
    GRAPHICSTICK::NoteGraphicsLost();
  m_inFlightTimed = UiBuffer();
  for (int plane = 0; plane < PLANE_COUNT; plane++)
  {
    PlaneState& state = m_planes[plane];
    if (!state.inCommit)
      continue;
    if (plane != detached && state.committedBo)
    {
      if (state.onScreen)
        state.returned.push_back({state.onScreen, state.onScreenFb, state.onScreenEpoch});
      state.onScreen = state.committedBo;
      state.onScreenFb = state.committedFb;
      state.onScreenEpoch = state.committedEpoch;
      state.committedBo = nullptr;
    }
    else if (plane != detached && state.committedCOwned)
    {
      if (state.onScreen)
        state.returned.push_back({state.onScreen, state.onScreenFb, state.onScreenEpoch});
      state.onScreen = nullptr;
      state.onScreenFb = state.committedFb;
      m_cbufUp = true;
    }
    state.committedCOwned = false;
    state.inCommit = false;
  }
}

void CPresentationCoordinator::ReclaimPlane(int plane)
{
  PlaneState& state = m_planes[plane];
  // A commit that outlived a timed-out hold belongs to this surface: its
  // buffer and its flip are dropped, never handed to the next surface.
  state.generation++;
  Drop(plane, state.ready);
  if (plane == PLANE_GRAPHICS)
  {
    for (UiBuffer& buffer : m_timed)
      Drop(plane, buffer);
    m_timed.clear();
  }
  if (state.committedBo)
    state.returned.push_back({state.committedBo, state.committedFb, state.committedEpoch});
  if (state.onScreen)
    state.returned.push_back({state.onScreen, state.onScreenFb, state.onScreenEpoch});
  state.inCommit = false;
  state.committedBo = nullptr;
  state.committedFb = 0;
  state.committedEpoch = 0;
  state.committedCOwned = false;
  state.onScreen = nullptr;
  state.onScreenFb = 0;
  state.onScreenEpoch = 0;
  // R4-M4: C's own flag, cleared with the books
  if (plane == PLANE_UI)
    m_cbufUp = false;
}

bool CPresentationCoordinator::ReapFence(UiBuffer& buffer)
{
  if (buffer.fence < 0)
    return true;
  struct pollfd pf = {buffer.fence, POLLIN, 0};
  if (poll(&pf, 1, 0) <= 0 || !(pf.revents & (POLLIN | POLLERR)))
    return false;
  close(buffer.fence);
  buffer.fence = -1;
  buffer.readyNs = MonotonicNs();
  return true;
}

void CPresentationCoordinator::Drop(int plane, UiBuffer& buffer)
{
  if (buffer.bo)
    m_planes[plane].returned.push_back({buffer.bo, buffer.fb, buffer.epoch});
  if (buffer.req)
    drmModeAtomicFree(buffer.req);
  if (buffer.fence >= 0)
    close(buffer.fence);
  buffer = UiBuffer();
  m_uiCond.notify_all();
}

bool CPresentationCoordinator::UiInFlight()
{
  std::unique_lock lock(m_uiMutex);
  if (!m_inFlight)
    return false;
  // A flip that never came (CRTC off mid-flight) no longer blocks a hold. Its
  // buffers stay where they are: only its own flip event retires them.
  if (MonotonicNs() - m_commitNs <= FLIP_LOST_NS)
    return true;
  if (!m_flipLostLogged)
  {
    m_flipLostLogged = true;
    m_report.uiLostFlips++;
    GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - no flip event 500 ms after a plane commit");
  }
  return false;
}

void CPresentationCoordinator::Process()
{
  OUTPUT::SetThreadRole("C");
  SetPriority(ThreadPriority::ABOVE_NORMAL);
  aml_presenter_set_running(true);
  m_planeOffOpen = true;
  m_lastTickNs = MonotonicNs();

  while (!m_bStop)
  {
    // 5.B2-7 (latched once W runs the mode sets); off and the skip run none of it
    const bool planeOffOnC = OUTPUT::PlaneOffOnC();
    unsigned int epoch = 0;
    // a hold is acknowledged only once the flip in flight has landed
    const bool held = aml_presenter_check_hold(epoch, !UiInFlight());
    StampPhase(PHASE_HOLD);
    m_bodyHeld |= held;
    if (epoch != m_seenEpoch)
    {
      m_seenEpoch = epoch;
      m_seenEpochNs = MonotonicNs();
      // a display epoch change can reset the vblank counter: a stale hold ends with it (a mode
      // set's plane-off also drops the buffer, ReclaimPlane)
      std::unique_lock lock(m_uiMutex);
      m_planes[PLANE_GRAPHICS].ready.staleDeferred = false;
      m_gtFlipSeqValid = false;
    }
    if (held)
    {
      SetState(State::HELD, epoch);
      m_report.held++;
      // no timer ticks to catch up on once it is released, and the vblanks
      // of the transaction are not missed ones
      m_lastTickNs = MonotonicNs();
      m_lastSeq = 0;
      if (planeOffOnC)
        RunPlaneOffs(true);
    }
    else
      CommitUi();
    StampPhase(PHASE_COMMIT);

    const bool video = m_videoAttached;
    const bool timeline = video && m_useTimeline;
    if (video && !timeline)
      QueueVblank(epoch);

    int fence[PLANE_COUNT];
    uint64_t fenceSeq[PLANE_COUNT];
    bool inFlight;
    {
      std::unique_lock lock(m_uiMutex);
      for (int plane = 0; plane < PLANE_COUNT; plane++)
      {
        fence[plane] = m_planes[plane].ready.fence;
        fenceSeq[plane] = m_planes[plane].ready.seq;
      }
      inFlight = m_inFlight;
    }

    struct pollfd fds[3 + PLANE_COUNT] = {
        {m_wakeFd, POLLIN, 0}, {m_masterFd, POLLIN, 0}, {-1, POLLIN, 0}};
    if (video)
      fds[2].fd = timeline ? m_tickFd.load() : m_vblankFd;
    for (int plane = 0; plane < PLANE_COUNT; plane++)
      fds[3 + plane] = {fence[plane], POLLIN, 0};
    const int timeoutMs = held ? 20 : (video || inFlight ? 50 : -1);
    int64_t timeoutNs = timeoutMs < 0 ? -1 : static_cast<int64_t>(timeoutMs) * 1000000;
    {
      // a GUI frame held for the slot is committed within its bound even with no tick
      std::unique_lock lock(m_uiMutex);
      // the held frame went another way (replaced, dropped at a reclaim): nothing to wait for
      if (m_uiHoldUntilNs && !m_planes[PLANE_UI].ready.req)
        m_uiHoldStartNs = m_uiHoldUntilNs = 0;
      // to the ns (ppoll): poll's ms rounded up let the hold run to 4 ms
      if (m_uiHoldUntilNs)
      {
        const int64_t left = std::max<int64_t>(0, m_uiHoldUntilNs - MonotonicNs());
        timeoutNs = timeoutNs < 0 ? left : std::min(timeoutNs, left);
      }
    }
    {
      // GT-D15 measurement: a late tick is C busy (a long body) or C not run (it waited in
      // poll from before the vblank)
      const int64_t enter = MonotonicNs();
      if (m_pollReturnNs)
      {
        m_lastBodyNs = enter - m_pollReturnNs;
        LogSlowBody(enter);
        const int64_t us = m_lastBodyNs / 1000;
        // a held body (a display transaction: C runs its plane offs, no frame is due) apart, so
        // the histogram is the playing bodies' only
        if (m_bodyHeld)
        {
          m_report.bodyHeld++;
          m_report.bodyHeldMaxUs = std::max(m_report.bodyHeldMaxUs, us);
        }
        else
        {
          m_report.bodyHist[us < 1000 ? 0 : us < 5000 ? 1 : us < 10000 ? 2 : us < 20000 ? 3 : 4]++;
          m_report.bodyMaxUs = std::max(m_report.bodyMaxUs, us);
        }
      }
      m_pollEnterNs = enter;
    }
    struct timespec ts = {static_cast<time_t>(timeoutNs / 1000000000),
                          static_cast<long>(timeoutNs % 1000000000)};
    if (ppoll(fds, 3 + PLANE_COUNT, timeoutNs < 0 ? nullptr : &ts, nullptr) < 0 &&
        errno != EINTR)
      GRAPHICSTICK::GTLogF(LOGERROR, "CPresentationCoordinator - poll failed: {}", strerror(errno));
    m_pollReturnNs = MonotonicNs();
    std::fill(std::begin(m_phaseNs), std::end(m_phaseNs), 0);
    m_bodyHeld = false;
    if (m_bStop)
      break;

    const bool woke = fds[0].revents & POLLIN;
    bool wakePending = woke;
    if (woke)
    {
      uint64_t count;
      if (read(m_wakeFd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake read failed: {}", strerror(errno));
    }

    for (int plane = 0; plane < PLANE_COUNT; plane++)
    {
      if (!(fds[3 + plane].revents & (POLLIN | POLLERR)))
        continue;
      std::unique_lock lock(m_uiMutex);
      UiBuffer& ready = m_planes[plane].ready;
      // the same submit: its fd may have been closed and the number reused
      if (ready.seq == fenceSeq[plane] && ready.fence == fence[plane])
      {
        close(ready.fence);
        ready.fence = -1;
        ready.readyNs = MonotonicNs();
      }
    }

    SPresentTick tick;
    bool gotTick = false;
    if (fds[1].revents & POLLIN)
      HandleEvents(m_masterFd, tick, gotTick, epoch);
    if ((fds[2].revents & POLLIN) && timeline)
      ReadTimelineTick(tick, gotTick, epoch);
    else if (fds[2].revents & POLLIN)
      HandleEvents(m_vblankFd, tick, gotTick, epoch);
    StampPhase(PHASE_EVENTS);

    const int64_t now = MonotonicNs();

    if (!video)
    {
      m_lastSeq = 0;
      if (planeOffOnC)
      {
        m_tickSeq = 0;
        m_tickVblankNs = 0;
      }
      if (!held)
      {
        SetState(State::IDLE, epoch);
        if (planeOffOnC)
          RunPlaneOffs(false);
        // GT-4: the route's end clear, with no ticks to pick it
        GRAPHICSTICK::SetCAdvancing(false);
        SelectTimed(false);
      }
      int flips;
      {
        std::unique_lock lock(m_uiMutex);
        flips = m_report.uiFlips;
      }
      if (m_report.ticks || flips >= REPORT_TICKS)
        LogReport();
      continue;
    }

    if (!gotTick)
    {
      // 5.V3-4: a wake with no tick (paused, or between vblanks; also while held)
      if (wakePending)
      {
        RunWakeTail();
        wakePending = false;
      }
      // GT-5: a buffer G handed in after its slot (GT-D15)
      SelectTimedLate(now);
      StampPhase(PHASE_NOTICK);
      // no vblank for a while (CRTC off or stalled): keep time with a timer
      // tick, as the render thread's 50 ms poll fallback did
      if (now - m_lastTickNs < SYNTHETIC_TICK_NS)
        continue;
      tick.seq = 0;
      tick.vblankNs = 0;
      tick.wokeNs = now;
      tick.epoch = epoch;
      m_report.synthetic++;
    }
    const bool tickHeld = RunVideoTick(tick);
    StampPhase(PHASE_TICK);
    if (!tickHeld)
      SelectTimed(true);
    StampPhase(PHASE_SELECT);
    // the tick first (plan §3 V3-4)
    if (wakePending)
      RunWakeTail();
    if (planeOffOnC)
      RunPlaneOffs(tickHeld);
    StampPhase(PHASE_END);
  }

  m_planeOffOpen = false;
  if (OUTPUT::PlaneOffOnC())
  {
    {
      std::unique_lock lock(m_uiMutex);
      for (const auto& op : m_planeOffs)
        op->state = PlaneOffOp::State::DROPPED;
      m_planeOffs.clear();
    }
    m_uiCond.notify_all();
  }
  aml_presenter_set_running(false);
}

void CPresentationCoordinator::QueueVblank(unsigned int epoch)
{
  const int64_t now = MonotonicNs();
  if (m_eventPending && now - m_queuedNs > EVENT_LOST_NS)
    m_eventPending = false;
  if (m_eventPending)
    return;

  uint64_t target = 0;
  if (drmCrtcQueueSequence(m_vblankFd, m_crtc, DRM_CRTC_SEQUENCE_RELATIVE, 1, &target, epoch) == 0)
  {
    m_eventPending = true;
    m_queuedNs = now;
  }
}

void CPresentationCoordinator::HandleEvents(int fd,
                                            SPresentTick& tick,
                                            bool& gotTick,
                                            unsigned int epoch)
{
  drmEventContext context = {};
  context.version = 4;
  context.sequence_handler = OnSequence;
  context.page_flip_handler2 = OnPageFlip;
  t_event.got = false;
  t_flipped = false;
  drmHandleEvent(fd, &context);

  if (t_flipped)
    OnFlip(t_flipTag, t_flipSequence, t_flipNs);

  if (!t_event.got)
    return;
  m_eventPending = false;
  // queued before a display transaction: its time may be the CRTC switching
  // off, not a vsync; the source is alive, so no timer tick either
  if (t_event.epoch != epoch)
  {
    m_report.stale++;
    m_lastTickNs = MonotonicNs();
    return;
  }
  tick.seq = t_event.seq;
  tick.vblankNs = static_cast<int64_t>(t_event.ns);
  tick.wokeNs = MonotonicNs();
  tick.epoch = epoch;
  gotTick = true;
  NoteVblank(tick.vblankNs);
}

void CPresentationCoordinator::ReadTimelineTick(SPresentTick& tick,
                                                bool& gotTick,
                                                unsigned int epoch)
{
  uint64_t count;
  if (read(m_tickFd, &count, sizeof(count)) < 0 && errno != EAGAIN)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - tick read failed: {}", strerror(errno));

  int64_t tl[PRESENTATION::TL_COUNT];
  // timer-made ticks are not vblanks: no vblank keeps the coordinator's own
  // timer tick, as with its own events
  if (!PRESENTATION::TimelineBoard().Read(tl) || tl[PRESENTATION::TL_SYNTHETIC] ||
      tl[PRESENTATION::TL_TICK] == m_lastTimelineTick)
    return;
  m_lastTimelineTick = tl[PRESENTATION::TL_TICK];
  const int64_t vblankNs = tl[PRESENTATION::TL_VBLANK_NS];
  // published before this attach: not a vblank this attach waits for
  if (vblankNs <= m_attachNs)
    return;
  // before the current display epoch: its time may be the CRTC switching off,
  // not a vsync (as an event queued before the transaction); the source is
  // alive, so no timer tick either
  if (vblankNs <= m_seenEpochNs)
  {
    m_report.stale++;
    m_lastTickNs = MonotonicNs();
    return;
  }
  tick.seq = static_cast<uint64_t>(tl[PRESENTATION::TL_KERNEL_SEQ]);
  tick.vblankNs = vblankNs;
  tick.wokeNs = MonotonicNs();
  tick.epoch = epoch;
  gotTick = true;
  m_report.timeline = true;
  NoteVblank(vblankNs);
}

void CPresentationCoordinator::NoteVblank(int64_t vblankNs)
{
  {
    std::unique_lock lock(m_vblankMutex);
    m_lastVblankNs = vblankNs;
  }
  m_vblankCond.notify_all();
}

bool CPresentationCoordinator::UiHoldForSlot()
{
  return m_videoAttached && m_gtFlipSeqValid && m_diagVblankSeq &&
         static_cast<int32_t>(m_gtFlipSeq - static_cast<uint32_t>(m_diagVblankSeq)) == 1 &&
         MonotonicNs() - m_gtFlipNs < UI_SLOT_HOLD_NS && GRAPHICSTICK::RouteOn();
}

void CPresentationCoordinator::CommitUi()
{
  {
    std::unique_lock lock(m_uiMutex);
    // the GUI hold ends when its condition does (the tick came, or the 3 ms bound): measured
    // here, before any commit in flight can delay the frame further (that wait is any GUI
    // frame's, not the hold's; RC17 read 16-20 ms by measuring to the commit)
    // RC21 am9pro FP n50 (uiHeldMax 5489 us): the hold ends at the wake that ended its
    // condition (the tick, or the bound's timeout); C's own body after that wake (a 5.2 ms tick
    // waiting 4.8 ms for RM's m_presentlock there) delays any GUI frame and is no hold: apart
    if (m_uiHoldStartNs && !UiHoldForSlot())
    {
      const int64_t now = MonotonicNs();
      const int64_t end = std::clamp(m_pollReturnNs, m_uiHoldStartNs, now);
      m_report.uiHeldMaxUs = std::max(m_report.uiHeldMaxUs, (end - m_uiHoldStartNs) / 1000);
      m_report.uiHoldEndToCommitMaxUs =
          std::max(m_report.uiHoldEndToCommitMaxUs, (now - end) / 1000);
      m_uiHoldStartNs = 0;
      m_uiHoldUntilNs = 0;
    }
    // one commit in flight across both planes; its flip can land before the
    // worker is back from the ioctl
    if (m_inFlight || m_committing.req)
      return;
    // 5.B2-7: a queued plane off goes first, with no flip in flight
    if (!m_planeOffs.empty())
    {
      if (m_planes[PLANE_UI].ready.req || m_planes[PLANE_GRAPHICS].ready.req)
        m_uiDeferred++;
      return;
    }
    if (m_cbufReq)
      PlaceClearedUi();
    // every plane with a submit whose GPU work is done goes in the one commit;
    // a plane left out keeps what it shows (R3)
    // F2 (RC20 FP n5994 serial 677, delta 2 behind a closing seek bar's GUI commit at every
    // vblank): the graphics plane is decided first, so the GUI hold below knows whether a
    // graphics buffer goes in this commit. Decided after the GUI (plane 0) it read a buffer the
    // stale hold then kept out as going: the GUI was not held and committed alone at the flip's
    // wake, and its flight pushed the held pick one more vblank late
    bool gfxHeld = false;
    const char* gfxWhy = nullptr; //!< why a picked graphics buffer stays out of this commit
    bool gaveBack = false;
    {
      UiBuffer& ready = m_planes[PLANE_GRAPHICS].ready;
      // GT-5 (review rA M2, rB M1): a late pick whose GPU fence or a commit in flight held it
      // past the point where its commit still latches on its frame's vblank would land one late
      // and push the next frame's composition late behind its flight: it goes back to the ready
      // list, and the next slot picks (the next frame's entry, or this one as the latest earlier)
      if (ready.req && ready.lateDeadlineNs && MonotonicNs() > ready.lateDeadlineNs)
      {
        GiveBackLatePick();
        gaveBack = true;
        gfxWhy = "late pick given back";
      }
      // GT-D15 (user ruling 2026-11-22, "re-sync at once"): a timed composition whose own frame
      // has left the screen is not committed at a flip's wake ahead of this vblank's slot: it
      // would land a vblank late and its flight would push every following pick one late (RC12
      // coreelec-br K1 n50: 37 frames). It waits for the slot, which replaces it with the
      // current frame's pick, or commits it there when nothing newer exists (a still frame).
      // Timed frame entries only: no clear, bridge or first page
      // Held for one slot only (RC14 coreelec-br FP n5994: serial 680 committed 184 ms, 11
      // vblanks, after its slot): the vblank whose slot decides is fixed at the first hold. A
      // GUI commit's flip at every vblank (a closing seek bar) kept the newest flip ahead of the
      // last tick, and a still page (nothing newer to replace it) was held again at each wake.
      // Once that slot has run, the buffer is committed at the next chance, never held again
      // Review fixes: the frame has left the screen only when a newer frame was released early
      // enough to be on VD1 at the landing vblank (2+ vblanks per frame: 25p@50, 29.97@59.94:
      // its own frame is still up at target + 1); the hold needs video ticking and lasts at
      // most one period after the flip (a detach or a counter reset never keeps it). The flip
      // event's sequence and tick.seq are one counter: the timeline's TL_KERNEL_SEQ is the
      // Timekeeper's drmCrtcQueueSequence sequence on the same CRTC (used only when the
      // coordinator's CRTC is the timekeeper's, AttachVideo), the own-vblank path's is the
      // vblank event's, and page_flip_handler2's is that CRTC's vblank count (low 32 bits)
      const int64_t holdPeriodNs = m_diagPeriodNs > 0 ? m_diagPeriodNs : 20000000;
      if (ready.req && ready.timed && !ready.clear && !ready.bridge &&
          !ready.firstPage && ready.target && m_gtFlipSeqValid && m_diagVblankSeq &&
          m_videoAttached && MonotonicNs() - m_gtFlipNs < holdPeriodNs)
      {
        const SReleasedFrame& newest = m_gtReleased[0];
        const bool frameGone =
            newest.valid && newest.qbuf != ready.qbuf &&
            static_cast<int32_t>(m_gtFlipSeq + 1 - (static_cast<uint32_t>(newest.tick) + 2)) >= 0;
        if (!ready.staleDeferred && frameGone &&
            static_cast<int32_t>(m_gtFlipSeq + 1 - static_cast<uint32_t>(ready.target)) > 0 &&
            static_cast<int32_t>(m_gtFlipSeq - static_cast<uint32_t>(m_diagVblankSeq)) > 0)
        {
          ready.staleDeferred = true;
          ready.staleSlotSeq = m_gtFlipSeq;
        }
        if (ready.staleDeferred &&
            static_cast<int32_t>(ready.staleSlotSeq - static_cast<uint32_t>(m_diagVblankSeq)) > 0)
        {
          gfxHeld = true;
          gfxWhy = "stale hold";
        }
      }
      // F1: a fence signalled since the pick (or the last poll) never costs this commit
      if (ready.req && !gfxHeld && !ReapFence(ready))
        gfxWhy = "fence pending";
    }
    Committing commit;
    std::vector<SUntimedPost> untimed;
    UiBuffer timedTags;
    for (int plane = 0; plane < PLANE_COUNT; plane++)
    {
      UiBuffer& ready = m_planes[plane].ready;
      if (plane == PLANE_GRAPHICS && gfxHeld)
        continue;
      if (plane == PLANE_UI && ready.req)
        ReapFence(ready);
      if (!ready.req || ready.fence >= 0)
        continue;
      // GT-D15 / MC-29 ("C issues all OSD commits in the post-tick slot while video is
      // attached"): a GUI frame ready at a flip's wake whose tick is imminent (the flip one
      // vblank past the last tick handled) waits for that slot, so GUI and graphics go in one
      // commit; committed alone it took the slot and the timed pick landed a vblank late behind
      // its flight (RC14 FP n5994 while the seek bar closed). Bounded at 3 ms after the flip: a
      // missing tick never holds the GUI. The route only, video ticking
      // never while a graphics buffer goes in this commit: the GUI joins it, rather than wait
      // for its flight (RC17: uiHeldMax 16-20 ms, one period, a graphics commit alone during
      // the hold)
      const UiBuffer& graphicsReady = m_planes[PLANE_GRAPHICS].ready;
      const bool graphicsNow = graphicsReady.req && !gfxHeld && graphicsReady.fence < 0;
      if (plane == PLANE_UI && !ready.cOwned && !graphicsNow && UiHoldForSlot())
      {
        if (!m_uiHoldStartNs)
        {
          m_uiHoldStartNs = MonotonicNs();
          m_report.uiHeldForSlot++;
        }
        m_uiHoldUntilNs = m_gtFlipNs + UI_SLOT_HOLD_NS;
        continue;
      }

      if (plane == PLANE_GRAPHICS)
      {
        // a plane the driver refused would take every GUI frame down with it
        if (m_graphicsRejected || (m_gfxSuspect && !m_gfxRetryArmed))
        {
          // GT-1 r1 N1: its posts are never shown
          if (s_untimedLog)
            LogUntimedLost(ready.untimed, "graphics plane refused");
          if (ready.u)
            m_gtCarriedU = std::max(m_gtCarriedU, ready.u);
          // review rB m9: G draws what is up again (a static menu gets no further post). Not in
          // the suspect hold (r2 N5): every buffer is dropped there until a GUI-only commit
          // succeeds, and GraphicsResubmit brings the fresh frame then
          if (ready.timed && m_graphicsRejected)
            GRAPHICSTICK::NoteGraphicsLost();
          Drop(plane, ready);
          continue;
        }
        // only over a GUI plane that shows something: the plane is switched
        // off with the GUI plane's buffer, and must never be the only one up
        if (!m_planes[PLANE_UI].onScreenFb && !commit.has[PLANE_UI])
          continue;
      }
      if (!commit.req)
        commit.req = ready.req;
      else
      {
        if (drmModeAtomicMerge(commit.req, ready.req) < 0)
          continue;
        drmModeAtomicFree(ready.req);
      }
      if (plane == PLANE_UI && !ready.cOwned)
        TimeUiCommit(std::max(ready.readyNs, m_diagUnblockedNs));
      commit.has[plane] = true;
      commit.bo[plane] = ready.bo;
      commit.generation[plane] = m_planes[plane].generation;
      commit.pts[plane] = ready.pts;
      m_planes[plane].inCommit = true;
      m_planes[plane].committedBo = ready.bo;
      m_planes[plane].committedFb =
          ready.bo || ready.cOwned ? ready.fb : m_planes[plane].onScreenFb;
      m_planes[plane].committedEpoch = ready.epoch;
      m_planes[plane].committedCOwned = ready.cOwned;
      commit.cOwned |= ready.cOwned;
      if (s_untimedLog)
        untimed.insert(untimed.end(), ready.untimed.begin(), ready.untimed.end());
      if (plane == PLANE_GRAPHICS && ready.timed)
      {
        timedTags.timed = true;
        timedTags.serial = ready.serial;
        timedTags.clear = ready.clear;
        timedTags.bridge = ready.bridge;
        timedTags.visible = ready.visible;
        timedTags.qbuf = ready.qbuf;
        timedTags.target = ready.target;
        timedTags.fb = ready.fb;
        timedTags.u = ready.u;
        timedTags.firstPage = ready.firstPage;
        timedTags.afterHold = ready.afterHold;
        timedTags.assUs = ready.assUs;
        timedTags.drawUs = ready.drawUs;
        timedTags.submitNs = ready.submitNs;
        timedTags.pickVblankNs = ready.pickVblankNs;
        timedTags.fenceAtPick = ready.fenceAtPick;
      }
      ready = UiBuffer();
    }
    // F1/F2 measurement (log only, at most 5 lines a second): a GUI commit sent alone while a
    // picked graphics buffer waits takes the flight its commit needs (RC20 FP: 1-2 periods)
    const UiBuffer& gfxLeft = m_planes[PLANE_GRAPHICS].ready;
    if (commit.req && commit.has[PLANE_UI] && !commit.has[PLANE_GRAPHICS] && m_videoAttached &&
        ((gfxLeft.req && gfxLeft.timed) || gaveBack))
    {
      m_report.uiAloneGfxWaits++;
      const int64_t now = MonotonicNs();
      if (now - m_gtUiAloneWindowNs >= 1000000000)
      {
        m_gtUiAloneWindowNs = now;
        m_gtUiAloneLines = 0;
      }
      if (++m_gtUiAloneLines <= 5)
        GRAPHICSTICK::GTLogF(
            LOGINFO, "GTICK gui alone while a graphics pick waits: why={} serial={} target={} flip={} tick={}",
            gfxWhy ? gfxWhy : "other", gfxLeft.req ? static_cast<int64_t>(gfxLeft.serial) : -1,
            gfxLeft.req ? static_cast<int64_t>(gfxLeft.target) : -1, m_gtFlipSeq, m_diagVblankSeq);
    }
    if (!commit.req)
      return;
    if (commit.cOwned)
      GRAPHICSTICK::GTLogF(LOGINFO, "CBUF on epoch={} fb={} graphics={}", m_cbufEpoch, m_cbuf.fb,
                commit.has[PLANE_GRAPHICS] ? 1 : 0);
    // in flight from here: the flip event can arrive before the ioctl returns
    m_committing = commit;
    m_inFlight = true;
    m_inCommitTag = ++m_commitTagSeq;
    if (s_untimedLog)
      m_untimedInFlight = std::move(untimed);
    m_inFlightGfxPts = commit.has[PLANE_GRAPHICS] ? commit.pts[PLANE_GRAPHICS] : DVD_NOPTS_VALUE;
    m_inFlightTimed = timedTags;
    // item 8: the L5 subtitle half of what G draws, with the commit that shows it
    if (timedTags.timed && timedTags.visible != m_planeSubsVisible)
    {
      m_planeSubsVisible = timedTags.visible;
      aml_dv_set_plane_subtitles_visible(m_planeSubsVisible);
    }
    m_flipLostLogged = false;
    m_commitNs = MonotonicNs();
    m_inFlightCommitNs = m_commitNs;
  }
  m_uiCond.notify_all();
}

void CPresentationCoordinator::TimeUiCommit(int64_t readyNs)
{
  // only while video ticks give the vblanks, from a recent one
  const int64_t now = MonotonicNs();
  const int64_t period = m_diagPeriodNs;
  if (!m_videoAttached || !readyNs || !m_diagVblankNs || period <= 0 ||
      now - m_diagVblankNs > 4 * period)
    return;
  // the first vblank after the submit became committable (fence, flight and plane offs)
  const int64_t since = readyNs - m_diagVblankNs;
  const int64_t periods = (since >= 0 ? since / period : -((-since + period - 1) / period)) + 1;
  const int64_t deadline = m_diagVblankNs + periods * period;
  m_report.uiTimed++;
  if (now > deadline)
    m_report.uiLate++;
}

void CPresentationCoordinator::PlaceClearedUi()
{
  PlaneState& ui = m_planes[PLANE_UI];
  // one shot per set (R4-M4)
  drmModeAtomicReqPtr req = std::exchange(m_cbufReq, nullptr);
  // a GUI buffer of this epoch goes up instead: refused submits never queue (SM4)
  const bool mainFirst = ui.ready.req || (ui.onScreen && ui.onScreenEpoch >= m_cbufEpoch);
  if (mainFirst || m_cbufUp)
  {
    drmModeAtomicFree(req);
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF not needed at epoch {} ({})", m_cbufEpoch,
              mainFirst ? "main's buffer first" : "already up");
    return;
  }
  // in the GUI plane's queue, so the graphics plane's ready frame joins its commit (SM7)
  ui.ready.fb = m_cbuf.fb;
  ui.ready.req = req;
  ui.ready.fence = -1;
  ui.ready.seq = ++m_submitSeq;
  ui.ready.epoch = m_cbufEpoch;
  ui.ready.cOwned = true;
}

void CPresentationCoordinator::CommitWorker()
{
  OUTPUT::SetThreadRole("C");
  // The meson commit ioctl can block for a vsync (and, in the worst case,
  // seconds) on the previous commit's cleanup: never on the coordinator.
  std::unique_lock lock(m_uiMutex);
  while (!m_commitStop)
  {
    if (!m_committing.req)
    {
      m_uiCond.wait(lock);
      continue;
    }
    const Committing commit = m_committing;
    const uint64_t tag = m_inCommitTag;
    lock.unlock();

    const int64_t start = MonotonicNs();
    const int ret = drmModeAtomicCommit(m_masterFd, commit.req,
                                        DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                                        reinterpret_cast<void*>(static_cast<uintptr_t>(tag)));
    const int err = errno;
    const double took = static_cast<double>(MonotonicNs() - start) / 1000.0;
    drmModeAtomicFree(commit.req);

    lock.lock();
    m_committing = Committing();
    m_diagUnblockedNs = MonotonicNs();
    m_report.commitMax = std::max(m_report.commitMax, took);
    // TEMP TICKDIAG (timebase audit Q6)
    if (took >= 20000.0)
      GRAPHICSTICK::GTLogF(LOGINFO, "TICKDIAG commit {:.0f} us (ui {} graphics {})", took,
                commit.has[PLANE_UI], commit.has[PLANE_GRAPHICS]);
    m_report.commitHist[took < 1000.0 ? 0 : took < 5000.0 ? 1 : took < 20000.0 ? 2 : 3]++;
    if (ret == 0)
    {
      if (commit.has[PLANE_UI])
        m_report.uiCommits++;
      if (commit.has[PLANE_GRAPHICS])
      {
        m_report.gfxCommits++;
        m_graphicsOn = true;
      }
      // the commit's success proves the CRTC lit
      if (m_gfxSuspect && commit.has[PLANE_GRAPHICS])
      {
        m_gfxSuspect = false;
        m_gfxRetryArmed = false;
        m_gfxRetriesRefused = 0;
        GRAPHICSTICK::GTLogF(LOGINFO, "CPresentationCoordinator - the graphics plane was accepted again "
                           "after a suspect refusal");
      }
      else if (m_gfxSuspect && !m_gfxRetryArmed)
      {
        m_gfxRetryArmed = true;
        m_gfxResubmit++;
      }
    }
    // a failed commit is never shown: its buffers go back to their producers,
    // unless their surface is gone
    else if (m_inFlight && m_inCommitTag == tag)
    {
      m_inFlight = false;
      m_inCommitTag = 0;
      m_diagUnblockedNs = MonotonicNs();
      // GT-1 r1 N1: a failed commit is never shown; its route-C posts are lost, GT-5's states
      // stay in m_gtPosts and are logged "carried" at the flip that shows them
      if (s_untimedLog)
        LogUntimedLost(m_untimedInFlight, "commit failed");
      if (m_inFlightTimed.u)
        m_gtCarriedU = std::max(m_gtCarriedU, m_inFlightTimed.u);
      // review rB m9: G draws what is up again (a static menu gets no further post)
      if (commit.has[PLANE_GRAPHICS] && GRAPHICSTICK::RouteOn())
        GRAPHICSTICK::NoteGraphicsLost();
      m_inFlightTimed = UiBuffer();
      m_inFlightGfxPts = DVD_NOPTS_VALUE;
      for (int plane = 0; plane < PLANE_COUNT; plane++)
      {
        PlaneState& state = m_planes[plane];
        if (!commit.has[plane] || commit.generation[plane] != state.generation)
          continue;
        if (state.committedBo)
          state.returned.push_back({state.committedBo, state.committedFb, state.committedEpoch});
        state.inCommit = false;
        state.committedBo = nullptr;
        state.committedFb = 0;
        state.committedCOwned = false;
      }
      if (m_report.uiFailed++ < 3)
        GRAPHICSTICK::GTLogF(LOGWARNING, "CPresentationCoordinator - plane commit failed: {}",
                  strerror(err));
      // not retried: main's first buffer of the epoch brings the GUI plane back (R4-M4);
      // the graphics plane is not blamed for it
      if (commit.cOwned)
        GRAPHICSTICK::GTLogF(LOGERROR, "CBUF refused: {}", strerror(err));
      else if (commit.has[PLANE_GRAPHICS] && !m_graphicsRejected && OUTPUT::RefusalConfirmLive())
      {
        m_report.gfxSuspect++;
        // a graphics commit while suspect is a retry after a GUI-only success
        if (m_gfxSuspect)
          m_gfxRetriesRefused++;
        m_gfxSuspect = true;
        m_gfxRetryArmed = false;
        if (m_gfxRetriesRefused >= 3)
        {
          m_graphicsRejected = true;
          GRAPHICSTICK::GTLogF(LOGERROR,
                    "CPresentationCoordinator - the graphics plane was refused ({}), committing "
                    "the GUI plane alone from now on (confirmed: {} retries after GUI-only "
                    "commits)",
                    strerror(err), m_gfxRetriesRefused);
        }
        else if (m_gfxSuspectLogs++ < 3)
          GRAPHICSTICK::GTLogF(LOGWARNING,
                    "CPresentationCoordinator - the graphics plane was refused ({}): suspect, "
                    "held until a GUI-only commit succeeds (retries refused {})",
                    strerror(err), m_gfxRetriesRefused);
      }
      else if (commit.has[PLANE_GRAPHICS] && !m_graphicsRejected)
      {
        m_graphicsRejected = true;
        GRAPHICSTICK::GTLogF(LOGERROR, "CPresentationCoordinator - the graphics plane was refused ({}), "
                            "committing the GUI plane alone from now on",
                  strerror(err));
      }
    }
    lock.unlock();
    const uint64_t one = 1;
    if (write(m_wakeFd, &one, sizeof(one)) < 0)
      GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
    lock.lock();
  }
}

void CPresentationCoordinator::SubmitTimed(std::vector<STimedBuffer>& buffers)
{
  {
    std::unique_lock lock(m_uiMutex);
    // review rB m6: a unit is refused whole when any of it is an old-regime redraw (item 5 (h)),
    // never split: a part would leave the new untimed state below the old one
    const auto refusedByEpoch = [this](const STimedBuffer& in)
    {
      return in.regimeEpoch &&
             std::any_of(m_timed.begin(), m_timed.end(),
                         [&](const UiBuffer& b)
                         {
                           return b.serial == in.serial && b.clear == in.clear &&
                                  b.bridge == in.bridge && b.session == in.session &&
                                  b.regimeEpoch > in.regimeEpoch;
                         });
    };
    const bool unitRefused =
        buffers.size() > 1 && std::any_of(buffers.begin(), buffers.end(), refusedByEpoch);
    for (STimedBuffer& in : buffers)
    {
      if (unitRefused)
      {
        UiBuffer refused;
        refused.bo = in.bo;
        refused.req = std::exchange(in.req, nullptr);
        refused.fence = std::exchange(in.fence, -1);
        Drop(PLANE_GRAPHICS, refused);
        m_report.gfxEpochDrop++;
        GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED gfxEpochDrop serial={} epoch={} (its unit)", in.serial,
                  in.regimeEpoch);
        continue;
      }
      // a redraw (regime, geometry, untimed images) of a composition C still holds replaces it
      auto it = std::find_if(m_timed.begin(), m_timed.end(),
                             [&](const UiBuffer& b)
                             {
                               return b.serial == in.serial && b.clear == in.clear &&
                                      b.bridge == in.bridge && b.session == in.session;
                             });
      if (it != m_timed.end())
      {
        // GT-5 item 5 (h): an old-regime redraw never replaces a new-regime entry
        if (in.regimeEpoch && it->regimeEpoch > in.regimeEpoch)
        {
          UiBuffer refused;
          refused.bo = in.bo;
          refused.req = std::exchange(in.req, nullptr);
          refused.fence = std::exchange(in.fence, -1);
          Drop(PLANE_GRAPHICS, refused);
          m_report.gfxEpochDrop++;
          GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED gfxEpochDrop serial={} epoch={} under={}", in.serial,
                    in.regimeEpoch, it->regimeEpoch);
          continue;
        }
        // item 5 (b) (ii): a waiting entry handed over again with a newer untimed state
        if (in.u > it->u && it->u)
          m_report.untimedReplaced++;
        Drop(PLANE_GRAPHICS, *it);
        m_timed.erase(it);
        m_report.gfxTimedReplaced++;
      }
      UiBuffer buffer;
      buffer.bo = in.bo;
      buffer.fb = in.fb;
      buffer.req = in.req;
      buffer.fence = in.fence;
      buffer.pts = in.pts;
      buffer.readyNs = in.fence < 0 ? MonotonicNs() : 0;
      buffer.seq = ++m_submitSeq;
      buffer.timed = true;
      buffer.serial = in.serial;
      buffer.clear = in.clear;
      buffer.bridge = in.bridge;
      buffer.visible = in.visible;
      buffer.session = in.session;
      buffer.u = in.u;
      buffer.firstPage = in.firstPage;
      buffer.assUs = in.assUs;
      buffer.drawUs = in.drawUs;
      buffer.submitNs = in.submitNs ? in.submitNs : MonotonicNs();
      buffer.regimeEpoch = in.regimeEpoch;
      buffer.untimedRoute = in.untimedRoute;
      const auto at = std::upper_bound(m_timed.begin(), m_timed.end(), in.serial,
                                       [](uint64_t s, const UiBuffer& b) { return s < b.serial; });
      m_timed.insert(at, std::move(buffer));
      m_report.gfxSubmits++;
      // C owns it from here
      in.req = nullptr;
      in.fence = -1;
    }
  }
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
}

int CPresentationCoordinator::TimedHeld()
{
  std::unique_lock lock(m_uiMutex);
  // GT-D15: a clear is not a composition drawn ahead (it goes at the first slot), so a floor's
  // clear held while C is (a seek's re-prime) no longer takes the segment's first frames' depth;
  // the buffer itself still counts in G's LOCK_CAP
  int held = 0;
  for (const UiBuffer& b : m_timed)
    if (!b.clear)
      held++;
  const UiBuffer& ready = m_planes[PLANE_GRAPHICS].ready;
  if (ready.timed && !ready.clear)
    held++;
  return held;
}

void CPresentationCoordinator::SelectTimed(bool ticked)
{
  if (!GRAPHICSTICK::Requested())
    return;
  bool woke = false;
  {
    std::unique_lock lock(m_uiMutex);
    // review rB m2: read under C's lock, so a buffer of a session that began after the read is
    // never dropped as stale (m_uiMutex -> GRAPHICSTICK's lock, the order NoteUntimedCommitted
    // already takes)
    bool on;
    uint64_t session;
    GRAPHICSTICK::RouteState(on, session);
    const GRAPHICSTICK::SFloor floor = GRAPHICSTICK::Floor();
    if (session != m_gtSession)
    {
      m_gtSession = session;
      m_gtPickedSerial = 0;
      m_gtPickedU = 0;
      // GT-5: a route session's states never show in the next one
      if (!m_gtPosts.empty())
      {
        std::vector<SUntimedPost> lost(m_gtPosts.begin(), m_gtPosts.end());
        m_gtPosts.clear();
        LogUntimedLost(lost, "route session ended");
      }
    }
    if (ticked)
      m_gtSlotMissed = false;
    const bool afterHold = ticked && m_gtWasWithheld && !m_gtWithheld;
    if (ticked)
      m_gtWasWithheld = m_gtWithheld;
    // r2 N2: a late pick past its deadline no longer waits for anything (its commit would land
    // late): back to the list before this slot's pick, so it never holds the next frame's slot.
    // Before the stale filter (review r3 R3-2): a floor or a session change since its pick
    // drops it like any other entry
    if (m_planes[PLANE_GRAPHICS].ready.req && m_planes[PLANE_GRAPHICS].ready.lateDeadlineNs &&
        MonotonicNs() > m_planes[PLANE_GRAPHICS].ready.lateDeadlineNs)
      GiveBackLatePick();
    const bool due = ticked && on && !m_gtWithheld && m_gtTarget.valid &&
                     m_gtTarget.serial > m_gtPickedSerial;
    if (m_timed.empty())
    {
      m_gtSlotMissed = due;
      if (!on && m_planeSubsVisible)
      {
        m_planeSubsVisible = false;
        aml_dv_set_plane_subtitles_visible(false);
      }
      return;
    }
    // M2: nothing at or below a floor is committed after it; a clear is the floor's own. Never
    // a composition older than one already picked (a redraw G made before it saw the newer)
    for (auto it = m_timed.begin(); it != m_timed.end();)
    {
      // a bridge is under no floor, but goes with its route (review r2 N4). GT-D12 (GT-4 r3 m-1):
      // a buffer of an earlier route session, held across the switch while C was, is gone too;
      // the session's own first composition stays (G does not redraw it)
      const bool stale = it->session != session ||
                         (it->bridge ? !on
                                     : (!on || it->serial <= floor.floor ||
                                        it->serial < m_gtPickedSerial));
      // review rB m2: an old session's clear, held across the switch, would sort after the new
      // session's first composition (serials restart per player) and drop it at the pick
      const bool oldClear = it->clear && it->session != session &&
                            std::any_of(m_timed.begin(), m_timed.end(), [&](const UiBuffer& b)
                                        { return b.session == session; });
      // r2 N6: a floor's clear that carries the menu is no longer the route's once it is off
      // (main draws the menu again); only an empty clear goes after the route
      const bool offMenuClear = it->clear && !on && it->u != 0;
      if ((!it->clear && stale) || oldClear || offMenuClear)
      {
        // item 5 (j): a state dropped whole is published as done, so G draws the next
        if (it->u && GRAPHICSTICK::NewestOff())
          GRAPHICSTICK::NoteUntimedCommitted(it->u);
        Drop(PLANE_GRAPHICS, *it);
        it = m_timed.erase(it);
        m_report.gfxTimedReplaced++;
        woke = true;
      }
      else
        ++it;
    }
    // GT-5 item 5 (f): a segment's first page, once picked, is committed at its slot or the next
    // flip; nothing replaces it meanwhile (later entries wait for the next slot)
    const UiBuffer& waiting = m_planes[PLANE_GRAPHICS].ready;
    // only while it waits for the flight or its fence, which end: a page CommitUi cannot place
    // (no GUI plane up) never holds the later entries back
    const bool pageWaits = waiting.req && waiting.timed && waiting.firstPage &&
                           (m_inFlight || waiting.fence >= 0);
    if (pageWaits)
      m_gtSlotMissed = false;
    // the newest buffer of a picture at or before the frame on screen next; a clear goes at
    // the first slot, before the start's T0 too (it only takes graphics away)
    const SReleasedFrame& target = m_gtTarget;
    int pick = -1;
    for (int i = 0; i < static_cast<int>(m_timed.size()) && !pageWaits; i++)
    {
      const UiBuffer& b = m_timed[i];
      // GT-5: an untimed-only buffer waits for the start's hold too (with video: C's withhold,
      // judged at its slot; with none: RM's start hold, cleared at E1's point)
      const bool bridgeHeld =
          b.untimedRoute && (ticked ? m_gtWithheld : GRAPHICSTICK::UntimedHeld());
      // R3-1: a floor's clear carrying the menu, before this session showed any untimed state
      // (a floor between the route start and T0), waits for the start's hold like the menu
      // itself; the plane is empty then (the start clear went), so holding it hides nothing
      const bool clearHeld = b.clear && b.u && m_gtPickedU == 0 && bridgeHeld;
      if ((b.clear && !clearHeld) || (b.bridge && on && !bridgeHeld) ||
          (ticked && on && !m_gtWithheld && target.valid && b.serial <= target.serial))
        pick = i;
    }
    // measurement (log only, at most 5 lines a second): a ticked slot that holds entries but
    // picks none, with what it holds (the RC8 Superman BD-J menu: 12 vblanks with no graphics
    // commit while a state was posted every vblank)
    // only an unexpected one: not while a first page waits for its commit, nor with no frame
    // to pick for or the start's withhold (before T0), where nothing is due; and only when C holds
    // a newer untimed state than the plane's (entries drawn ahead for frames still to come, with
    // nothing new for this one, are the normal case)
    const bool newerUntimed = std::any_of(m_timed.begin(), m_timed.end(),
                                          [this](const UiBuffer& b) { return b.u > m_gtPickedU; });
    if (pick < 0 && ticked && on && !m_timed.empty() && !pageWaits && target.valid &&
        !m_gtWithheld && newerUntimed)
    {
      const int64_t now = MonotonicNs();
      if (now - m_gtNoPickWindowNs >= 1000000000)
      {
        m_gtNoPickWindowNs = now;
        m_gtNoPickLines = 0;
      }
      if (++m_gtNoPickLines <= 5)
      {
        std::string held;
        for (const UiBuffer& b : m_timed)
          held += StringUtils::Format(" {}:u{}{}{}{}", b.serial, b.u, b.clear ? "c" : "",
                                      b.bridge ? "b" : "", b.session != session ? "o" : "");
        GRAPHICSTICK::GTLogF(LOGINFO,
                             "GTICK no pick target={} withheld={} picked={} pageWaits={} held:{}",
                             target.valid ? static_cast<int64_t>(target.serial) : -1,
                             m_gtWithheld ? 1 : 0, m_gtPickedSerial, pageWaits ? 1 : 0, held);
      }
    }
    if (pick < 0)
    {
      // with a first page waiting in the queue this slot picks nothing (not a miss)
      if (!pageWaits && ticked && on && !m_gtWithheld && target.valid &&
          target.serial > GRAPHICSTICK::Processed())
        m_report.gfxMissing++;
      if (!pageWaits)
        m_gtSlotMissed = due;
    }
    else
    {
      for (int i = 0; i < pick; i++)
      {
        Drop(PLANE_GRAPHICS, m_timed[i]);
        if (!m_timed[i].clear)
          m_report.gfxTimedReplaced++;
      }
      UiBuffer chosen = std::move(m_timed[pick]);
      m_timed.erase(m_timed.begin(), m_timed.begin() + pick + 1);
      chosen.qbuf = target.qbuf;
      chosen.target = target.tick + 2;
      chosen.afterHold = afterHold;
      chosen.pickVblankNs = m_diagVblankNs;
      // F1 (RC20 FP: every late placement had fence=1): a fence already signalled is closed
      // here, so this slot's commit takes the buffer; left open, CommitUi skipped it as pending
      // and a GUI frame committed alone took the slot (the fence's own wake came after)
      if (chosen.fence >= 0)
        chosen.fenceAtPick = ReapFence(chosen) ? 1 : 0;
      if (!chosen.clear && !chosen.bridge)
      {
        m_gtPickedSerial = std::max(m_gtPickedSerial, chosen.serial);
        GRAPHICSTICK::NotePickedSerial(session, m_gtPickedSerial);
      }
      // item 5 (d): no commit carries an older untimed state than the plane's (impossible by
      // G's re-draw unit; gated at 0)
      NotePickedU(chosen);
      UiBuffer& ready = m_planes[PLANE_GRAPHICS].ready;
      // review rA m1: route C's frame after a hand-back is newer than the route's end clear; the
      // start clear must still replace route C's last frame (review r2 N6)
      if (chosen.clear && !on && ready.req && !ready.timed)
        Drop(PLANE_GRAPHICS, chosen);
      else if (ready.req)
      {
        if (ready.timed && !ready.clear)
          m_report.gfxTimedReplaced++;
        else
          m_report.gfxReplaced++;
        // the re-sync: a pick held back because its own frame had left the screen, replaced by
        // the current frame's (gated at 0 once C's stalls are gone)
        if (ready.staleDeferred)
        {
          m_report.stalePickReplaced++;
          const int64_t now = MonotonicNs();
          if (now - m_gtStaleWindowNs >= 1000000000)
          {
            m_gtStaleWindowNs = now;
            m_gtStaleLines = 0;
          }
          if (++m_gtStaleLines <= 5)
            GRAPHICSTICK::GTLogF(LOGWARNING,
                                 "GTICK stalePickReplaced serial={} target={} by serial={} flip={}",
                                 ready.serial, ready.target, chosen.serial, m_gtFlipSeq);
        }
        Drop(PLANE_GRAPHICS, ready);
      }
      if (chosen.req)
        ready = std::move(chosen);
      woke = true;
    }
  }
  if (woke)
  {
    // G may wait for one of these buffers
    GRAPHICSTICK::Kick();
    m_uiCond.notify_all();
    const uint64_t one = 1;
    if (write(m_wakeFd, &one, sizeof(one)) < 0)
      GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
  }
}

void CPresentationCoordinator::GiveBackLatePick()
{
  UiBuffer& ready = m_planes[PLANE_GRAPHICS].ready;
  UiBuffer back = std::move(ready);
  ready = UiBuffer();
  back.lateDeadlineNs = 0;
  m_report.gfxLatePickBack++;
  // r2 N4: a redraw of it G handed in meanwhile (it found nothing to replace) is newer: the
  // given-back buffer goes, else it sorts first among its serial so the newest is picked
  const bool newer = std::any_of(m_timed.begin(), m_timed.end(),
                                 [&](const UiBuffer& b)
                                 {
                                   return b.serial == back.serial && b.clear == back.clear &&
                                          b.bridge == back.bridge && b.session == back.session;
                                 });
  if (newer)
  {
    Drop(PLANE_GRAPHICS, back);
    return;
  }
  const auto at = std::lower_bound(m_timed.begin(), m_timed.end(), back.serial,
                                   [](const UiBuffer& b, uint64_t s) { return b.serial < s; });
  m_timed.insert(at, std::move(back));
}

void CPresentationCoordinator::NotePickedU(const UiBuffer& chosen)
{
  if (!chosen.u)
    return;
  // item 5 (d): no commit carries an older untimed state than the plane's (impossible by G's
  // re-draw unit; gated at 0). A segment's first page starts its own order (review rB m5): the
  // frames before it may carry a newer state made while it waited (a keep-alive expiry, a
  // CleanUp), which is no regress
  if (chosen.firstPage)
    m_gtPickedU = chosen.u;
  else if (chosen.u < m_gtPickedU)
  {
    m_report.untimedRegress++;
    GRAPHICSTICK::GTLogF(LOGERROR, "GUNTIMED untimedRegress serial={} u={} under={}", chosen.serial,
              chosen.u, m_gtPickedU);
  }
  m_gtPickedU = std::max(m_gtPickedU, chosen.u);
  // gnewest_off's publication (item 5 (j)); G also reads it while C does not advance (R3-3)
  GRAPHICSTICK::NoteUntimedCommitted(chosen.u);
}

void CPresentationCoordinator::StampPhase(int phase)
{
  m_phaseNs[phase] = MonotonicNs();
}

void CPresentationCoordinator::LogSlowBody(int64_t enter)
{
  // GT-D15 measurement (log only, graphicstick_on): where a body over 3 ms went, phase by phase
  // from the poll's return (RC6 coreelec-br: bodies of 4.9-5.3 ms at INFO as at debug, the next
  // tick late by as much). 0: the phase did not run in that body
  if (m_lastBodyNs < 3000000 || !GRAPHICSTICK::Requested())
    return;
  static constexpr const char* names[PHASE_COUNT] = {"events", "present", "tick",   "select",
                                                     "end",    "notick",  "hold",   "commit"};
  std::string parts;
  int64_t last = m_pollReturnNs;
  for (int i = 0; i < PHASE_COUNT; i++)
  {
    if (!m_phaseNs[i])
    {
      parts += StringUtils::Format(" {}=0", names[i]);
      continue;
    }
    parts += StringUtils::Format(" {}={}", names[i], (m_phaseNs[i] - last) / 1000);
    last = m_phaseNs[i];
  }
  GRAPHICSTICK::GTLogF(LOGINFO, "GTLAT C body us={}{} rest={} held={} tail={} wake={}",
                       m_lastBodyNs / 1000, parts, (enter - last) / 1000, m_bodyHeld ? 1 : 0,
                       m_phaseTailUs, m_phaseWakeUs);
  m_phaseTailUs = m_phaseWakeUs = 0;
}

void CPresentationCoordinator::SelectTimedLate(int64_t now)
{
  if (!m_gtSlotMissed || !GRAPHICSTICK::Requested())
    return;
  // the commit must land on the vblank this interval's slot was for, never later: else the next
  // slot picks (for the next frame, which a late commit of this one would push off)
  const int64_t period = m_diagPeriodNs;
  if (period <= 0 || !m_diagVblankNs || m_diagVblankNs + period - now < LATE_PICK_MARGIN_NS)
  {
    m_gtSlotMissed = false;
    return;
  }
  const SReleasedFrame& target = m_gtTarget;
  {
    std::unique_lock lock(m_uiMutex);
    bool on;
    uint64_t session;
    GRAPHICSTICK::RouteState(on, session);
    const GRAPHICSTICK::SFloor floor = GRAPHICSTICK::Floor();
    UiBuffer& ready = m_planes[PLANE_GRAPHICS].ready;
    // a commit in flight would hold it to the flip, one vblank late: the next slot decides
    if (!on || m_gtWithheld || !target.valid || session != m_gtSession || m_inFlight || ready.req)
      return;
    int pick = -1;
    for (int i = 0; i < static_cast<int>(m_timed.size()); i++)
    {
      const UiBuffer& b = m_timed[i];
      if (!b.clear && !b.bridge && b.session == session && b.serial > floor.floor &&
          b.serial >= m_gtPickedSerial && b.serial <= target.serial)
        pick = i;
    }
    if (pick < 0)
      return;
    for (int i = 0; i < pick; i++)
    {
      Drop(PLANE_GRAPHICS, m_timed[i]);
      if (!m_timed[i].clear)
        m_report.gfxTimedReplaced++;
    }
    UiBuffer chosen = std::move(m_timed[pick]);
    m_timed.erase(m_timed.begin(), m_timed.begin() + pick + 1);
    chosen.qbuf = target.qbuf;
    chosen.target = target.tick + 2;
    // its fence is usually still pending: the commit must still go out by the margin, or CommitUi
    // gives it back to the next slot
    chosen.lateDeadlineNs = m_diagVblankNs + period - LATE_PICK_MARGIN_NS;
    if (chosen.fence >= 0)
      chosen.fenceAtPick = ReapFence(chosen) ? 1 : 0;
    m_gtPickedSerial = std::max(m_gtPickedSerial, chosen.serial);
    GRAPHICSTICK::NotePickedSerial(session, m_gtPickedSerial);
    NotePickedU(chosen);
    ready = std::move(chosen);
    m_gtSlotMissed = false;
    m_report.gfxLatePick++;
  }
  GRAPHICSTICK::Kick();
  m_uiCond.notify_all();
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
}

void CPresentationCoordinator::NoteUntimedRoute(std::vector<SUntimedPost> posts)
{
  std::unique_lock lock(m_uiMutex);
  for (SUntimedPost& post : posts)
  {
    // a state a flip already passed (G took it after a newer one showed) is judged at once
    // by the next flip, as a replaced one
    auto at = std::upper_bound(m_gtPosts.begin(), m_gtPosts.end(), post.u,
                               [](uint64_t u, const SUntimedPost& p) { return u < p.u; });
    m_gtPosts.insert(at, std::move(post));
  }
  // bounded: a route with no flips (the plane off) never shows them
  while (m_gtPosts.size() > 256)
    m_gtPosts.pop_front();
}

bool CPresentationCoordinator::PublishedPaused()
{
  return s_publishedState.load() == static_cast<int>(State::PAUSED);
}

void CPresentationCoordinator::LogUntimedLost(std::vector<SUntimedPost>& posts, const char* why)
{
  for (const SUntimedPost& post : posts)
    LogUntimed(post, why, "lost", -1, 0, 0);
  posts.clear();
}

bool CPresentationCoordinator::UntimedLog()
{
  return s_untimedLog;
}

void CPresentationCoordinator::NoteUntimed(int plane, std::vector<SUntimedPost> posts)
{
  std::unique_lock lock(m_uiMutex);
  m_untimedNext[plane].insert(m_untimedNext[plane].end(), posts.begin(), posts.end());
}

void CPresentationCoordinator::OnFlip(uint64_t tag, unsigned int sequence, int64_t flipNs)
{
  std::unique_lock lock(m_uiMutex);
  if (!m_inFlight || tag != m_inCommitTag)
    return;
  // the newest vblank C has seen (its flip can come before that vblank's tick)
  m_gtFlipSeq = sequence;
  m_gtFlipSeqValid = true;
  m_gtFlipNs = MonotonicNs();
  std::vector<SUntimedPost> untimed;
  if (s_untimedLog && flipNs > 0)
  {
    // the period from the flips' own vblank stamps, so menu-only screens have one too;
    // only over a short span (a mode change between two flips would skew it)
    const unsigned int vblanks = sequence - m_untimedFlipSeq;
    if (m_untimedFlipNs && vblanks > 0 && vblanks <= 120 && flipNs > m_untimedFlipNs)
      m_untimedPeriodNs = (flipNs - m_untimedFlipNs) / vblanks;
    m_untimedFlipSeq = sequence;
    m_untimedFlipNs = flipNs;
    untimed = std::move(m_untimedInFlight);
    m_untimedInFlight.clear();
  }
  const int64_t untimedPeriodNs = m_untimedPeriodNs;
  const UiBuffer timed = std::exchange(m_inFlightTimed, UiBuffer());
  const int64_t commitNs = m_inFlightCommitNs;
  const int64_t periodNs = m_diagPeriodNs;
  int delta = 0;
  if (timed.timed && !timed.clear && !timed.bridge)
  {
    delta = static_cast<int32_t>(sequence - static_cast<unsigned int>(timed.target));
    m_report.gfxTimed++;
    // the start's still's composition at T0 is on time by item 10 (its delta counts the hold)
    if (delta > 0 && !timed.afterHold)
      m_report.gfxLate++;
    // item 5 (f): pageLate when a first page missed its frame's vblank
    if (timed.firstPage)
    {
      m_report.firstPages++;
      if (delta > 0)
        m_report.pageLate++;
    }
  }
  // GT-5: the states this flip shows: its own and every older one not shown yet (item 5 (i))
  std::vector<SUntimedPost> routeShown;
  const uint64_t carriedU = m_gtCarriedU;
  if (timed.timed && timed.u > m_gtShownU)
  {
    m_gtShownU = timed.u;
    while (!m_gtPosts.empty() && m_gtPosts.front().u <= timed.u)
    {
      routeShown.push_back(std::move(m_gtPosts.front()));
      m_gtPosts.pop_front();
    }
  }
  if (m_inFlightGfxPts != DVD_NOPTS_VALUE)
    CountPlacement(m_inFlightGfxPts, sequence);
  bool cbufUp = false;
  int64_t replacedMs = -1;
  for (int plane = 0; plane < PLANE_COUNT; plane++)
  {
    PlaneState& state = m_planes[plane];
    if (!state.inCommit)
      continue;
    if (plane == PLANE_UI)
    {
      m_uiFlips++;
      if (state.committedCOwned)
      {
        // the GUI buffer it replaced is off screen
        if (state.onScreen)
          state.returned.push_back({state.onScreen, state.onScreenFb, state.onScreenEpoch});
        state.onScreen = nullptr;
        state.onScreenEpoch = 0;
        m_cbufUp = cbufUp = true;
        m_cbufUpNs = MonotonicNs();
      }
      else if (m_cbufUp && state.committedBo)
      {
        m_cbufUp = false;
        replacedMs = (MonotonicNs() - m_cbufUpNs) / 1000000;
      }
    }
    // a property-only commit leaves the buffer on screen where it is
    if (state.committedBo)
    {
      if (state.onScreen)
        state.returned.push_back({state.onScreen, state.onScreenFb, state.onScreenEpoch});
      state.onScreen = state.committedBo;
      state.onScreenEpoch = state.committedEpoch;
    }
    state.onScreenFb = state.committedFb;
    state.inCommit = false;
    state.committedBo = nullptr;
    state.committedFb = 0;
    state.committedCOwned = false;
  }
  m_inFlight = false;
  m_inCommitTag = 0;
  m_diagUnblockedNs = MonotonicNs();
  const double flip = static_cast<double>(MonotonicNs() - m_commitNs) / 1000.0;
  m_report.uiFlips++;
  m_report.flipSum += flip;
  m_report.flipMax = std::max(m_report.flipMax, flip);

  // a replaced cleared buffer, once a later GUI plane flip took it off the plane
  std::vector<DumbBuffer> freed;
  const PlaneState& ui = m_planes[PLANE_UI];
  for (auto it = m_cbufRetired.begin(); it != m_cbufRetired.end();)
  {
    if (m_uiFlips > it->flips && it->buffer.fb != ui.onScreenFb && it->buffer.fb != ui.committedFb)
    {
      freed.push_back(it->buffer);
      it = m_cbufRetired.erase(it);
    }
    else
      ++it;
  }
  const uint64_t epoch = m_cbufEpoch;
  lock.unlock();
  // a post replaced before its commit is judged on this flip and names what showed (by=)
  for (const SUntimedPost& post : untimed)
  {
    const SUntimedPost& shown = untimed.back();
    const bool replaced = &post != &shown;
    LogUntimed(post, replaced ? std::to_string(shown.u) : std::string("self"),
               post.carried ? "carried"
               : replaced   ? "replaced"
               : post.cleared ? "cleared"
                              : "shown",
               sequence, flipNs, untimedPeriodNs);
  }
  for (const SUntimedPost& post : routeShown)
  {
    const bool self = post.u == timed.u;
    const char* what = post.u <= carriedU ? "carried"
                       : !self            ? (post.skipped ? "skipped" : "replaced")
                       : post.firstPage   ? "first-page"
                       : post.cleared     ? "cleared"
                                          : "shown";
    LogUntimed(post, self ? std::string("self") : std::to_string(timed.u), what, sequence,
               flipNs, untimedPeriodNs);
  }
  // the scan-out probe joins these on flip=: the OSD change latched at that vblank (§4)
  if (timed.timed && timed.clear)
    GRAPHICSTICK::GTLogF(LOGINFO, "GCLEAR serial={} flip={} fb={} u={}", timed.serial, sequence, timed.fb,
              timed.u);
  else if (timed.timed && timed.bridge)
    GRAPHICSTICK::GTLogF(LOGINFO, "GBRIDGE serial={} flip={} fb={} u={}", timed.serial, sequence, timed.fb,
              timed.u);
  else if (timed.timed)
  {
    // GT-D15 measurement: G's libass and draw times, its hand-in against the picking slot's
    // vblank (negative: ahead), the fence at the pick, and the commit against that vblank
    const int64_t slot = timed.pickVblankNs;
    GRAPHICSTICK::GTLogF(LOGINFO,
              "GPLACE serial={} qbuf={} flip={} delta={} period_us={} fb={} u={} page={} hold={} "
              "ass_us={} draw_us={} submit_us={} fence={} commit_us={}",
              timed.serial, timed.qbuf, sequence, delta, periodNs / 1000, timed.fb, timed.u,
              timed.firstPage ? 1 : 0, timed.afterHold ? 1 : 0, timed.assUs, timed.drawUs,
              slot && timed.submitNs ? (timed.submitNs - slot) / 1000 : 0, timed.fenceAtPick,
              slot && commitNs ? (commitNs - slot) / 1000 : 0);
  }
  // the buffer this one replaced is G's again
  if (timed.timed)
    GRAPHICSTICK::Kick();
  if (cbufUp)
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF up epoch={} flip_seq={}", epoch, sequence);
  if (replacedMs >= 0)
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF replaced by main after {} ms", replacedMs);
  for (DumbBuffer& dumb : freed)
  {
    GRAPHICSTICK::GTLogF(LOGINFO, "CBUF retired fb={} {}x{} freed", dumb.fb, dumb.width, dumb.height);
    FreeDumb(dumb);
  }
}

void CPresentationCoordinator::CountPlacement(double pts, unsigned int sequence)
{
  // the flip event carries the low bits of the same vblank count
  const OnScreen* shown = nullptr;
  for (const OnScreen& entry : m_onScreen)
  {
    if (static_cast<int32_t>(sequence - static_cast<unsigned int>(entry.fromSeq)) >= 0)
      shown = &entry;
  }
  if (!shown || m_frameUs <= 0.0)
    return;
  const int frames = static_cast<int>(std::lround((pts - shown->pts) / m_frameUs));
  m_report.gfxPlacement[std::clamp(frames, -2, 2) + 2]++;
  GRAPHICSTICK::GTLogF(LOGDEBUG,
            "CPresentationCoordinator - graphics for pts {:.3f} on screen at vblank {} with video "
            "pts {:.3f} (shown from vblank {}, last tick {}): {:+d} frames",
            pts / 1000000.0, sequence, shown->pts / 1000000.0, shown->fromSeq, m_lastSeq, frames);
}

bool CPresentationCoordinator::RunVideoTick(SPresentTick& tick)
{
  // a hold that landed during the wait is acknowledged before any release
  AmlEpochHold hold;
  if (aml_presenter_check_hold(tick.epoch, !UiInFlight(), &hold))
  {
    m_lastTickNs = tick.wokeNs;
    return true;
  }
  tick.holdSite = hold.site;
  tick.holdTid = hold.tid;
  tick.holdReleaseNs = hold.releaseNs;

  const int64_t diagWait = MonotonicNs();
  std::unique_lock lock(m_videoMutex);
  // TEMP TICKDIAG (timebase audit Q6)
  if (MonotonicNs() - diagWait > 5000000)
    GRAPHICSTICK::GTLogF(LOGINFO, "TICKDIAG video mutex wait {} us",
              (MonotonicNs() - diagWait) / 1000);
  if (!m_video)
    return false;

  SPresentResult result;
  // GT-5 (item 10): the route's start withhold is judged at the vblank this slot's commit lands on
  tick.periodNs = m_diagPeriodNs;
  const int64_t start = MonotonicNs();
  m_video->PresentTick(tick, result);
  StampPhase(PHASE_PRESENT);
  const int64_t work = MonotonicNs() - start;
  m_lastTickNs = tick.wokeNs;
  // 5.V3-4: C's geometry tail, the RM locks released (outside work: the release is the QBUF's)
  m_geometryOnC = result.geom.renderer != nullptr;
  const std::shared_ptr<CBaseRenderer> renderer = result.geom.renderer;
  if (m_geometryOnC)
  {
    const int64_t tailStart = MonotonicNs();
    const bool wrote = m_video->GeometryTailOnC(result);
    const int64_t tailNs = MonotonicNs() - tailStart;
    m_phaseTailUs = tailNs / 1000; // GT-D15 measurement: GTLAT C body tail=
    m_report.tails++;
    m_lastTailNs = MonotonicNs();
    m_report.tailWrites += wrote;
    m_report.tailMaxNs = std::max(m_report.tailMaxNs, tailNs);
    if (m_tickPeriodNs && tailNs > m_tickPeriodNs)
      m_report.tailsOverVblank++;
  }
  // 5.V3-4f: the worker's wake for the buffers PresentTick freed, after the release
  if (result.wakeWorker)
  {
    // GT-D15 measurement: the wake takes RM's m_presentlock (V's and main's too): GTLAT wake=
    const int64_t wakeStart = MonotonicNs();
    m_video->WakeWorkerOnC();
    m_phaseWakeUs = (MonotonicNs() - wakeStart) / 1000;
  }

  // GT-4: this tick picks for the frame released at an earlier one (G-X1, offset 1); the frame
  // released now is G's next Released
  // GT-5 (review rA M1, rB M2): whether C picks frame entries at all, for G's untimed-only path.
  // Paused is the player's pause (as main's IsPausedPlayback), not the clock's: a scheduled
  // start's clock waits for T0 while its first frames' entries (the first page) are due
  if (GRAPHICSTICK::Requested())
    GRAPHICSTICK::SetCAdvancing(GRAPHICSTICK::RouteOn() && result.configured &&
                                CDataCacheCore::GetInstance().GetSpeed() != 0.0f);
  if (GRAPHICSTICK::RouteOn() && result.configured)
  {
    m_gtTarget = GRAPHICSTICK::TestDelay1() ? m_gtReleased[1] : m_gtReleased[0];
    m_gtWithheld = result.graphicsWithheld;
    if (result.newFrame && tick.vblankNs && result.qbufShown && result.qbufIndex >= 0)
    {
      m_gtReleased[1] = m_gtReleased[0];
      m_gtReleased[0] = {result.overlaySerial, result.qbufIndex, tick.seq, true};
      GRAPHICSTICK::SReleased released;
      released.serial = result.overlaySerial;
      released.segmentGen = result.segmentGen;
      released.qbuf = result.qbufIndex;
      released.tick = tick.seq;
      if (auto* aml = dynamic_cast<CRendererAML*>(renderer.get()))
        released.rects.valid =
            aml->VideoGeometryFromC(released.rects.source, released.rects.dest,
                                    released.rects.view, released.rects.rgen);
      GRAPHICSTICK::PostReleased(released);
      if (GRAPHICSTICK::ReleasedLog())
        GRAPHICSTICK::GTLogF(LOGINFO, "GRELEASED serial={} gen={} qbuf={} tick={}", released.serial,
                  released.segmentGen, released.qbuf, released.tick);
    }
  }
  else
  {
    m_gtReleased[0] = m_gtReleased[1] = m_gtTarget = SReleasedFrame();
    m_gtWithheld = false;
  }

  if (tick.vblankNs && OUTPUT::PlaneOffOnC())
  {
    // 5.B2-7: the tick after a plane off C ran, released on its vblank or not
    if (m_planeOffTickOwed)
    {
      m_planeOffTickOwed = false;
      GRAPHICSTICK::GTLogF(LOGINFO, "PLANEOFF next tick n={} seq={} missed={} woke_us={} released_us={}",
                m_planeOffSeq, tick.seq,
                m_tickSeq && tick.seq > m_tickSeq ? tick.seq - m_tickSeq - 1 : 0,
                (tick.wokeNs - tick.vblankNs) / 1000, (start + work - tick.vblankNs) / 1000);
    }
    if (m_tickSeq && tick.seq == m_tickSeq + 1)
      m_tickPeriodNs = tick.vblankNs - m_tickVblankNs;
    m_tickVblankNs = tick.vblankNs;
    m_tickSeq = tick.seq;
  }

  // a frame released after vblank S is on VD1 from S+2: amvideo's vsync ISR takes
  // it at S+1 and writes it through vsync RDMA, which applies at the next vsync
  // (S6 has no pre-vsync). Traced on the box (v4l2_qbuf, amvideo_vf_peek,
  // drm_vblank_event: 718 of 718 frames). An OSD commit issued after S shows
  // at S+1, the vblank its flip event reports.
  if (!result.configured)
    m_onScreen.clear();
  else if (result.newFrame && tick.vblankNs)
  {
    if (m_onScreen.size() >= 8)
      m_onScreen.erase(m_onScreen.begin());
    m_onScreen.push_back({tick.seq + 2, result.pts});
    m_frameUs = result.frametime;
    if (PRESENTATION::ShadowActive())
      ShadowFrameOnScreen(tick, result.pts, result.playing);
  }

  if (!result.configured)
    SetState(State::IDLE, tick.epoch);
  else if (result.displayLost)
    SetState(State::DISPLAY_LOST, tick.epoch);
  else
    SetState(result.playing ? State::PLAYING : State::PAUSED, tick.epoch);

  Account(tick, result, work, start + work);
  return false;
}

void CPresentationCoordinator::RunWakeTail()
{
  // off the path: nothing, no lock
  if (!m_geometryOnC)
    return;
  // RC22 coreelec-br K1 (3 of 6 runs): wake tails of 83-124 ms mid-playback ("wake max
  // 123640 us", the GTLAT notick= phase; tail=/wake= there are the tick's), each pushing the
  // next placements 4-6 vblanks late. While video ticks, the tick tail runs the same geometry
  // at every vblank (the frame-locked moment): a wake tail between two of them only runs it up
  // to a period earlier, so C does not run it then. Paused, held or detached (no tick tail for
  // two periods) it runs as before
  // test only (geomwake_always, read at Start: no file I/O on C): the old behaviour
  if (!s_geomWakeAlways && m_tickPeriodNs > 0 && m_lastTailNs &&
      MonotonicNs() - m_lastTailNs < 2 * m_tickPeriodNs)
  {
    m_report.wakeTicking++;
    return;
  }
  std::unique_lock lock(m_videoMutex);
  if (!m_video)
    return;
  const int64_t start = MonotonicNs();
  const char* busy = nullptr;
  SRMLockOwner owner;
  SGeomWakeSteps steps;
  const bool wrote = m_video->GeometryWakeOnC(busy, owner, steps);
  const int64_t ns = MonotonicNs() - start;
  // measurement (log only, at most 5 lines a second): where a wake tail over 3 ms went, step by
  // step (steady clock = CLOCK_MONOTONIC); 0: not reached
  if (ns > 3000000)
  {
    const int64_t now = MonotonicNs();
    if (now - m_wakeSlowWindowNs >= 1000000000)
    {
      m_wakeSlowWindowNs = now;
      m_wakeSlowLines = 0;
    }
    if (++m_wakeSlowLines <= 5)
    {
      int64_t last = steps.start;
      const auto step = [&last](int64_t t) -> int64_t
      {
        if (!t)
          return 0;
        const int64_t us = (t - last) / 1000;
        last = t;
        return us;
      };
      const int64_t held = step(steps.held);
      const int64_t statelock = step(steps.statelock);
      const int64_t enter = step(steps.enter);
      const int64_t apply = step(steps.apply);
      const int64_t snapshot = step(steps.snapshot);
      const int64_t state = step(steps.state);
      const int64_t rects = step(steps.rects);
      const int64_t applied = step(steps.applied);
      const int64_t end = step(steps.end);
      GRAPHICSTICK::GTLogF(LOGINFO,
                           "GEOMWAKE slow us={} held={} statelock={} enter={} decoder={} "
                           "snapshot={} state={} rects={} applied={} handover={} busy={}",
                           ns / 1000, held, statelock, enter, apply, snapshot, state, rects,
                           applied, end, busy ? busy : "-");
    }
  }
  if (busy)
  {
    // measurement (log only, at most 5 lines a second): the lock a frame's tick would wait for
    // too (TICKDIAG splits its statelock/presentlock waits)
    m_report.wakeBusy++;
    const int64_t now = MonotonicNs();
    if (now - m_wakeBusyWindowNs >= 1000000000)
    {
      m_wakeBusyWindowNs = now;
      m_wakeBusyLines = 0;
    }
    if (++m_wakeBusyLines <= 5)
      GRAPHICSTICK::GTLogF(LOGINFO, "GEOMWAKE busy lock={}: {}", busy,
                           CRenderManager::LockOwnerText(owner, now)); // steady = MONOTONIC
    // a kept decoder: the tail itself ran; otherwise it was skipped
    if (std::strcmp(busy, "presentlock (decoder kept)") != 0)
      return;
  }
  m_report.wakeTails++;
  m_report.wakeWrites += wrote;
  m_report.wakeMaxNs = std::max(m_report.wakeMaxNs, ns);
}

void CPresentationCoordinator::ShadowFrameOnScreen(const SPresentTick& tick,
                                                   double pts,
                                                   bool playing)
{
  int64_t tl[PRESENTATION::TL_COUNT];
  if (!PRESENTATION::TimelineBoard().Read(tl) || !tl[PRESENTATION::TL_PERIOD_DEN])
    return;
  const double periodNs = 1e9 * static_cast<double>(tl[PRESENTATION::TL_PERIOD_NUM]) /
                          static_cast<double>(tl[PRESENTATION::TL_PERIOD_DEN]);
  const int64_t onScreenNs = tick.vblankNs + static_cast<int64_t>(2 * periodNs);
  // a frame shown while the clock stands still (the first picture of a
  // scheduled start) is still on screen when the clock reaches its pts: its
  // own instant doesn't pair with the clock
  const int64_t values[PRESENTATION::VP_COUNT] = {playing ? static_cast<int64_t>(pts) : 0,
                                                  playing ? onScreenNs : 0};
  PRESENTATION::VideoPinsBoard().Write(values);
  if (m_shadowStartFrames < 2)
  {
    m_shadowStartFrames++;
    GRAPHICSTICK::GTLogF(LOGINFO, "PINS video: frame {} after the start, pts {:.3f}, on screen at {} ns",
              m_shadowStartFrames, pts / 1e6, onScreenNs);
  }
}

void CPresentationCoordinator::SetState(State state, unsigned int epoch)
{
  if (state == m_state)
    return;
  if (state != State::PLAYING)
    m_shadowStartFrames = 0;
  GRAPHICSTICK::GTLogF(LOGINFO, "CPresentationCoordinator - {} -> {} (display epoch {})",
            StateName(static_cast<int>(m_state)), StateName(static_cast<int>(state)), epoch);
  m_state = state;
  s_publishedState = static_cast<int>(state);
}

const char* CPresentationCoordinator::PublishedStateName()
{
  return StateName(s_publishedState.load());
}

void CPresentationCoordinator::Account(const SPresentTick& tick,
                                       const SPresentResult& result,
                                       int64_t workNs,
                                       int64_t releasedNs)
{
  Report& r = m_report;
  r.ticks++;
  if (tick.vblankNs)
  {
    // 5.V3-1: the vblanks UI commits are timed against
    if (m_diagVblankSeq && tick.seq == m_diagVblankSeq + 1)
      m_diagPeriodNs = tick.vblankNs - m_diagVblankNs;
    m_diagVblankNs = tick.vblankNs;
    m_diagVblankSeq = tick.seq;
    if (m_lastSeq && tick.seq > m_lastSeq + 1)
      r.missed += static_cast<int>(tick.seq - m_lastSeq - 1);
    m_lastSeq = tick.seq;
    const double wake = static_cast<double>(tick.wokeNs - tick.vblankNs) / 1000.0;
    // GT-D15 measurement (route only): where a late tick's time went. poll_since_us > 0: C sat in
    // poll from before the vblank (not run: scheduling or the event late); body_us: the loop
    // body before that poll
    if (wake > 3000.0 && GRAPHICSTICK::Requested())
      GRAPHICSTICK::GTLogF(LOGINFO,
                "GTLAT C tick seq={} wake_us={:.0f} poll_since_us={} poll_return_us={} body_us={}",
                tick.seq, wake, (tick.vblankNs - m_pollEnterNs) / 1000,
                (m_pollReturnNs - tick.vblankNs) / 1000, m_lastBodyNs / 1000);
    r.woke++;
    r.wakeSum += wake;
    r.wakeMax = std::max(r.wakeMax, wake);
    if (result.configured)
    {
      const int64_t bin = std::max<int64_t>(0, releasedNs - tick.vblankNs) / 10000;
      if (bin < RELEASE_BINS)
        r.releaseHist[bin]++;
      else
        r.releaseOver++;
    }
  }
  else
  {
    m_lastSeq = 0;
    m_diagVblankSeq = 0;
  }

  const double work = static_cast<double>(workNs) / 1000.0;
  r.workSum += work;
  r.workMax = std::max(r.workMax, work);
  r.skipped += result.skipped;
  // each file starts from no offset
  if (!result.configured)
    m_shadowFrames = m_shadowCandidateN = 0;
  if (result.shadow && !result.shadowSynced)
    r.shadowUnsynced++;
  else if (result.shadow)
  {
    const double diff = result.shadowDiff / 1000.0;
    if (!r.shadowN)
      r.shadowMin = r.shadowMax = diff;
    r.shadowN++;
    r.shadowSum += diff;
    r.shadowSumSq += diff * diff;
    r.shadowMin = std::min(r.shadowMin, diff);
    r.shadowMax = std::max(r.shadowMax, diff);
    r.shadowAdjustSum += result.shadowAdjust / 1000.0;
    const double frame = result.frametime / 1000.0;
    if (frame > 0.0)
    {
      if (std::abs(diff) > frame / 2)
        r.shadowFolds++;
      // audio's reference moved by whole frames against the screen, and stayed
      // there for a second of samples (a start or seek passes through a few)
      const int frames = static_cast<int>(std::lround(diff / frame));
      if (frames == m_shadowFrames)
        m_shadowCandidateN = 0;
      else
      {
        if (frames != m_shadowCandidate)
        {
          m_shadowCandidate = frames;
          m_shadowCandidateN = 0;
        }
        if (++m_shadowCandidateN == SHADOW_HOLD_SAMPLES)
        {
          GRAPHICSTICK::GTLogF(LOGINFO,
                    "real_player shadow: audio reference {:+d} frames from the screen (was "
                    "{:+d}): diff={:.2f}ms vsyncAdjust={:.2f}ms",
                    frames, m_shadowFrames, diff, result.shadowAdjust / 1000.0);
          m_shadowFrames = frames;
          m_shadowCandidateN = 0;
        }
      }
    }
  }

  if (result.newFrame)
  {
    r.frames++;
    GTLOG_FC(LOGDEBUG, LOGAVTIMING,
                "CPresentationCoordinator - released pts:{:.3f} seq:{} wake:{:.0f}us shadow:{}",
                result.pts / 1000000.0, tick.seq,
                tick.vblankNs ? static_cast<double>(tick.wokeNs - tick.vblankNs) / 1000.0 : -1.0,
                result.shadow ? fmt::format("{:.3f}ms", result.shadowDiff / 1000.0) : "-");
  }
  else if (result.playing)
    r.repeats++;

  if (r.ticks >= REPORT_TICKS)
    LogReport();
}

void CPresentationCoordinator::LogReport()
{
  std::unique_lock lock(m_uiMutex);
  const Report& r = m_report;
  if (!r.ticks && !r.uiSubmits)
    return;
  const int drops = ReadKernelDrops();
  GRAPHICSTICK::GTLogF(r.ticks ? LOGINFO : LOGDEBUG,
            "real_player coordinator: ticks={} source={} missed={} synthetic={} stale={} held={} "
            "frames={} repeats={} skipped={} wake mean={:.0f}us max={:.0f}us "
            "work mean={:.0f}us max={:.0f}us kernel drops={} | gui submits={} replaced={} "
            "commits={} failed={} flips={} lost={} commit max={:.0f}us "
            "[<1ms {} <5ms {} <20ms {} >=20ms {}] flip mean={:.0f}us max={:.0f}us "
            "| graphics submits={} replaced={} commits={} placement [<-1 {} -1 {} 0 {} +1 {} >+1 {}]",
            r.ticks, r.timeline ? "timeline" : "own", r.missed, r.synthetic, r.stale, r.held, r.frames, r.repeats, r.skipped,
            r.woke ? r.wakeSum / r.woke : 0.0, r.wakeMax, r.ticks ? r.workSum / r.ticks : 0.0,
            r.workMax, drops >= 0 && m_kernelDrops >= 0 ? drops - m_kernelDrops : -1,
            r.uiSubmits, r.uiReplaced, r.uiCommits, r.uiFailed, r.uiFlips, r.uiLostFlips,
            r.commitMax, r.commitHist[0], r.commitHist[1], r.commitHist[2], r.commitHist[3],
            r.uiFlips ? r.flipSum / r.uiFlips : 0.0, r.flipMax, r.gfxSubmits, r.gfxReplaced,
            r.gfxCommits, r.gfxPlacement[0], r.gfxPlacement[1], r.gfxPlacement[2],
            r.gfxPlacement[3], r.gfxPlacement[4]);
  if (r.gfxSuspect)
    GRAPHICSTICK::GTLogF(LOGINFO, "real_player coordinator: graphics suspect refusals={}", r.gfxSuspect);
  if (r.gfxTimed || r.gfxTimedReplaced || r.gfxMissing)
    GRAPHICSTICK::GTLogF(LOGINFO,
              "GTICK report timed={} gfxLate={} gfxMissing={} gfxReplaced={} latePick={} "
              "latePickBack={} stalePickReplaced={} uiHeldForSlot={} uiHeldMax_us={} "
              "uiHoldEndToCommitMax_us={} uiAloneGfxWaits={}",
              r.gfxTimed, r.gfxLate, r.gfxMissing, r.gfxTimedReplaced, r.gfxLatePick,
              r.gfxLatePickBack, r.stalePickReplaced, r.uiHeldForSlot, r.uiHeldMaxUs,
              r.uiHoldEndToCommitMaxUs, r.uiAloneGfxWaits);
  // GT-D15: C's loop body (bounded: no I/O on it) and the log writer that took C's lines
  if (GRAPHICSTICK::Requested())
  {
    GRAPHICSTICK::GTLogF(LOGINFO,
                         "GTICK C body [<1ms {} <5ms {} <10ms {} <20ms {} >=20ms {}] max_us={} "
                         "held={} held_max_us={}",
                         r.bodyHist[0], r.bodyHist[1], r.bodyHist[2], r.bodyHist[3],
                         r.bodyHist[4], r.bodyMaxUs, r.bodyHeld, r.bodyHeldMaxUs);
  }
  // the video path's log writer, in each report (its drops are lost lines)
  GRAPHICSTICK::GTLogF(LOGINFO, "{}", GRAPHICSTICK::GTLogStatsLine(true));
  // review rB m7: whenever GT-5's route ran in this report's window, both halves per window
  // (G's counters are taken and reset with C's)
  if (GRAPHICSTICK::Requested() && !GRAPHICSTICK::UntimedOff())
  {
    bool any = false;
    const std::string counts = GRAPHICSTICK::UntimedCountsLine(true, any);
    if (any || GRAPHICSTICK::RouteOn() || r.untimedReplaced || r.untimedRegress ||
        r.gfxEpochDrop || r.firstPages)
      GRAPHICSTICK::GTLogF(LOGINFO,
                "GUNTIMED report untimedReplaced={} untimedRegress={} gfxEpochDrop={} "
                "firstPageFlips={} pageLate={} {}",
                r.untimedReplaced, r.untimedRegress, r.gfxEpochDrop, r.firstPages, r.pageLate,
                counts);
  }
  if (r.uiTimed)
    GRAPHICSTICK::GTLogF(LOGINFO, "GEOMDIAG ui commits late={} of {} timed over {} ticks", r.uiLate,
              r.uiTimed, r.ticks);
  if (std::any_of(std::begin(r.releaseHist), std::end(r.releaseHist), [](int n) { return n; }) ||
      r.releaseOver)
  {
    std::string bins;
    for (int i = 0; i < RELEASE_BINS; i++)
      if (r.releaseHist[i])
        bins += fmt::format(" {}:{}", i, r.releaseHist[i]);
    GRAPHICSTICK::GTLogF(LOGINFO, "GEOMDIAG release after vblank, 10 us bin:ticks{} over={}", bins,
              r.releaseOver);
  }
  if (r.tails || r.wakeTails || r.wakeBusy || r.wakeTicking)
    GRAPHICSTICK::GTLogF(LOGINFO,
              "GEOMDIAG C tails: tick {} (wrote {}, max {} us, over a vblank {}), wake {} "
              "(wrote {}, max {} us, busy {}, not run while ticking {})",
              r.tails, r.tailWrites, r.tailMaxNs / 1000, r.tailsOverVblank, r.wakeTails,
              r.wakeWrites, r.wakeMaxNs / 1000, r.wakeBusy, r.wakeTicking);
  if (r.shadowN)
  {
    const double mean = r.shadowSum / r.shadowN;
    GRAPHICSTICK::GTLogF(LOGINFO,
              "real_player shadow: n={} audio-minus-screen mean={:.2f}ms sd={:.2f}ms "
              "min={:.2f}ms max={:.2f}ms beyond half a frame={} vsyncAdjust mean={:.2f}ms "
              "unsynced={}",
              r.shadowN, mean,
              std::sqrt(std::max(0.0, r.shadowSumSq / r.shadowN - mean * mean)), r.shadowMin,
              r.shadowMax, r.shadowFolds, r.shadowAdjustSum / r.shadowN, r.shadowUnsynced);
  }
  else if (r.shadowUnsynced)
    GRAPHICSTICK::GTLogF(LOGINFO, "real_player shadow: clock sync off, {} samples not compared",
              r.shadowUnsynced);
  m_kernelDrops = drops;
  m_report = Report();
}
