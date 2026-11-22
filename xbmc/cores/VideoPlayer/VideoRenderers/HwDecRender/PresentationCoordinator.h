/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/VideoRenderers/HdrGraphics.h"
#include "threads/Thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class CRenderManager;
struct SPresentTick;
struct SPresentResult;
struct gbm_bo;
typedef struct _drmModeAtomicReq* drmModeAtomicReqPtr;

/*!
 * \brief real_player presentation coordinator (docs/presentation_planes_design.md,
 * samurihl tree). One per process, owned by the win system: the only thread that
 * commits the GUI plane, and, while a render manager is attached, the one that
 * releases video frames on every hardware vblank (CRenderManager::PresentTick).
 * It never waits on another thread; the display executor parks it with the
 * display fence (aml_presenter_hold_acquire), which it acknowledges only with no
 * flip in flight.
 */
class CPresentationCoordinator : private CThread
{
public:
  //! 5.E2-6 (r2 L5): C's state as last set, any thread
  static const char* PublishedStateName();
  /*!
   * \param tickFd the timekeeper's tick eventfd and tickCrtc its CRTC: with
   * them, video ticks come from the timeline while the CRTC presenting is that
   * one, and the coordinator queues its own vblank events otherwise.
   */
  explicit CPresentationCoordinator(int masterFd, int tickFd = -1, uint32_t tickCrtc = 0);
  ~CPresentationCoordinator() override;

  bool Start();
  void Stop();
  //! the running coordinator, or nullptr
  static CPresentationCoordinator* Get();

  //! Starts per-vsync presentation steps for the render manager; true once a
  //! vblank event has arrived to drive them.
  bool AttachVideo(CRenderManager* renderManager);
  //! a timekeeper started after this coordinator; used from the next attach
  void SetTickSource(int tickFd, uint32_t tickCrtc);
  //! Returns with no presentation step running.
  void DetachVideo(CRenderManager* renderManager);

  enum Plane
  {
    PLANE_UI = 0, //!< the GUI plane (primary)
    PLANE_GRAPHICS, //!< menus and subtitles (overlay, under the GUI plane)
    PLANE_COUNT
  };

  /*!
   * \brief Hand over a locked buffer for a plane. The coordinator commits it
   * once its GPU fence has signalled and no flip is in flight, together with
   * whatever the other plane has ready; a newer submit replaces one not yet
   * committed. bo may be null for a property-only change (fb is then the one
   * on screen). Takes ownership of req and fence. pts: the video frame the
   * content goes with, for the placement count (graphics only). epoch: the
   * UI surface's (5.B2-4); a UI submit below the current UI epoch is refused:
   * 0 is returned, req and fence are freed and bo stays the caller's.
   */
  uint64_t Submit(int plane,
                  gbm_bo* bo,
                  uint32_t fb,
                  drmModeAtomicReqPtr req,
                  int fence,
                  double pts = DVD_NOPTS_VALUE,
                  uint64_t epoch = 0);
  uint64_t SubmitUi(gbm_bo* bo, uint32_t fb, drmModeAtomicReqPtr req, int fence, uint64_t epoch)
  {
    return Submit(PLANE_UI, bo, fb, req, fence, DVD_NOPTS_VALUE, epoch);
  }
  //! Waits until that submit leaves the queue: committed, replaced or dropped.
  bool WaitTaken(uint64_t seq, std::chrono::milliseconds timeout);
  //! Buffers no longer on screen, for their producer to release.
  void TakeReturned(int plane, std::vector<gbm_bo*>& returned);
  //! the GUI plane's, each with the UI epoch it was submitted with
  void TakeReturned(std::vector<gbm_bo*>& returned, std::vector<uint64_t>& epochs);
  //! Before a plane's surface goes: every buffer comes back (display fence held).
  void DetachSurface(int plane);
  void DetachUiSurface() { DetachSurface(PLANE_UI); }
  //! 5.B2-4: DetachUiSurface, then the UI epoch steps; format: the GUI surface's, for
  //! C's cleared buffer. Returns the new epoch.
  uint64_t DetachUiSurfaceNewEpoch(uint32_t format);
  uint64_t UiEpoch();
  //! 5.B2-4: a GUI buffer of an epoch below this one is still queued, committed or on
  //! screen (C's own buffer is not counted)
  bool HoldsUiEpochBelow(uint64_t epoch);
  /*!
   * \brief 5.B2-4, on W in a mode set's job while parked: C's cleared GUI-size buffer
   * (made only when the size changes) and its flip request for this set, built by
   * build(fb, src width, src height) under the display's lock. C commits it once, at
   * its first slot after the unpark, unless a GUI buffer of the epoch is there first.
   */
  void PrepareClearedUi(int width,
                        int height,
                        uint64_t epoch,
                        const std::function<drmModeAtomicReqPtr(uint32_t, int&, int&)>& build);
  /*!
   * \brief 5.B2-5b (rule S1'), in a mode set's job while W holds the park, on W or (5.B2-7)
   * on C (no hold taken here): the GUI plane off in off, a blocking commit made with m_uiMutex released
   * (built by the caller under the display's lock, ownership taken; null: none could be
   * built), every GUI buffer back to returned, and the UI epoch set to newEpoch, from which
   * Submit refuses older buffers. False when the off was refused or not built: the books are
   * cleared anyway (R4-M2), the old buffer may still be scanned out (the SB3 state).
   * format: the GUI surface's, for C's cleared buffer (0: unchanged). newEpoch is set to the
   * epoch taken (always above the current one). acked: false when W's park was not
   * acknowledged twice; the reclaim then runs unparked, as today's DetachSurface does after its
   * park timeout. Serialised with DisableGraphicsPlane (its off carries the GUI plane's fb).
   */
  bool ReclaimUiForModeSet(uint64_t& newEpoch,
                           drmModeAtomicReqPtr off,
                           uint32_t format,
                           bool acked = true);
  //! 5.B2-5b (R4-M3), main, at the bound of its handler's wait: every GUI buffer below epoch
  //! back to returned, on screen or not; no ioctl
  void ForgetUiEpoch(uint64_t epoch);
  /*!
   * \brief Switches the graphics plane off, in a blocking commit built by
   * build(fb of the GUI plane on screen) while the coordinator is parked, and
   * takes its buffers back. False when there was nothing to carry it.
   */
  bool DisableGraphicsPlane(const std::function<drmModeAtomicReqPtr(uint32_t)>& build);
  /*!
   * \brief 5.B2-7 (OUTPUT::PlaneOffOnC): DisableGraphicsPlane's off without a hold, run by C
   * on its own thread (see RunPlaneOffs); who names the caller. Inline DisableGraphicsPlane
   * when off, when C is not running, and when C did not answer in 2 s.
   */
  bool SwitchGraphicsOff(const std::function<drmModeAtomicReqPtr(uint32_t)>& build,
                         const char* who);
  /*!
   * \brief In a mode set's job on W, before the commit: the graphics plane off (graphicsOff
   * empty: no graphics plane), then rule S1''s reclaim (ReclaimUiForModeSet with uiOff()'s
   * request). 5.B2-7 and parked: both run by C in its held branch; what C has not started in
   * 2 s runs on W, under W's acknowledged park, with no further hold. Otherwise both inline
   * on W, as before 5.B2-7. Returns the reclaim's result.
   */
  bool PlanesOffForModeSet(const std::function<drmModeAtomicReqPtr(uint32_t)>& graphicsOff,
                           const std::function<drmModeAtomicReqPtr()>& uiOff,
                           uint64_t& newEpoch,
                           uint32_t format,
                           bool parked);
  //! the driver refused a commit carrying the graphics plane
  bool GraphicsRefused() const { return m_graphicsRejected; }
  //! 5.B3-10a: moves when C wants a fresh graphics frame for a suspect plane's retry
  uint64_t GraphicsResubmit() const { return m_gfxResubmit.load(); }
  //! GUNTIMED: special://profile/guntimed_log, read once at Start
  static bool UntimedLog();
  //! GUNTIMED: the plane's next submit carries these posts to the flip that shows them
  void NoteUntimed(int plane, std::vector<SUntimedPost> posts);

  //! 5.G2 GT-4 (G): one drawn composition for C's ready list
  struct STimedBuffer
  {
    gbm_bo* bo = nullptr;
    uint32_t fb = 0;
    drmModeAtomicReqPtr req = nullptr;
    int fence = -1;
    double pts = DVD_NOPTS_VALUE;
    uint64_t serial = 0; //!< the overlays of the pictures from serial on
    //! a cleared buffer after a flush or at the route's end, shown at C's next slot
    bool clear = false;
    bool visible = false;
    //! the untimed images alone (no picture's overlays), shown at the next slot as a clear is,
    //! under no floor
    bool bridge = false;
    uint64_t session = 0; //!< G's route session it was drawn for (GT-D12)
    // GT-D15 measurement (log only, GPLACE): G's libass render and draw, and its hand-in time
    int64_t assUs = -1;
    int64_t drawUs = -1;
    int64_t submitNs = 0; //!< CLOCK_MONOTONIC
    // GT-5 (design §10.2 item 5): the untimed state it carries, tagged {serial, u, regimeEpoch}
    uint64_t u = 0; //!< 0: none (GT-4's bridge, or nothing untimed drawn with it)
    bool firstPage = false; //!< a segment's first page with its frame: never dropped once picked
    uint64_t regimeEpoch = 0; //!< item 5 (h): G's regime when it drew it
    //! drawn on GT-5's untimed route: an untimed-only buffer waits for the start's hold
    bool untimedRoute = false;
  };
  /*!
   * \brief 5.G2 GT-4 (G): drawn timed compositions. C holds each (a newer one of the same
   * serial replaces it) and commits it at the tick that follows the release of a frame at or
   * past serial (item 3). All of them go in under one lock, so C's next selection sees all or
   * none. Ownership of each buffer as Submit.
   */
  void SubmitTimed(std::vector<STimedBuffer>& buffers);
  //! G: timed buffers C holds and has not committed
  int TimedHeld();
  //! 5.G2 GT-5 (G, with guntimed_log): untimed states G took, logged as GUNTIMED by state at the
  //! first flip of a buffer carrying them or a newer one
  void NoteUntimedRoute(std::vector<SUntimedPost> posts);
  //! 5.G2 GT-5: C's state is PAUSED (G's keep-alive TTL reads it, not IsPausedPlayback)
  static bool PublishedPaused();

protected:
  void Process() override;

private:
  enum class State
  {
    IDLE,
    PLAYING,
    PAUSED,
    DISPLAY_LOST,
    HELD,
  };

  struct UiBuffer
  {
    gbm_bo* bo = nullptr;
    uint32_t fb = 0;
    drmModeAtomicReqPtr req = nullptr;
    int fence = -1;
    uint64_t seq = 0;
    double pts = DVD_NOPTS_VALUE;
    uint64_t epoch = 0;
    bool cOwned = false; //!< C's cleared buffer (no bo, never returned)
    int64_t readyNs = 0; //!< 5.V3-1: committable from (its GPU fence signalled), monotonic
    std::vector<SUntimedPost> untimed; //!< GUNTIMED, incl. those of buffers it replaced
    // GT-4
    bool timed = false;
    uint64_t serial = 0;
    bool clear = false;
    bool bridge = false; //!< the untimed images alone
    bool visible = false;
    uint64_t session = 0; //!< G's route session (GT-D12)
    uint64_t u = 0; //!< GT-5: the untimed state it carries
    bool firstPage = false;
    uint64_t regimeEpoch = 0;
    bool untimedRoute = false;
    //! GT-5 late pick (review rA M2, rB M1): committed only up to this (monotonic), else it goes
    //! back to the ready list for the next slot; 0: a slot's pick, no deadline
    int64_t lateDeadlineNs = 0;
    //! GT-D15: picked at the first slot after C's start withhold (on screen at T0): its frame was
    //! released, and held as a still, before T0, so delta counts the hold
    bool afterHold = false;
    // GT-D15 measurement (log only)
    int64_t assUs = -1;
    int64_t drawUs = -1;
    int64_t submitNs = 0;
    int64_t pickVblankNs = 0; //!< the vblank of the slot that picked it
    int fenceAtPick = -1; //!< its GPU fence at the pick: 1 signalled, 0 pending, -1 none
    int64_t qbuf = -1; //!< of the frame it was picked for
    uint64_t target = 0; //!< that frame's first vblank on VD1
  };

  struct Returned
  {
    gbm_bo* bo;
    uint32_t fb;
    uint64_t epoch;
  };

  struct PlaneState
  {
    UiBuffer ready;
    bool inCommit = false; //!< part of the commit in flight
    gbm_bo* committedBo = nullptr; //!< what that commit puts on screen (null: properties only)
    uint32_t committedFb = 0;
    uint64_t committedEpoch = 0;
    bool committedCOwned = false;
    gbm_bo* onScreen = nullptr;
    uint32_t onScreenFb = 0;
    uint64_t onScreenEpoch = 0;
    std::vector<Returned> returned;
    uint64_t generation = 0; //!< surfaces detached so far
  };

  struct Committing
  {
    drmModeAtomicReqPtr req = nullptr;
    bool has[PLANE_COUNT] = {};
    gbm_bo* bo[PLANE_COUNT] = {};
    uint64_t generation[PLANE_COUNT] = {};
    double pts[PLANE_COUNT] = {DVD_NOPTS_VALUE, DVD_NOPTS_VALUE};
    bool cOwned = false; //!< carries C's cleared buffer
  };

  //! 5.B2-7: a plane off posted to C
  struct PlaneOffOp
  {
    bool ui = false; //!< S1''s reclaim; else the graphics plane off
    const char* who = "";
    std::function<drmModeAtomicReqPtr(uint32_t)> graphicsOff;
    std::function<drmModeAtomicReqPtr()> uiOff;
    uint64_t epoch = 0; //!< the UI epoch asked for, then taken
    uint32_t format = 0;
    int64_t postedNs = 0;
    bool uiFlipAtPost = false;
    enum class State
    {
      QUEUED,
      RUNNING,
      DONE,
      DROPPED, //!< C stopped first
    } state = State::QUEUED;
    bool result = false;
  };

  struct DumbBuffer
  {
    uint32_t handle = 0;
    uint32_t fb = 0;
    int width = 0;
    int height = 0;
  };

  //! the video frame on screen from a vblank on
  struct OnScreen
  {
    uint64_t fromSeq = 0;
    double pts = 0.0;
  };

  void QueueVblank(unsigned int epoch);
  void HandleEvents(int fd, SPresentTick& tick, bool& gotTick, unsigned int epoch);
  void ReadTimelineTick(SPresentTick& tick, bool& gotTick, unsigned int epoch);
  void NoteVblank(int64_t vblankNs);
  void CommitUi();
  //! 5.V3-1 (log only, under m_uiMutex on C's thread): a GUI plane commit against its vblank
  void TimeUiCommit(int64_t readyNs);
  void CommitWorker();
  //! flipNs: the flip's vblank time from the event (CLOCK_MONOTONIC)
  void OnFlip(uint64_t tag, unsigned int sequence, int64_t flipNs);
  //! graphics flipped at sequence: count its frame against the video frame shown
  void CountPlacement(double pts, unsigned int sequence);
  void Drop(int plane, UiBuffer& buffer); // under m_uiMutex
  //! under a hold: a commit still marked in flight lost its flip (under m_uiMutex)
  void AbandonLostCommit(int detached);
  void ReclaimPlane(int plane); // under m_uiMutex
  //! C's cleared buffer into the GUI plane's queue, for this pass (under m_uiMutex)
  void PlaceClearedUi();
  bool CreateDumb(int width, int height, uint32_t format, DumbBuffer& dumb);
  void FreeDumb(DumbBuffer& dumb);
  bool UiInFlight();
  //! the graphics plane off with its books, the caller parked or C itself (no hold)
  bool GraphicsPlaneOff(const std::function<drmModeAtomicReqPtr(uint32_t)>& build);
  //! posts ops to C and waits until deadline; false: not all ran (each op's state says which;
  //! a QUEUED one is no longer C's)
  bool RunByC(const std::vector<std::shared_ptr<PlaneOffOp>>& ops,
              std::chrono::steady_clock::time_point deadline);
  //! C's thread, held or after a tick's QBUF: the queued plane offs, only with no GUI flip in
  //! flight, so the blocking commit can delay C's next tick, never this one's release
  void RunPlaneOffs(bool held);
  //! true: a hold landed first, nothing was released
  bool RunVideoTick(SPresentTick& tick);
  //! GT-4 (C's thread): drops what a floor or a later pick made stale, and moves the buffer of
  //! the frame on screen next into the graphics plane's queue. ticked: after a video tick
  void SelectTimed(bool ticked);
  //! GT-5 (GT-D15, C's thread, a wake with no tick): the frame on screen next missed its slot
  //! and G has handed its buffer in since; picked if its commit can still land on that vblank
  void SelectTimedLate(int64_t now);
  //! under m_uiMutex: the untimed order of a picked buffer (untimedRegress, gnewest_off)
  void NotePickedU(const UiBuffer& chosen);
  //! under m_uiMutex: the graphics plane's ready late pick goes back to the list (its deadline
  //! passed)
  void GiveBackLatePick();
  bool m_gtSlotMissed = false; //!< C's thread: this interval's slot found no buffer for its frame
  // GT-5, under m_uiMutex: G's posts not shown yet; the newest state a flip showed, picked, and
  // whose first commit failed (its posts are logged "carried")
  std::deque<SUntimedPost> m_gtPosts;
  uint64_t m_gtShownU = 0;
  uint64_t m_gtPickedU = 0;
  uint64_t m_gtCarriedU = 0;
  //! under m_uiMutex: GT-1 r1 N1, the posts of a commit that failed or lost its flip
  void LogUntimedLost(std::vector<SUntimedPost>& posts, const char* why);
  //! GT-4: a released frame, for the pick at the next tick
  struct SReleasedFrame
  {
    uint64_t serial = 0;
    int64_t qbuf = -1;
    uint64_t tick = 0;
    bool valid = false;
  };
  SReleasedFrame m_gtReleased[2]; //!< [0] the newest, before this tick's
  SReleasedFrame m_gtTarget; //!< C's thread: the frame of this tick's pick
  bool m_gtWithheld = false;
  bool m_gtWasWithheld = false; //!< C's thread: the last ticked slot's withhold
  // GT-D15 measurement (C's thread, log only): when C last entered its poll, and how long the
  // loop body before it ran
  int64_t m_pollEnterNs = 0;
  int64_t m_pollReturnNs = 0;
  int64_t m_lastBodyNs = 0;
  int64_t m_inFlightCommitNs = 0; //!< the commit time of the commit in flight
  //! review rB m-1: the newest picture serial picked this route session; nothing older goes up
  uint64_t m_gtPickedSerial = 0;
  uint64_t m_gtSession = 0;
  void RunWakeTail(); //!< 5.V3-4
  bool m_geometryOnC = false; //!< 5.V3-4: the last tick's session is on the path (C only)
  void ShadowFrameOnScreen(const SPresentTick& tick, double pts, bool playing);
  void SetState(State state, unsigned int epoch);
  void Account(const SPresentTick& tick,
               const SPresentResult& result,
               int64_t workNs,
               int64_t releasedNs);
  void LogReport();

  const int m_masterFd;
  std::atomic<int> m_tickFd;
  std::atomic<uint32_t> m_tickCrtc;
  //! set at each attach: the timeline drives the video ticks
  std::atomic<bool> m_useTimeline{false};
  std::atomic<int64_t> m_attachNs{0};
  int64_t m_lastTimelineTick = -1;
  unsigned int m_seenEpoch = 0;
  int64_t m_seenEpochNs = 0; //!< when this thread first saw the current display epoch
  int m_vblankFd = -1;
  int m_wakeFd = -1;
  std::atomic<uint32_t> m_crtc{0};
  bool m_eventPending = false;
  int64_t m_queuedNs = 0;
  uint64_t m_lastSeq = 0;
  int64_t m_lastTickNs = 0;
  State m_state = State::IDLE;
  int m_shadowStartFrames = 0;

  std::mutex m_videoMutex; // held across a presentation step
  CRenderManager* m_video = nullptr;
  std::atomic<bool> m_videoAttached{false};
  std::mutex m_vblankMutex;
  std::condition_variable m_vblankCond;
  int64_t m_lastVblankNs = 0;

  //! 5.B2-5b (review S2): held across a plane-off's snapshot, blocking commit and books, by
  //! DisableGraphicsPlane and ReclaimUiForModeSet; taken before m_uiMutex. From 5.B2-7 C takes
  //! it too: no holder waits on C
  std::mutex m_planeOffMutex;
  std::mutex m_uiMutex;
  std::condition_variable m_uiCond;
  PlaneState m_planes[PLANE_COUNT];
  uint64_t m_submitSeq = 0;
  uint64_t m_commitTagSeq = 0;
  bool m_inFlight = false; //!< one commit in flight across both planes (R1)
  uint64_t m_inCommitTag = 0;
  Committing m_committing; //!< handed to the commit worker, ioctl not yet returned
  // GUNTIMED, under m_uiMutex: posts for each plane's next submit; those of the commit in
  // flight; the last flip event and the vblank period derived from flip events
  std::vector<SUntimedPost> m_untimedNext[PLANE_COUNT];
  std::vector<SUntimedPost> m_untimedInFlight;
  unsigned int m_untimedFlipSeq = 0;
  int64_t m_untimedFlipNs = 0;
  int64_t m_untimedPeriodNs = 0;
  bool m_commitStop = false;
  std::thread m_commitThread;
  bool m_flipLostLogged = false;
  // 5.B2-7, under m_uiMutex
  std::deque<std::shared_ptr<PlaneOffOp>> m_planeOffs;
  std::atomic<bool> m_planeOffOpen{false}; //!< C's loop runs and takes plane offs
  int m_uiDeferred = 0; //!< GUI commits left for a queued plane off
  // 5.B2-7, C's thread: the tick after a plane off, for its log line
  uint64_t m_planeOffSeq = 0;
  bool m_planeOffTickOwed = false;
  int64_t m_tickVblankNs = 0;
  uint64_t m_tickSeq = 0;
  int64_t m_tickPeriodNs = 0;
  int64_t m_commitNs = 0;
  double m_inFlightGfxPts = DVD_NOPTS_VALUE; //!< the graphics pts of the commit in flight
  // GT-4, under m_uiMutex: the timed buffers by serial, and the one in the commit in flight
  std::vector<UiBuffer> m_timed;
  UiBuffer m_inFlightTimed; //!< its tags only (bo, req and fence are the commit's)
  bool m_planeSubsVisible = false;
  std::vector<OnScreen> m_onScreen; //!< coordinator thread only
  double m_frameUs = 0.0;
  bool m_graphicsOn = false; //!< the graphics plane is enabled on screen
  //! a commit carrying it failed: left out from then on
  std::atomic<bool> m_graphicsRejected{false};
  // 5.B3-10a, under m_uiMutex: a refusal on a dark CRTC must not latch. A refused plane is held
  // out until a GUI-only commit succeeds, then retried with a fresh frame; the third refused
  // retry latches
  bool m_gfxSuspect = false;
  bool m_gfxRetryArmed = false;
  int m_gfxRetriesRefused = 0;
  int m_gfxSuspectLogs = 0;
  std::atomic<uint64_t> m_gfxResubmit{0};

  // 5.B2-4, under m_uiMutex: the UI surface epoch (0: off, or before the first set)
  // and C's cleared buffer
  uint64_t m_uiEpoch = 0;
  uint32_t m_uiFormat = 0;
  int m_uiRefused = 0;
  DumbBuffer m_cbuf;
  drmModeAtomicReqPtr m_cbufReq = nullptr; //!< single use: this set's flip request
  uint64_t m_cbufEpoch = 0;
  bool m_cbufUp = false; //!< C's buffer is the GUI plane's, as C's books have it
  int64_t m_cbufUpNs = 0;
  uint64_t m_uiFlips = 0;
  struct Retired
  {
    DumbBuffer buffer;
    uint64_t flips; //!< m_uiFlips when it was replaced
  };
  //! replaced at a GUI size change: freed after a later GUI plane flip
  std::vector<Retired> m_cbufRetired;

  static constexpr int RELEASE_BINS = 1000;
  struct Report
  {
    int ticks = 0;
    bool timeline = false; //!< the ticks came from the timeline
    int missed = 0; //!< vblanks between consecutive ticks
    int synthetic = 0;
    int stale = 0; //!< events from a previous display epoch, dropped
    int held = 0;
    int frames = 0;
    int repeats = 0; //!< playing, and no new frame
    int skipped = 0;
    int woke = 0;
    double wakeSum = 0.0, wakeMax = 0.0; //!< us after the hardware vblank
    double workSum = 0.0, workMax = 0.0; //!< us in the presentation step
    int uiSubmits = 0, uiReplaced = 0, uiCommits = 0, uiFailed = 0, uiFlips = 0,
        uiLostFlips = 0;
    double commitMax = 0.0; //!< us in the commit ioctl
    int commitHist[4] = {}; //!< ioctl time: <1 ms, <5 ms, <20 ms, >=20 ms
    double flipSum = 0.0, flipMax = 0.0; //!< us from commit to flip event
    int gfxSubmits = 0, gfxReplaced = 0, gfxCommits = 0; //!< graphics plane
    int gfxSuspect = 0; //!< 5.B3-10a: refusals held as suspect
    //! graphics flips by frames from their video frame: <-1, -1, 0, +1, >+1
    int gfxPlacement[5] = {};
    //! GT-4: timed buffers placed after their frame's first vblank; ticks whose frame G had not
    //! reached; timed buffers dropped unshown (replaced, passed, or under a floor)
    int gfxLate = 0, gfxMissing = 0, gfxTimedReplaced = 0, gfxTimed = 0;
    //! GT-5 (GT-D15): missed at the slot, picked when G handed it in before the next vblank
    int gfxLatePick = 0;
    //! late picks whose fence or a flight held them past their deadline: back to the next slot
    int gfxLatePickBack = 0;
    //! GT-D15: C's loop bodies [<1 ms, <5, <10, <20, >=20 ms] and the longest, us
    int bodyHist[5] = {};
    int64_t bodyMaxUs = 0;
    //! GT-5: item 5 (b) (ii) replaces, untimedRegress (must stay 0), item 5 (h) refusals,
    //! first pages flipped and those after their frame's vblank
    int untimedReplaced = 0, untimedRegress = 0, gfxEpochDrop = 0, firstPages = 0, pageLate = 0;
    //! Phase 4 shadow, ms: audio's reference minus the presentation reference
    int shadowN = 0;
    int shadowFolds = 0; //!< samples more than half a frame apart
    double shadowSum = 0.0, shadowSumSq = 0.0, shadowMin = 0.0, shadowMax = 0.0;
    double shadowAdjustSum = 0.0;
    int shadowUnsynced = 0; //!< samples with clock sync off, not in the stats
    //! 5.V3-1: GUI plane commits timed against the vblanks, and those issued after the first
    //! vblank that followed the submit becoming committable (shown a vblank late). Committable:
    //! its fence signalled, no commit in flight or in the ioctl, no plane off queued
    int uiTimed = 0, uiLate = 0;
    //! 5.V3-4 (ccost): the presentation step's end after the hardware vblank with a video
    //! configured, 10 us bins
    int releaseHist[RELEASE_BINS] = {};
    int releaseOver = 0; //!< at or past RELEASE_BINS * 10 us
    //! 5.V3-4: C's geometry tails (after the QBUF's release; not in work)
    int tails = 0;
    int tailWrites = 0;
    int tailsOverVblank = 0;
    int64_t tailMaxNs = 0;
    int wakeTails = 0;
    int wakeWrites = 0;
    int64_t wakeMaxNs = 0;
  } m_report;
  //! 5.V3-1 (under m_uiMutex): when CommitUi could last start a commit again (the flip in flight
  //! landed or was abandoned, a failed or returned ioctl, a plane off done), monotonic
  int64_t m_diagUnblockedNs = 0;
  //! 5.V3-1, C's thread: the last tick's vblank and the period between consecutive ones
  int64_t m_diagVblankNs = 0;
  uint64_t m_diagVblankSeq = 0;
  int64_t m_diagPeriodNs = 0;
  int m_shadowFrames = 0; //!< whole frames between the references, as last logged
  int m_shadowCandidate = 0; //!< a different whole-frame count, and how long it held
  int m_shadowCandidateN = 0;
  int m_kernelDrops = -1;
};
