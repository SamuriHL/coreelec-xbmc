/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncTimeline.h"

#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "threads/Event.h"
#include "threads/Thread.h"
#include "utils/PresentationTimeline.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"

#include <algorithm>

#include <time.h>

using namespace PRESENTATION;

namespace
{
int64_t MonotonicToHostOffset()
{
  struct timespec mono = {};
  clock_gettime(CLOCK_MONOTONIC, &mono);
  const int64_t host = CurrentHostCounter();
  return host - (static_cast<int64_t>(mono.tv_sec) * 1000000000 + mono.tv_nsec);
}
} // namespace

CVideoSyncTimeline::CVideoSyncTimeline(CVideoReferenceClock* clock) : CVideoSync(clock)
{
}

bool CVideoSyncTimeline::Setup()
{
  // Start from a fresh tick: the reference clock anchors its time at Setup, and a
  // tick up to a period old would count a whole period that has partly gone,
  // stepping the clock forward by the difference (as CVideoSyncAML waits for a
  // fresh vblank). The timekeeper ticks every period and bumps once more when it
  // stops, so the wait returns.
  const uint32_t seen = TimelineTicks().load(std::memory_order_acquire);
  int64_t tl[TL_COUNT];
  if (!TimelineClockActive() || !TimelineBoard().Read(tl))
  {
    CLog::Log(LOGWARNING, "CVideoSyncTimeline: no timeline, falling back to the system clock");
    return false;
  }
  TimelineTicks().wait(seen, std::memory_order_acquire);
  if (!TimelineClockActive() || !TimelineBoard().Read(tl) || tl[TL_PERIOD_NUM] <= 0 ||
      tl[TL_PERIOD_DEN] <= 0)
  {
    CLog::Log(LOGWARNING, "CVideoSyncTimeline: no timeline, falling back to the system clock");
    return false;
  }
  m_tick = static_cast<uint64_t>(tl[TL_TICK]);
  m_epoch = tl[TL_EPOCH];
  m_rate = static_cast<double>(tl[TL_PERIOD_DEN]) / static_cast<double>(tl[TL_PERIOD_NUM]);
  m_offset = MonotonicToHostOffset();
  CLog::Log(LOGINFO, "CVideoSyncTimeline: counting timeline ticks (epoch {} tick {}, {:.6f} Hz)",
            m_epoch, m_tick, m_rate);
  return true;
}

void CVideoSyncTimeline::Run(CEvent& stopEvent)
{
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  while (true)
  {
    // Check after the load, before sleeping: the timekeeper clears the flag before
    // its last bump, so a count that already includes that bump shows the flag
    // cleared. It ticks every period (synthetic ticks while vblanks are missing),
    // so a sleep always ends.
    const uint32_t seen = TimelineTicks().load(std::memory_order_acquire);
    if (stopEvent.Signaled() || !TimelineClockActive())
      break;
    TimelineTicks().wait(seen, std::memory_order_acquire);
    if (stopEvent.Signaled() || !TimelineClockActive())
      break;

    int64_t tl[TL_COUNT];
    if (!TimelineBoard().Read(tl))
      continue;
    if (tl[TL_EPOCH] != m_epoch)
    {
      // the mode's timing changed: restart so the clock takes the new period
      CLog::Log(LOGDEBUG, "CVideoSyncTimeline: epoch {} -> {}, restarting the clock", m_epoch,
                tl[TL_EPOCH]);
      break;
    }
    const uint64_t tick = static_cast<uint64_t>(tl[TL_TICK]);
    if (tick <= m_tick)
      continue;

    // CLOCK_MONOTONIC is slewed by NTP against the host counter: follow it,
    // at most 1 ms per tick (as CVideoSyncAML does)
    const int64_t drift = MonotonicToHostOffset() - m_offset;
    m_offset += std::clamp(drift, static_cast<int64_t>(-1000000), static_cast<int64_t>(1000000));

    m_refClock->UpdateClock(static_cast<int>(tick - m_tick),
                            static_cast<uint64_t>(m_offset + tl[TL_VBLANK_NS]));
    m_tick = tick;
  }
}

void CVideoSyncTimeline::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoSyncTimeline: cleaning up");
}

float CVideoSyncTimeline::GetFps()
{
  CLog::Log(LOGDEBUG, "CVideoSyncTimeline: fps: {:.6f}", m_rate);
  return static_cast<float>(m_rate);
}
