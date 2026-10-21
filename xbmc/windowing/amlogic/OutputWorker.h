/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Phase 5 step 5.B1: W, the one writer of link and DV state (design §2 I3, §4.1, §5). Jobs run
// one at a time, highest lane first, never preempted inside a job. A caller either waits for its
// job (Run) or posts it (Post). special://profile/outputworker_off runs every job inline on its
// caller, which is today's code.
//
// The write accounting logs each (target, thread) pair that writes link or DV state the first
// time it is seen ("OUTPUT write"), so a write that does not go through W shows up by name.

namespace OUTPUT
{

enum class Lane
{
  DECODER = 0, // TeardownPre/Post, DvPrepare, DvApply, disc-session engage/release
  RESTORE,
  START,
  MIDPLAY,
  SETTINGS,
  COUNT,
};

class COutputWorker
{
public:
  static COutputWorker& Get();
  ~COutputWorker() { Stop(); }

  void Start();
  void Stop();

  //! true when jobs run on W (started, and not outputworker_off)
  bool Active() const;
  //! outputworker_off, read once (valid before Start)
  static bool SwitchedOff();
  bool OnWorker() const;

  //! Runs fn on W and waits for it. Inline when W is not active or when called on W. Logs an
  //! ERROR once the wait passes errorAfter. The caller never abandons a job it waits for, so fn
  //! may use the caller's locals. An exception fn throws reaches the caller, as it would inline.
  void Run(Lane lane,
           const char* name,
           std::function<void()> fn,
           uint64_t generation = 0,
           std::chrono::milliseconds errorAfter = std::chrono::milliseconds(5000));
  //! Queues fn on W and returns (inline when W is not active; queued behind the running job
  //! when called on W). fn must capture by value.
  void Post(Lane lane, const char* name, std::function<void()> fn, uint64_t generation = 0);

private:
  struct Result
  {
    bool done = false;
    std::exception_ptr error;
  };

  struct Job
  {
    const char* name;
    std::function<void()> fn;
    uint64_t generation;
    uint64_t id;
    std::shared_ptr<Result> result; // guarded by m_mutex; null for a posted job
  };

  COutputWorker() = default;
  void Process();

  mutable std::mutex m_mutex;
  std::condition_variable m_cond;
  std::condition_variable m_doneCond;
  std::array<std::deque<Job>, static_cast<size_t>(Lane::COUNT)> m_lanes;
  std::thread m_thread;
  std::thread::id m_workerId;
  bool m_running = false;
  bool m_stop = false;
  bool m_stopping = false;
  uint64_t m_nextId = 1;
  // test: special://profile/outputworker_stall holds "<job name> <ms>"; W sleeps before that job
  std::string m_stallJob;
  int m_stallMs = 0;
};

//! records a write of link or DV state; target is a sysfs path or "drm:<property>"
void NoteWrite(const std::string& target);
//! from here on, sysfs writes of link or DV state are recorded too
void StartAccounting();
void StopAccounting();

// 5.B2-1 test hooks: each special://profile/outputdrive_* file is read once, default off. The
// steps are design §4.2's ("1" park .. "8" display reset), at today's inline points.
//! outputdrive_stopat "<step> [ms]": posts a playback stop at that step, then waits <ms> there
void TestStopAt(const char* step);
//! outputdrive_commitfail "[n]": the n-th mode set after the boot set (default 1) fails once,
//! after its commit
bool TestCommitFail();
//! outputdrive_demuxblock "<ms>": a mode set's lost walk makes P's next demux read sleep <ms>
void ArmDemuxBlock();
void TestDemuxBlock();
//! a player open drops an arm left by an earlier set
void DisarmDemuxBlock();
//! outputdrive_cstall "<ms>": C withholds its ack of a mode set's park for <ms>
int TestCStallMs();
//! outputdrive_skip "<substep>[,<substep>]": true for a listed 5.B2 sub-step (bisect only)
bool OutputDriveSkip(const std::string& substep);

//! 5.B2: GLES init records whether the presentation coordinator started
void NoteCoordinatorStarted(bool started);
//! 5.B2's switch (outputdrive_off), read at the first call: on needs DisplayState, a
//! validated SoC and a started coordinator; off = today's code
bool OutputDriveOn();

} // namespace OUTPUT
