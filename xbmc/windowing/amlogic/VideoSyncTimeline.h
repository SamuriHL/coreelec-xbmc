/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The reference clock's vblank source when the timekeeper keeps time
// (special://profile/timeline_clock; design: docs/presentation_coordinator_design.md
// §15, step 2.1). It reads the timeline instead of the kernel: the timekeeper
// stays the only vblank reader, and the clock counts its ticks at the
// timeline's nominal period.

#include "windowing/VideoSync.h"

#include <cstdint>

class CVideoSyncTimeline : public CVideoSync
{
public:
  explicit CVideoSyncTimeline(CVideoReferenceClock* clock);
  bool Setup() override;
  void Run(CEvent& stopEvent) override;
  void Cleanup() override;
  float GetFps() override;

private:
  uint64_t m_tick = 0;
  int64_t m_epoch = 0;
  double m_rate = 0.0; // the timeline's nominal rate (Hz)
  // host counter (MONOTONIC_RAW) minus CLOCK_MONOTONIC, slewed per tick
  int64_t m_offset = 0;
};
