/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ThreadRole.h"

#include "utils/log.h"

#include <cstdint>
#include <cstring>

#include <dlfcn.h>
#include <unistd.h>

namespace OUTPUT
{
namespace
{
constexpr const char* ROLE_NAMES[ROLE_COUNT] = {"unknown", "main", "P", "V", "C",
                                                "RefClock", "G", "W", "A", "timekeeper"};
} // namespace

void SetThreadRole(const char* role)
{
  for (int i = 0; i < ROLE_COUNT; i++)
  {
    if (std::strcmp(ROLE_NAMES[i], role) == 0)
    {
      threadRole = i;
      CLog::Log(LOGINFO, "THREADROLE {} tid {}", role, gettid());
      return;
    }
  }
  CLog::Log(LOGERROR, "THREADROLE {} is not a role (tid {})", role, gettid());
}

int ResolveThreadRole()
{
  threadRole = gettid() == getpid() ? ROLE_MAIN : ROLE_UNKNOWN;
  return threadRole;
}

void LogMainCoupling(const SCouplingSite& site, int role, const void* caller)
{
  const char* file = std::strrchr(site.file, '/');
  file = file ? file + 1 : site.file;
  Dl_info info = {};
  if (!caller)
    CLog::Log(LOGINFO, "MAINCOUPLING {}@{}:{} role={} tid={}", site.what, file, site.line,
              ROLE_NAMES[role], gettid());
  else if (dladdr(caller, &info) && info.dli_fname)
    CLog::Log(LOGINFO, "MAINCOUPLING {}@{}:{} role={} tid={} caller={}+{:#x}", site.what, file,
              site.line, ROLE_NAMES[role], gettid(), info.dli_fname,
              reinterpret_cast<uintptr_t>(caller) - reinterpret_cast<uintptr_t>(info.dli_fbase));
  else
    CLog::Log(LOGINFO, "MAINCOUPLING {}@{}:{} role={} tid={} caller={}", site.what, file,
              site.line, ROLE_NAMES[role], gettid(), caller);
}

} // namespace OUTPUT
