/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/log.h"

#include <unistd.h>

// Phase 5 step 5.V1: the role of a video-side thread (design I1), for MAINCOUPLING. Thread names
// are truncated to 15 characters, so the role is a tag set at thread start.

namespace OUTPUT
{

//! P, V, C, RefClock, G, W, A or timekeeper; null on any other thread
inline thread_local const char* threadRole = nullptr;

//! at thread start; logs the tid the role runs on
inline void SetThreadRole(const char* role)
{
  threadRole = role;
  CLog::Log(LOGINFO, "THREADROLE {} tid {}", role, gettid());
}

} // namespace OUTPUT
