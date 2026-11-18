/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "rendering/RenderSystemTypes.h"
#include "utils/AMLUtils.h"
#include "utils/HDRCapabilities.h"
#include "windowing/Resolution.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class CAMLDisplay;

// Phase 5 step 5.V1-2: what the output is, published by W (design I3) after each commit and
// caps refresh. The caps readers take its values (5.V1-3); the others still read today's
// source. The shadows count "DISPLAYSTATE <field> match n diff m skipped k ..." against today's
// read. displaystate_off: nothing is published, compared or logged.

namespace DISPLAYSTATE
{

//! 5.B3-3: a mode by its facts, never by an index into the resolution table
struct ModeFacts
{
  std::string strId; //!< DisplayState's desktopMode: the DRM mode string
  int screenWidth = 0;
  int screenHeight = 0;
  float refresh = 0.0f;
  uint32_t dwFlags = 0;
  bool operator==(const ModeFacts& other) const
  {
    return strId == other.strId && screenWidth == other.screenWidth &&
           screenHeight == other.screenHeight && refresh == other.refresh &&
           dwFlags == other.dwFlags;
  }
  bool operator!=(const ModeFacts& other) const { return !(*this == other); }
};
ModeFacts FactsOf(const RESOLUTION_INFO& info);
//! "<strId> <w>x<h> @ <hz> flags <f>", for the logs
std::string FactsString(const ModeFacts& facts);

struct DisplayState
{
  uint64_t epoch = 0;
  bool connected = false;
  std::string modeString;
  // the committed mode; refresh is the one the display runs
  float refresh = 0.0f;
  int screenWidth = 0;
  int screenHeight = 0;
  uint32_t dwFlags = 0;
  std::string strId;
  float fps = 60.0f; //!< CGraphicContext::GetFPS's rule
  //! the set kept the live mode string, so the display may run the other 1000/1001 rate of it
  bool fracHeld = false;
  float requestedFps = 60.0f; //!< the set's request, by GetFPS's rule
  bool windowed = false;
  int drmFd = -1;
  uint32_t crtcId = 0;
  CHDRCapabilities hdrCaps;
  bool support3d = false;
  bool hdrPq = false;
  bool hdrHlg = false;
  bool hdr10Plus = false;
  bool has2160 = false; //!< a table entry with iScreenHeight >= 2160
  uint64_t capsEpoch = 0;
  //! 5.E2-11 D2.5: the sink's signature with these caps (DV format, its TV-led and player-led
  //! dv_cap bits, HDR10/HDR10+/HLG, the device's DV), stamped on a Start job at DECIDE with capsEpoch from the same snapshot
  uint32_t sinkSig = 0;
  // 5.B3-3 (design §3): the GUI plane as BuildFlipRequest programs it, read at each publish
  // under the DRM lock, only under hotplugworker (0 otherwise); log-only ("DISPLAYSTATE ui-ids")
  // until B3-4's readers take them
  uint32_t primaryPlaneId = 0;
  int uiSrcW = 0; //!< the mode set's m_width/m_height
  int uiSrcH = 0;
  int uiScreenW = 0; //!< m_ScreenWidth/m_ScreenHeight (the stereo rule of the mode set)
  int uiScreenH = 0;
  //! G's graphics plane state at the publish, for the logs only. G sets and clears
  //! m_overlayActive with its own flips, between publishes, so this copy goes stale: B3-4 and
  //! later must not read it (a G-side publisher first)
  bool overlayActive = false;
  //! W's no-player Restore target for "desktop" (B3-8): the session's desktop mode, or the
  //! PREFERRED mode on a table without it, or the last one kept when the table has neither; empty
  //! before W's first caps publish under B3-8
  ModeFacts desktopMode;
  //! 5.V3-3: the GUI row W set (its res), unread with the 5.V3 latch off
  struct WRow
  {
    int width = 0; //!< iWidth
    int height = 0;
    int overscanLeft = 0;
    int overscanTop = 0;
    int overscanRight = 0;
    int overscanBottom = 0;
    float pixelRatio = 1.0f;
    int blanking = 0;
    int subtitles = 0;
    float refreshRate = 0.0f; //!< the res's fRefreshRate (refresh is the link's committed one)
  } wRow;
  //! 5.V3-3: the seq of the mode publish this state carries; 0 before the first latched one
  uint64_t modeSeq = 0;
  //! set only by 5.E2's deferred Restore; always false in 5.B3
  bool linkUncommitted = false;
  //! W's last HPD read (from B3-6); not read before
  HpdRead hpd;
};

//! null before the seed and under displaystate_off
std::shared_ptr<const DisplayState> Get();
//! 5.B3-4: the DRM-object readers' source under hotplugworker: the last published state
//! (immutable, never freed while held), whatever a caps reader's skip cause. Null without
//! hotplugworker (latched at W's start) or before the first publish: the reader keeps HEAD's
//! live read
std::shared_ptr<const DisplayState> LinkState();

// W's publishes (or inline on their caller while W is not running)
//! returns the publish's modeSeq (5.V3-3), 0 with the 5.V3 latch off
uint64_t PublishMode(const CAMLDisplay& display, const RESOLUTION_INFO& res, bool windowed);
//! has2160: the caller's value (5.B3-7b: W's own table, or main's read of its table); without it
//! the publish reads CDisplaySettings. desktopMode (5.B3-8, W's caps step): stored with these
//! caps and kept by every later publish; null keeps the published one
void PublishCaps(const CAMLDisplay& display,
                 std::optional<bool> has2160 = std::nullopt,
                 const ModeFacts* desktopMode = nullptr);
//! res null: no committed mode yet
void PublishSeed(const CAMLDisplay& display, const RESOLUTION_INFO* res, bool windowed);

//! main, before a write of the caps, the DRM ids or the table; the next caps publish ends it
void CapsWriteBegins();
//! a mode set's commit: W's job after PublishMode under 5.B2-3, else main after the job
void NoteCommit();
uint64_t CommitCount();

// 5.B2-3 (I5): the timing readers take the committed mode
//! 5.B2-3's switch: OUTPUT::OutputDriveOn() and no outputdrive_skip "B2-3", read once
bool CommitTimingOn();
struct ModeTiming
{
  float fps = 60.0f; //!< CGraphicContext::GetFPS's rule, of the committed refresh
  int screenHeight = 0;
  bool windowed = false;
  //! a frac-held set, or a failure after the commit: the latency tweak keeps today's bucket (the
  //! set's target, GC's), so the tweak that landed the audio stays
  bool todaysTweak = false;
};
//! the last publish's mode, one lock-free word (the coordinator reads it); false before the
//! first publish of a mode and while disconnected, with R10's log line when a field is named
bool CommittedTiming(ModeTiming& timing, const char* field = nullptr);
//! W's commit published and GC went back to its old row without a new set (5.V2-1's VTIMING
//! shadow logs it)
bool Restored();
//! CGraphicContext::SetVideoResolutionInternal: before m_Resolution changes, and once switched
//! is known
void GcSetBegins();
void GcSetEnds(bool switched);
//! 5.B3-7b (O1): GC's set (SetVideoResolutionInternal) on main's stack, unwound by a throw too; a
//! hotplug reply is not applied inside one
class CGcSetScope
{
public:
  CGcSetScope();
  ~CGcSetScope();
  CGcSetScope(const CGcSetScope&) = delete;
  CGcSetScope& operator=(const CGcSetScope&) = delete;
};
//! main: the GC sets on its stack
int GcSetDepth();

// shadows of main's reads (render manager): only when GC's committed mode (strId + refresh) is
// DisplayState's, else a skip; committed: a skip after a commit is a FAIL
void ShadowMainMode(const char* fpsField,
                    const char* heightField,
                    const RESOLUTION_INFO& gcRes,
                    float gcFps,
                    bool committed = false);
void ShadowMainWindowed(const char* field, const RESOLUTION_INFO& gcRes, bool windowed);

// shadows at readers off main: a difference from GC:455 to the publish, or after a failure
// after the commit until the next publish, counts in its window; one where GC's fps is the
// other 1000/1001 rate of a held mode counts as frac-held
struct ModeShadow
{
  std::shared_ptr<const DisplayState> state; //!< null: skipped
  bool inSet = false;
  bool restored = false;
};
ModeShadow BeginModeShadow();
//! GC's fps (today's read) differs from DisplayState's by 1000/1001 at a held mode
bool FracHeld(const ModeShadow& shadow, float todayFps);
void CountMode(const ModeShadow& shadow,
               const char* field,
               double today,
               double displayState,
               bool fracHeld = false);

//! 5.B3-3, at each GUI-plane flip build (BuildFlipRequest), with its own reads: counted as
//! "DISPLAYSTATE ui-ids" against the published fields, only under hotplugworker. Called under
//! the DRM lock, except from the legacy FlipPage (not used while the coordinator runs); it
//! takes only DisplayState's leaf locks
void ShadowUiIds(uint32_t planeId, uint32_t crtcId, int srcW, int srcH, int screenW, int screenH);

// shadows of the caps fields at their accessors
void ShadowHdrCaps(const CHDRCapabilities& today);
void ShadowDv(bool today);
void Shadow3d(bool today);

// 5.V1-3: the caps readers' values. Under on, the published value: the one the shadow compared
// with today's read (the accessor's shadow, run on this thread for the value passed in, or
// the shadow here); today's read before W's first publish, while disconnected, while main
// writes the caps or after a failed caps read, logged once per cause and publish. Under off,
// today's read.
CHDRCapabilities HdrCaps(const CHDRCapabilities& today);
//! CWinSystemAmlogic::IsHDRDisplay's rule
bool HdrDisplay(bool today);
//! 5.V2-4: the same rule on the last published caps, for the video side (a pending or failed
//! caps read keeps them; none published: false). Compares nothing
bool PublishedHdrDisplay();
bool Dv(bool today);
bool Support3d(bool today);
bool HdrPq(bool today);
bool HdrHlg(bool today);
bool Hdr10Plus(bool today);
bool Has2160(bool today);
//! 5.V1-6 (MC-46): the DRM fd and CRTC id, by the same rules
void DrmIds(int& fd, uint32_t& crtc);

// 5.V1-8 (S1): what main's GUI state gives the video side, published by main
struct GuiInputs
{
  GuiInputs() = default;
  //! 5.V1-8's publish of the stereo mode alone ({mode})
  GuiInputs(RenderStereoMode mode) : stereoMode(mode) {}

  RenderStereoMode stereoMode = RenderStereoMode::OFF;
  // 5.B3-3: W's hotplug inputs (B3-7b's field table), published only under hotplugworker
  //! GetCurrentResolution()'s facts; nullopt: desktop (not above RES_DESKTOP)
  std::optional<ModeFacts> guiResolution;
  bool nativeGui = false; //!< DISABLEGUISCALING
  bool videoPlaying = false; //!< IsPlayingVideo()
  ModeFacts videoResolution; //!< the facts of GC's GetVideoResolution() entry
  bool published = false; //!< false before W's start publish and under off
};
//! the stereo mode only (5.V1-8)
void PublishGuiInputs(const GuiInputs& inputs);
//! the published stereo mode, counted against today's GC read; today's read before main's
//! first publish and under displaystate_off
RenderStereoMode GuiStereoMode(RenderStereoMode today);
// 5.B3-3, each a no-op without hotplugworker (OUTPUT::HotplugWorkerOn), logged "GUIINPUTS ..."
//! at W's start, after the hotplug latch: every field
void PublishGuiInputsAtStart(bool nativeGui);
//! CDisplaySettings::SetCurrentResolution (any thread): guiResolution from the current
//! resolution
void PublishGuiResolution();
//! the DISABLEGUISCALING branch, before the table is rebuilt (OnSettingChanging, the incoming
//! value), and OnSettingChanged (the committed one); any thread
void PublishNativeGui(bool nativeGui);
//! main, MessagePump: videoPlaying and videoResolution, published when they change
void PublishGuiPlayback();
//! the published inputs (W's hotplug job, from B3-7b); published false before the start publish
GuiInputs GetGuiInputs();

// 5.E2-5 (shadow): DECIDE's GUI-side inputs, by value: main publishes them, P's ResolveOutput
// reads them (docs/phase5_e2_plan.md E2-5). Published under aml_displaystate_on() and
// aml_transaction_shadow_on() only
struct DecideInputs
{
  ResolutionInputs res; //!< ChooseBestResolution's inputs
  bool fullScreenVideo = false;
  bool fullScreenRoot = false;
  bool calibrating = false;
  RenderStereoMode userStereo = RenderStereoMode::UNDEFINED; //!< GetStereoModeByUser
  uint64_t seq = 0; //!< 0: never published
  std::chrono::steady_clock::time_point at;
};
//! the publishes' switch: displaystate, the transaction shadow and a validated SoC (where CN runs)
bool DecideShadowOn();
//! main only: takes the inputs; publishes (and logs "DECIDEINPUTS seq=") when they changed, or
//! always with force. open: a player's open; main's pump re-publishes until P has read them
void PublishDecideInputs(const char* why, bool force = false, bool open = false);
//! main's pump: PublishDecideInputs at most every 250 ms, only between a player's open and P's
//! read (E2-5 review M2)
void PublishDecideInputsTick();
//! P: the published inputs; taken ends the pump's re-publishes for this open
DecideInputs GetDecideInputs(bool taken = false);
//! 5.E2-10 (D1 item 6): P, a reopen or the 3D answer: the pump runs again, its next publish forced
void RequestDecideInputs();

//! 5.V2-6: GC's m_bFullScreenRoot, published at its writes (any thread), for the player's open
void PublishFullScreenRoot(bool root);
bool FullScreenRoot();
//! 5.E2-8b D4: WINDOW_FULLSCREEN_VIDEO initialised (main, at its INIT / DEINIT). The window it
//! replaced has deinitialised by then, its way-out set counted in GuiSetsInFlight
void SetFullScreenVideoWindowActive(bool active);
bool FullScreenVideoWindowActive();

// 5.E2-8b: the engaged start's Start job (docs/phase5_e2_8b_impl.md D2, D4 step 7, D5, D9)
enum class StartJobState : uint8_t
{
  NOT_POSTED,
  QUEUED,
  PARKING,
  COMMITTED,
  DROPPED,
};
const char* StartJobStateName(StartJobState state);
//! W, at the commit result (committed) or the no-change test (no-change); seq = s_commitSeq
struct SOutputDone
{
  uint64_t T = 0;
  bool committed = false;
  uint64_t seq = 0;
};
//! W: the job's one final reply (D5 step 5)
enum class StartSettleKind
{
  DONE,
  NO_CHANGE,
  NOT_ARMED,
  SUPERSEDED,
  CANCELLED,
  STOPPING,
  FAILED,
  FAILED_PARK,
  DROPPED,
};
const char* StartSettleKindName(StartSettleKind kind);
struct SSettleDone
{
  uint64_t T = 0;
  uint32_t e = 0;
  StartSettleKind kind = StartSettleKind::DONE;
};
//! main's answer to RM (RM::TxAdoptAnswer's form, mapped one to one by the sink)
enum class StartAnswer
{
  ADOPTED,
  BEHIND,
  BEHIND_GUI,
  FAILED,
  FAILED_SET, //!< the job's own set failed (kind FAILED/FAILED_PARK)
  RECOVERING,
  RECOVERED,
};
//! main's adopt, to P after the answer (and after the answer's PLAYER_AVCHANGE put)
enum class StartAdoptForm
{
  ADOPTED,
  BEHIND,
  BEHIND_GUI,
  FAILED,
  RECOVERING,
};
const char* StartAdoptFormName(StartAdoptForm form);
struct STxAdopted
{
  uint64_t T = 0;
  StartAdoptForm form = StartAdoptForm::FAILED;
};
//! P's receiver, registered on the job at the post and cleared at the player's exit. Each call
//! is made under the job's sink mutex: it must only queue (no wait, no lock that P holds while
//! clearing the sink)
class IStartJobSink
{
public:
  virtual ~IStartJobSink() = default;
  virtual void OnOutputDone(const SOutputDone& reply) = 0; //!< W
  virtual void OnSettleDone(const SSettleDone& reply) = 0; //!< W
  //! main, after GC's set returned and outside the gfx lock: RM::TxAdoptAnswer(gen, answer);
  //! true when RM posted VideoParamsChange
  virtual bool OnAdoptAnswer(unsigned int gen, StartAnswer answer) = 0;
  virtual void OnTxAdopted(const STxAdopted& reply) = 0; //!< main, after OnAdoptAnswer
  //! 5.E2-11 D3.2: W or main, a Restore inside STARTING settled: START's CAS expects e
  virtual void OnRestoreSettled(uint64_t T, uint32_t e) = 0;
};
struct SStartJob
{
  std::atomic<StartJobState> state{StartJobState::NOT_POSTED};
  uint64_t T = 0;
  unsigned int gen = 0;
  // the request, by value (D5)
  RESOLUTION_INFO res;
  int resIndex = -1;
  RenderStereoMode stereo = RenderStereoMode::OFF;
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  bool fullScreen = true;
  bool fracRequest = false;
  bool filmStart = true;
  double tweakMs = 0.0;
  //! W: the job's mode set ran (false: dropped, or discarded unrun at W's stop: answer failed)
  std::atomic_bool ran{false};
  //! W sets it when the job runs; main's answer (any form) clears it
  std::atomic_bool adoptPending{false};
  //! 5.E2-11 D0: P, under the player guard right before START's word CAS; W reads it only for
  //! a word at NONE (the lead's first instant against PRIME..SETTLE)
  std::atomic_bool started{false};
  //! D4: P, the 3D question's dialog open (TxAskPass's predicate); W does not defer then
  std::atomic_bool askOpen{false};
  //! D2.5: P at DECIDE, before the queue: the caps publish and W's sink signature it decided on
  uint64_t capsEpoch = 0;
  uint32_t sinkSig = 0;
  //! D3.3: W, a Restore committed inside this start: (count << 32) | the word's e after it
  std::atomic<uint64_t> restoreCommit{0};

  //! one CAS from any of from to to; the state found is left in found
  bool Cas(std::initializer_list<StartJobState> from, StartJobState to, StartJobState* found = nullptr);
  void SetSink(IStartJobSink* sink);
  void ClearSink();
  //! 5.E2-11 gate 2: false once the player's exit cleared the sink (TxReleaseJob)
  bool HasSink();
  // the replies, logged with D10's fixed strings and given to the sink under its mutex
  void OutputDone(bool committed, uint64_t seq);
  //! once per job; a later call is logged only
  void SettleDone(StartSettleKind kind);
  bool AdoptAnswer(StartAnswer answer);
  void TxAdopted(StartAdoptForm form);
  //! 5.E2-11 D3.2: RESTORED{T, e}, once per Restore settled inside STARTING
  void RestoreSettled(uint32_t e);

private:
  std::mutex m_sinkMutex;
  IStartJobSink* m_sink = nullptr;
  std::atomic_bool m_settleReplied{false};
};
//! P, the D4 step 7 Dekker step: NOT_POSTED -> QUEUED, and back while a GUI set is in flight
bool StartJobQueue(SStartJob& job);
bool StartJobUnqueue(SStartJob& job);
//! GC, a GUI set while the job is open (D9): QUEUED|PARKING -> DROPPED; false at any other state
bool StartJobDropForGuiSet(SStartJob& job, const char* by);
//! P's exit (D8): NOT_POSTED|QUEUED|PARKING -> DROPPED; false only at COMMITTED (or DROPPED)
bool StartJobDropForExit(SStartJob& job, const char* by);
//! D2: the open's job, CASed null -> job (false: another player's job is still open)
bool PublishTxOpenJob(const std::shared_ptr<SStartJob>& job);
//! job -> null (false: not the open job)
bool ClearTxOpenJob(const std::shared_ptr<SStartJob>& job);
std::shared_ptr<SStartJob> TxOpenJob();
//! 5.E2-12a D1 (docs/phase5_e2_12_impl.md): W -> P, under a leaf lock (W posts after its list
//! lock is released); P drains it every main-loop pass
struct STxInboxEntry
{
  enum class Kind
  {
    PICK_DEFERRED, //!< a user-pick deferred in the window: its target by value
    VS10_RAN, //!< a replayed or re-queued VS10 press has run: raise its trigger
  } kind = Kind::PICK_DEFERRED;
  uint64_t reqId = 0;
  uint32_t f = 0;
  RESOLUTION_INFO res;
};
void PostTxInbox(STxInboxEntry entry);
std::vector<STxInboxEntry> TakeTxInbox();
//! 5.E2-12a D2.2: the open job old -> amend (false: old is not the open job)
bool ReplaceTxOpenJob(const std::shared_ptr<SStartJob>& old,
                      const std::shared_ptr<SStartJob>& amend);
//! 5.E2-12a D2.4: P's overtake of the open job by an amend: QUEUED|PARKING -> DROPPED (true also
//! when already DROPPED); false at COMMITTED
bool StartJobDropForAmend(SStartJob& job, unsigned int amend);
//! main: GC's set inside main's adopt of a W reply (the Start job's, or a hotplug Restore's
//! ADOPT form) is on main's stack (it is not a GUI set, D9)
bool InStartAdopt();
void SetInStartAdopt(bool in);

// 5.E2-8b: GC's facts P's DECIDE reads (docs/phase5_e2_8b_impl.md D2, D4 step 7, D9)
//! GC's fullscreen-exit counter (seq_cst, no lock): SetFullScreenVideo bumps it right after it
//! computes `exiting`, before its desktop set or trigger, under every adjustrefreshrate setting
uint64_t FullScreenExits();
void NoteFullScreenExit();
//! GC's mode sets in flight (seq_cst), read by P's Dekker step: SetVideoResolutionInternal's
//! scope, counted in at its entry before the drop CAS, out when the set returns, or for a set
//! main gave up on (15 s) when W ends its job
int GuiSetsInFlight();
class CGuiSetInFlight
{
public:
  CGuiSetInFlight();
  ~CGuiSetInFlight();
  CGuiSetInFlight(const CGuiSetInFlight&) = delete;
  CGuiSetInFlight& operator=(const CGuiSetInFlight&) = delete;

private:
  friend bool KeepGuiSetInFlight();
  CGuiSetInFlight* m_previous;
  bool m_kept = false;
};
//! main, at the give-up of its set's job: the innermost scope's count stays past the scope's
//! end. True when one was kept: W then calls GuiSetInFlightEnded when that job ends
bool KeepGuiSetInFlight();
void GuiSetInFlightEnded();
//! GC, a GUI set while TxOpenJob is open (D9; not the adopt of a W reply, not Back's own desktop
//! set): the drop CAS, then the guiSet notice tagged preActivation (read on main:
//! GetActiveWindow() != WINDOW_FULLSCREEN_VIDEO)
void GuiSetDuringStart(SStartJob& job, bool preActivation);
//! the guiSet notice: seq the newest notice's number (0: none), preActivation when any notice
//! after `since` was tagged pre-activation
struct GuiSetNotice
{
  uint64_t seq = 0;
  bool preActivation = false;
};
GuiSetNotice ReadGuiSetNotice(uint64_t since);
//! GC, Back's desktop set (SetFullScreenVideo's `exiting` branch, its targetResolutionOnStop
//! set only): no drop CAS and no guiSet notice (the exit counter is its notice). Thread-local
class CFullScreenExitSet
{
public:
  CFullScreenExitSet();
  ~CFullScreenExitSet();
  CFullScreenExitSet(const CFullScreenExitSet&) = delete;
  CFullScreenExitSet& operator=(const CFullScreenExitSet&) = delete;
};
bool InFullScreenExitSet();

// 5.B2-8 (D11, D12): a DV-mode publish posts OutputChanged; main's handler asks RM to re-resolve
// the GUI/OSD encoding at its next render pass
//! the switch: ModeSetsOnW and no outputdrive_skip "B2-8", latched at its first read after W runs
bool DvModePublishOn();
//! the writers of the DV output mode (aml_dv_set_output_mode)
enum DvWriter : unsigned int
{
  DV_WRITER_VS10 = 1,
  DV_WRITER_VS10_FOLLOW = 2,
  DV_WRITER_DV_APPLY = 4, //!< a decoder open: its first picture configures
  DV_WRITER_TEARDOWN = 8, //!< a decoder close: the next decoder's first picture configures
};
//! W, inside the writer's job: the epoch bump (no commit count, no timing bump) and the post
void PublishDvMode(unsigned int oldMode, unsigned int mode, DvWriter writer);
//! main, from MessagePump: the posts since the last call; sets the re-encode request unless only
//! decoder writers posted
void HandleOutputChanged();
//! main, RM's render pass: the pending request's epoch, consumed; 0 when none
uint64_t TakeReencodeRequest();
//! 5.E2-7 (P5-33, docs/phase5_e2_7_impl.md D9): the DisplayState epoch of a VS10 DV publish not
//! yet re-encoded by B2-8, on transaction sessions (0: none). V's Configure skips its
//! ConfigChanged term while it is set, so the GUI re-encode stays B2-8's
uint64_t P533Owed();
//! clears the owed epoch: 0 any (a file change, a stop), else only that epoch (B2-8's pass for it)
void P533Clear(uint64_t epoch, const char* why);

// main only: the shadows' readers only count
//! logs new DIFF/FAIL details, and the counters every 10 s
void Tick();
//! the counters changed since their last line
void LogCounters();

} // namespace DISPLAYSTATE
