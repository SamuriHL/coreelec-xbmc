/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

/*!
 * \brief The one writer of libbluray's disc clock (design §18.6-18.8).
 *
 * Disc time (BD-J and HDMV timers) runs at 1x and freezes only for an output
 * change: from a display loss or a held start until the start instant that
 * ends it. The target is level-triggered from the inputs:
 * (displayLost || startHeld) ? FROZEN at now : RUN at max(now, T0). T0 comes
 * only from the release of a real hold and from a display reset's scheduled
 * resume. An input that outlives its cap is ignored (ERROR) until it changes.
 *
 * Inputs arrive on the main thread (display) and the VideoPlayer thread
 * (hold, bounds, begin/end). Evaluate is a leaf: it takes this object's
 * mutex and the disc clock's, nothing else.
 */
class CDiscTimeOwner
{
public:
  //! a disc input opened; disabled = the clock is never frozen
  void Begin(bool enabled);
  //! the disc input closes: any freeze ends
  void End();
  //! runAtNs: CLOCK_MONOTONIC ns of a scheduled resume (0 = none)
  void SetDisplayLost(bool lost, int64_t runAtNs = 0);
  //! a real held start (never a no-hold scheduled start) and its release
  void SetStartHeld(bool held, int64_t runAtNs = 0);
  void CheckBounds();

private:
  void Evaluate(const char* why);
  void Write(bool running, int64_t atNs, const char* why);

  std::mutex m_lock;
  bool m_active = false;
  bool m_displayLost = false;
  bool m_startHeld = false;
  bool m_lostExpired = false;
  bool m_heldExpired = false;
  std::chrono::steady_clock::time_point m_lostSince;
  std::chrono::steady_clock::time_point m_heldSince;
  std::chrono::milliseconds m_lostCap{0};
  int64_t m_runAtNs = 0; //!< T0, pending until used
  bool m_frozen = false; //!< as last written
  int64_t m_writtenRunAtNs = 0;
};
