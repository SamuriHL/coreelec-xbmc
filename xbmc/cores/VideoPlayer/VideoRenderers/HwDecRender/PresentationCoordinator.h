/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/Thread.h"

#include <atomic>
#include <cstdint>

class CRenderManager;
struct SPresentTick;
struct SPresentResult;

/*!
 * \brief real_player presentation coordinator (docs/presentation_planes_design.md,
 * samurihl tree). Wakes on every hardware vblank, from DRM CRTC sequence events
 * on its own fd, and runs the render manager's per-vsync presentation step:
 * frame selection, clock sync and the video plane release. It never waits on
 * another thread; the display executor parks it with the display fence
 * (aml_presenter_hold_acquire).
 */
class CPresentationCoordinator : private CThread
{
public:
  explicit CPresentationCoordinator(CRenderManager& renderManager);
  ~CPresentationCoordinator() override;

  //! Opens the vblank source and starts the thread; false leaves it inactive.
  bool Start();
  void Stop();
  bool IsActive() const { return m_active; }

protected:
  void Process() override;

private:
  enum class State
  {
    IDLE,
    PLAYING,
    PAUSED,
    DISPLAY_LOST,
    HELD,
  };

  bool WaitVblank(SPresentTick& tick, unsigned int epoch);
  void SetState(State state, unsigned int epoch);
  void Account(const SPresentTick& tick, const SPresentResult& result, int64_t workNs);
  void LogReport();

  CRenderManager& m_renderManager;
  std::atomic<bool> m_active{false};
  int m_fd = -1;
  uint32_t m_crtc = 0;
  bool m_eventPending = false;
  int64_t m_queuedNs = 0;
  uint64_t m_lastSeq = 0;
  State m_state = State::IDLE;

  struct Report
  {
    int ticks = 0;
    int missed = 0; //!< vblanks between consecutive ticks
    int synthetic = 0;
    int stale = 0; //!< events from a previous display epoch, dropped
    int held = 0;
    int frames = 0;
    int repeats = 0; //!< playing, and no new frame
    int skipped = 0;
    int woke = 0;
    double wakeSum = 0.0, wakeMax = 0.0; //!< us after the hardware vblank
    double workSum = 0.0, workMax = 0.0; //!< us in the presentation step
  } m_report;
  int m_kernelDrops = -1;
};
