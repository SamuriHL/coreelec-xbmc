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
    Drop(m_ready);
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

uint64_t CPresentationCoordinator::SubmitUi(gbm_bo* bo, drmModeAtomicReqPtr req, int fence)
{
  uint64_t seq;
  {
    std::unique_lock lock(m_uiMutex);
    if (m_ready.bo)
    {
      Drop(m_ready);
      m_report.uiReplaced++;
    }
    m_ready.bo = bo;
    m_ready.req = req;
    m_ready.fence = fence;
    m_ready.seq = seq = ++m_submitSeq;
    m_report.uiSubmits++;
  }
  const uint64_t one = 1;
  if (write(m_wakeFd, &one, sizeof(one)) < 0)
    CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake failed: {}", strerror(errno));
  return seq;
}

bool CPresentationCoordinator::WaitTaken(uint64_t seq, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(m_uiMutex);
  return m_uiCond.wait_for(lock, timeout, [&] { return m_ready.seq != seq; });
}

void CPresentationCoordinator::TakeReturned(std::vector<gbm_bo*>& returned)
{
  std::unique_lock lock(m_uiMutex);
  returned.insert(returned.end(), m_returned.begin(), m_returned.end());
  m_returned.clear();
}

void CPresentationCoordinator::DetachUiSurface()
{
  // parked, and no flip in flight
  CAmlPresenterHold hold;
  std::unique_lock lock(m_uiMutex);
  // A commit that outlived a timed-out hold belongs to this surface: its
  // buffer and its flip are dropped, never handed to the next surface.
  m_uiGeneration++;
  Drop(m_ready);
  if (m_inCommit)
    m_returned.push_back(m_inCommit);
  if (m_onScreen)
    m_returned.push_back(m_onScreen);
  m_inCommit = nullptr;
  m_inCommitTag = 0;
  m_onScreen = nullptr;
}

void CPresentationCoordinator::Drop(UiBuffer& buffer)
{
  if (buffer.bo)
    m_returned.push_back(buffer.bo);
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
  if (!m_inCommit)
    return false;
  // A flip that never came (CRTC off mid-flight) no longer blocks a hold. Its
  // buffers stay where they are: only its own flip event retires them.
  if (MonotonicNs() - m_commitNs <= FLIP_LOST_NS)
    return true;
  if (!m_flipLostLogged)
  {
    m_flipLostLogged = true;
    m_report.uiLostFlips++;
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no flip event 500 ms after a GUI commit");
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
      // no timer ticks to catch up on once it is released
      m_lastTickNs = MonotonicNs();
    }
    else
      CommitUi();

    const bool video = m_videoAttached;
    if (video)
      QueueVblank(epoch);

    int fence;
    uint64_t fenceSeq;
    bool inFlight;
    {
      std::unique_lock lock(m_uiMutex);
      fence = m_ready.fence;
      fenceSeq = m_ready.seq;
      inFlight = m_inCommit != nullptr;
    }

    struct pollfd fds[4] = {{m_wakeFd, POLLIN, 0}, {m_masterFd, POLLIN, 0}, {-1, POLLIN, 0},
                            {-1, POLLIN, 0}};
    if (video)
      fds[2].fd = m_vblankFd;
    fds[3].fd = fence;
    const int timeout = held ? 20 : (video || inFlight ? 50 : -1);
    if (poll(fds, 4, timeout) < 0 && errno != EINTR)
      CLog::Log(LOGERROR, "CPresentationCoordinator - poll failed: {}", strerror(errno));
    if (m_bStop)
      break;

    if (fds[0].revents & POLLIN)
    {
      uint64_t count;
      if (read(m_wakeFd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        CLog::Log(LOGDEBUG, "CPresentationCoordinator - wake read failed: {}", strerror(errno));
    }

    if (fds[3].revents & (POLLIN | POLLERR))
    {
      std::unique_lock lock(m_uiMutex);
      // the same submit: its fd may have been closed and the number reused
      if (m_ready.seq == fenceSeq && m_ready.fence == fence)
      {
        close(m_ready.fence);
        m_ready.fence = -1;
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
    OnFlip(t_flipTag);

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
  UiBuffer buffer;
  uint64_t generation;
  uint64_t tag;
  {
    std::unique_lock lock(m_uiMutex);
    // one commit in flight, and only a buffer the GPU has finished
    if (!m_ready.bo || m_ready.fence >= 0 || m_inCommit)
      return;
    buffer = m_ready;
    m_ready = UiBuffer();
    generation = m_uiGeneration;
    tag = ++m_commitTagSeq;
  }
  m_uiCond.notify_all();

  const int64_t start = MonotonicNs();
  // the tag identifies this commit's flip event
  const int ret = drmModeAtomicCommit(m_masterFd, buffer.req,
                                      DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                                      reinterpret_cast<void*>(static_cast<uintptr_t>(tag)));
  const int err = errno;
  const int64_t end = MonotonicNs();
  drmModeAtomicFree(buffer.req);

  std::unique_lock lock(m_uiMutex);
  if (generation != m_uiGeneration)
    return; // its surface is gone
  if (ret == 0)
  {
    m_inCommit = buffer.bo;
    m_inCommitTag = tag;
    m_flipLostLogged = false;
    m_commitNs = end;
    m_report.uiCommits++;
    m_report.commitMax = std::max(m_report.commitMax, static_cast<double>(end - start) / 1000.0);
    return;
  }
  // never shown: back to the render thread
  m_returned.push_back(buffer.bo);
  if (m_report.uiFailed++ < 3)
    CLog::Log(LOGWARNING, "CPresentationCoordinator - GUI plane commit failed: {}",
              strerror(err));
}

void CPresentationCoordinator::OnFlip(uint64_t tag)
{
  std::unique_lock lock(m_uiMutex);
  if (!m_inCommit || tag != m_inCommitTag)
    return;
  if (m_onScreen)
    m_returned.push_back(m_onScreen);
  m_onScreen = m_inCommit;
  m_inCommit = nullptr;
  m_inCommitTag = 0;
  const double flip = static_cast<double>(MonotonicNs() - m_commitNs) / 1000.0;
  m_report.uiFlips++;
  m_report.flipSum += flip;
  m_report.flipMax = std::max(m_report.flipMax, flip);
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
  if (result.newFrame)
  {
    r.frames++;
    CLog::LogFC(LOGDEBUG, LOGAVTIMING,
                "CPresentationCoordinator - released pts:{:.3f} seq:{} wake:{:.0f}us",
                result.pts / 1000000.0, tick.seq,
                tick.vblankNs ? static_cast<double>(tick.wokeNs - tick.vblankNs) / 1000.0 : -1.0);
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
            "commits={} failed={} flips={} lost={} commit max={:.0f}us flip mean={:.0f}us "
            "max={:.0f}us",
            r.ticks, r.missed, r.synthetic, r.stale, r.held, r.frames, r.repeats, r.skipped,
            r.woke ? r.wakeSum / r.woke : 0.0, r.wakeMax, r.ticks ? r.workSum / r.ticks : 0.0,
            r.workMax, drops >= 0 && m_kernelDrops >= 0 ? drops - m_kernelDrops : -1,
            r.uiSubmits, r.uiReplaced, r.uiCommits, r.uiFailed, r.uiFlips, r.uiLostFlips,
            r.commitMax, r.uiFlips ? r.flipSum / r.uiFlips : 0.0, r.flipMax);
  m_kernelDrops = drops;
  m_report = Report();
}
