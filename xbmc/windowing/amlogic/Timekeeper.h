/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// The timekeeper (design: docs/presentation_coordinator_design.md §2.1, §3).
// Phase 1 runs it in shadow: it keeps the timeline from the vblanks and a
// low-priority reporter compares it with today's clocks. Nothing reads the
// timeline to present yet. Enabled by special://profile/timekeeper_shadow.

#include "AudioFollower.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

class CTimekeeper
{
public:
  CTimekeeper(int masterFd, uint32_t crtcId);
  ~CTimekeeper();

  bool Start();
  void Stop();

private:
  // the time thread: vblanks in, timeline out; no lock, no log, no allocation
  // on the steady path
  void Run();
  void OnVblank(uint64_t kernelSeq, int64_t ns, int64_t now);
  void OnTimeout(int64_t now);
  void CheckRate(int64_t ns);
  void Publish(int64_t vblankNs, bool synthetic);
  bool ReadMode(uint64_t& num, uint64_t& den);
  bool ReadModeNow(uint64_t& num, uint64_t& den);
  void FindFracProperty();
  int FracPolicy();
  double NominalNs() const { return 1e9 * static_cast<double>(m_num) / static_cast<double>(m_den); }

  // the reporter: samples the boards once a second, logs every 10 s
  void Report();

  int m_masterFd;
  uint32_t m_crtcId;
  int m_fd = -1;
  uint32_t m_connectorId = 0;
  uint32_t m_fracPropId = 0;
  int m_wakeFd = -1;
  std::thread m_thread;
  std::thread m_reporter;
  CAudioFollower m_follower;
  std::atomic<bool> m_stop{false};
  std::mutex m_reportMutex;
  std::condition_variable m_reportCond;

  // time-thread state
  uint64_t m_num = 1001;
  uint64_t m_den = 24000;
  uint64_t m_epoch = 1;
  uint64_t m_tick = 0;
  uint64_t m_tickAtLastReal = 0;
  int64_t m_lastRealNs = 0;
  int64_t m_lastPublishedNs = 0;
  uint64_t m_lastKernelSeq = 0;
  bool m_pending = false;
  int64_t m_modeSerial = 0;
  int m_failedQueues = 0;
  int64_t m_queuedNs = 0;
  bool m_suspect = false;
  int m_validRun = 0;
  bool m_valid = true;
  int64_t m_epochStartNs = 0;
  int64_t m_lastOnGridNs = 0;
  int m_onGridRun = 0;
  uint64_t m_epochStartTick = 0;
  int64_t m_rateRefNs = 0;
  uint64_t m_rateRefTick = 0;

  // counters the reporter reads (written only by the time thread)
  std::atomic<int> m_rtResult{-1};     // 0 if SCHED_FIFO was granted, else errno
  std::atomic<uint64_t> m_skips{0};    // vblanks the kernel delivered more than one period apart
  std::atomic<uint64_t> m_synthetic{0};
  std::atomic<uint64_t> m_heldBack{0}; // real vblanks that would have stepped the counter back
  std::atomic<uint64_t> m_queueErrors{0};
  std::atomic<uint64_t> m_modeReads{0};
  std::atomic<int64_t> m_modeReadMaxUs{0};
  std::atomic<uint64_t> m_validEpoch{0};
  std::atomic<int64_t> m_validTicks{0};
  std::atomic<int64_t> m_validNs{0};
  std::atomic<uint64_t> m_modeOldNum{0}, m_modeOldDen{0};
  std::atomic<uint64_t> m_rateCorrections{0}; // epochs the panel's measured rate started
  static constexpr int WAKE_BINS = 401; // 50 µs bins to 20 ms, then overflow
  std::array<std::atomic<uint32_t>, WAKE_BINS> m_wake{};
  std::atomic<int64_t> m_wakeMaxUs{0};
};
