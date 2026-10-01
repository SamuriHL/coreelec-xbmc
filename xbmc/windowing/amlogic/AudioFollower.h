/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The audio clock follower (design: docs/presentation_coordinator_design.md
// §15.16-15.17). Step 1 is log-only: it measures the sink's phase against the
// timeline and logs the PLL trim each control law would apply, writing nothing.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class CAudioFollower
{
public:
  ~CAudioFollower();

  void Start();
  void Stop();

private:
  void Run();

  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::mutex m_mutex;
  std::condition_variable m_cond;
};
