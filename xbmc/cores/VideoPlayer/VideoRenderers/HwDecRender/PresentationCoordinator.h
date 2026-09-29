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
#include <condition_variable>
#include <cstdint>
#include <mutex>
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

  /*!
   * \brief Hand over a locked GUI buffer. The coordinator commits it once its
   * GPU fence has signalled and no flip is in flight; a newer buffer replaces
   * one not yet committed. Takes ownership of req and fence.
   */
  void SubmitUi(gbm_bo* bo, drmModeAtomicReqPtr req, int fence);
  //! Buffers no longer on screen, for the render thread to release.
  void TakeReturned(std::vector<gbm_bo*>& returned);
  //! Before the GUI surface goes: every buffer comes back (display fence held).
  void DetachUiSurface();

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
    drmModeAtomicReqPtr req = nullptr;
    int fence = -1;
  };

  void QueueVblank(unsigned int epoch);
  void HandleEvents(int fd, SPresentTick& tick, bool& gotTick, unsigned int epoch);
  void CommitUi();
  void OnFlip();
  void Drop(UiBuffer& buffer); // under m_uiMutex
  bool UiInFlight();
  void RunVideoTick(SPresentTick& tick);
  void SetState(State state, unsigned int epoch);
  void Account(const SPresentTick& tick, const SPresentResult& result, int64_t workNs);
  void LogReport();

  const int m_masterFd;
  int m_vblankFd = -1;
  int m_wakeFd = -1;
  uint32_t m_crtc = 0;
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
  UiBuffer m_ready;
  gbm_bo* m_inCommit = nullptr;
  int64_t m_commitNs = 0;
  gbm_bo* m_onScreen = nullptr;
  std::vector<gbm_bo*> m_returned;

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
    double flipSum = 0.0, flipMax = 0.0; //!< us from commit to flip event
  } m_report;
  int m_kernelDrops = -1;
};
