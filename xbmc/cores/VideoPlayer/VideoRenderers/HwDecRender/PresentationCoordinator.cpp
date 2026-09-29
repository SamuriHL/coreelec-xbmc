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
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

namespace
{
constexpr int REPORT_TICKS = 1200;
constexpr int POLL_TIMEOUT_MS = 50;
// a queued event the CRTC never delivered (it went off): queue another
constexpr int64_t EVENT_LOST_NS = 100000000;

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

void OnSequence(int fd, uint64_t sequence, uint64_t ns, uint64_t userData)
{
  t_event.got = true;
  t_event.seq = sequence;
  t_event.ns = ns;
  t_event.epoch = userData;
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

CPresentationCoordinator::CPresentationCoordinator(CRenderManager& renderManager)
  : CThread("PresentCoord"), m_renderManager(renderManager)
{
}

CPresentationCoordinator::~CPresentationCoordinator()
{
  Stop();
}

bool CPresentationCoordinator::Start()
{
  if (m_active)
    return true;

  auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem || winSystem->GetDRMDeviceFd() < 0)
    return false;

  // Its own fd: vblank events are delivered per file, and the win system's fd
  // carries the page-flip events the GUI flip consumes.
  char* path = drmGetDeviceNameFromFd2(winSystem->GetDRMDeviceFd());
  if (path)
  {
    m_fd = open(path, O_RDWR | O_CLOEXEC);
    free(path);
  }
  m_crtc = winSystem->GetDRMCrtcId();
  if (m_fd < 0 || m_crtc == 0)
  {
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no DRM vblank source (fd:{} crtc:{}), "
                          "video stays on the render thread", m_fd, m_crtc);
    if (m_fd >= 0)
      close(m_fd);
    m_fd = -1;
    return false;
  }

  // take over only from a source that delivers: one vblank event
  uint64_t target = 0;
  struct pollfd pfd = {m_fd, POLLIN, 0};
  drmEventContext context = {};
  context.version = 4;
  context.sequence_handler = OnSequence;
  t_event.got = false;
  if (drmCrtcQueueSequence(m_fd, m_crtc, DRM_CRTC_SEQUENCE_RELATIVE, 1, &target, 0) == 0 &&
      poll(&pfd, 1, 100) > 0)
    drmHandleEvent(m_fd, &context);
  if (!t_event.got)
  {
    CLog::Log(LOGWARNING, "CPresentationCoordinator - no vblank event from crtc {}, "
                          "video stays on the render thread", m_crtc);
    close(m_fd);
    m_fd = -1;
    return false;
  }

  m_eventPending = false;
  m_lastSeq = 0;
  m_state = State::IDLE;
  m_report = Report();
  m_kernelDrops = ReadKernelDrops();
  m_active = true;
  Create();
  CLog::Log(LOGINFO, "CPresentationCoordinator - started (crtc {})", m_crtc);
  return true;
}

void CPresentationCoordinator::Stop()
{
  if (!m_active)
    return;

  StopThread(true);
  m_active = false;
  if (m_fd >= 0)
    close(m_fd);
  m_fd = -1;
  LogReport();
  CLog::Log(LOGINFO, "CPresentationCoordinator - stopped");
}

void CPresentationCoordinator::Process()
{
  SetPriority(ThreadPriority::ABOVE_NORMAL);
  aml_presenter_set_running(true);

  while (!m_bStop)
  {
    unsigned int epoch = 0;
    if (aml_presenter_check_hold(epoch))
    {
      SetState(State::HELD, epoch);
      m_report.held++;
      Sleep(std::chrono::milliseconds(5));
      continue;
    }

    SPresentTick tick;
    if (!WaitVblank(tick, epoch) || m_bStop)
      continue;
    // a hold that landed during the wait is acknowledged before any release
    if (aml_presenter_check_hold(tick.epoch))
      continue;

    SPresentResult result;
    const int64_t start = MonotonicNs();
    m_renderManager.PresentTick(tick, result);
    const int64_t work = MonotonicNs() - start;

    if (!result.configured)
      SetState(State::IDLE, tick.epoch);
    else if (result.displayLost)
      SetState(State::DISPLAY_LOST, tick.epoch);
    else
      SetState(result.playing ? State::PLAYING : State::PAUSED, tick.epoch);

    Account(tick, result, work);
  }

  aml_presenter_set_running(false);
}

bool CPresentationCoordinator::WaitVblank(SPresentTick& tick, unsigned int epoch)
{
  const int64_t now = MonotonicNs();
  if (m_eventPending && now - m_queuedNs > EVENT_LOST_NS)
    m_eventPending = false;

  if (!m_eventPending)
  {
    uint64_t target = 0;
    if (drmCrtcQueueSequence(m_fd, m_crtc, DRM_CRTC_SEQUENCE_RELATIVE, 1, &target, epoch) == 0)
    {
      m_eventPending = true;
      m_queuedNs = now;
    }
  }

  struct pollfd pfd = {m_fd, POLLIN, 0};
  if (poll(&pfd, 1, POLL_TIMEOUT_MS) > 0 && (pfd.revents & POLLIN))
  {
    drmEventContext context = {};
    context.version = 4;
    context.sequence_handler = OnSequence;
    t_event.got = false;
    drmHandleEvent(m_fd, &context);
    if (t_event.got)
    {
      m_eventPending = false;
      // queued before a display transaction: its time may be the CRTC
      // switching off, not a vsync
      if (t_event.epoch != epoch)
      {
        m_report.stale++;
        return false;
      }
      tick.seq = t_event.seq;
      tick.vblankNs = static_cast<int64_t>(t_event.ns);
      tick.wokeNs = MonotonicNs();
      tick.epoch = epoch;
      return true;
    }
  }

  // no vblank within the timeout (CRTC off or stalled): keep time with a
  // timer tick, as the render thread's 50 ms poll fallback did
  tick.seq = 0;
  tick.vblankNs = 0;
  tick.wokeNs = MonotonicNs();
  tick.epoch = epoch;
  m_report.synthetic++;
  return true;
}

void CPresentationCoordinator::SetState(State state, unsigned int epoch)
{
  if (state == m_state)
    return;
  CLog::Log(LOGINFO, "CPresentationCoordinator - {} -> {} (display epoch {})",
            StateName(static_cast<int>(m_state)), StateName(static_cast<int>(state)), epoch);
  m_state = state;
}

void CPresentationCoordinator::Account(const SPresentTick& tick, const SPresentResult& result, int64_t workNs)
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
  const Report& r = m_report;
  if (!r.ticks)
    return;
  const int drops = ReadKernelDrops();
  CLog::Log(LOGINFO,
            "real_player coordinator: ticks={} missed={} synthetic={} stale={} held={} "
            "frames={} repeats={} skipped={} wake mean={:.0f}us max={:.0f}us "
            "work mean={:.0f}us max={:.0f}us kernel drops={}",
            r.ticks, r.missed, r.synthetic, r.stale, r.held, r.frames, r.repeats, r.skipped,
            r.woke ? r.wakeSum / r.woke : 0.0, r.wakeMax, r.workSum / r.ticks, r.workMax,
            drops >= 0 && m_kernelDrops >= 0 ? drops - m_kernelDrops : -1);
  m_kernelDrops = drops;
  m_report = Report();
}
