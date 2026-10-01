/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDClock.h"

#include "VideoReferenceClock.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"

#include <cmath>
#include <time.h>
#include <inttypes.h>
#include <math.h>
#include <memory>
#include <mutex>

CDVDClock::CDVDClock()
{
  std::unique_lock lock(m_systemsection);

  m_pauseClock = 0;
  m_bReset = true;
  m_paused = false;
  m_speedAfterPause = DVD_PLAYSPEED_PAUSE;
  m_iDisc = 0;
  m_maxspeedadjust = 5.0;
  m_systemAdjust = 0;
  m_speedAdjust = 0;
  m_startClock = 0;
  m_vSyncAdjust = 0;
  m_frameTime = DVD_TIME_BASE / 60.0;

  m_videoRefClock = std::make_unique<CVideoReferenceClock>();
  m_lastSystemTime = m_videoRefClock->GetTime();
  m_systemOffset = m_videoRefClock->GetTime();
  m_systemFrequency = CurrentHostFrequency();
  m_systemUsed = m_systemFrequency;
}

CDVDClock::~CDVDClock() = default;

// Returns the current absolute clock in units of DVD_TIME_BASE (usually microseconds).
double CDVDClock::GetAbsoluteClock(bool interpolated /*= true*/)
{
  std::unique_lock lock(m_systemsection);

  int64_t current;
  current = m_videoRefClock->GetTime(interpolated);

  return SystemToAbsolute(current);
}

double CDVDClock::GetClock(bool interpolated /*= true*/)
{
  std::unique_lock lock(m_critSection);

  int64_t current = m_videoRefClock->GetTime(interpolated);
  ApplyScheduledResume(current);
  m_systemAdjust += m_speedAdjust * (current - m_lastSystemTime);
  m_lastSystemTime = current;

  return SystemToPlaying(current);
}

double CDVDClock::GetClock(double& absolute, bool interpolated /*= true*/)
{
  int64_t current = m_videoRefClock->GetTime(interpolated);

  {
    std::unique_lock lock(m_systemsection);
    absolute = SystemToAbsolute(current);
  }

  // m_systemsection covers the absolute-clock conversion only. m_systemAdjust,
  // m_lastSystemTime, m_speedAdjust and everything SystemToPlaying() reads are
  // playing-clock state that every other accessor in this file guards with
  // m_critSection - including the single-argument GetClock() directly above,
  // which runs these same three lines under it. Holding only m_systemsection
  // here left this overload racing ErrorAdjust(), Discontinuity(), SetSpeed()
  // and its own sibling overload. The two locks are taken in sequence, never
  // nested, so this introduces no ordering constraint.
  std::unique_lock lock(m_critSection);

  ApplyScheduledResume(current);
  m_systemAdjust += m_speedAdjust * (current - m_lastSystemTime);
  m_lastSystemTime = current;

  return SystemToPlaying(current);
}

double CDVDClock::ReduceVsyncAdjust(double adjustment) const
{
  // ★ RenderManager derives this from fmod(renderPts - nextFramePts, frametime).
  // fmod takes the sign of its DIVIDEND, so the result spans
  // (-frametime, +frametime) - two full frame periods for a quantity that is
  // physically a sub-frame display phase. Left unreduced it parks near a whole
  // frame: measured on am9pro 2026-08-11 at -39.775ms of a 41.708ms frame, when
  // the true phase was -1.9ms.
  //
  // That matters because CAudioSinkAE::GetClock() adds this to the clock audio
  // is synced against, so audio gets aligned to a point displaced by almost a
  // full frame. And because the phase slides, the displacement slides with it:
  // over a 7.6 minute TrueHD run vsyncAdjust ramped +29.31ms while ActiveAE's
  // syncerror fell -29.31ms, their sum holding constant to within 0.01ms. That
  // is the entire "S6 TrueHD A/V drift" - ErrorAdjust then moves VIDEO a whole
  // frame to chase an error that was never physical. See
  // ../../../docs/s6_truehd_av_drift.md (samurihl work tree, NOT Kodi's docs/).
  //
  // ★ Deliberately reduced HERE rather than at the fmod in RenderManager. The
  // renderer feeds its own copy into "renderPts += frametime/2 - m_syncOffset",
  // where a whole-frame shift changes which frame is selected for display -
  // reducing it there would alter render cadence. Reducing it here cannot:
  // GetVsyncAdjust() has exactly two callers, CAudioSinkAE::GetClock() (the
  // defect) and the "!= 0" test in ErrorAdjust below, which a whole-frame shift
  // does not affect.
  //
  // fmod first so a pathological input cannot spin the range reduction.
  //
  // Near half a frame the two representatives are equally physical, and a
  // phase sitting there flips sides between windows - each flip steps the
  // clock audio is synced against by a whole frame. Keep the side already
  // published while the value stays within 3/4 of a frame (hysteresis), so
  // only a phase that really rotates on changes side.
  if (m_frameTime > 0.0)
  {
    adjustment = fmod(adjustment, m_frameTime);
    if (adjustment > m_frameTime / 2)
      adjustment -= m_frameTime;
    else if (adjustment <= -m_frameTime / 2)
      adjustment += m_frameTime;
    // After a drop (pause, seek, display loss) the side is kept from the
    // phase last held: a resume lands on the park it measured against that
    // side, and the other would move it by a whole frame.
    if (m_vSyncAdjustHasPhase || m_vSyncAdjustHintValid)
    {
      const double previous = m_vSyncAdjustHasPhase ? m_vSyncAdjust : m_vSyncAdjustHint;
      const double other = adjustment > 0 ? adjustment - m_frameTime : adjustment + m_frameTime;
      if (fabs(other - previous) < fabs(adjustment - previous) &&
          fabs(other) <= m_frameTime * 0.75)
        adjustment = other;
    }
  }

  return adjustment;
}

void CDVDClock::SetVsyncAdjust(double adjustment, unsigned int phaseGeneration)
{
  std::unique_lock lock(m_critSection);

  // measured before a drop that has happened since: it belongs to the old phase
  if (phaseGeneration != m_vSyncPhaseGeneration)
    return;

  m_vSyncAdjust = ReduceVsyncAdjust(adjustment);
  m_vSyncAdjustHasPhase = true;
  m_vSyncAdjustPending = false;
}

void CDVDClock::ResetVsyncAdjust()
{
  std::unique_lock lock(m_critSection);
  m_vSyncAdjust = 0;
  m_vSyncAdjustHasPhase = false;
  m_vSyncAdjustHintValid = false;
  m_vSyncAdjustPending = true;
  m_vSyncPhaseGeneration++;
}

void CDVDClock::ClearVsyncAdjust(bool settled)
{
  std::unique_lock lock(m_critSection);
  DropVsyncPhase(settled);
}

void CDVDClock::LoseVsyncAdjust()
{
  std::unique_lock lock(m_critSection);
  DropVsyncPhase(false);
  // pending even if no phase was held (sync was off before the display went):
  // the returning display is measured again, or settled if sync stays off
  m_vSyncAdjustPending = true;
}

void CDVDClock::DropVsyncPhase(bool settled)
{
  // A phase that is dropped while clock sync may still come back (display
  // lost, clock reset) is pending again: the renderer re-seeds it from its
  // next playing frame, and a passthrough start sync must not land before.
  if (settled)
    m_vSyncAdjustPending = false;
  else if (m_vSyncAdjustHasPhase)
    m_vSyncAdjustPending = true;
  if (m_vSyncAdjustHasPhase)
  {
    m_vSyncAdjustHint = m_vSyncAdjust;
    m_vSyncAdjustHintValid = !settled;
  }
  else if (settled)
    m_vSyncAdjustHintValid = false;
  m_vSyncAdjust = 0;
  m_vSyncAdjustHasPhase = false;
  m_vSyncPhaseGeneration++;
}

void CDVDClock::SettleVsyncAdjust()
{
  std::unique_lock lock(m_critSection);
  m_vSyncAdjustPending = false;
}

unsigned int CDVDClock::GetVsyncPhaseGeneration(bool& hasPhase) const
{
  std::unique_lock lock(m_critSection);
  hasPhase = m_vSyncAdjustHasPhase;
  return m_vSyncPhaseGeneration;
}

bool CDVDClock::IsVsyncAdjustPending() const
{
  std::unique_lock lock(m_critSection);
  return m_vSyncAdjustPending;
}

double CDVDClock::GetVsyncAdjust()
{
  std::unique_lock lock(m_critSection);
  return m_vSyncAdjust;
}

void CDVDClock::Pause(bool pause)
{
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();

  if (pause && !m_paused)
  {
    if (!m_pauseClock)
      m_speedAfterPause = m_systemFrequency * DVD_PLAYSPEED_NORMAL / m_systemUsed;
    else
      m_speedAfterPause = DVD_PLAYSPEED_PAUSE;

    SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_paused = true;
  }
  else if (!pause && m_paused)
  {
    m_paused = false;
    SetSpeed(m_speedAfterPause);
  }
}

bool CDVDClock::IsPaused() const
{
  std::unique_lock lock(m_critSection);
  // a scheduled resume whose instant has come runs, though the next GetClock
  // applies it
  if (m_resumeAt && m_videoRefClock->GetTime() >= m_resumeAt)
    return false;
  return m_pauseClock != 0;
}

void CDVDClock::Reset()
{
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();
  m_bReset = true;
}

bool CDVDClock::ScheduleResume(int iSpeed, double lead, int64_t& startNs, double& startClock)
{
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();
  const int64_t current = m_videoRefClock->GetTime();
  if (m_paused || !m_pauseClock || iSpeed != DVD_PLAYSPEED_NORMAL || lead <= 0.0)
  {
    SetSpeedAt(iSpeed, current);
    return false;
  }
  struct timespec mono = {};
  clock_gettime(CLOCK_MONOTONIC, &mono);
  const int64_t leadSystem = static_cast<int64_t>(lead * m_systemFrequency);
  m_resumeAt = current + leadSystem;
  m_resumeSpeed = iSpeed;
  m_scheduleNs = static_cast<int64_t>(mono.tv_sec) * 1000000000 + mono.tv_nsec +
                 static_cast<int64_t>(lead * 1e9);
  m_scheduleClock = SystemToPlaying(current);
  m_scheduleEpoch++;
  m_scheduleValid = true;
  startNs = m_scheduleNs;
  startClock = m_scheduleClock;
  return true;
}

bool CDVDClock::GetScheduledStart(int64_t& startNs, double& startClock, unsigned int& epoch) const
{
  std::unique_lock lock(m_critSection);
  if (!m_scheduleValid)
    return false;
  startNs = m_scheduleNs;
  startClock = m_scheduleClock;
  epoch = m_scheduleEpoch;
  return true;
}

void CDVDClock::ApplyScheduledResume(int64_t current)
{
  if (!m_resumeAt || current < m_resumeAt)
    return;
  const int64_t at = m_resumeAt;
  m_resumeAt = 0;
  // anchored at the scheduled instant, not at whenever a reader came by
  SetSpeedAt(m_resumeSpeed, at);
}

void CDVDClock::CancelScheduledResume()
{
  m_resumeAt = 0;
  m_scheduleValid = false;
}

void CDVDClock::Advance(double time)
{
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();

  if (m_pauseClock)
  {
    m_pauseClock += time / DVD_TIME_BASE * m_systemFrequency;
  }
}

void CDVDClock::SetSpeed(int iSpeed)
{
  // this will sometimes be a little bit of due to rounding errors, ie clock might jump a bit when changing speed
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();
  SetSpeedAt(iSpeed, m_videoRefClock->GetTime());
}

void CDVDClock::SetSpeedAt(int iSpeed, int64_t current)
{
  if (m_paused)
  {
    m_speedAfterPause = iSpeed;
    return;
  }

  if (iSpeed == DVD_PLAYSPEED_PAUSE)
  {
    if (!m_pauseClock)
      m_pauseClock = current;
    return;
  }

  int64_t newfreq = m_systemFrequency * DVD_PLAYSPEED_NORMAL / iSpeed;

  // Resuming from a pause shifts the clock by the pause length, and a speed
  // change rescales it: either way it now stands at another point of the
  // display's vsync cadence (measured am9pro: the phase moved 4 ms after a
  // resume, under an audio landing already made against the old one).
  if (m_pauseClock && newfreq == m_systemUsed && m_vSyncAdjustHasPhase && m_frameTime > 0.0)
  {
    // A plain resume at the same speed with the phase held throughout: the
    // clock stood still for the pause while the display ran on at its fixed
    // rate, so the phase moved by exactly the pause length, modulo a frame
    // (measured am9pro, 9 pauses of 3-9 s: within 0.5 ms of the renderer's
    // own measurement). Carry it over instead of dropping it, so a resume
    // need not wait for a new one; the renderer still re-measures (the
    // generation changes) and replaces it.
    const double paused =
        static_cast<double>(current - m_pauseClock) * DVD_TIME_BASE / m_systemFrequency;
    m_vSyncAdjust = ReduceVsyncAdjust(m_vSyncAdjust + fmod(paused, m_frameTime));
    m_vSyncPhaseGeneration++;
  }
  else if (m_pauseClock || newfreq != m_systemUsed)
    DropVsyncPhase(false);
  if (m_pauseClock)
  {
    m_startClock += current - m_pauseClock;
    m_pauseClock = 0;
  }

  m_startClock = current - (int64_t)((double)(current - m_startClock) * newfreq / m_systemUsed);
  m_systemUsed = newfreq;
}

void CDVDClock::SetSpeedAdjust(double adjust)
{
  CLog::Log(LOGDEBUG, "CDVDClock::SetSpeedAdjust - adjusted:{:f}", adjust);

  std::unique_lock lock(m_critSection);
  m_speedAdjust = adjust;
}

double CDVDClock::GetSpeedAdjust() const
{
  std::unique_lock lock(m_critSection);
  return m_speedAdjust;
}

double CDVDClock::ErrorAdjust(double error, const char* log)
{
  std::unique_lock lock(m_critSection);

  double clock, absolute, adjustment;
  clock = GetClock(absolute);

  // skip minor updates while speed adjust is active
  // -> adjusting buffer levels
  if (m_speedAdjust != 0 && error < DVD_MSEC_TO_TIME(100))
  {
    return 0;
  }

  adjustment = error;

  if (m_vSyncAdjust != 0)
  {
    // Audio ahead is more noticeable then audio behind video.
    // Correct if aufio is more than 20ms ahead or more then
    // 27ms behind. In a worst case scenario we switch from
    // 20ms ahead to 21ms behind (for fps of 23.976)
    if (error > 0.02 * DVD_TIME_BASE)
      adjustment = m_frameTime;
    else if (error < -0.027 * DVD_TIME_BASE)
      adjustment = -m_frameTime;
    else
      adjustment = 0;
  }

  if (adjustment == 0)
    return 0;

  // a whole-frame step (or no phase at all): the display phase is unchanged
  CancelScheduledResume();
  Rebase(clock + adjustment, absolute);

  CLog::Log(LOGDEBUG, "CDVDClock::ErrorAdjust - {} - error:{:f}, adjusted:{:f}", log, error,
            adjustment);
  return adjustment;
}

void CDVDClock::Discontinuity(double clock, double absolute)
{
  std::unique_lock lock(m_critSection);
  CancelScheduledResume();
  Rebase(clock, absolute);
  // the clock now stands at an arbitrary point of the display's vsync cadence
  DropVsyncPhase(false);
}

void CDVDClock::Rebase(double clock, double absolute)
{
  m_startClock = AbsoluteToSystem(absolute);
  if(m_pauseClock)
    m_pauseClock = m_startClock;
  m_iDisc = clock;
  m_bReset = false;
  m_systemAdjust = 0;
  m_speedAdjust = 0;
}

void CDVDClock::SetMaxSpeedAdjust(double speed)
{
  std::unique_lock lock(m_speedsection);

  m_maxspeedadjust = speed;
}

//returns the refreshrate if the videoreferenceclock is running, -1 otherwise
int CDVDClock::UpdateFramerate(double fps, double* interval /*= NULL*/)
{
  //sent with fps of 0 means we are not playing video
  if(fps == 0.0)
    return -1;

  {
    // SetVsyncAdjust() and ErrorAdjust() both read m_frameTime under
    // m_critSection and divide by it; this write held no lock at all. Scoped so
    // it is released before m_speedsection is taken below - the two are never
    // held together anywhere in this file and this keeps it that way.
    std::unique_lock lock(m_critSection);
    m_frameTime = 1/fps * DVD_TIME_BASE;
  }

  //check if the videoreferenceclock is running, will return -1 if not
  double rate = m_videoRefClock->GetRefreshRate(interval);

  if (rate <= 0)
    return -1;

  std::unique_lock lock(m_speedsection);

  double weight = (rate * 2) / fps;

  //set the speed of the videoreferenceclock based on fps, refreshrate and maximum speed adjust set by user
  if (m_maxspeedadjust > 0.05)
  {
    if (weight / MathUtils::round_int(weight) < 1.0 + m_maxspeedadjust / 100.0 &&
      weight / MathUtils::round_int(weight) > 1.0 - m_maxspeedadjust / 100.0)
      weight = MathUtils::round_int(weight);
  }
  double speed = (rate * 2.0 ) / (fps * weight);
  lock.unlock();

  m_videoRefClock->SetSpeed(speed);

  return rate;
}

bool CDVDClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const
{
  return m_videoRefClock->GetClockInfo(MissedVblanks, ClockSpeed, RefreshRate);
}

double CDVDClock::SystemToAbsolute(int64_t system) const
{
  return DVD_TIME_BASE * (double)(system - m_systemOffset) / m_systemFrequency;
}

int64_t CDVDClock::AbsoluteToSystem(double absolute) const
{
  return (int64_t)(absolute / DVD_TIME_BASE * m_systemFrequency) + m_systemOffset;
}

double CDVDClock::SystemToPlaying(int64_t system)
{
  int64_t current;

  if (m_bReset)
  {
    m_startClock = system;
    m_systemUsed = m_systemFrequency;
    if(m_pauseClock)
      m_pauseClock = m_startClock;
    m_iDisc = 0;
    m_systemAdjust = 0;
    m_speedAdjust = 0;
    DropVsyncPhase(false);
    m_bReset = false;
  }

  if (m_pauseClock)
    current = m_pauseClock;
  else
    current = system;

  return DVD_TIME_BASE * (double)(current - m_startClock + m_systemAdjust) / m_systemUsed + m_iDisc;
}

double CDVDClock::GetClockSpeed() const
{
  std::unique_lock lock(m_critSection);

  double speed = (double)m_systemFrequency / m_systemUsed;
  return m_videoRefClock->GetSpeed() * speed + m_speedAdjust;
}
