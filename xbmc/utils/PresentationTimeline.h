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
#include <chrono>
#include <cstdint>
#include <numeric>

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
  CS_SPEED_PPM, // CDVDClock's speed against the reference clock's ticks, less 1, in ppm
  CS_RUN_SINCE_NS, // the vblank of the first tick of this run of CS_PLAYING (0: not playing)
  CS_COUNT
};

// the latest frame and audio sample at their outputs (shadow only)
enum VideoPins
{
  VP_PTS_US,       // pts of the last released frame
  VP_ON_SCREEN_NS, // CLOCK_MONOTONIC of the vblank it is on VD1 from
  VP_COUNT
};

enum AudioPins
{
  AQ_PTS_US,     // pts of the first sample of the last audible buffer written
  AQ_ON_PINS_NS, // CLOCK_MONOTONIC at which that sample leaves the HDMI pins
  AQ_LABEL_US,   // the label of that sample: pts less the codec's label lag
  AQ_COUNT
};

// written by the mode setter just before it commits a mode
enum ModeNotice
{
  MN_SERIAL,     // bumps per notice
  MN_PERIOD_NUM, // the new mode's period = NUM / DEN seconds
  MN_PERIOD_DEN,
  MN_COUNT
};

// A DRM mode's frame period as a fraction of a second. Amlogic makes the
// 1000/1001 rates through the connector's FRAC_RATE_POLICY while the mode
// keeps the integer-rate clock.
inline void ModePeriod(uint32_t htotal,
                       uint32_t vtotal,
                       uint32_t clockKHz,
                       uint32_t vrefresh,
                       bool fractional,
                       uint64_t& num,
                       uint64_t& den)
{
  num = static_cast<uint64_t>(htotal) * vtotal;
  den = static_cast<uint64_t>(clockKHz) * 1000;
  if (fractional && (vrefresh == 24 || vrefresh == 30 || vrefresh == 48 || vrefresh == 60 ||
                     vrefresh == 120 || vrefresh == 240))
  {
    num *= 1001;
    den *= 1000;
  }
  const uint64_t g = std::gcd(num, den);
  if (g)
  {
    num /= g;
    den /= g;
  }
}

inline CSeqBoard<VP_COUNT>& VideoPinsBoard()
{
  static CSeqBoard<VP_COUNT> board;
  return board;
}

inline CSeqBoard<AQ_COUNT>& AudioPinsBoard()
{
  static CSeqBoard<AQ_COUNT> board;
  return board;
}

inline CSeqBoard<MN_COUNT>& ModeBoard()
{
  static CSeqBoard<MN_COUNT> board;
  return board;
}

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

// The timeline's tick, bumped by the timekeeper after each publish. Readers that
// present per tick sleep on it (std::atomic::wait, a futex) instead of polling;
// the timekeeper also bumps it when it stops, so no reader sleeps forever.
inline std::atomic<uint32_t>& TimelineTicks()
{
  // 32-bit: a futex word, so wait/notify go straight to the kernel
  static std::atomic<uint32_t> ticks{0};
  return ticks;
}
// Issue 27 (measurement, log only): P's demuxer reads, as the coordinator sees them. P writes,
// C reads; nothing waits.
struct SPlayerReads
{
  std::atomic<int64_t> readStartNs{
      0}; //!< CLOCK_MONOTONIC at the start of the read in progress; 0: none
  std::atomic<bool> stallEligible{false}; //!< P: a stall would be judged now (playing 1x, no menu,
      //!< still, held start, stream end or realtime stream)
  std::atomic<uint64_t> stalls{0}; //!< P's stall path taken ("stalled at clock …, caching")
  std::atomic<int64_t> starvedNs{0}; //!< C: when it last judged a starvation (the read-stall test)
  std::atomic<bool> plainInput{false}; //!< P: a plain file or stream (no disc, no navigation)
  //! C: the starvation freeze acts here (validated, no starve_off)
  std::atomic<bool> freezeOn{false};
  //! V: held a starvation's picture; P takes it at its next stall check
  std::atomic<bool> pictureHeld{false};
  //! V: a starvation's picture held now (until a packet or a flush)
  std::atomic<bool> pictureHeldNow{false};
  //! ISS27-D2 (log only): the start of the disc I/O call (an image's blocks, a disc file's bytes,
  //! a DVD's sectors) P is in, inside its demuxer read; 0: none (a navigator or BD-J wait, a still)
  std::atomic<int64_t> ioStartNs{0};
  //! ISS27-D2 test: the next disc I/O call inside P's read runs the pread_stall "d..." stall
  std::atomic<bool> ioStallArmed{false};
  std::atomic<int64_t> reads{0}; //!< since C's last report
  std::atomic<int64_t> maxNs{0};
  std::atomic<int64_t> over100ms{0};
  std::atomic<int64_t> over1s{0};
};
inline SPlayerReads& PlayerReads()
{
  static SPlayerReads reads;
  return reads;
}
//! ISS27-D2 (log only): where a stalled title's frames are. P, V and the AML codec write; the
//! still entry and the stall-window lines read. Lock-free, relaxed: a log, not a decision
struct SStallDiag
{
  std::atomic<double> demuxBlDts{0.0}; //!< P: the last base-layer (main video) packet demuxed
  std::atomic<double> demuxBlPts{0.0};
  std::atomic<double> demuxElDts{0.0}; //!< P: the last enhancement-layer packet demuxed
  std::atomic<double> demuxElPts{0.0};
  std::atomic<int> pairQueued{0}; //!< codec: BL/EL packets waiting for their partner
  std::atomic<int> pairHead{-1}; //!< codec: the oldest waiting one: 0 BL, 1 EL, -1 none
  std::atomic<double> pairHeadDts{0.0};
  std::atomic<double> decoderInPts{0.0}; //!< codec: the last access unit the decoder accepted
  std::atomic<double> decoderOutPts{0.0}; //!< codec: the last picture the decoder gave out
  std::atomic<int64_t> untilNs{0}; //!< P: the window lines run until then after a long read
  std::atomic<int> lines{0}; //!< lines left in this window
};
inline SStallDiag& StallDiag()
{
  static SStallDiag diag;
  return diag;
}
//! the stall window: P's read in progress for 300 ms or more, or up to 3 s after one returned;
//! 600 lines at most per window. True: log this line
inline bool StallDiagLine()
{
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  const int64_t read = PlayerReads().readStartNs.load(std::memory_order_relaxed);
  SStallDiag& diag = StallDiag();
  const bool on =
      (read && now - read >= 300000000) || now < diag.untilNs.load(std::memory_order_relaxed);
  if (!on)
  {
    diag.lines.store(600, std::memory_order_relaxed);
    return false;
  }
  return diag.lines.fetch_sub(1, std::memory_order_relaxed) > 0;
}

//! P's thread inside its demuxer read: only its disc I/O is stamped (BD-J and the GUI read the
//! disc too, on their own threads)
inline int& PlayerReadDepth()
{
  thread_local int depth = 0;
  return depth;
}
//! ISS27-D2 test: the armed pread_stall "d..." stall, run inside a disc I/O call (P sets it)
inline std::atomic<void (*)()>& DiscIoStall()
{
  static std::atomic<void (*)()> stall{nullptr};
  return stall;
}
//! ISS27-D2 (log only): stamps a disc I/O call made inside P's read (the outermost one)
class CDiscIoScope
{
public:
  CDiscIoScope()
  {
    if (PlayerReadDepth() != 1)
      return;
    m_on = true;
    PlayerReadDepth() = 2;
    PlayerReads().ioStartNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count(),
                                  std::memory_order_release);
    if (PlayerReads().ioStallArmed.exchange(false))
    {
      if (void (*stall)() = DiscIoStall().load())
        stall();
    }
  }
  ~CDiscIoScope()
  {
    if (!m_on)
      return;
    PlayerReads().ioStartNs.store(0, std::memory_order_release);
    PlayerReadDepth() = 1;
  }
  CDiscIoScope(const CDiscIoScope&) = delete;
  CDiscIoScope& operator=(const CDiscIoScope&) = delete;

private:
  bool m_on = false;
};

// Bumped by the audio sink each time a scheduled start lands: the audio
// follower measures its phase from there.
inline std::atomic<uint64_t>& AudioLandings()
{
  static std::atomic<uint64_t> landings{0};
  return landings;
}

// set while the timekeeper runs as the reference clock's vblank source
// (default on validated SoCs, off with special://profile/timelineclock_off; design §15
// step 2.1)
inline std::atomic<bool>& TimelineClockActive()
{
  static std::atomic<bool> active{false};
  return active;
}

// set while the phase-1 shadow runs (special://profile/timekeeper_shadow)
inline std::atomic<bool>& ShadowActive()
{
  static std::atomic<bool> active{false};
  return active;
}

} // namespace PRESENTATION
