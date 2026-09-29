/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/Thread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
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
  explicit CPresentationCoordinator(int masterFd);
  ~CPresentationCoordinator() override;

  bool Start();
  void Stop();
  //! the running coordinator, or nullptr
  static CPresentationCoordinator* Get();

  //! Starts per-vsync presentation steps for the render manager; true once a
  //! vblank event has arrived to drive them.
  bool AttachVideo(CRenderManager* renderManager);
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
   * on screen). Takes ownership of req and fence.
   */
  uint64_t Submit(int plane, gbm_bo* bo, uint32_t fb, drmModeAtomicReqPtr req, int fence);
  uint64_t SubmitUi(gbm_bo* bo, uint32_t fb, drmModeAtomicReqPtr req, int fence)
  {
    return Submit(PLANE_UI, bo, fb, req, fence);
  }
  //! Waits until that submit leaves the queue: committed, replaced or dropped.
  bool WaitTaken(uint64_t seq, std::chrono::milliseconds timeout);
  //! Buffers no longer on screen, for their producer to release.
  void TakeReturned(int plane, std::vector<gbm_bo*>& returned);
  void TakeReturned(std::vector<gbm_bo*>& returned) { TakeReturned(PLANE_UI, returned); }
  //! Before a plane's surface goes: every buffer comes back (display fence held).
  void DetachSurface(int plane);
  void DetachUiSurface() { DetachSurface(PLANE_UI); }
  /*!
   * \brief Switches the graphics plane off, in a blocking commit built by
   * build(fb of the GUI plane on screen) while the coordinator is parked, and
   * takes its buffers back. False when there was nothing to carry it.
   */
  bool DisableGraphicsPlane(const std::function<drmModeAtomicReqPtr(uint32_t)>& build);

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
  };

  struct PlaneState
  {
    UiBuffer ready;
    bool inCommit = false; //!< part of the commit in flight
    gbm_bo* committedBo = nullptr; //!< what that commit puts on screen (null: properties only)
    uint32_t committedFb = 0;
    gbm_bo* onScreen = nullptr;
    uint32_t onScreenFb = 0;
    std::vector<gbm_bo*> returned;
    uint64_t generation = 0; //!< surfaces detached so far
  };

  struct Committing
  {
    drmModeAtomicReqPtr req = nullptr;
    bool has[PLANE_COUNT] = {};
    gbm_bo* bo[PLANE_COUNT] = {};
    uint64_t generation[PLANE_COUNT] = {};
  };

  void QueueVblank(unsigned int epoch);
  void HandleEvents(int fd, SPresentTick& tick, bool& gotTick, unsigned int epoch);
  void CommitUi();
  void CommitWorker();
  void OnFlip(uint64_t tag);
  void Drop(int plane, UiBuffer& buffer); // under m_uiMutex
  //! under a hold: a commit still marked in flight lost its flip (under m_uiMutex)
  void AbandonLostCommit(int detached);
  void ReclaimPlane(int plane); // under m_uiMutex
  bool UiInFlight();
  void RunVideoTick(SPresentTick& tick);
  void SetState(State state, unsigned int epoch);
  void Account(const SPresentTick& tick, const SPresentResult& result, int64_t workNs);
  void LogReport();

  const int m_masterFd;
  int m_vblankFd = -1;
  int m_wakeFd = -1;
  std::atomic<uint32_t> m_crtc{0};
  bool m_eventPending = false;
  int64_t m_queuedNs = 0;
  uint64_t m_lastSeq = 0;
  int64_t m_lastTickNs = 0;
  State m_state = State::IDLE;

  std::mutex m_videoMutex; // held across a presentation step
  CRenderManager* m_video = nullptr;
  std::atomic<bool> m_videoAttached{false};
  std::mutex m_vblankMutex;
  std::condition_variable m_vblankCond;
  int64_t m_lastVblankNs = 0;

  std::mutex m_uiMutex;
  std::condition_variable m_uiCond;
  PlaneState m_planes[PLANE_COUNT];
  uint64_t m_submitSeq = 0;
  uint64_t m_commitTagSeq = 0;
  bool m_inFlight = false; //!< one commit in flight across both planes (R1)
  uint64_t m_inCommitTag = 0;
  Committing m_committing; //!< handed to the commit worker, ioctl not yet returned
  bool m_commitStop = false;
  std::thread m_commitThread;
  bool m_flipLostLogged = false;
  int64_t m_commitNs = 0;
  bool m_graphicsOn = false; //!< the graphics plane is enabled on screen
  bool m_graphicsRejected = false; //!< a commit carrying it failed: left out from then on

  struct Report
  {
    int ticks = 0;
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
    //! Phase 4 shadow, ms: audio's reference minus the presentation reference
    int shadowN = 0;
    int shadowFolds = 0; //!< samples more than half a frame apart
    double shadowSum = 0.0, shadowSumSq = 0.0, shadowMin = 0.0, shadowMax = 0.0;
    double shadowAdjustSum = 0.0;
    int shadowUnsynced = 0; //!< samples with clock sync off, not in the stats
  } m_report;
  int m_shadowFrames = 0; //!< whole frames between the references, as last logged
  int m_shadowCandidate = 0; //!< a different whole-frame count, and how long it held
  int m_shadowCandidateN = 0;
  int m_kernelDrops = -1;
};
