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
#include "utils/log.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
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

CPresentationCoordinator::CPresentationCoordinator(int masterFd)
  : CThread("PresentCoord"), m_masterFd(masterFd)
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

  {
    std::unique_lock lock(m_uiMutex);
    for (int plane = 0; plane < PLANE_COUNT; plane++)
      Drop(plane, m_planes[plane].ready);
  }
  close(m_vblankFd);
  close(m_wakeFd);
  m_vblankFd = m_wakeFd = -1;
  LogReport();
  CLog::Log(LOGINFO, "CPresentationCoordinator - stopped");
}

bool CPresentationCoordinator::AttachVideo(CRenderManager* renderManager)
{
  {
    std::unique_lock lock(m_videoMutex);
    m_video = renderManager;
  }
  // the CRTC is resolved on the render thread, which also runs the hotplug
  if (auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
    m_crtc = winSystem->GetDRMCrtcId();

  const int64_t start = MonotonicNs();
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

uint64_t CPresentationCoordinator::Submit(
    int plane, gbm_bo* bo, uint32_t fb, drmModeAtomicReqPtr req, int fence, double pts)
{
  uint64_t seq;
  {
    std::unique_lock lock(m_uiMutex);
    UiBuffer& ready = m_planes[plane].ready;
    if (ready.req)
    {
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
    ready.seq = seq = ++m_submitSeq;
    if (plane == PLANE_UI)
      m_report.uiSubmits++;
    else
      m_report.gfxSubmits++;
  }
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
  std::vector<gbm_bo*>& from = m_planes[plane].returned;
  returned.insert(returned.end(), from.begin(), from.end());
  from.clear();
}

void CPresentationCoordinator::DetachSurface(int plane)
{
  // parked, and no flip in flight
  CAmlPresenterHold hold;
  std::unique_lock lock(m_uiMutex);
  AbandonLostCommit(plane);
  ReclaimPlane(plane);
}

bool CPresentationCoordinator::DisableGraphicsPlane(
    const std::function<drmModeAtomicReqPtr(uint32_t)>& build)
{
  // parked, and no flip in flight: this thread owns the commit
  CAmlPresenterHold hold;
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
        state.returned.push_back(state.onScreen);
      state.onScreen = state.committedBo;
      state.onScreenFb = state.committedFb;
      state.committedBo = nullptr;
    }
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
    state.returned.push_back(state.committedBo);
  if (state.onScreen)
    state.returned.push_back(state.onScreen);
  state.inCommit = false;
  state.committedBo = nullptr;
  state.committedFb = 0;
  state.onScreen = nullptr;
  state.onScreenFb = 0;
}

void CPresentationCoordinator::Drop(int plane, UiBuffer& buffer)
{
  if (buffer.bo)
    m_planes[plane].returned.push_back(buffer.bo);
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
  SetPriority(ThreadPriority::ABOVE_NORMAL);
  aml_presenter_set_running(true);
  m_lastTickNs = MonotonicNs();

  while (!m_bStop)
  {
    unsigned int epoch = 0;
    // a hold is acknowledged only once the flip in flight has landed
    const bool held = aml_presenter_check_hold(epoch, !UiInFlight());
    if (held)
    {
      SetState(State::HELD, epoch);
      m_report.held++;
      // no timer ticks to catch up on once it is released, and the vblanks
      // of the transaction are not missed ones
      m_lastTickNs = MonotonicNs();
      m_lastSeq = 0;
    }
    else
      CommitUi();

    const bool video = m_videoAttached;
    if (video)
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
      fds[2].fd = m_vblankFd;
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
    if (fds[2].revents & POLLIN)
      HandleEvents(m_vblankFd, tick, gotTick, epoch);

    const int64_t now = MonotonicNs();

    if (!video)
    {
      m_lastSeq = 0;
      if (!held)
        SetState(State::IDLE, epoch);
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
    RunVideoTick(tick);
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
  {
    std::unique_lock lock(m_vblankMutex);
    m_lastVblankNs = tick.vblankNs;
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
        if (m_graphicsRejected)
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
      m_planes[plane].committedFb = ready.bo ? ready.fb : m_planes[plane].onScreenFb;
      ready = UiBuffer();
    }
    if (!commit.req)
      return;
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

void CPresentationCoordinator::CommitWorker()
{
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
          state.returned.push_back(state.committedBo);
        state.inCommit = false;
        state.committedBo = nullptr;
        state.committedFb = 0;
      }
      if (m_report.uiFailed++ < 3)
        CLog::Log(LOGWARNING, "CPresentationCoordinator - plane commit failed: {}",
                  strerror(err));
      if (commit.has[PLANE_GRAPHICS] && !m_graphicsRejected)
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
  for (PlaneState& state : m_planes)
  {
    if (!state.inCommit)
      continue;
    // a property-only commit leaves the buffer on screen where it is
    if (state.committedBo)
    {
      if (state.onScreen)
        state.returned.push_back(state.onScreen);
      state.onScreen = state.committedBo;
    }
    state.onScreenFb = state.committedFb;
    state.inCommit = false;
    state.committedBo = nullptr;
    state.committedFb = 0;
  }
  m_inFlight = false;
  m_inCommitTag = 0;
  const double flip = static_cast<double>(MonotonicNs() - m_commitNs) / 1000.0;
  m_report.uiFlips++;
  m_report.flipSum += flip;
  m_report.flipMax = std::max(m_report.flipMax, flip);
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

void CPresentationCoordinator::RunVideoTick(SPresentTick& tick)
{
  // a hold that landed during the wait is acknowledged before any release
  if (aml_presenter_check_hold(tick.epoch, !UiInFlight()))
  {
    m_lastTickNs = tick.wokeNs;
    return;
  }

  std::unique_lock lock(m_videoMutex);
  if (!m_video)
    return;

  SPresentResult result;
  const int64_t start = MonotonicNs();
  m_video->PresentTick(tick, result);
  const int64_t work = MonotonicNs() - start;
  m_lastTickNs = tick.wokeNs;

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
  }

  if (!result.configured)
    SetState(State::IDLE, tick.epoch);
  else if (result.displayLost)
    SetState(State::DISPLAY_LOST, tick.epoch);
  else
    SetState(result.playing ? State::PLAYING : State::PAUSED, tick.epoch);

  Account(tick, result, work);
}

void CPresentationCoordinator::SetState(State state, unsigned int epoch)
{
  if (state == m_state)
    return;
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
            "real_player coordinator: ticks={} missed={} synthetic={} stale={} held={} "
            "frames={} repeats={} skipped={} wake mean={:.0f}us max={:.0f}us "
            "work mean={:.0f}us max={:.0f}us kernel drops={} | gui submits={} replaced={} "
            "commits={} failed={} flips={} lost={} commit max={:.0f}us "
            "[<1ms {} <5ms {} <20ms {} >=20ms {}] flip mean={:.0f}us max={:.0f}us "
            "| graphics submits={} replaced={} commits={} placement [<-1 {} -1 {} 0 {} +1 {} >+1 {}]",
            r.ticks, r.missed, r.synthetic, r.stale, r.held, r.frames, r.repeats, r.skipped,
            r.woke ? r.wakeSum / r.woke : 0.0, r.wakeMax, r.ticks ? r.workSum / r.ticks : 0.0,
            r.workMax, drops >= 0 && m_kernelDrops >= 0 ? drops - m_kernelDrops : -1,
            r.uiSubmits, r.uiReplaced, r.uiCommits, r.uiFailed, r.uiFlips, r.uiLostFlips,
            r.commitMax, r.commitHist[0], r.commitHist[1], r.commitHist[2], r.commitHist[3],
            r.uiFlips ? r.flipSum / r.uiFlips : 0.0, r.flipMax, r.gfxSubmits, r.gfxReplaced,
            r.gfxCommits, r.gfxPlacement[0], r.gfxPlacement[1], r.gfxPlacement[2],
            r.gfxPlacement[3], r.gfxPlacement[4]);
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
