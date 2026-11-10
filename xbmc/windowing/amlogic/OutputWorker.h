/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
  //! 5.B3-7a (O4): as Post, but dropped, and false, when W is not active (stopped, stopping or
  //! not yet started): never inline on the caller. fn must capture by value.
  bool PostIfRunning(Lane lane, const char* name, std::function<void()> fn, uint64_t generation = 0);
  //! Queues fn on W once after has passed (W's clock, 5.B2-6). Dropped, and false, when W is
  //! not active; a stop drops the ones not yet due. fn must capture by value.
  bool PostAfter(Lane lane,
                 const char* name,
                 std::chrono::milliseconds after,
                 std::function<void()> fn);
  //! jobs W has started (5.B2-5a: a shadow's reads raced one)
  uint64_t JobsStarted() const { return m_jobsStarted.load(); }

  // 5.E2-3: the display word of design §4.7 as a shadow (docs/phase5_e2_plan.md E2-3). One packed
  // 64-bit word {state 4, sub 1, T 24, F 10, e 24, spanOpen 1}, CASed at today's transitions and
  // logged "WORD <old> -> <new> (row <n>)". A lost race re-reads and re-decides ("WORD race",
  // at most 4 tries, then "WORD CAS failed"); a hook whose row does not hold for the word it reads
  // logs "WORD skip" with HEAD's loss state beside it. Nothing reads the word for behaviour.
  // Every caller tests aml_transaction_shadow_on() first.
  enum class WordState : uint8_t
  {
    NONE = 0,
    STARTING,
    LEADING,
    FREEZING,
    LOST,
    CONVERTING,
    PAUSED,
    RESET_OWED,
    RETURNING,
  };
  struct SWord
  {
    WordState state = WordState::NONE;
    bool sub = false; //!< FREEZING: parked (else claimed); any other state: W's display hold
    uint32_t t = 0; //!< transaction id, 24 bits
    uint32_t f = 0; //!< file serial, 10 bits, 0 = no player
    uint32_t e = 0; //!< output epoch, 24 bits
    bool spanOpen = false;
  };
  //! one decision on the word read: the next word and its row, a precondition miss, or nothing
  struct SWordStep
  {
    enum class Kind
    {
      NOTHING,
      APPLY,
      SKIP,
    } kind = Kind::NOTHING;
    SWord next;
    const char* row = "";
    const char* note = nullptr;
    std::string expected; //!< SKIP: the word the row needs
  };
  //! a P hook's HEAD: its m_lossState now, and whether HEAD already left the state this hook
  //! mirrors (a skip is then explained)
  struct SWordHead
  {
    const char* name = nullptr;
    bool moved = false;
  };
  static uint64_t PackWord(const SWord& word);
  static SWord UnpackWord(uint64_t value);
  static std::string WordText(uint64_t value);
  uint64_t LoadWord() const { return m_word.load(std::memory_order_acquire); }
  //! W, a loss it detects (e+1, the span set): walk = the span's lost walk follows (row 11's
  //! claim for an open player), else a loss inside an open span (the re-stamp)
  void WordLoss(bool walk);
  //! W, after the span's lost walk: a claim no player parked is released (row 13)
  void WordLossWalked();
  //! W, a mode set's commit (e+1)
  void WordCommitted(bool notConnected);
  //! W, the lost span's close: the span bit, when no player's return cleared it
  void WordSpanClosed();
  //! P, a file's open (today's start: row 29, or 28+29 for a next file); mirror is the player's
  //! own loss state. The new T, or 0 when the CAS failed
  uint32_t WordOpen(uint32_t ownT, WordState mirror);
  //! the FREEZE body begins (OnLostDisplay past its early returns); newcomer: on P (row 29)
  void WordFreezeBody(uint32_t ownT, bool newcomer, const SWordHead& head);
  //! the FREEZE body done, the player's state LOST
  void WordFrozen(uint32_t ownT, bool newcomer, const SWordHead& head);
  //! P's conversion to a pause (row 18's two steps)
  void WordConverting(uint32_t ownT, const SWordHead& head);
  void WordPaused(uint32_t ownT, const SWordHead& head);
  //! the display reset: the player's state RETURNING (rows 16, 20, 21+22, chosen from HEAD's
  //! state after the spin, headFrom), then NONE (row 23)
  void WordReturning(uint32_t ownT, WordState headFrom, const SWordHead& head);
  void WordReturned(uint32_t ownT, const SWordHead& head);
  //! P's stop (row 28)
  void WordStop(uint32_t ownT);

private:
  //! read, decide, CAS, log; a lost race re-reads and re-decides (bounded). True when applied
  bool WordApply(const std::function<SWordStep(const SWord&)>& decide,
                 const SWordHead* head = nullptr);

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

  struct TimedJob
  {
    std::chrono::steady_clock::time_point due;
    Lane lane;
    Job job;
  };

  COutputWorker() = default;
  void Process();
  //! moves the due timed jobs into their lanes (m_mutex held); the earliest left, if any
  std::optional<std::chrono::steady_clock::time_point> ReleaseDueLocked();

  mutable std::mutex m_mutex;
  std::condition_variable m_cond;
  std::condition_variable m_doneCond;
  std::array<std::deque<Job>, static_cast<size_t>(Lane::COUNT)> m_lanes;
  std::vector<TimedJob> m_timed;
  std::thread m_thread;
  std::thread::id m_workerId;
  bool m_running = false;
  bool m_stop = false;
  bool m_stopping = false;
  uint64_t m_nextId = 1;
  std::atomic<uint64_t> m_jobsStarted{0};
  // 5.E2-3: the display word (shadow) and its counters
  std::atomic<uint64_t> m_word{0};
  std::atomic<uint32_t> m_wordT{0};
  std::atomic<uint32_t> m_wordF{0};
  // test: special://profile/outputworker_stall holds "<job name> <ms>"; W sleeps before that job
  std::string m_stallJob;
  int m_stallMs = 0;
};

//! records a write of link or DV state; target is a sysfs path or "drm:<property>"
void NoteWrite(const std::string& target);
//! from here on, sysfs writes of link or DV state are recorded too
void StartAccounting();
void StopAccounting();

//! the first line of special://profile/<name>, or nullopt without the file
std::optional<std::string> ReadTestSwitch(const char* name);
//! a "<substep>[,<substep>]" bisect file (outputdrive_skip, hotplugworker_skip, vconfigure_skip),
//! logged by tag
std::vector<std::string> ReadSkipList(const char* name, const char* tag);

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
//! outputdrive_cstall "<ms> [n]": C withholds its ack of a mode set's park for <ms>; with n, only
//! of the n-th mode set's park after the boot set (5.B2-5b: a start whose retry is not stalled)
int TestCStallMs();
int TestCStallNth();
//! outputdrive_order "today": 5.B2-5c's A/B, W's mode sets keep B2-5b's order (the lost walk
//! after the planes off)
bool TestOrderToday();
//! outputdrive_skip "<substep>[,<substep>]": true for a listed 5.B2 sub-step (bisect only)
bool OutputDriveSkip(const std::string& substep);

//! 5.B2: GLES init records whether the presentation coordinator started
void NoteCoordinatorStarted(bool started);
//! 5.B2's switch (outputdrive_off), read at the first call: on needs DisplayState, a
//! validated SoC and a started coordinator; off = today's code
bool OutputDriveOn();

// 5.B2-5b: W runs main's mode sets (design §4.2, §4.5, §4.6)
//! the win system latches it once, when W starts after the boot set (OutputDriveOn, no
//! outputdrive_skip "B2-5b", and B2-3 and B2-4 on); false before that and under off
void NoteModeSetsOnW(bool on);
bool ModeSetsOnW();
//! 5.B2-6: SETTLE (the delayed display reset) on W's clock, and ActiveAE's device-change replay:
//! ModeSetsOnW and no outputdrive_skip "B2-6". Not latched (ModeSetsOnW is false until W starts);
//! the switch line is logged once
bool SettleOnW();
//! 5.V2-10: SETTLE tests W's own facts (W's committed reply, W's connected read) instead of main's
//! handler flags, and is armed on W at delay 0 too (Q12, Q13): SettleOnW, HotplugOnW, vconfigure
//! on and no vconfigure_skip "V2-10"; the switch line is logged once
bool SettleGatesOnW();
//! 5.B2-5c: the lost walk runs right after W's park (ModeSetsOnW, no outputdrive_skip "B2-5c",
//! no outputdrive_order "today")
bool LostFirstOn();
//! 5.B2-7: C runs the blocking plane-off commits (G's and S1''s), and no thread but W in its
//! park takes a presenter hold: ModeSetsOnW and no outputdrive_skip "B2-7"
bool PlaneOffOnC();

//! the reply W gave a request of main's (design §4.5)
enum class DriveReply
{
  NONE = 0,
  DONE,
  DONE_NO_CHANGE,
  NOT_COMMITTED,
  FAILED,
  FAILED_PARK, //!< not given from 5.B2-5b's park retry on (kept for RM's test)
  GAVE_UP, //!< main stopped waiting at 15 s: the outcome is unknown, not a failure
};
const char* DriveReplyName(DriveReply reply);
//! main: the reply of its last request, numbered (the give-up counts as FAILED)
void NoteDriveReply(DriveReply reply);
//! 5.B3-7a (O2's one id space, unswitched): the next mode-set request id, from 1; main's
//! requests take it (today's only poster, so today's sequence)
uint64_t NextDriveId();
DriveReply LastDriveReply(uint64_t& seq);
//! main: a request built inside this scope is the render manager's (origin=rm, r1 M4); startHeld:
//! the player holds a start for this decision (5.B2-6: a film start, Q6)
class CDriveOriginRm
{
public:
  explicit CDriveOriginRm(bool startHeld = false);
  ~CDriveOriginRm();
  CDriveOriginRm(const CDriveOriginRm&) = delete;
  CDriveOriginRm& operator=(const CDriveOriginRm&) = delete;

private:
  bool m_startHeld;
};
bool DriveOriginRm();
//! the request is RM's for a start the player holds (5.B2-6, Q6)
bool DriveFilmStart();
//! main: the RESOLUTION GC is setting, for a late reply's GC update (r1 L11)
void NoteDriveResolution(int res);
int DriveResolution();

// 5.B3: the hotplug path on W (design §6)
//! the win system latches the switches once, at W's start, after NoteModeSetsOnW, and logs
//! "HOTPLUGWORKER on|off (<reason>) hpdRestore N|Y|absent|unreadable hpdCb present|absent".
//! hotplugworker = ModeSetsOnW and no hotplugworker_off; hpdnorestore (B3-10's N write) =
//! hotplugworker, no hpdnorestore_off, no hotplugworker_nohpdcb and the parameter readable. Both
//! false before the latch and under off; HpdNoRestoreOn implies HotplugWorkerOn
void LatchHotplugWorker();
bool HotplugWorkerOn();
bool HpdNoRestoreOn();
//! hotplugworker_skip "<substep>[,<substep>]": true for a listed 5.B3 sub-step (bisect only)
bool HotplugWorkerSkip(const std::string& substep);
//! 5.B3-6: W's hotplug detection as a log-only shadow: HotplugWorkerOn and no
//! hotplugworker_skip "B3-6"; the switch line is logged once (read first at W's start)
bool DetectShadowOn();
//! 5.B3-7a: the ordering rules (O3: the lost generation re-checked by every reset inside
//! m_resourceSection; O4: the FD thread's post only to a running W): HotplugWorkerOn and no
//! hotplugworker_skip "B3-7a"; the switch line is logged once (read first at W's start)
bool OrderingOn();
//! 5.B3-7b: W runs the hotplug (the read, the Lost fan-out, the re-init, the caps and the Restore)
//! and main applies its replies: OrderingOn and no hotplugworker_skip "B3-7b"; the switch line
//! is logged once (read first at W's start)
bool HotplugOnW();
//! 5.B3-8: W's Restore takes §6 row 2's target (the video's mode as recorded, else §4.7's
//! no-player target: the GUI resolution, desktop as W's desktopMode, else the connector's
//! PREFERRED mode): HotplugOnW and no hotplugworker_skip "B3-8"; the switch line is logged once
//! (read first at W's start)
bool RestoreRow2On();
//! 5.B3-9a: W's Restore routes an unrouted connector the HPD read has settled (the dark branch)
//! = HotplugOnW() and no hotplugworker_skip "B3-9a"
bool DarkRoutingOn();
//! 5.B3-9b: W's Restore sends no forced UPDATE after its own mode commit on a lit link
//! = DarkRoutingOn() and no hotplugworker_skip "B3-9b"
bool LinkRequestOn();
//! 5.B3-10a: C latches a graphics refusal only when its retries, each after a GUI-only commit
//! that succeeded, are refused three times = LinkRequestOn() and no hotplugworker_skip
//! "B3-10a". RefusalConfirmLive is its latched value for C, false until the latch block reads it
bool RefusalConfirmOn();
bool RefusalConfirmLive();
//! 5.B3-9c: a reconnect that has no live mode gives a placeholder RES_DESKTOP ("dummy_l")
//! desktopMode's row = RefusalConfirmOn() and no hotplugworker_skip "B3-9c"
bool DesktopSlotOn();
//! 5.B3-10c: W writes hpd_restore N at its start (a plug-in stays dark for W's Restore), Y at
//! exit = DesktopSlotOn(), HpdNoRestoreOn() and no hotplugworker_skip "B3-10b" or "B3-10c"
bool HpdNoRestoreWriteOn();
//! W, at its start under HpdNoRestoreWriteOn: N, unless the exit has begun
void WriteHpdNoRestore();
//! W, at every hotplug event: ERROR once when the N it wrote reads otherwise (M8)
void CheckHpdNoRestore();
//! main, before W's stop: Y if W wrote N; no N write after it
void ExitHpdNoRestore();
//! 5.B3-10d: W records the video's mode only in a hotplug job that acts on its edge (no echo,
//! invalid read, wait or disconnected retry) = HpdNoRestoreWriteOn() and no hotplugworker_skip
//! "B3-10d"
bool VideoRecordOnActOn();
//! the win system notes it once, at main's first pump after W's start under HotplugOnW, when W's
//! hotplug job takes over; false before (the boot set's seed and caps still read main's table
//! on W, main waiting)
void NoteHotplugJobsOnW();
bool HotplugJobsOnW();

// 5.B3-1 test hooks: each special://profile/hotplugworker_* file is read once, default off
//! hotplugworker_hold "<ms>": in each hotplug job (B3-7b), or each B3-6 shadow job, W sleeps <ms>
//! between the job's receipt count and its HPD read (0: off). An edge inside the hold is read by
//! this job, and its receipt posts the next one
int TestHotplugHoldMs();
//! hotplugworker_gfxrefuse "<n>": G's graphics submits from its nth on (counted from G's start,
//! its boot submit included) carry FB_ID 0, which the kernel refuses on a lit CRTC; 0 = off
int TestGraphicsRefuseAt();
//! hotplugworker_holdhpd: a hotplug job that starts while hpd_state reads 0 spins (1 ms, at most
//! 30 s) until it reads 1, so the plug-out's read lands in the plug-in's hpd-to-callback window
bool TestHotplugHoldHpd();
//! hotplugworker_retry "<ms>": the P5-9 timer's first, shortest period (0: today's)
int TestHotplugRetryMs();
//! hotplugworker_capsfail "<n>": called once per reconnect's caps refresh; true at the n-th,
//! which then reports a one-mode list
bool TestHotplugCapsFail();
//! reads hotplugworker_capsfail's file without counting a refresh
void PrimeTestHotplugCapsFail();
//! hotplugworker_reinit "<period ms> <seconds>": an injected re-init job on W every period for
//! that long ({0, 0}: off)
std::pair<int, int> TestHotplugReinit();
//! hotplugworker_nohpdcb: W behaves as on a kernel without hpd_cb_state/hpd_cb_seq
bool TestHotplugNoHpdCb();
//! hotplugworker_racegap "<ms>": W sleeps between clearing jobQueued and reading receiptSeq
int TestHotplugRaceGapMs();
//! hotplugworker_settlegap "<ms>": under OrderingOn, W sleeps between SETTLE's outside reset
//! test and its lock, widening O3's race window for the B3-7a gate (0: off; 1-1000 ms, a
//! larger value is clamped). Primed at W's start under OrderingOn: no reset probes the file
int TestHotplugSettleGapMs();
//! vconfigure_handlerstall "<ms>" (test only, 5.V2-10's gate): main sleeps before the handler of
//! every done or not-committed set, holding SETTLE's main gates (0: off)
int TestHandlerStallMs();
//! hotplugworker_applythrow "<n> [times] [table|adopt]": called once per reply apply; true for
//! the n-th apply and the times-1 after it (times default 1): main throws in that apply, after its
//! table step, or inside it (table), or after the adopt's GC set (adopt)
bool TestHotplugApplyThrow();
//! where TestHotplugApplyThrow's apply throws: "after table" (the default), "table" or "adopt"
const char* TestHotplugApplyThrowAt();
//! hotplugworker_setafter "<ms>": a plug-in's receipt also posts, <ms> later, a settings
//! resolution change from main (-1: off)
int TestHotplugSetAfterMs();

} // namespace OUTPUT
