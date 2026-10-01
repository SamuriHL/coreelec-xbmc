/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The presentation timeline and the boards that feed its shadow comparison
// (design: docs/presentation_coordinator_design.md, phase 1).
//
// Each board is a seqlock over atomic fields: one writer, any number of
// readers, no lock and no allocation on either side. A reader that races the
// writer retries.

#include <atomic>
#include <cstdint>

namespace PRESENTATION
{

template<int N>
class CSeqBoard
{
public:
  void Write(const int64_t (&values)[N])
  {
    const uint32_t seq = m_seq.load(std::memory_order_relaxed);
    m_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < N; i++)
      m_values[i].store(values[i], std::memory_order_relaxed);
    m_seq.store(seq + 2, std::memory_order_release);
  }

  // false while nothing has been written, or if the writer kept racing
  bool Read(int64_t (&values)[N]) const
  {
    for (int tries = 0; tries < 64; tries++)
    {
      const uint32_t seq = m_seq.load(std::memory_order_acquire);
      if (seq & 1)
        continue;
      for (int i = 0; i < N; i++)
        values[i] = m_values[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (m_seq.load(std::memory_order_relaxed) == seq)
        return seq != 0;
    }
    return false;
  }

private:
  std::atomic<uint32_t> m_seq{0};
  std::atomic<int64_t> m_values[N]{};
};

// written by the timekeeper at every tick
enum Timeline
{
  TL_TICK,       // the timeline's own tick counter (never steps back)
  TL_KERNEL_SEQ, // the kernel's vblank sequence of the last real tick
  TL_VBLANK_NS,  // its CLOCK_MONOTONIC timestamp (synthetic: the computed one)
  TL_PERIOD_NUM, // nominal period = NUM / DEN seconds, from the DRM mode
  TL_PERIOD_DEN,
  TL_EPOCH,      // bumps when the mode's timing changes
  TL_SYNTHETIC,  // 1 while ticks are timer-made
  TL_COUNT
};

// written by the audio sink after each delay read (shadow only)
enum AudioPosition
{
  AP_FRAMES_PLAYED, // frames written since the sink opened, minus the delay
  AP_RATE,          // sample rate of the open device
  AP_MONO_NS,       // CLOCK_MONOTONIC right after the status read
  AP_HTSTAMP_NS,    // the driver's timestamp of that position (its own clock)
  AP_WALL_NS,       // CLOCK_REALTIME right after the read, to map a wall-clock htstamp
  AP_OPEN_ID,       // bumps whenever the position restarts (open, flush, recover)
  AP_COUNT
};

// written by the coordinator's video tick (shadow only)
enum CoordinatorSample
{
  CS_SEQ,       // the kernel vblank sequence the coordinator acted on
  CS_VBLANK_NS, // its timestamp
  CS_CLOCK_US,  // CDVDClock::GetClock() at that tick, in µs
  CS_PLAYING,
  CS_COUNT
};

inline CSeqBoard<TL_COUNT>& TimelineBoard()
{
  static CSeqBoard<TL_COUNT> board;
  return board;
}

inline CSeqBoard<AP_COUNT>& AudioBoard()
{
  static CSeqBoard<AP_COUNT> board;
  return board;
}

inline CSeqBoard<CS_COUNT>& CoordinatorBoard()
{
  static CSeqBoard<CS_COUNT> board;
  return board;
}

// set while the phase-1 shadow runs (special://profile/timekeeper_shadow)
inline std::atomic<bool>& ShadowActive()
{
  static std::atomic<bool> active{false};
  return active;
}

} // namespace PRESENTATION
