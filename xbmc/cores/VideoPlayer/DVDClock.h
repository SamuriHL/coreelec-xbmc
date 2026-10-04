/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"

#include <memory>
#include <stdint.h>

class CVideoReferenceClock;

class CDVDClock
{
public:

  CDVDClock();
  ~CDVDClock();

  double GetClock(bool interpolated = true);
  double GetClock(double& absolute, bool interpolated = true);

  double ErrorAdjust(double error, const char* log);
  void Discontinuity(double clock, double absolute);
  void Discontinuity(double clock = 0LL)
  {
    Discontinuity(clock, GetAbsoluteClock());
  }

  void Reset();
  void SetSpeed(int iSpeed);
  void SetSpeedAdjust(double adjust);
  double GetSpeedAdjust() const;

  double GetClockSpeed()
      const; /**< get the current speed of the clock relative normal system time */

  /* tells clock at what framerate video is, to  *
   * allow it to adjust speed for a better match */
  int UpdateFramerate(double fps, double* interval = NULL);

  void SetMaxSpeedAdjust(double speed);

  double GetAbsoluteClock(bool interpolated = true);
  double GetFrequency() { return (double)m_systemFrequency ; }

  bool GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const;
  //! phaseGeneration: GetVsyncPhaseGeneration() when the phase was measured
  void SetVsyncAdjust(double adjustment, unsigned int phaseGeneration);
  double GetVsyncAdjust();
  //! renderer reset: no phase, and the display's phase is still to come
  void ResetVsyncAdjust();
  //! no phase; settled = none is coming either (clock sync is off)
  void ClearVsyncAdjust(bool settled);
  //! the display is lost: no phase, and the returning display's is to come
  void LoseVsyncAdjust();
  //! the renderer was reset and has not published this display's phase yet
  bool IsVsyncAdjustPending() const;
  //! changes whenever the phase is dropped, reset or carried across a pause:
  //! samples measured under an older generation belong to a phase that no
  //! longer holds. hasPhase: a phase is held (carried, or not yet dropped).
  unsigned int GetVsyncPhaseGeneration(bool& hasPhase) const;
  //! no phase is coming soon (no frame is being played): stop waiting for one
  void SettleVsyncAdjust();

  void Pause(bool pause);
  bool IsPaused() const;
  void Advance(double time);

  //! Resume the clock held at speed 0 at a scheduled instant instead of now
  //! (design §15, step 2.2): it stays paused for `lead` seconds, then runs at
  //! iSpeed anchored exactly at that instant, so the audio output can land its
  //! first sample there. Any other writer before then cancels the schedule,
  //! except SetSpeed at the scheduled speed, which it already applies.
  //! startNs: the instant (CLOCK_MONOTONIC); startClock: the clock there.
  //! false (the clock resumed now) if it was not held at speed 0.
  bool ScheduleResume(int iSpeed, double lead, int64_t& startNs, double& startClock);
  //! A start on the running clock for a stream that joins it (an audio track
  //! switch, design 15.34): the audio output lands its first sample at a vblank
  //! `lead` seconds from now. false if the clock is held or a resume pending.
  bool ScheduleJoin(double lead, int64_t& startNs, double& startClock);
  //! the scheduled resume, while no writer has changed the clock since
  bool GetScheduledStart(int64_t& startNs, double& startClock, unsigned int& epoch) const;
  //! the player holds the clock for a start (it then resumes or schedules it)
  void SetStartHeld(bool held);
  //! a start the clock has yet to make: held for it (startNs 0), or scheduled
  //! and not yet reached (startNs its CLOCK_MONOTONIC instant). startClock is
  //! the clock it starts from. A frame shown before then is shown into a clock
  //! that has not started, so selection must use the clock on screen then.
  bool GetPendingStart(int64_t& startNs, double& startClock);

protected:
  //! caller holds m_critSection
  void DropVsyncPhase(bool settled);
  //! a phase reduced to one frame, on the side of the phase held (or last held)
  double ReduceVsyncAdjust(double adjustment) const;
  //! caller holds m_critSection
  void Rebase(double clock, double absolute);
  //! caller holds m_critSection
  void SetSpeedAt(int iSpeed, int64_t current);
  //! caller holds m_critSection: resume now if the scheduled instant has come
  void ApplyScheduledResume(int64_t current);
  //! caller holds m_critSection
  void CancelScheduledResume();
  double SystemToAbsolute(int64_t system) const;
  int64_t AbsoluteToSystem(double absolute) const;
  double SystemToPlaying(int64_t system);
  //! caller holds m_critSection: the reference-clock time of the vblank at
  //! least `lead` seconds from `current`; sets m_scheduleNs to it
  int64_t ScheduleInstant(int64_t current, double lead);
  static unsigned int NextScheduleEpoch();

  mutable CCriticalSection m_critSection;
  int64_t m_systemUsed;
  int64_t m_startClock;
  int64_t m_pauseClock;
  double m_iDisc;
  bool m_bReset;
  bool m_paused;
  int m_speedAfterPause;
  std::unique_ptr<CVideoReferenceClock> m_videoRefClock;

  int64_t m_systemFrequency;
  int64_t m_systemOffset;
  CCriticalSection m_systemsection;

  int64_t m_systemAdjust;
  int64_t m_lastSystemTime;
  double m_speedAdjust;
  double m_vSyncAdjust;
  bool m_vSyncAdjustPending = false;
  bool m_vSyncAdjustHasPhase = false;
  double m_vSyncAdjustHint = 0.0; // the phase last held before a drop
  bool m_vSyncAdjustHintValid = false;
  unsigned int m_vSyncPhaseGeneration = 0;
  double m_frameTime;

  double m_maxspeedadjust;
  CCriticalSection m_speedsection;

  int64_t m_resumeAt = 0; // reference-clock time of a pending scheduled resume
  bool m_startHeld = false;
  int m_resumeSpeed = 0;
  bool m_scheduleValid = false;
  int64_t m_scheduleNs = 0;
  double m_scheduleClock = 0.0;
  unsigned int m_scheduleEpoch = 0;
};
