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
#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRenderer.h"
#include "cores/VideoSettings.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/SystemClock.h"
#include "utils/Geometry.h"
#include "windowing/Resolution.h"

#include <atomic>
#include <deque>
#include <list>
#include <memory>

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
struct VideoPicture;

class CWinRenderer;
class CLinuxRenderer;
class CLinuxRendererGL;
class CLinuxRendererGLES;
class CRenderManager;

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
  void TriggerUpdateResolution(float fps, int width, int height, std::string &stereomode);
  void SetViewMode(int iViewMode);
  void PreInit();
  void UnInit();
  bool Flush(bool wait, bool saveBuffers);
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
  bool AddVideoPicture(const VideoPicture& picture, volatile std::atomic_bool& bStop, EINTERLACEMETHOD deintMethod, bool wait);
  void AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts);
  void ShowVideo(bool enable);
  void SetDisplayLost(bool lost) { m_displayLost = lost; }
  //! real_player E1: output-mode decisions made so far (UpdateResolution passes)
  unsigned int GetResolutionDecisions() const { return m_resolutionDecisions; }
  bool IsResolutionUpdatePending() const { return m_bTriggerUpdateResolution; }
  //! real_player E1: the player holds the start for the output mode; its first
  //! picture is decoded, so the mode decision need not wait for it on screen
  void SetStartHeld(bool held) { m_startHeld = held; }
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
  int WaitForBuffer(volatile std::atomic_bool& bStop,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

  /**
   * Can be called by player for lateness detection. This is done best by
   * looking at the end of the queue.
   */
  bool GetStats(int &lateframes, double &pts, int &queued, int &discard);

  /**
   * Video player call this on flush in order to discard any queued frames
   */
  void DiscardBuffer();

  void SetDelay(int delay) { m_videoDelay = delay; }
  int GetDelay() { return m_videoDelay; }

  void SetVideoSettings(const CVideoSettings& settings);

protected:

  void RenderWithoutPicture(bool gui, bool configured);
  void PresentHdrGraphics(int idx, const CRect& source, const CRect& dest, const CRect& view);
  bool GraphicsWithheld();

  void PresentSingle(bool clear, DWORD flags, DWORD alpha);
  void PresentFields(bool clear, DWORD flags, DWORD alpha);
  void PresentBlend(bool clear, DWORD flags, DWORD alpha);

  //! vblankNs: the hardware vblank the coordinator woke on (0 = none), or
  //! -1 on the render thread, which asks the win system
  void PrepareNextRender(int64_t vblankNs);
  bool IsPresenting();
  bool IsGuiLayer();

  bool Configure();
  void CreateRenderer();
  void DeleteRenderer();

  //! Video-only tap: serve VIDEO capture requests from the just-presented frame.
  void ServiceVideoCaptures();

  void UpdateLatencyTweak();
  void CheckEnableClockSync();
  //! render thread: the display timing PrepareNextRender uses, for either thread
  void PublishDisplayTiming();

  // real_player: the presentation coordinator's per-vsync step
  void PresentTick(const SPresentTick& tick, SPresentResult& result);
  void ShadowReference(SPresentResult& result, bool forced);
  void StartCoordinator();
  void StopCoordinator();

  CBaseRenderer *m_pRenderer = nullptr;
  //! Owns the video tap's private FBO; render-thread only, reset in UnInit
  std::unique_ptr<KODI::RENDERING::CAPTURE::CCaptureBlit> m_captureBlit;
  OVERLAY::CRenderer m_overlays;
  CDebugRenderer m_debugRenderer;
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
  std::atomic_bool m_startHeld{false};
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
  } m_Queue[NUM_BUFFERS]{};

  std::deque<int> m_free;
  std::deque<int> m_queued;
  std::deque<int> m_discard;

  std::unique_ptr<VideoPicture> m_pConfigPicture;

  VideoPicture m_picture{};

  float m_fps = 0.0;
  unsigned int m_orientation = 0;
  int m_NumberBuffers = 0;
  int m_lateframes = -1;
  int m_amdv_wait_delay = -1;
  double m_presentpts = 0.0;
  EPRESENTSTEP m_presentstep = PRESENT_IDLE;
  XbmcThreads::EndTime<> m_presentTimer;
  bool m_forceNext = false;
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
  std::atomic<bool> m_displayReset{false};

  CPresentationCoordinator* m_coordinator = nullptr; // the win system's; AML builds only
  bool m_coordinatorDelivers = false; //!< its vblank events arrive
  //! the coordinator releases the video frames; set in Configure() under all locks
  bool m_presenterMode = false;
  //! the source the render thread snapshotted in FrameMove; not freed until
  //! the next FrameMove
  int m_renderSource = -1;
  std::atomic<float> m_timingFps{60.0f};
  std::atomic<double> m_timingLatencyMs{0.0}; //!< latency tweak + display latency
  std::atomic<int> m_presentVblanks{-1};
  std::atomic<unsigned int> m_timingEpoch{0}; //!< display epoch it was published in
  //! Phase 4 shadow: the last frame released while playing, and its vblank
  //! (coordinator thread only)
  bool m_shadowValid = false;
  double m_shadowPts = 0.0;
  int64_t m_shadowReleaseNs = 0;
  int64_t m_prepareAnchorNs = 0; //!< the vblank time PrepareNextRender selected by
  unsigned int m_shadowGeneration = 0;
};
