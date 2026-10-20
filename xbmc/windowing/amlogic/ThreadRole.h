/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <cstdint>

// Phase 5 step 5.V1: the role of a video-side thread (design I1), for MAINCOUPLING. Thread names
// are truncated to 15 characters, so the role is a tag set at thread start.
//
// MAINCOUPLING logs "MAINCOUPLING <site> role=<role> tid=<tid>" the first time a thread that is
// not main takes a lock main shares or calls an accessor of main's state at a site. Inside an
// accessor the line also names the caller of that first call, as an offset in the binary
// (addr2line resolves it).

namespace OUTPUT
{

constexpr int ROLE_UNKNOWN = 0; //!< untagged, and not the process's first thread
constexpr int ROLE_MAIN = 1;
constexpr int ROLE_COUNT = 10;

//! index into the role names; -1 until tagged or first asked
inline thread_local int threadRole = -1;

//! at thread start: P, V, C, RefClock, G, W, A or timekeeper; logs the tid it runs on
void SetThreadRole(const char* role);
int ResolveThreadRole();
inline int ThreadRole()
{
  return threadRole >= 0 ? threadRole : ResolveThreadRole();
}

struct SCouplingSite
{
  const char* what;
  const char* file;
  int line;
  std::atomic<uint32_t> seen{0}; //!< roles logged here
};

void LogMainCoupling(const SCouplingSite& site, int role, const void* caller);

inline void NoteMainCoupling(SCouplingSite& site, const void* caller)
{
  const int role = ThreadRole();
  if (role == ROLE_MAIN)
    return;
  const uint32_t bit = 1u << role;
  if (site.seen.load(std::memory_order_relaxed) & bit)
    return;
  if (!(site.seen.fetch_or(bit, std::memory_order_relaxed) & bit))
    LogMainCoupling(site, role, caller);
}

} // namespace OUTPUT

//! at a main-shared lock take or accessor call
#define MAINCOUPLING(what) \
  do \
  { \
    static OUTPUT::SCouplingSite mcSite{what, __FILE__, __LINE__}; \
    OUTPUT::NoteMainCoupling(mcSite, nullptr); \
  } while (false)

//! first statement of a main-shared accessor: the site is its caller
#define MAINCOUPLING_ACCESSOR(what) \
  do \
  { \
    static OUTPUT::SCouplingSite mcSite{what, __FILE__, __LINE__}; \
    OUTPUT::NoteMainCoupling(mcSite, __builtin_return_address(0)); \
  } while (false)
