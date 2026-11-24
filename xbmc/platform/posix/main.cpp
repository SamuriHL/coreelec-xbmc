/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PlatformPosix.h"
#include "application/AppEnvironment.h"
#include "application/AppParamParser.h"
#include "platform/xbmc.h"

#if defined(TARGET_LINUX) || defined(TARGET_FREEBSD)
#include "platform/linux/AppParamParserLinux.h"
#endif

#ifdef TARGET_WEBOS
#include "platform/linux/AppParamParserWebOS.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <string>

#include <sys/resource.h>

namespace
{
extern "C" void XBMC_POSIX_HandleSignal(int sig)
{
  // Setting an atomic flag is one of the only useful things that is permitted by POSIX
  // in signal handlers
  CPlatformPosix::RequestQuit();
}
} // namespace

int main(int argc, char* argv[])
{
#if defined(_DEBUG)
  struct rlimit rlim;
  rlim.rlim_cur = rlim.rlim_max = RLIM_INFINITY;
  if (setrlimit(RLIMIT_CORE, &rlim) == -1)
    fprintf(stderr, "Failed to set core size limit (%s).\n", strerror(errno));
#endif

  // Set up global SIGINT/SIGHUP/SIGTERM handler
  struct sigaction signalHandler;
  std::memset(&signalHandler, 0, sizeof(signalHandler));
  signalHandler.sa_handler = &XBMC_POSIX_HandleSignal;
  signalHandler.sa_flags = SA_RESTART;
  sigaction(SIGINT, &signalHandler, nullptr);
  sigaction(SIGHUP, &signalHandler, nullptr);
  sigaction(SIGTERM, &signalHandler, nullptr);

#if defined(HAVE_LIBBLURAY)
  // libbluray's BD-J starts a JVM in this process. Without -Xrs the JVM takes SIGINT, SIGHUP
  // and SIGTERM over from the handler above, and on a stop calls exit() from its own thread:
  // the static destructors then run while the main thread still renders. Set before any thread
  // exists (setenv is not thread-safe); appended to a value the environment already gives
  {
    const char* options = std::getenv("JAVA_TOOL_OPTIONS");
    if (!options || !std::strstr(options, "-Xrs"))
    {
      const std::string value = options && *options ? std::string(options) + " -Xrs" : "-Xrs";
      setenv("JAVA_TOOL_OPTIONS", value.c_str(), 1);
    }
  }
#endif

  setlocale(LC_NUMERIC, "C");

#ifdef TARGET_WEBOS
  CAppParamParserWebOS appParamParser;
#elif defined(TARGET_LINUX) || defined(TARGET_FREEBSD)
  CAppParamParserLinux appParamParser;
#else
  CAppParamParser appParamParser;
#endif
  appParamParser.Parse(argv, argc);

  CAppEnvironment::SetUp(appParamParser.GetAppParams());
  int status = XBMC_Run(true);
  CAppEnvironment::TearDown();

  return status;
}
