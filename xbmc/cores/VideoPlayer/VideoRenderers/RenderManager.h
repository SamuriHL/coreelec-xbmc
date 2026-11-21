/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDClock.h"
#include "DebugRenderer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRenderer.h"
#include "cores/VideoSettings.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/SystemClock.h"
#include "utils/Geometry.h"
#include "windowing/Resolution.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "PlatformDefs.h"

namespace KODI
{
namespace RENDERING
{
namespace CAPTURE
{
class CCaptureBlit;
}
} // namespace RENDERING
} // namespace KODI

class CDVDOverlayContainer;
class CPresentationCoordinator;
struct SUntimedPost;
struct VideoPicture;

class CWinRenderer;
class CLinuxRenderer;
class CLinuxRendererGL;
class CLinuxRendererGLES;
namespace DISPLAYSTATE
{
struct DecideInputs;
}
class CRenderManager;
namespace DISPLAYSTATE
{
struct DisplayState;
}

class IRenderMsg
{
  friend CRenderManager;
public:
  virtual ~IRenderMsg() = default;
protected:
  virtual void VideoParamsChange() = 0;
  virtual void GetDebugInfo(std::string &audio, std::string &video, std::string &general) = 0;
  virtual void UpdateClockSync(bool enabled) = 0;
  virtual void UpdateRenderInfo(CRenderInfo &info) = 0;
  virtual void UpdateRenderBuffers(int queued, int discard, int free) = 0;
  virtual void UpdateGuiRender(bool gui) = 0;
  virtual void UpdateVideoRender(bool video) = 0;
  virtual CVideoSettings GetVideoSettings() const = 0;
};

// real_player: one vsync as the presentation coordinator woke on it
struct SPresentTick
{
  uint64_t seq = 0;
  int64_t vblankNs = 0; //!< CLOCK_MONOTONIC hardware time; 0 = timer fallback tick
  int64_t wokeNs = 0;
  unsigned int epoch = 0; //!< display epoch (aml_presenter_epoch)
  const char* holdSite = nullptr; //!< the hold whose release started epoch; null = none
  int holdTid = 0;
  int64_t holdReleaseNs = 0; //!< steady_clock
};

class CAMLCodec;

//! 5.V3-4 (plan M2 (a)): a view mode C computes at main's SetViewMode moments, from the
//! same inputs
struct SViewModeRequest
{
  uint64_t gen = 0;
  int viewMode = 0;
  float customZoomAmount = 1.0f;
  float customPixelRatio = 1.0f;
  float customVerticalShift = 0.0f;
  bool customNonLinStretch = false;
  unsigned int fileGen = 0; //!< 5.V3-7: the file the request belongs to (RM's m_fileGen)
};

//! 5.V3-4: C's inputs from RM, copied under m_statelock on the path only
struct SGeometryIn
{
  std::shared_ptr<CBaseRenderer> renderer;
  unsigned int rgen = 0;
  uint64_t request = 0; //!< the fullscreen request word
  SViewModeRequest viewMode;
  bool v37 = false; //!< 5.V3-7: a newer file's request waits for that file's first frame
  unsigned int fileGen = 0; //!< RM's m_fileGen: a decoder first seen at a QBUF takes it
  // the configure facts
  unsigned int sourceWidth = 0;
  unsigned int sourceHeight = 0;
  unsigned int flags = 0;
  unsigned int orientation = 0;
  float aspectRatio = 1.0f;
  bool alwaysClip = false;
  std::string videoStereoMode;
};

struct SPresentResult
{
  bool configured = false;
  bool playing = false; //!< clock running, display not lost
  bool displayLost = false;
  bool newFrame = false;
  double pts = 0.0;
  int skipped = 0; //!< queued frames discarded unshown this vsync
  //! Phase 4 shadow: the clock audio syncs to (clock + vsyncAdjust) minus the
  //! presentation reference (the released frame's pts advanced from its vblank,
  //! less the display latency the renderer selects frames by), DVD time
  bool shadow = false;
  double shadowDiff = 0.0;
  double shadowAdjust = 0.0; //!< the vsyncAdjust in it
  bool shadowSynced = false; //!< clock sync on (off: no phase, raw lateness)
  double frametime = 0.0;
  SGeometryIn geom; //!< 5.V3-4: empty off the path
  std::shared_ptr<CAMLCodec> qbufCodec; //!< 5.V3-4: the QBUFed frame's decoder (the path)
  bool qbufShown = false; //!< queued to be shown
  bool wakeWorker = false; //!< 5.V3-4f: buffers freed on the path, the worker woken after the tick
  // 5.G2 GT-4, the released frame (newFrame)
  uint64_t overlaySerial = 0; //!< 0: its overlays do not travel with it
  unsigned int segmentGen = 0;
  int64_t qbufIndex = -1; //!< amlvideo's running v4l2 index; -1: not queued on the path
  bool graphicsWithheld = false; //!< START to T0 or a held start: C commits no timed graphics
};

class CRenderManager
{
  friend CPresentationCoordinator;

public:
  CRenderManager(CDVDClock &clock, IRenderMsg *player);
  virtual ~CRenderManager();

  // Functions called from render thread
  void GetVideoRect(CRect& source, CRect& dest, CRect& view) const;
  float GetAspectRatio() const;
  unsigned int GetOrientation() const;
  void FrameMove();
  void FrameWait(std::chrono::milliseconds duration);
  void Render(bool clear, DWORD flags = 0, DWORD alpha = 255, bool gui = true);

  void UpdateResolution();
  //! kind: the E2-8b notice kind while main is shut out (null: trigger / trigger-params)
  void TriggerUpdateResolution(float fps,
                               int width,
                               int height,
                               std::string& stereomode,
                               const char* kind = nullptr);
  //! the next mode decision may re-clock a 1000/1001 rate difference (a content start);
  //! armed at the next Configure, or now for the running stream
  void ArmContentReclock(bool now);
  void SetViewMode(int iViewMode);
  //! 5.V3-3: this player's fullscreen notice request, seq << 1 | 1 (0: none), written by
  //! CWinSystemBase::PostFullScreenVideo; C honours it until main's noticeSeqDone reaches seq
  std::atomic<uint64_t>& FullScreenRequest() { return m_fullScreenRequest; }
  void PreInit();
  //! a new file in the same session: resets what belongs to the last one
  void BeginFile();
  //! test (vconfigure_mainstall): the player arms its "stop" or "nextfile" event
  void ArmTestStall(const char* event);
  void UnInit();
  bool Flush(bool wait, bool saveBuffers, bool overlaysOnly = false);
  bool IsConfigured() const;
  void ToggleDebug();
  void ToggleDebugVideo();

  /*!
   * \brief Set the subtitle vertical position,
   * it depends on current screen resolution
   * \param value The subtitle position in pixels
   * \param save If true, the value will be saved to resolution info
   */
  void SetSubtitleVerticalPosition(const int value, bool save);

  // Functions called from GUI
  bool Supports(ERENDERFEATURE feature) const;
  bool Supports(ESCALINGMETHOD method) const;

  int GetSkippedFrames()  { return m_QueueSkip; }
  void DisplayReset() { m_displayReset = true; }

  bool Configure(const VideoPicture& picture, float fps, unsigned int orientation, int buffers = 0);
  // forceGen >= 0 (a stream worker): force the picture without waiting; the tick
  // that takes it clears the force, marks the generation taken and wakes the worker
  // overlays (5.V2-7): the picture's own, stored with it (taken)
  bool AddVideoPicture(const VideoPicture& picture,
                       volatile std::atomic_bool& bStop,
                       EINTERLACEMETHOD deintMethod,
                       bool wait,
                       int forceGen = -1,
                       OVERLAY::FrameOverlays* overlays = nullptr,
                       bool* txAccepted = nullptr);
  // 5.E2-6: C's picture side of CN's start gate on an engaged open (docs/phase5_e2_6_impl.md).
  // P arms a candidate at CN's arm (the file's first set only); RM engages it inside that set's
  // first AddVideoPicture (transaction_on, no "transaction_skip gate", a worker's force gen, a
  // geometrytick session) and from then refuses the force, records the start picture's
  // acceptance (txAccepted), shows it once on C when CN's state is committed (the pre-roll), and
  // selects nothing else until START opens the gate. Every call is a no-op on a not-engaged open
  //! P, at every ArmStartGate: clears an older gate; candidate for the file's first set. True when
  //! the cleared gate held an accepted picture (P restores the decoder's speed)
  bool ArmTxPictureGate(unsigned int gen, bool candidate, const char* why);
  //! P, at START (or the sync's no-release branch, a stop, a close): opens the gate. True when it
  //! held an accepted picture
  bool OpenTxPictureGate(const char* why);
  //! 5.E2-11 D3.3: P, a Restore committed inside the open start: the shut gate's shown pre-roll
  //! is shown again from the queue's head (the start picture is not re-queued); false when the
  //! gate is not engaged and shut with a pre-roll shown
  bool TxReShowPreRoll(unsigned int gen, uint32_t e);
  //! V's CodecSpeed: the decoder is held paused (an engaged, shut gate with its picture accepted)
  bool TxHoldsDecoder() const { return m_txHoldsDecoder.load(); }
  //! 5.E2-8a (D3, D1): P, an engaged start. Graphics withheld from OPEN to START or the exit
  //! (GraphicsWithheld's own clause, not E1's m_startHeld); the bound (13 s, not counted while
  //! the display is lost) is armed at CN's opening and re-armed at E1's hold
  void SetTxStartHeld(bool held);
  void ArmTxStartHeldBound();
  //! 5.E2-9b D3 (E28B-1, docs/phase5_e2_9b_impl.md): G-X1, how long before T0 the overlays may
  //! show; 0 in 9b (overlays at T0)
  static constexpr int64_t TX_GRAPHICS_GX1_NS = 0;
  //! P: an engaged start's RM overlays withheld up to t0Ns (CLOCK_MONOTONIC ns, the scheduled
  //! start's T0) less G-X1; 0 clears. Stored before the holds clear (release order)
  void SetTxGraphicsFrom(int64_t t0Ns)
  {
    m_txGraphicsFromNs.store(t0Ns != 0 ? t0Ns - TX_GRAPHICS_GX1_NS : 0, std::memory_order_release);
  }
  //! P: the set it engaged; RM's first-picture legacy verdict for that set is published for P
  void SetTxEngagedGen(unsigned int gen) { m_txEngagedGen = gen; }
  //! P: the engaged set RM gave a legacy verdict for (0 = none)
  unsigned int TxLegacyVerdict() const { return m_txLegacyVerdict.load(); }
  // 5.E2-8b: the engaged start decided on P, main shut out (docs/phase5_e2_8b_impl.md). Every
  // call below is a no-op on a not-engaged open (m_txShutGen 0, no outcome record)
  //! E2-8b D8: an exit's `how` for a job not past its drop CAS (or never posted)
  enum class TxHow
  {
    NONE,
    RERAISE, //!< the trigger set, m_videostarted kept, the frac arm restored
    OWED, //!< the owed mark: the new gen's first configure (or a KEEPALIVE arm) raises
    START, //!< 5.E2-9c D3 (r6 M1): TxStartPoint only; START reads the hint compare's record
  };
  //! E2-8b D5b: an exit's `need` for a job past its drop CAS (NONE: not past it, or START)
  enum class TxNeed
  {
    NONE,
    NONE_NEEDED,
    JOB_OK,
    FRESH,
    GUI_OWN,
    LATER,
    LEFT,
  };
  //! E2-8b D5b: the adopt's answer. FAILED_SET = the table's `failed` for a Start job whose own
  //! set failed (kind FAILED/FAILED_PARK, nothing on the wire): its hand-back restores the frac
  //! arm, sets the rm-retry flag and posts one VideoParamsChange (Plan vs code 6)
  enum class TxAnswer
  {
    NONE,
    ADOPTED,
    BEHIND,
    BEHIND_GUI,
    FAILED,
    FAILED_SET,
    RECOVERING,
    RECOVERED,
  };
  //! E2-8b D3: a trigger raised while main is shut out: the facts RM holds after the write
  struct STxNotice
  {
    const char* kind = ""; //!< fps, configure, reclock, trigger, trigger-params, vs10, rm-retry, owed
    float fps = 0.0f;
    int width = 0;
    int height = 0;
    std::string stereoMode;
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  };
  //! 5.E2-9c D3: a no-picture DECIDE's facts, taken from the stream's hints on P
  struct SStartFacts
  {
    float fps = 0.0f; //!< what V's first Configure reports (not doubled); fpsInvalid: the mode's
    int width = 0;
    int height = 0;
    std::string stereoMode;
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
    bool frac = false; //!< reclockEligible && !fpsInvalid (ResolveStart adds its own terms)
    bool fpsInvalid = false;
    bool interlaceHint = false; //!< hint.interlaced, logged only
  };
  //! 5.E2-9c D3/D4: a picture against a hint decision. On the gen's first configure 2f is f for
  //! a 25 / 29.97 fact (E29A-6: fps2x set), and fps is left out with fpsInvalid
  static bool TxHintFactsSame(const SStartFacts& hint,
                              float fps,
                              int width,
                              int height,
                              const std::string& stereoMode,
                              StreamHdrType hdrType,
                              bool firstConfigure,
                              bool* fps2x = nullptr);
  enum class TxHintResult
  {
    NONE, //!< no picture compared
    SAME,
    DIFFERENT,
  };
  //! 5.E2-9c D3: P at a hint job's post: RM's hint flag set for gen, unless a configuring
  //! Configure of gen came first (then SAME/DIFFERENT, the picture's facts in picture)
  TxHintResult TxHintPosted(unsigned int gen, const SStartFacts& facts, STxNotice& picture);
  //! 5.E2-9c D4: P before START: a configuring Configure of gen ran (its live facts in picture)
  bool TxHintConfigured(unsigned int gen, STxNotice& picture) const;
  //! 5.E2-9c D4: late pictures that contradicted a hint decision after START
  unsigned int TxLatePictureContradicted() const { return m_txLateContradicted.load(); }
  //! E2-8b D2: P at the engage: main's UpdateResolution shut out for gen, the trigger folded,
  //! the start's outcome record opened (T: the word's, for the TXSTART lines)
  void TxShutMain(unsigned int gen, uint64_t T);
  //! E2-8b D8/D7: P's exit (its first step after the CAS) or START: main let back in; `how` for
  //! a job not past its drop CAS (need NONE), else D5b's need (and commit-in-flight)
  //! 5.G-3 (§4.3 a): keepShut at START: main stays shut out for P's mid-play path session
  void TxOpenMain(unsigned int gen, TxHow how, TxNeed need, bool keepShut = false);
  //! E2-8b D5/D5b: every adopt form (main, after GC's set, outside the gfx lock) and P's
  //! backstop (FAILED). True when it posted VideoParamsChange (the caller posts none)
  bool TxAdoptAnswer(unsigned int gen, TxAnswer form);
  //! E2-8b D5b: P, a stop or next file after an exit (the gen P kept from it)
  void TxFileEnded(unsigned int gen);
  //! E2-8b D5b (code review A-M1): P, a later set's arm after an exit (the gen P kept from it):
  //! an outcome still unresolved (commit-in-flight) is resolved as D8's `later` row
  void TxLaterSet(unsigned int gen);
  //! 5.E2-10 (D1 item 3): an exit's outcome no longer awaits main's answer (no record of the
  //! gen, not past the drop CAS, answered, or none needed), so a reopen may replace the record
  bool TxOutcomeResolved(unsigned int gen);
  //! E2-8b D5: W's OutputDone reached P: C's pre-roll term for the shut gen
  void SetTxOutputDone(unsigned int gen);
  //! E2-8b D8: P, a KEEPALIVE later set's arm (it never configures): the owed mark raised.
  //! True when one was owed
  bool TxRaiseOwed(const char* why);
  //! E2-8b D3: P each CheckStartGate pass: the notices since the last take, oldest first
  std::vector<STxNotice> TakeTxNotices();
  //! E2-8b test (transaction_noticeat): P raises a notice, RM's facts (trigger) or others
  //! (changed: an fps notice 1 fps off RM's)
  void TxTestNotice(bool changed);
  //! E2-8b test (displaystate_mainstall "straddle <ms>", code review A-L2): main, OpenFile before
  //! P's engage: the trigger raised once, so main's held pass can straddle OPEN
  void TxTestStraddleRaise();
  //! 5.V2-7 (MC-41): with the presenter, a picture carries its overlays to AddVideoPicture
  //! instead of AddOverlay, and the buffer calls leave the overlay lock alone
  bool OverlaysTravel() const;
  void AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts);
  void ShowVideo(bool enable);
  void SetDisplayLost(bool lost) { m_displayLost = lost; }
  //! real_player E1: output-mode decisions made so far (UpdateResolution passes)
  unsigned int GetResolutionDecisions() const { return m_resolutionDecisions; }
  //! a mode decision is due: the session's first, or one for fullscreen video
  //! (UpdateResolution decides only then, so a windowed trigger waits for the user).
  //! 5.B2-2: also while a consumed trigger's set runs; decisions gets the count read
  //! with it (left unset under outputdrive_off)
  bool IsResolutionUpdatePending(std::optional<unsigned int>& decisions) const;
#if defined(HAS_LIBAMCODEC)
  //! 5.B2-5b: the retry will not run (left fullscreen, a new file): its in-flight flag goes
  void AbandonRmRetry(const char* why);
#endif
  //! real_player E1: the player holds the start for the output mode; its first
  //! picture is decoded, so the mode decision need not wait for it on screen
  void SetStartHeld(bool held) { m_startHeld = held; }
  //! start order design §4: the player's start gate for its stream set gen; a decision on
  //! the first configure after it is counted for the set, without waiting for the picture
  void SetModeFirstGate(bool gated, unsigned int gen);
  enum class ModeFirstState
  {
    UNCONFIGURED, //!< no configure of the set yet
    PENDING, //!< a decision is due or running
    DECIDED, //!< a decision made on the set's configure has been counted
    UNCHANGED, //!< the set's configure needs no decision
  };
  ModeFirstState GetModeFirstState(unsigned int gen) const;
#if defined(HAS_LIBAMCODEC)
  // 5.E2-5 (shadow): DECIDE's ResolveOutput on P against main's decision for the same CN set
  // (docs/phase5_e2_plan.md E2-5): each side's record is logged "DECIDE shadow set=N P|main",
  // and the second to arrive logs "DECIDE shadow set=N MATCH|DIFF <fields>"
  struct DecideSide
  {
    bool decides = false; //!< main: a mode set was asked for; P: the start would decide
    const char* why = ""; //!< no decision: "no mode decision due", "3D ask", "not fullscreen"
    RESOLUTION res = RES_INVALID;
    std::string mode; //!< the chosen mode's string
    bool change = false; //!< P: the choice differs from the video resolution it read
    bool frac = false; //!< fracReclock
    bool filmStart = false; //!< the request's film-start origin (a start the player holds)
    int hdr = 0; //!< StreamHdrType
    bool ask = false; //!< the 3D ASK predicate
    std::string note; //!< the inputs' provenance (seq, age, arm source)
    bool inconclusive = false; //!< P read after main took its inputs (E2-5 review M4)
  };
  //! P: ResolveOutput for CN set gen from the published inputs (latest, the root at the open),
  //! RM's own decision inputs (as main reads them) and the settings mirror. cnArmed: CN's arm
  //! predicate (fullscreen) for the set
  void DecideShadowPlayer(unsigned int gen,
                          const DISPLAYSTATE::DecideInputs& latest,
                          bool rootAtOpen,
                          bool cnArmed);
  //! E2-8b D4 step 2: DECIDE's choice for the engaged start, by value
  struct SStartResolve
  {
    const char* exclusion = nullptr; //!< D1's exit (`3D start`, `refresh off`, ...); else none
    RESOLUTION res = RES_INVALID;
    RESOLUTION_INFO info; //!< latest.res.infos[res]
    bool change = false; //!< res differs from the video resolution read
    bool frac = false; //!< fracRequest (TxTakeArm makes it the arm it took)
    bool filmStart = true;
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
    RenderStereoMode stereo = RenderStereoMode::OFF; //!< the GUI's (2D) mode
    double tweakMs = 0.0; //!< the latency tweak for info + hdrType
    float fps = 0.0f;
    //! RM's picture facts as read with fps (code review A-L1: the notices' baseline)
    int width = 0;
    int height = 0;
    std::string stereoMode;
    bool mayChooseMode = false;
    bool refreshOff = false; //!< 5.E2-10 9d: adjust refresh rate off: the incumbent mode
    bool held = false; //!< the disc mode hold
    bool anchor = false; //!< the count sets the disc-mode anchor
  };
  //! E2-8b D4 step 2 (Plan vs code 2): DecideShadowPlayer's pure body on RM's current facts,
  //! the published inputs and the settings mirror; consumes nothing
  //! 5.E2-9c D3: facts (a no-picture DECIDE) replace RM's configured picture
  //! 5.E2-10 9d: P's 3D target for the video's stereo mode (stream); null: a stereoscopic
  //! stream is the `3D start` exclusion (transaction_skip ask)
  struct SStartStereo
  {
    std::string stream;
    RenderStereoMode target = RenderStereoMode::OFF;
  };
  SStartResolve ResolveStart(unsigned int gen,
                             const DISPLAYSTATE::DecideInputs& latest,
                             bool rootAtOpen,
                             bool fullScreen,
                             const SStartFacts* facts = nullptr,
                             const SStartStereo* stereo = nullptr,
                             bool log = true);
  //! 5.E2-10 9d: the configured picture's stereo mode, as ResolveStart reads it
  std::string TxPictureStereoMode();
  //! E2-8b D4 step 4: the frac arm consumed for the start (today's `consumed` line), kept with
  //! its original time for a reraise or a failed hand-back; r.frac becomes the arm taken
  //! (5.E2-9c D3, r4 H1, r5 M3: on a hint DECIDE OR'd with the hint's, and that one recorded)
  void TxTakeArm(unsigned int gen, SStartResolve& r, bool hintDecide = false);
  //! 5.G-2 (docs/phase5_g0_design.md §4.2): P, while its path session plays: main's mid-play
  //! decisions against ResolveStart, logged "MIDPLAY shadow"
  void TxMidShadowPass(uint32_t T);
  //! 5.G-2: P, at path on and its end: main's records not yet compared are dropped
  void TxMidShadowReset();
  //! 5.G-3 (§4.0): P, the path session's end: main let back in, the notices raised as the
  //! trigger (raise: a later set's arm; also when P owes a decision) or dropped. -1 when main
  //! was not shut out for a path session, else the notices' count
  int TxMidPlayEnd(bool raise, bool owed);
  //! 5.G-3 (§4.3 b): P's DECIDE: ResolveStart on RM's facts and latest, W's last committed mode
  //! as the incumbent. 5.G-5: forceFullScreen, a pick's DECIDE with the GUI over the video (a
  //! pick sets at once, as today's direct set)
  SStartResolve TxMidResolve(const DISPLAYSTATE::DecideInputs& latest,
                             bool forceFullScreen = false);
  //! 5.G-5 (§4.5): P's setting job's mode: W's last committed one on latest's table (r2 N1)
  RESOLUTION TxMidIncumbent(const DISPLAYSTATE::DecideInputs& latest);
  //! 5.G-3: P, a notice is queued (no lock: P's every-pass test)
  bool TxMidNoticesPending() const { return m_txNoticed.load(); }
  //! 5.G-3 (§4.3 b): P hands one decision back to main (3D start, no inputs): one consume with
  //! main still shut out after it
  void TxMidHandBack(const char* why);
  //! 5.G-3: main's handed-back decision is not yet consumed or done
  bool TxMidHandBackBusy();
  //! 5.G-3 (S9): P, a failed mid-play set's retry: the arm its DECIDE took, back
  void TxMidRestoreArm();
  //! 5.G-3 (§4.3 c, S5): main, a mid-play job's adopt answer (after GC's set returned): the
  //! after-set tweak, then VideoParamsChange when post (P posts a no-change's or a failed
  //! set's). Returns post
  bool TxMidAdoptAnswer(unsigned int seq, bool post, const char* answer);
  //! E2-8b D4: UpdateResolution's render timeout for the start's decision (elapsed since the
  //! configure or trigger, as main measures it; 5.E2-9c D3: or since `from`)
  //! 5.E2-11 D1.3: notBefore, a later reference (a span's close) when set
  bool TxRenderTimedOut(std::chrono::steady_clock::time_point from = {},
                        std::chrono::steady_clock::time_point notBefore = {});
#endif
  //! the pictures queued from now on belong to this disc segment generation
  void SetIncomingSegmentGen(unsigned int gen) { m_incomingSegmentGen = gen; }

  /*!
   * \brief True if any subtitle/overlay is visible on the current presented
   *  frame. Per-frame accurate. See OVERLAY::CRenderer::HasVisibleOverlay
   *  for the libass vs PGS/DVB/SPU details.
   *
   *  Must be called after CRenderManager::FrameMove has run this frame
   *  (which calls PrepareOverlays). Reads cached state; cheap.
   */
  bool HasVisibleOverlay() const;

  /*! \brief Give the overlay renderer the player's overlay container so it can
   *  clear a stale overlay plane when the container empties during a stall. */
  void SetOverlayContainer(CDVDOverlayContainer* container)
  {
    m_overlays.SetOverlayContainer(container);
  }

  /**
   * If player uses buffering it has to wait for a buffer before it calls
   * AddVideoPicture and AddOverlay. It waits for max 50 ms before it returns -1
   * in case no buffer is available. Player may call this in a loop and decides
   * by itself when it wants to drop a frame.
   */
  // a stream worker (design §17): its eventfd, written when a buffer is freed and
  // when its forced start picture is taken; -1 clears
  void SetWorkerWakeFd(int fd);
  //! 5.V3-4: this renderer is on the path (the presenter, the latch); V adds its worker
  bool GeometryTickRenderer() const;
  //! 5.V3-4: V applies the decoder side; a settings or view mode change then wakes V
  //! 5.G2 GT-4: and the timed-graphics route is decided for the configure it follows
  void SetDecoderSideOnV(bool on);
  unsigned int DecoderSettingsGen() const
  {
    return m_decoderSettingsGen.load(std::memory_order_relaxed);
  }
  //! 5.V3-4 (C): the tail after PresentTick (its locks released), and the wake tail
  bool GeometryTailOnC(SPresentResult& result); //!< true when it wrote
  bool GeometryWakeOnC();
  //! 5.V3-4f (C): the worker's wake for the buffers PresentTick freed, after the release
  void WakeWorkerOnC();
  //! 5.V3-4 (V): reset the decoders C used
  void DrainCodecsFromC();
  bool ForcedPictureTaken(int gen) const;
  void CancelForce(int gen);
  // non-blocking WaitForBuffer: the level, or -1 with no free buffer (retryIn
  // set while the GUI is not rendering: the discard waits for the clock)
  int TryGetBuffer(std::chrono::milliseconds& retryIn, bool releaseOverlays = true);

  int WaitForBuffer(volatile std::atomic_bool& bStop,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(100),
                    bool releaseOverlays = true);

  /**
   * Can be called by player for lateness detection. This is done best by
   * looking at the end of the queue.
   */
  bool GetStats(int &lateframes, double &pts, int &queued, int &discard);
  //! the pts of the first frame not yet released, and how far a held clock
  //! may step to it (the display latency, a vsync and a frame)
  bool GetResumePts(double& pts, double& maxStep);

  /**
   * Video player call this on flush in order to discard any queued frames
   */
  void DiscardBuffer();

  void SetDelay(int delay) { m_videoDelay = delay; }
  int GetDelay() { return m_videoDelay; }
  //! the display latency tweak moves the sound (the player lands it with the
  //! audio offset), not the picture
  void SetLatencyTweakToAudio(bool toAudio);
  //! the display latency tweak in ms
  double GetLatencyTweak() const;

  void SetVideoSettings(const CVideoSettings& settings);
  //! 5.V3-1 (log only, P): how fileGen was opened, for its landing population
  void NoteFileOrigin(unsigned int fileGen, bool nextFile, bool fullscreen);
  //! 5.V3-1 (log only): the player's per-file settings request for fileGen is done (P's
  //! outbound thread), timed against the file's first release
  void NoteSettingsLanded(unsigned int fileGen);
  //! 5.V3-7 (outbound thread): the file's per-file settings landed; with V37 its view mode
  //! becomes a request (C applies it at the file's first frame, or at the next tick)
  void SettingsLanded(unsigned int fileGen);
  //! test (vconfigure_mainstall "sleep settings <ms>"): delays the load on the outbound thread
  void SettingsLoadTestStall();
  unsigned int FileGen() const { return m_fileGen; }

protected:
  void NotifyWorker(); // under m_presentlock

  void RenderWithoutPicture(bool gui, bool configured);
  void PresentHdrGraphics(int idx, const CRect& source, const CRect& dest, const CRect& view);
  bool GraphicsWithheld();
#if defined(HAS_LIBAMCODEC)
  //! GUNTIMED (guntimed_log, main): a change of the untimed HDR images, or their going
  //! away, is a post. toPlane: route C's posts, sent with the images; null: route B's,
  //! carried by main's next GUI buffer
  void NoteUntimedPost(const std::vector<std::shared_ptr<CDVDOverlay>>& images,
                       bool untimed,
                       std::vector<SUntimedPost>* toPlane);
  void NoteUntimedOnGui(int idx);
  std::vector<const CDVDOverlay*> m_untimedLast;
  bool m_untimedLastUntimed = false;
  uint64_t m_untimedU = 0;
#endif

  void PresentSingle(CBaseRenderer& renderer, bool clear, DWORD flags, DWORD alpha);
  void PresentFields(CBaseRenderer& renderer, bool clear, DWORD flags, DWORD alpha);
  void PresentBlend(CBaseRenderer& renderer, bool clear, DWORD flags, DWORD alpha);

  //! a coordinator tick's display timing, when it presents ahead of main's publish (5.V1-4)
  struct STickTiming
  {
    float fps;
    double latencyMs;
  };
  float TimingFps(const STickTiming* timing) const
  {
    return timing ? timing->fps : m_timingFps.load();
  }
  double TimingLatencyMs(const STickTiming* timing) const
  {
    return timing ? timing->latencyMs : m_timingLatencyMs.load();
  }

  //! vblankNs: the hardware vblank the coordinator woke on (0 = none), or
  //! -1 on the render thread, which asks the win system
  void PrepareNextRender(int64_t vblankNs, const STickTiming* timing = nullptr);
  bool IsPresenting();
  bool IsGuiLayer();

  bool Configure();
#if defined(HAS_LIBAMCODEC)
  //! 5.V2-4: V builds and configures an Amlogic picture's renderer and swaps it in
  bool ConfigureOnV(const VideoPicture& picture, float fps, unsigned int orientation, int buffers);
  //! 5.V2-4, main at FrameMove's top: retires renderers and applies a V swap's main duties;
  //! true when it did (the first frame after a configure)
  bool ApplyConfigureDuties();
#endif
  void CreateRenderer();
  void DeleteRenderer();

  //! Video-only tap: serve VIDEO capture requests from the just-presented frame.
  void ServiceVideoCaptures(CBaseRenderer& renderer);

  //! the tweak for the current mode, or for the mode about to be set; site names it in the
  //! VTIMING shadow
  void UpdateLatencyTweak(const char* site, RESOLUTION target = RES_INVALID);
  void CheckEnableClockSync(const STickTiming* timing = nullptr);
  //! render thread: the display timing PrepareNextRender uses, for either thread
  void PublishDisplayTiming(const char* site = "pass");
  //! 5.B2-8 (D11): main's OutputChanged request, re-encoded with no new picture
  void ReencodeOnOutputChange();

public:
  //! 5.V2-1: a timing as composed from its inputs
  struct SComposedTiming
  {
    float refresh = 60.0f; //!< the latency table's refresh (0 windowed)
    int height = 0;
    bool hdrUsed = false;
    double tweakMs = 0.0;
    float fps = 60.0f;
    double latencyMs = 0.0;
    bool operator==(const SComposedTiming&) const = default;
  };
  //! UpdateLatencyTweak's B2-3 branch and PublishDisplayTiming's latency, from the video side's
  //! inputs only: W's published state, V's picture facts, P's tweak placement and the
  //! AdvancedSettings table (state null: none published)
  static SComposedTiming ComposeTiming(const DISPLAYSTATE::DisplayState* state,
                                       StreamHdrType hdrType,
                                       int vblanks,
                                       bool tweakToAudio);
  //! 5.V2-4b: the video side composes the display timing (V's configured renderers)
  static bool ComposesTiming();
  //! 5.G-3 (code review r1 S5): this renderer composes its timing now (main's Configure clears it)
  bool ComposingTiming() const { return m_composing.load(); }

protected:
  //! 5.V2-4b: V's picture facts for the timing, from one swap, as one word
  struct STimingFacts
  {
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
    int vblanks = -1; //!< no renderer: the composition takes the Amlogic one's
    uint32_t gen = 0;
  };
  std::atomic<uint64_t> m_timingFacts{0};
  std::atomic<uint64_t> m_fullScreenRequest{0};
  //! under m_statelock
  void WriteTimingFacts(StreamHdrType hdrType, int vblanks);
  STimingFacts ReadTimingFacts() const;
  //! 5.V2-4b: the timing is composed (a renderer V configured, or none yet), never stored by
  //! main; false after a configure on main's pass, which keeps HEAD's stored values
  std::atomic_bool m_composing{false};
  //! the composition of the published inputs now
  SComposedTiming ComposeNow() const;
  //! coordinator: what its last composition line logged
  struct SCoordinatorTiming
  {
    SComposedTiming timing;
    uint64_t stateEpoch = 0;
    uint32_t factsGen = 0;
    bool toAudio = false;
    bool operator==(const SCoordinatorTiming&) const = default;
  } m_composedLast;
  //! main: VTIMING shadow, the composition against main's values (latency: compare it too;
  //! toAudio: the placement main read); log only
  void ShadowTiming(const char* site, const SComposedTiming& today, bool latency, bool toAudio);
  //! main: what the last shadow line logged
  struct SShadowKey
  {
    const char* site = nullptr;
    SComposedTiming video;
    SComposedTiming today;
    uint64_t stateEpoch = 0;
    bool restored = false;
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
    int vblanks = 0;
    bool toAudio = false;
    bool operator==(const SShadowKey&) const = default;
  } m_vtimingLast;
  //! main: per site; target counts pre-set, where main composes the mode about to be set
  struct SShadowCount
  {
    const char* site = nullptr;
    uint64_t match = 0;
    uint64_t diff = 0;
    uint64_t target = 0;
  };
  std::vector<SShadowCount> m_vtimingCounts;
  //! V's picture fact for the shadow: m_picture.hdrType, stored with it
  std::atomic<StreamHdrType> m_pictureHdrType{StreamHdrType::HDR_TYPE_NONE};

  //! test (vconfigure_mainstall): main stalls here when the hook's event is armed (or immediate)
  void TestStall(const char* event, bool immediate = false);
  //! the hook's event: 1 waiting for the file's first picture, 2 armed, 3 the file's stop stalled
  std::atomic<int> m_testStallArmed{0};
  //! pictures queued (V, under m_presentlock) and the count at BeginFile (P), for the test stalls
  std::atomic<unsigned int> m_picturesAdded{0};
  std::atomic<unsigned int> m_filePicturesFrom{0};
  //! test (vconfigure_forceswap): V's next picture reconfigures
  std::atomic<bool> m_testForceSwap{false};
  //! 5.V2-1: generations for the logs, of the player (this manager) and of its file
  inline static std::atomic<unsigned int> s_playerGens{0};
  const unsigned int m_playerGen{++s_playerGens};
  std::atomic<unsigned int> m_fileGen{0};
  //! 5.V3-1 (GEOMDIAG, log only): the file's first release to the renderer (coordinator) and
  //! its settings landing (P's outbound thread), steady ns; each time is stored before its file
  std::atomic<int64_t> m_diagReleaseNs{0};
  std::atomic<unsigned int> m_diagReleaseFile{0};
  std::atomic<bool> m_diagNextFile{false}; //!< the current file's origin (NoteFileOrigin)
  std::atomic<int64_t> m_diagLandedNs{0};
  std::atomic<unsigned int> m_diagLandedFile{0};
  //! the RM lead: frames queued at each QBUF of the file, by count (the last bin: that many or
  //! more); logged at the next file and at the end
  static constexpr int LEAD_BINS = 16;
  std::atomic<uint32_t> m_diagLead[LEAD_BINS] = {};
  void LogLead(unsigned int fileGen);

  // real_player: the presentation coordinator's per-vsync step
  void PresentTick(const SPresentTick& tick, SPresentResult& result);
  void ShadowReference(SPresentResult& result, bool forced, const STickTiming* timing);
  bool StartCoordinator(); //!< true: attached by this call
  void StopCoordinator();

  //! main's unlocked users take a copy under m_statelock
  std::shared_ptr<CBaseRenderer> m_pRenderer;
  //! 5.V2-4 (m_statelock): renderers of another kind V swapped out, deleted on main (GL)
  std::vector<std::shared_ptr<CBaseRenderer>> m_retire;
  //! 5.V2-4 (m_statelock): bumped by each V swap; main draws a renderer only once it applied the
  //! swap's duties (m_appliedRgen, main only)
  unsigned int m_rgen = 0;
  unsigned int m_appliedRgen = 0;
  //! 5.V2-4 (m_statelock): V builds a renderer; the coordinator presents nothing meanwhile
  bool m_vConfiguring = false;
  //! 5.V2-4: UnInit ran (written under m_statelock and m_presentlock); V's swap is refused
  std::atomic_bool m_closed{false};
  unsigned int m_mainPassLogged = 0; //!< V: the file whose main pass was logged
  unsigned int m_pinRefusedLogged = 0; //!< main: the rgen whose refused draw was logged
  unsigned int m_firstPresentFile = 0; //!< coordinator: the file whose first present was logged
  //! main: a draw refused for a swap whose duties main has not applied, once per swap
  void LogPinRefused(const char* where);
  //! Owns the video tap's private FBO; render-thread only, reset in UnInit
  std::unique_ptr<KODI::RENDERING::CAPTURE::CCaptureBlit> m_captureBlit;
  OVERLAY::CRenderer m_overlays;
  CDebugRenderer m_debugRenderer;
  //! one set-up attempt per session when built on first show (a failed one is not retried per frame)
  bool m_debugRendererTried{false};
  mutable CCriticalSection m_statelock;
  mutable CCriticalSection m_presentlock;
  CCriticalSection m_datalock;
  std::atomic<bool> m_bTriggerUpdateResolution{false};
  bool m_bRenderGUI = true;
  bool m_renderedDebugOverlay = false;
  bool m_renderDebug = false;
  bool m_renderDebugVideo = false;
  XbmcThreads::EndTime<> m_debugTimer;
  std::atomic_bool m_showVideo = {false};
  std::atomic_bool m_displayLost = {false};
  std::atomic<unsigned int> m_resolutionDecisions{0};
  bool m_resolutionInFlight = false; //!< m_statelock: consumed, its count not yet bumped (5.B2-2)
  //! written under m_statelock: 5.B2-5b (r2 N5), an origin=rm set failed and its trigger was
  //! raised once more
  std::atomic_bool m_rmRetry{false};
  std::atomic_bool m_startHeld{false};
  //! start order design §4: written under m_statelock (the gate and its set by the player,
  //! the configure's tag by V or main, the decision by main)
  std::atomic_bool m_modeFirstGate{false};
  std::atomic<unsigned int> m_modeFirstGen{0};
#if defined(HAS_LIBAMCODEC)
  // 5.E2-5 (shadow): the compare slot (a leaf lock; nothing logs under it)
  void DecideShadowRecord(unsigned int gen, bool player, const DecideSide& side);
  std::mutex m_decideMutex;
  unsigned int m_decideGen = 0;
  std::optional<DecideSide> m_decideP;
  std::optional<DecideSide> m_decideM;
  //! RM's decision inputs as the set's configure left them (m_statelock), for P's read
  struct DecideFacts
  {
    unsigned int gen = 0;
    float fps = 0.0f;
    int width = 0;
    int height = 0;
    std::string stereoMode;
    StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
    bool armed = false;
    std::chrono::steady_clock::time_point armedAt;
  } m_decideFacts;
  //! 5.G-2: main's decision on the path session: facts, arm and inputs at its consume
  struct SMidShadow
  {
    uint64_t seq = 0;
    SStartFacts facts; //!< frac: the arm main consumed
    std::shared_ptr<const DISPLAYSTATE::DecideInputs> live; //!< main's own read; seq: published
    std::shared_ptr<const DISPLAYSTATE::DecideInputs> published; //!< what P would read
    std::shared_ptr<const DISPLAYSTATE::DisplayState> committed; //!< W's last commit
    RESOLUTION res = RES_INVALID;
    bool frac = false;
    const char* reply = "none";
    bool noChange = false;
  };
  std::deque<SMidShadow> m_midShadow; //!< m_statelock
  uint64_t m_midShadowSeq = 0; //!< m_statelock
  std::atomic<bool> m_midShadowReady{false};
#endif
  std::atomic<unsigned int> m_modeFirstCfgGen{0};
  std::atomic<unsigned int> m_modeFirstDecidedGen{0};
  //! a consumed trigger whose count has not landed
  std::atomic_bool m_modeFirstInFlight{false};
  bool ModeFirstTagged() const
  {
    return m_modeFirstGate && m_modeFirstCfgGen.load() == m_modeFirstGen.load();
  }
  void TagModeFirstConfigure();
  void CountModeFirstDecision(unsigned int gen);
  //! E2-8b D3 (m_statelock held): every trigger raise. Shut: a notice, no trigger (false)
  bool RaiseTriggerLocked(const char* kind);
  //! E2-8b D5b: one engaged start's outcome (m_statelock)
  struct SStartOutcome
  {
    unsigned int gen = 0; //!< 0: no record
    uint64_t T = 0;
    bool exited = false; //!< an exit wrote need (past the drop CAS)
    bool closed = false; //!< START, or an exit of a job not past its drop CAS
    TxNeed need = TxNeed::NONE;
    TxAnswer answer = TxAnswer::NONE;
    bool counted = false;
    bool handedBack = false; //!< RM set the trigger for main (hand-back or reraise)
    bool anchor = false; //!< the count sets the disc-mode anchor (the start's choice, D4)
  } m_txOutcome;
  //! E2-8b D5b: one table step's effect, applied after m_statelock
  struct STxResolution
  {
    const char* action = nullptr; //!< logged as the TXSTART outcome line's action
    std::string note;
    bool post = false; //!< VideoParamsChange
  };
  //! E2-8b D5b (m_statelock held): the table once both inputs are known (the provisional row
  //! with the start open); testbench/e28b_outcome_model.py's resolve()
  void ResolveTxOutcomeLocked(STxResolution& r);
  //! E2-8b D5b (m_statelock held): the recovery set's reply (the model's recovered())
  void RecoveredTxOutcomeLocked(STxResolution& r);
  //! E2-8b D5b (m_statelock held, code review A-M1): the recovery set failed: `failed`, the
  //! table's cell for the exit's need (none-needed / later / fresh: nothing, as recovered())
  void RecoveryFailedTxOutcomeLocked(STxResolution& r);
  void CountTxStartLocked(STxResolution& r, const char* action);
  void HandBackTxLocked(STxResolution& r, bool rmRetry);
  void LogTxOutcomeLocked(const STxResolution& r) const;
  //! E2-8b D4/D8 (m_statelock held): the frac arm DECIDE took, back with its original time
  void RestoreTxArmLocked(const char* why);
  std::atomic<unsigned int> m_txShutGen{0}; //!< written under m_statelock; 0 = main decides
  //! 5.G-3 (m_statelock): the shut is P's mid-play path session's (kept past START)
  bool m_txMidShut = false;
  bool m_txMidHandBack = false; //!< m_statelock: one consume handed back
  std::atomic_bool m_txMidHandBackRunning{false}; //!< main: the handed-back pass runs
  //! m_statelock held: "before START" (S2): a path session's shut is after START
  bool TxBeforeStartLocked() const { return m_txShutGen && !m_txMidShut; }
  unsigned int m_txShutLoggedGen = 0; //!< main: the shut-out line once per gen
  bool m_txOwedDecision = false; //!< m_statelock: D8's owed mark
  std::atomic<unsigned int> m_txOutputGen{0}; //!< m_statelock: W's OutputDone reached P
  std::atomic_bool m_txCommitInFlight{false}; //!< m_statelock: D5b's inflight
  std::vector<STxNotice> m_txNotices; //!< m_statelock
  std::atomic_bool m_txNoticed{false}; //!< 5.G-3: a notice queued since the last take
  struct
  {
    bool taken = false;
    std::chrono::steady_clock::time_point at;
  } m_txArmTaken; //!< m_statelock: DECIDE's arm take (E2-8b D4 step 4)
  // 5.E2-9c D3 (m_statelock)
  //! the gen a configuring Configure ran for (not RM:802's tag)
  unsigned int m_txConfiguredGen = 0;
  //! the hint job's gen while its first picture is owed (0: none); the re-clock arm waits
  unsigned int m_txHintPictureOwed = 0;
  SStartFacts m_txHintFacts;
  //! the clearing Configure's compare, read at START (D4, r5 M1)
  TxHintResult m_txHintCompare = TxHintResult::NONE;
  STxNotice m_txHintPicture;
  std::atomic<unsigned int> m_txLateContradicted{0};
  //! the late-picture ERROR (m_statelock)
  void LogTxLatePictureLocked(const char* when) const;
  //! the configuring Configure's clearing compare (fps: the rate it configures at)
  TxHintResult TxHintConfigureLocked(float fps, const char* path);
  bool m_graphicsWithheld = false; //!< render thread: withheld at the last FrameMove
  bool m_overlayVisible = false; //!< render thread: at the last GUI pass
  //! from a start the clock had yet to make until its display phase is measured:
  //! no early release (it would put a frame up ahead of the start)
  bool m_startGate = false;
  std::atomic_bool m_sessionModeDecided{false}; //!< this file's first output mode is set
  std::atomic<unsigned int> m_incomingSegmentGen{0}; //!< generation of the pictures queued next

  enum EPRESENTSTEP
  {
    PRESENT_IDLE     = 0
  , PRESENT_FLIP
  , PRESENT_FRAME
  , PRESENT_FRAME2
  , PRESENT_READY
  };

  enum EPRESENTMETHOD
  {
    PRESENT_METHOD_SINGLE = 0,
    PRESENT_METHOD_BLEND,
    PRESENT_METHOD_BOB,
  };

  enum ERENDERSTATE
  {
    STATE_UNCONFIGURED = 0,
    STATE_CONFIGURING,
    STATE_CONFIGURED,
  };
  ERENDERSTATE m_renderState = STATE_UNCONFIGURED;
  CEvent m_stateEvent;

  /// Display latency tweak value from AdvancedSettings for the current refresh rate
  /// in milliseconds
  double m_latencyTweak = 0.0;
  std::atomic<double> m_latencyTweakMs{0.0};
  std::atomic<bool> m_tweakToAudio{false};
  /// Display latency updated in PrepareNextRender in DVD clock units, includes m_latencyTweak
  double m_displayLatency = 0.0;
  std::atomic_int m_videoDelay = {};

  int m_QueueSize = 2;
  int m_QueueSkip = 0;

  struct SPresent
  {
    double         pts;
    unsigned int   segmentGen; //!< disc segment generation of the picture (6.1)
    EFIELDSYNC     presentfield;
    EPRESENTMETHOD presentmethod;
    bool framedOverlays; //!< 5.V2-7: overlays below are the picture's
    uint64_t overlaySerial;
    OVERLAY::FrameOverlays overlays;
  } m_Queue[NUM_BUFFERS]{};
  //! 5.V3-5 (MC-24): main's copy of the pinned frame's fields, so C can release every discard
  //! on the path; written at FrameMove's pin under m_presentlock, read by main
  struct SPin
  {
    bool onPath = false;
    int index = -1;
    double pts = 0.0;
    EFIELDSYNC presentfield = FS_NONE;
    EPRESENTMETHOD presentmethod = PRESENT_METHOD_SINGLE;
    // the overlay copy's (the buffer can be V's again before it runs)
    bool framedOverlays = false;
    uint64_t overlaySerial = 0;
    unsigned int segmentGen = 0;
    OVERLAY::FrameOverlays overlays;
  } m_pin;
  EFIELDSYNC PinnedField() const;
  //! 5.V2-7, under m_presentlock: the last serial given; a flush drops overlays up to the floor
  uint64_t m_overlaySerial = 0;
  uint64_t m_overlayFloor = 0;
  uint64_t m_flushFloor = 0; // 5.V2-8: the serial at P's overlays-only post

  std::deque<int> m_free;
  std::deque<int> m_queued;
  std::deque<int> m_discard;

  std::unique_ptr<VideoPicture> m_pConfigPicture;

  VideoPicture m_picture{};

  float m_fps = 0.0;
  std::atomic<double> m_pictureDuration{0.0}; // of the last picture added
  unsigned int m_orientation = 0;
  int m_NumberBuffers = 0;
  int m_lateframes = -1;
  //! written by the stream thread (Configure), the render thread (count down)
  //! and the player thread (BeginFile)
  std::atomic<int> m_amdv_wait_delay{-1};
  //! 5.E2-7 (L-9): reconfigures of this RM, counted on the INFO line (V's Configure only)
  std::atomic<unsigned int> m_configureChanges{0};
  //! 5.E2-7 (D9): the owed VS10 epoch last logged as skipped
  std::atomic<uint64_t> m_p533Logged{0};
  double m_presentpts = 0.0;
  EPRESENTSTEP m_presentstep = PRESENT_IDLE;
  XbmcThreads::EndTime<> m_presentTimer;
  bool m_forceNext = false;
  int m_forceGen = -1; // a worker's forced start picture (m_presentlock)
  // 5.E2-6: the engaged open's picture gate (m_presentlock; gen also atomic for C's
  // m_statelock-section read)
  struct STxPictureGate
  {
    bool candidate = false;
    bool engaged = false;
    bool shut = false;
    bool accepted = false;
    double acceptedPts = DVD_NOPTS_VALUE;
    bool prerollShown = false;
    unsigned int prerollEpoch = 0;
    bool prerollLostLogged = false;
    bool prerollMismatchLogged = false;
    bool forceRefusedLogged = false;
    bool logFirstAfterOpen = false;
    //! 5.E2-11 D3.3: a Restore's re-show pending (its e; -1: none) and the start picture's pts
    int64_t reshowE = -1;
    double reshowStartPts = DVD_NOPTS_VALUE;
  } m_txGate;
  std::atomic<unsigned int> m_txGateGen{0};
  std::atomic_bool m_txHoldsDecoder{false};
  std::atomic_bool m_txStartHeld{false};
  std::atomic<int64_t> m_txStartHeldSince{0}; //!< steady_clock rep; 0 = bound not armed
  std::atomic_bool m_txStartHeldBoundLogged{false};
  std::atomic<int64_t> m_txGraphicsFromNs{0}; //!< 5.E2-9b D3: steady_clock ns; 0 = none
  std::atomic<unsigned int> m_txEngagedGen{0};
  std::atomic<unsigned int> m_txLegacyVerdict{0};
  bool m_txPrerollDue = false; //!< C: committed and not lost, read in the m_statelock section
  bool m_txPrerollThisTick = false; //!< C: this tick's flip is the pre-roll
  bool m_txFirstRelease = false; //!< C: the next release is the engaged open's first (no drop)
  //! 5.E2-6 (review L5): TXGATE lines written after the locks are released (a slow log write
  //! never holds C or V on m_presentlock)
  struct STxLog
  {
    std::vector<std::string> lines;
    ~STxLog();
  };
  //! m_presentlock held: resets the gate (logged with why when it was engaged)
  void ClearTxGateLocked(const char* why, STxLog& log);
  //! m_presentlock held: a flush or discard before the start: the start picture is re-accepted
  void TxReacceptLocked(const char* why, STxLog& log);
  //! m_presentlock held: PrepareNextRender's lines go to PresentTick's deferred log
  void TxLogLocked(std::string line);
  std::vector<std::string>* m_txLogSink = nullptr;
  unsigned int m_txTickEpoch = 0; //!< C: the tick's epoch, for PrepareNextRender's pre-roll
  std::atomic<int> m_forceDoneGen{-1};
  int m_workerWakeFd = -1; // m_presentlock
  int m_presentsource = -1;
  int m_presentsourcePast = -1;
  XbmcThreads::ConditionVariable m_presentevent;
  CEvent m_flushEvent;
  CEvent m_initEvent;
  CDVDClock &m_dvdClock;
  IRenderMsg *m_playerPort;
  int m_render_timeout;

  struct CClockSync
  {
    void Reset();
    double m_error = 0.0;
    double m_ref = 0.0;
    bool m_refValid = false;
    bool m_adjustSeeded = false;
    int m_disabledFrames = 0;
    int m_idleMoves = 0; // FrameMoves with sync on and no frame prepared
    // phase seed: the current block of samples, and the previous block's mean
    double m_seedSum = 0.0;
    int m_seedCount = 0;
    double m_seedPrev = 0.0;
    bool m_seedPrevValid = false;
    unsigned int m_phaseGeneration = 0; // the clock's, when this state was built
    unsigned int m_landingGeneration = 0; // the grid landing last verified
    bool m_landingDeparted = false;
    int m_errCount = 0;
    double m_syncOffset = 0.0;
    bool m_enabled = false;
  };
  CClockSync m_clockSync;

  // real_player Phase 0: X = vsync-wait return -> clock sample, split by
  // whether the previous loop swapped a GUI frame
  struct CSampleOffsetStats
  {
    void Add(double us, bool guiRendered);
    void AddPhase(double err); // the unwrapped phase sample the vsync adjust averages
    struct Bucket
    {
      int n = 0;
      double sum = 0.0, sumSq = 0.0, min = 0.0, max = 0.0;
      int phaseN = 0;
      double phaseSum = 0.0;
    };
    Bucket m_bucket[2];
    int m_total = 0;
    int m_last = 0;
  };
  CSampleOffsetStats m_sampleOffset;

  // steady_clock: differenced only to bound the wait for the video layer to
  // start, so a wall-clock step must not be able to expire it early.
  std::chrono::time_point<std::chrono::steady_clock> m_videostarted;
  // the content-start re-clock arm, under m_statelock
  bool m_reclockPending = false;
  bool m_reclockArmed = false;
  std::chrono::time_point<std::chrono::steady_clock> m_reclockArmedAt;
  void PromoteReclockArm();
  std::atomic<bool> m_displayReset{false};

  CPresentationCoordinator* m_coordinator = nullptr; // the win system's; AML builds only
  bool m_coordinatorDelivers = false; //!< its vblank events arrive
  bool m_coordinatorStopped = false; //!< StopCoordinator ran: no attach until the next PreInit
  unsigned int m_coordinatorStops = 0; //!< bumped by StopCoordinator; an attach across one is undone
  //! the coordinator releases the video frames; set in Configure() under all locks
  std::atomic_bool m_presenterMode{false};
  std::atomic_bool m_decoderSideOnV{false};
  std::atomic<unsigned int> m_decoderSettingsGen{0};
  void DecoderSettingsChanged(); // under m_statelock
  bool GeometryTickSession() const { return m_decoderSideOnV && m_presenterMode; }
  //! 5.G2 GT-4 (on V, after a configure): the route decision; nullptr: on
  const char* GraphicsTickRefusal();
  void UpdateGraphicsRoute();
  //! the file the route was last on in
  unsigned int m_gtickOnFile = 0;
  //! 5.V3-4: the view mode request C computes from (m_statelock)
  SViewModeRequest m_viewModeRequest;
  void RecordViewMode(int viewMode, const CVideoSettings& settings,
                      const char* site); // under m_statelock
  unsigned int m_viewModeFileGen = 0; //!< 5.V3-7: the file whose request is in (m_statelock)
  void FillGeometryIn(SGeometryIn& in); // under m_statelock
  //! C's renderer (m_statelock): moved only by C's PresentTick and cleared by UnInit, so a
  //! copy C holds in its tail is never the last; a stale one waits in m_cReleased, reset by V
  //! at its next swap and by UnInit (never m_retire: no EndRenderSession)
  std::shared_ptr<CBaseRenderer> m_cRenderer;
  std::vector<std::shared_ptr<CBaseRenderer>> m_cReleased;
  //! decoders C used, reset by V (WorkerDuties) and UnInit: never the last owner on C
  std::vector<std::shared_ptr<CAMLCodec>> m_codecReleased; // m_presentlock
  void ReleaseCodecFromC(std::shared_ptr<CAMLCodec>&& codec);
  //! 5.V3-4 (D7): the renderer's rects, C's published ones on the path
  void RendererVideoRect(const CBaseRenderer& renderer, CRect& source, CRect& dest, CRect& view) const;
  bool m_videoRectOnC = false; //!< main: this pass reads C's rects (set in Render)
  uint64_t m_rgenMismatch = 0; //!< main: passes refused for C's rgen
  //! the source the render thread snapshotted in FrameMove; not freed until
  //! the next FrameMove
  int m_renderSource = -1;
  std::atomic<float> m_timingFps{60.0f};
  std::atomic<double> m_timingLatencyMs{0.0}; //!< latency tweak + display latency
  std::atomic<int> m_presentVblanks{-1};
  std::atomic<unsigned int> m_timingEpoch{0}; //!< display epoch it was published in
  //! displaystate on: main's last publish and the timing inputs' counter it read first, as one
  //! record (seqlock; writers serialised by writeLock, which the coordinator never takes)
  struct STimingSnapshot
  {
    std::mutex writeLock;
    uint32_t writerSeq = 0;
    std::atomic<uint32_t> seq{0};
    std::atomic<float> fps{60.0f};
    std::atomic<double> latencyMs{0.0};
    std::atomic<uint64_t> inputsGen{UINT64_MAX}; //!< none published yet
    std::atomic<uint64_t> commitCount{UINT64_MAX}; //!< DISPLAYSTATE::CommitCount, read first
  } m_timingSnapshot;
  void WriteTimingSnapshot(float fps, double latencyMs, uint64_t inputsGen, uint64_t commitCount);
  //! false on a read that overlapped a write
  bool ReadTimingSnapshot(STickTiming& timing, uint64_t& inputsGen, uint64_t& commitCount) const;
  //! 5.B2-3's switch, read at construction
  bool m_commitTiming = false;
  //! 5.B2-3: what a publish now would compose, from the committed mode (coordinator thread);
  //! inputsGen: the timing inputs' counter, read first
  bool ComposeCommitTiming(STickTiming& timing, uint64_t& inputsGen) const;
  //! ticks presented with the snapshot for one display epoch (coordinator thread)
  struct SAheadTicks
  {
    unsigned int epoch = 0;
    int ticks = 0;
    int64_t sinceNs = 0; //!< steady_clock
    const char* holdSite = nullptr;
    int holdTid = 0;
    STickTiming timing{};
    uint64_t inputsGen = 0;
    uint64_t commits = 0; //!< 5.B2-3: the W commit composed from; 0 = main's snapshot
  } m_ahead;
  static void LogAheadTicks(const SAheadTicks& ahead, int64_t endNs, const char* end);
  //! ticks refused for one display epoch, logged once when that epoch ends (coordinator thread)
  struct SRefusedTicks
  {
    unsigned int epoch = 0;
    int ticks = 0;
    int64_t sinceNs = 0; //!< steady_clock, the first refused tick
    const char* holdSite = nullptr; //!< the hold whose release started the epoch; null = unknown
    int holdTid = 0;
    int64_t holdReleaseNs = 0; //!< steady_clock
  } m_refused;
  //! 5.B2-3: ticks presented with an fps other than the committed mode's (coordinator thread)
  struct SFpsMismatch
  {
    unsigned int epoch = 0;
    int ticks = 0;
    int64_t sinceNs = 0;
    float presented = 0.0f;
    float committed = 0.0f;
  } m_fpsMismatch;
  //! the run this tick ends, and the one it begins (ticks 0 = none), logged after the locks
  void CheckCommittedFps(const SPresentTick& tick,
                         const STickTiming* timing,
                         SFpsMismatch& ended,
                         SFpsMismatch& begun);
  static void LogFpsMismatch(const SFpsMismatch& run, int64_t endNs, bool ended);
  //! counts a refused tick; returns the refusal of an earlier epoch it ends (ticks 0 = none)
  SRefusedTicks NoteRefusedTick(const SPresentTick& tick, int64_t nowNs);
  static void LogRefusedTicks(const SRefusedTicks& refused, int64_t endNs, bool published);
  //! Phase 4 shadow: the last frame released while playing, and its vblank
  //! (coordinator thread only)
  bool m_shadowValid = false;
  double m_shadowPts = 0.0;
  int64_t m_shadowReleaseNs = 0;
  int64_t m_prepareAnchorNs = 0; //!< the vblank time PrepareNextRender selected by
  unsigned int m_shadowGeneration = 0;
};
