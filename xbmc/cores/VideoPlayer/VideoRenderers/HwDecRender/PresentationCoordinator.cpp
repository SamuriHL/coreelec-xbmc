/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PresentationCoordinator.h"

#include "ServiceBroker.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "platform/linux/SysfsPath.h"
#include "utils/AMLUtils.h"
#include "utils/PresentationTimeline.h"
#include "utils/log.h"
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
// 5.B2-7: a plane off C has not started by then runs inline (today's hold bound)
constexpr auto PLANE_OFF_BOUND = std::chrono::seconds(2);

std::atomic<CPresentationCoordinator*> s_instance{nullptr};

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
}

int ReadKernelDrops()
{
  const auto value =
      CSysfsPath("/sys/module/aml_media/parameters/video_drop_vf_cnt").Get<std::string>();
  return value ? std::atoi(value->c_str()) : -1;
}

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
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no event fds (vblank:{} wake:{})",
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
  aml_presenter_set_wake_fd(m_wakeFd);
  m_commitStop = false;
  m_commitThread = std::thread(&CPresentationCoordinator::CommitWorker, this);
  Create();
  s_instance = this;
  CLog::Log(LOGINFO, "CPresentationCoordinator - started, commits the GUI plane (crtc {})", m_crtc.load());
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
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
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
  CLog::Log(LOGINFO, "CPresentationCoordinator - stopped");
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
  // read (racy, as today: this runs on VideoPlayer::OpenFile's caller via RM PreInit, not with
  // the re-init)
  if (auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
    m_crtc = winSystem->GetDRMCrtcId();

  const int64_t start = MonotonicNs();
  const bool timeline = m_tickFd >= 0 && m_crtc == m_tickCrtc;
  if (m_tickFd >= 0 && !timeline)
    CLog::Log(LOGWARNING, "CPresentationCoordinator - crtc {} is not the timekeeper's ({}), "
                          "queueing its own vblank events", m_crtc.load(), m_tickCrtc.load());
  m_attachNs = start;
  m_useTimeline = timeline;
  m_videoAttached = true;
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));

  // presenting needs a vblank source that delivers: a vblank after this
  // point, not an event left queued by an earlier attach
  std::unique_lock lock(m_vblankMutex);
  const bool delivering = m_vblankCond.wait_for(lock, std::chrono::milliseconds(150),
                                                [&] { return m_lastVblankNs > start; });
  if (!delivering)
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no vblank event from crtc {}, "
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
        CLog::Log(LOGINFO, "UIEPOCH refused submit {} of epoch {} below {}", refused, epoch,
                  current);
      return 0;
    }
    UiBuffer& ready = m_planes[plane].ready;
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
    ready.seq = seq = ++m_submitSeq;
    if (plane == PLANE_UI)
      m_report.uiSubmits++;
    else
      m_report.gfxSubmits++;
  }
  if (replacedCbuf)
    CLog::Log(LOGINFO, "CBUF replaced by main before its commit");
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
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
      CLog::Log(LOGERROR, "UIEPOCH ASSERT a returned buffer carries C's fb {}", buffer.fb);
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
    CLog::Log(LOGINFO, "CBUF dropped an unused request at epoch {}", epoch);
  // Still parked, with nothing in C's books on it: its RmFB takes the GUI plane off before
  // commit 1, as the EGL destroy's RmFB of main's buffer does today. The next set makes a
  // new buffer.
  if (shown.fb)
  {
    CLog::Log(LOGINFO, "CBUF off at the detach: fb={} {}x{} freed", shown.fb, shown.width,
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
    CLog::Log(LOGERROR, "UIEPOCH ASSERT the GUI plane reclaim runs with C not parked");
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
    CLog::Log(LOGERROR, "UIEPOCH ASSERT epoch {} asked for at epoch {}, {} taken", requested,
              previous, newEpoch);
  if (stale)
    CLog::Log(LOGINFO, "CBUF dropped an unused request at epoch {}", newEpoch);
  if (done)
    CLog::Log(LOGINFO, "PLANE ui off done epoch={} fb={} cbuf={} took_us={}{}", newEpoch, shownFb,
              cbufShown ? 1 : 0, tookUs, acked ? "" : " (unparked)");
  else if (!off)
    CLog::Log(LOGWARNING, "PLANE ui off not built (no plane or CRTC) epoch={} fb={} n={}",
              newEpoch, shownFb, ++s_unbuilt);
  else
    CLog::Log(LOGERROR, "PLANE ui off refused (SB3 state) epoch={} fb={} {} n={}", newEpoch,
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
  CLog::Log(LOGERROR, "UIEPOCH ForgetUiEpoch below {}: {} buffers", epoch, forgotten);
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
    CLog::Log(LOGERROR, "CBUF no buffer for a {}x{} GUI", width, height);
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
  CLog::Log(LOGINFO, "CBUF armed epoch={} fb={} {}x{} src={}x{} made={} request={}{}", epoch, fb,
            width, height, srcWidth, srcHeight, reuse ? 0 : 1, req ? 1 : 0,
            stale ? " (stale epoch: dropped)" : "");
  if (req && (srcWidth != width || srcHeight != height))
    CLog::Log(LOGERROR, "CBUF request src {}x{} is not the buffer's {}x{}", srcWidth, srcHeight,
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
    CLog::Log(LOGERROR, "CBUF dumb buffer {}x{} failed: {}", width, height, strerror(errno));
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
    CLog::Log(LOGERROR, "CBUF fb {}x{} failed (cleared {}): {}", width, height, cleared,
              strerror(errno));
    FreeDumb(dumb);
    return false;
  }
  CLog::Log(LOGINFO, "CBUF made fb={} {}x{} pitch={}", dumb.fb, width, height, create.pitch);
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
        CLog::Log(LOGWARNING, "CPresentationCoordinator - graphics plane off failed: {}",
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
    CLog::Log(LOGERROR, "graphics plane off: C did not answer in 2 s, inline (who={})", who);
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
      CLog::Log(LOGERROR,
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
    CLog::Log(LOGWARNING, "CGraphicsPlaneAML - plane left to go with its buffers");

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
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
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
    CLog::Log(LOGINFO,
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
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no flip event 500 ms after a plane commit");
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
    if (epoch != m_seenEpoch)
    {
      m_seenEpoch = epoch;
      m_seenEpochNs = MonotonicNs();
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
    const int timeout = held ? 20 : (video || inFlight ? 50 : -1);
    if (poll(fds, 3 + PLANE_COUNT, timeout) < 0 && errno != EINTR)
      CLog::Log(LOGERROR, "CPresentationCoordinator - poll failed: {}", strerror(errno));
    if (m_bStop)
      break;

    if (fds[0].revents & POLLIN)
    {
      uint64_t count;
      if (read(m_wakeFd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake read failed: {}", strerror(errno));
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
    if (planeOffOnC)
      RunPlaneOffs(tickHeld);
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
    OnFlip(t_flipTag, t_flipSequence);

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
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - tick read failed: {}", strerror(errno));

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

void CPresentationCoordinator::CommitUi()
{
  {
    std::unique_lock lock(m_uiMutex);
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
    Committing commit;
    for (int plane = 0; plane < PLANE_COUNT; plane++)
    {
      UiBuffer& ready = m_planes[plane].ready;
      if (!ready.req || ready.fence >= 0)
        continue;
      if (plane == PLANE_GRAPHICS)
      {
        // a plane the driver refused would take every GUI frame down with it
        if (m_graphicsRejected || (m_gfxSuspect && !m_gfxRetryArmed))
        {
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
      ready = UiBuffer();
    }
    if (!commit.req)
      return;
    if (commit.cOwned)
      CLog::Log(LOGINFO, "CBUF on epoch={} fb={} graphics={}", m_cbufEpoch, m_cbuf.fb,
                commit.has[PLANE_GRAPHICS] ? 1 : 0);
    // in flight from here: the flip event can arrive before the ioctl returns
    m_committing = commit;
    m_inFlight = true;
    m_inCommitTag = ++m_commitTagSeq;
    m_inFlightGfxPts = commit.has[PLANE_GRAPHICS] ? commit.pts[PLANE_GRAPHICS] : DVD_NOPTS_VALUE;
    m_flipLostLogged = false;
    m_commitNs = MonotonicNs();
  }
  m_uiCond.notify_all();
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
    CLog::Log(LOGINFO, "CBUF not needed at epoch {} ({})", m_cbufEpoch,
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
    m_report.commitMax = std::max(m_report.commitMax, took);
    // TEMP TICKDIAG (timebase audit Q6)
    if (took >= 20000.0)
      CLog::Log(LOGINFO, "TICKDIAG commit {:.0f} us (ui {} graphics {})", took,
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
        CLog::Log(LOGINFO, "CPresentationCoordinator - the graphics plane was accepted again "
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
        CLog::Log(LOGWARNING, "CPresentationCoordinator - plane commit failed: {}",
                  strerror(err));
      // not retried: main's first buffer of the epoch brings the GUI plane back (R4-M4);
      // the graphics plane is not blamed for it
      if (commit.cOwned)
        CLog::Log(LOGERROR, "CBUF refused: {}", strerror(err));
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
          CLog::Log(LOGERROR,
                    "CPresentationCoordinator - the graphics plane was refused ({}), committing "
                    "the GUI plane alone from now on (confirmed: {} retries after GUI-only "
                    "commits)",
                    strerror(err), m_gfxRetriesRefused);
        }
        else if (m_gfxSuspectLogs++ < 3)
          CLog::Log(LOGWARNING,
                    "CPresentationCoordinator - the graphics plane was refused ({}): suspect, "
                    "held until a GUI-only commit succeeds (retries refused {})",
                    strerror(err), m_gfxRetriesRefused);
      }
      else if (commit.has[PLANE_GRAPHICS] && !m_graphicsRejected)
      {
        m_graphicsRejected = true;
        CLog::Log(LOGERROR, "CPresentationCoordinator - the graphics plane was refused ({}), "
                            "committing the GUI plane alone from now on",
                  strerror(err));
      }
    }
    lock.unlock();
    const uint64_t one = 1;
    if (write(m_wakeFd, &one, sizeof(one)) < 0)
      CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
    lock.lock();
  }
}

void CPresentationCoordinator::OnFlip(uint64_t tag, unsigned int sequence)
{
  std::unique_lock lock(m_uiMutex);
  if (!m_inFlight || tag != m_inCommitTag)
    return;
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
  if (cbufUp)
    CLog::Log(LOGINFO, "CBUF up epoch={} flip_seq={}", epoch, sequence);
  if (replacedMs >= 0)
    CLog::Log(LOGINFO, "CBUF replaced by main after {} ms", replacedMs);
  for (DumbBuffer& dumb : freed)
  {
    CLog::Log(LOGINFO, "CBUF retired fb={} {}x{} freed", dumb.fb, dumb.width, dumb.height);
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
  CLog::Log(LOGDEBUG,
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
    CLog::Log(LOGINFO, "TICKDIAG video mutex wait {} us",
              (MonotonicNs() - diagWait) / 1000);
  if (!m_video)
    return false;

  SPresentResult result;
  const int64_t start = MonotonicNs();
  m_video->PresentTick(tick, result);
  const int64_t work = MonotonicNs() - start;
  m_lastTickNs = tick.wokeNs;

  if (tick.vblankNs && OUTPUT::PlaneOffOnC())
  {
    // 5.B2-7: the tick after a plane off C ran, released on its vblank or not
    if (m_planeOffTickOwed)
    {
      m_planeOffTickOwed = false;
      CLog::Log(LOGINFO, "PLANEOFF next tick n={} seq={} missed={} woke_us={} released_us={}",
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

  Account(tick, result, work);
  return false;
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
    CLog::Log(LOGINFO, "PINS video: frame {} after the start, pts {:.3f}, on screen at {} ns",
              m_shadowStartFrames, pts / 1e6, onScreenNs);
  }
}

void CPresentationCoordinator::SetState(State state, unsigned int epoch)
{
  if (state == m_state)
    return;
  if (state != State::PLAYING)
    m_shadowStartFrames = 0;
  CLog::Log(LOGINFO, "CPresentationCoordinator - {} -> {} (display epoch {})",
            StateName(static_cast<int>(m_state)), StateName(static_cast<int>(state)), epoch);
  m_state = state;
}

void CPresentationCoordinator::Account(const SPresentTick& tick,
                                       const SPresentResult& result,
                                       int64_t workNs)
{
  Report& r = m_report;
  r.ticks++;
  if (tick.vblankNs)
  {
    if (m_lastSeq && tick.seq > m_lastSeq + 1)
      r.missed += static_cast<int>(tick.seq - m_lastSeq - 1);
    m_lastSeq = tick.seq;
    const double wake = static_cast<double>(tick.wokeNs - tick.vblankNs) / 1000.0;
    r.woke++;
    r.wakeSum += wake;
    r.wakeMax = std::max(r.wakeMax, wake);
  }
  else
    m_lastSeq = 0;

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
          CLog::Log(LOGINFO,
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
    CLog::LogFC(LOGDEBUG, LOGAVTIMING,
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
  CLog::Log(r.ticks ? LOGINFO : LOGDEBUG,
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
    CLog::Log(LOGINFO, "real_player coordinator: graphics suspect refusals={}", r.gfxSuspect);
  if (r.shadowN)
  {
    const double mean = r.shadowSum / r.shadowN;
    CLog::Log(LOGINFO,
              "real_player shadow: n={} audio-minus-screen mean={:.2f}ms sd={:.2f}ms "
              "min={:.2f}ms max={:.2f}ms beyond half a frame={} vsyncAdjust mean={:.2f}ms "
              "unsynced={}",
              r.shadowN, mean,
              std::sqrt(std::max(0.0, r.shadowSumSq / r.shadowN - mean * mean)), r.shadowMin,
              r.shadowMax, r.shadowFolds, r.shadowAdjustSum / r.shadowN, r.shadowUnsynced);
  }
  else if (r.shadowUnsynced)
    CLog::Log(LOGINFO, "real_player shadow: clock sync off, {} samples not compared",
              r.shadowUnsynced);
  m_kernelDrops = drops;
  m_report = Report();
}
