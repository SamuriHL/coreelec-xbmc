/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/amlogic/ThreadRole.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <fmt/format.h>

// GT-D21 (measurement, log only): how long C and G wait for the locks they share with threads
// that are not real-time (main, P, V, W, the log writer). A take tries first; only a contended
// take is timed, and only on C or G. Other threads lock as before.
namespace LOCKWAIT
{
enum Id
{
  RM_STATE, //!< RM m_statelock (C's tick; fed by RM's own measurement)
  RM_PRESENT, //!< RM m_presentlock (same)
  UI, //!< the coordinator's m_uiMutex
  VIDEO, //!< the coordinator's m_videoMutex
  ROUTE, //!< GRAPHICSTICK s_mutex
  UNTIMED, //!< GRAPHICSTICK s_untimedMutex
  HOLD, //!< AMLUtils s_holdMutex (presenter holds)
  RECT, //!< CRendererAML m_rectMutex
  SHOW, //!< CGraphicsPlaneAML m_showSection
  LIBASS, //!< a libass handler's m_section (G's render)
  GTLOG, //!< GTLOG s_logMutex
  COUNT
};

inline constexpr const char* NAMES[COUNT] = {"statelock", "presentlock", "ui",   "video",
                                             "route",     "untimed",     "hold", "rect",
                                             "show",      "libass",      "gtlog"};

struct SCounters
{
  std::atomic<int64_t> takes{0};
  std::atomic<int64_t> waits{0}; //!< contended takes
  std::atomic<int64_t> maxNs{0};
  std::atomic<int64_t> overMs{0}; //!< waits over 1 ms
};

//! [0] C, [1] G
inline SCounters g_counters[2][COUNT];

constexpr int ROLE_C = 4;
constexpr int ROLE_G = 6;

//! the counters of the calling thread's role; nullptr: not C or G
inline SCounters* Mine(Id id)
{
  const int role = OUTPUT::ThreadRole();
  if (role == ROLE_C)
    return &g_counters[0][id];
  if (role == ROLE_G)
    return &g_counters[1][id];
  return nullptr;
}

inline int64_t NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

//! one take on C or G that waited waitNs (0: it did not wait)
inline void NoteWait(SCounters& c, int64_t waitNs)
{
  c.takes.fetch_add(1, std::memory_order_relaxed);
  if (waitNs <= 0)
    return;
  c.waits.fetch_add(1, std::memory_order_relaxed);
  if (waitNs > 1000000)
    c.overMs.fetch_add(1, std::memory_order_relaxed);
  int64_t was = c.maxNs.load(std::memory_order_relaxed);
  while (waitNs > was && !c.maxNs.compare_exchange_weak(was, waitNs, std::memory_order_relaxed))
  {
  }
}

//! a take measured elsewhere (RM's tracked locks, which see their own contention)
inline void Note(Id id, bool contended, int64_t waitNs)
{
  if (SCounters* c = Mine(id))
    NoteWait(*c, contended ? std::max<int64_t>(1, waitNs) : 0);
}

//! the lock, as std::unique_lock lock(m) would take it; a contended take on C or G is timed
template<typename M>
std::unique_lock<M> Take(M& m, Id id)
{
  std::unique_lock<M> lock(m, std::try_to_lock);
  SCounters* c = Mine(id);
  if (lock.owns_lock())
  {
    if (c)
      NoteWait(*c, 0);
    return lock;
  }
  if (!c)
  {
    lock.lock();
    return lock;
  }
  const int64_t start = NowNs();
  lock.lock();
  NoteWait(*c, std::max<int64_t>(1, NowNs() - start));
  return lock;
}

//! "GLOCK <C|G> <lock>=<takes>/<waits>/<max_us>/<over_1ms> …" for locks taken since the last
//! call with reset; empty when none
inline std::string ReportLine(int thread, bool reset)
{
  std::string line;
  for (int i = 0; i < COUNT; i++)
  {
    SCounters& c = g_counters[thread][i];
    const int64_t takes = reset ? c.takes.exchange(0) : c.takes.load();
    const int64_t waits = reset ? c.waits.exchange(0) : c.waits.load();
    const int64_t maxNs = reset ? c.maxNs.exchange(0) : c.maxNs.load();
    const int64_t overMs = reset ? c.overMs.exchange(0) : c.overMs.load();
    if (!takes)
      continue;
    line += fmt::format(" {}={}/{}/{}/{}", NAMES[i], takes, waits, maxNs / 1000, overMs);
  }
  if (line.empty())
    return line;
  return fmt::format("GLOCK {} takes/waits/max_us/over_1ms:{}", thread == 0 ? "C" : "G", line);
}

} // namespace LOCKWAIT
