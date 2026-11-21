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
#include "windowing/amlogic/GraphicsRasterAML.h"
#include "windowing/amlogic/GraphicsTick.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include <GLES2/gl2.h>

#include <EGL/egl.h>

class CAMLDisplay;
class CDVDOverlayImage;
class CDVDSubtitlesLibass;
class CPresentationCoordinator;
struct gbm_bo;
struct gbm_device;
struct gbm_surface;
typedef struct _drmModeAtomicReq* drmModeAtomicReqPtr;

namespace KODI::UTILS::EGL
{
class CEGLFence;
}

/*!
 * \brief real_player graphics plane (docs/presentation_planes_design.md §17,
 * samurihl tree): the producer for osd1, under the GUI plane. It renders on
 * its own thread, in its own ES context (not shared with the GUI's) on the
 * GUI's EGLDisplay, into a fixed 1920x1080 GBM surface, and hands finished
 * buffers to the presentation coordinator, which commits them with the GUI
 * plane. It releases its own buffers and never holds more than four (R5).
 * It shows the HDR disc graphics the GUI's video pass hands over (step 5b),
 * or a test pattern; with nothing to show it idles enabled with a cleared
 * (transparent) buffer.
 */
class CGraphicsPlaneAML : private CThread
{
public:
  CGraphicsPlaneAML(EGLDisplay display,
                    gbm_device* device,
                    int drmFd,
                    CAMLDisplay* amlDisplay,
                    CPresentationCoordinator* coordinator,
                    bool testPattern);
  ~CGraphicsPlaneAML() override;

  bool Start();
  //! Switches the plane off and frees its surface; the coordinator must still run.
  void Stop();
  //! Switches the plane off while the GUI plane still shows a buffer to carry
  //! the commit; the next frame switches it on again. who: the caller, for C's log.
  void SwitchOff(const char* who);
  //! true once it can take graphics over from the GUI plane
  bool CanShow() const;
  //! shows these graphics from the next frame on (an empty set hides the plane). On the GT-4
  //! route: the untimed images, drawn over each timed composition
  void Show(SHdrGraphics graphics);
  //! 5.G2 GT-4 (any thread): why it cannot draw the timed graphics; nullptr: it can
  const char* TickRefusal() const;

protected:
  void Process() override;

private:
  struct STexture
  {
    std::shared_ptr<CDVDOverlay> image; //!< keeps the key alive
    GLuint texture = 0;
    int width = 0; //!< of the texture: the visible box of the image
    int height = 0;
    CRect crop; //!< that box in the image, empty when it is the whole image
  };

  bool InitGL();
  void DeinitGL();
  bool InitProgram();
  //! true when there was a new set to take
  bool TakeShown();
  void UpdateTextures();
  void DrawGraphics();
  //! where the image goes on the plane, in plane pixels, over the video rects of g
  CRect Place(const CDVDOverlayImage& image, const STexture& texture, const SHdrGraphics& g) const;
  //! draws and submits a frame; false when it could not be handed over. pts:
  //! the video frame a new composition goes with (placement count only)
  bool SubmitFrame(bool visible, int64_t nowNs, double pts = DVD_NOPTS_VALUE);
  //! after a draw: the swap, the buffer and its flip request; false leaves nothing locked
  bool TakeFrame(gbm_bo*& bo, uint32_t& fb, drmModeAtomicReqPtr& req, int& fence);
  //! re-commits what is on screen with the current geometry (R6)
  bool SubmitGeometry();
  void ReleaseReturned();
  uint32_t FbFromBo(gbm_bo* bo);
  //! 5.G2 GT-3: builds G's renderer when graphicstick_on or gselftest asks for it (M5)
  void InitRaster();
  //! gselftest: m_shown through G's renderer offscreen, timed; every Nth against the reference
  void ShadowDraw();

  // 5.G2 GT-4: the timed graphics, drawn ahead and picked by C per tick
  //! one picture's overlays as drawn: what a later picture is compared with
  struct SComposition
  {
    uint64_t serial = 0;
    double pts = 0.0;
    OVERLAY::FrameOverlays overlays;
    GRAPHICSTICK::SRects rects; //!< the ones it was (or is to be) drawn with
    std::vector<const CDVDOverlay*> images; //!< identity: the images ...
    std::vector<std::shared_ptr<const OVERLAY::SQuads>> glyphs; //!< ... and libass's output
    bool hidden = false; //!< no video window: drawn cleared (D-G2-1)
    bool redraw = false; //!< submitted, but an input of its draw changed since
    bool bridge = false; //!< the untimed images alone (no picture's overlays shown)
  };
  //! one route step; true when work is left that waits for a buffer
  bool RouteStep();
  void RouteStart(uint64_t floorGen);
  void RouteEnd();
  //! the libass output of comp's overlays at its pts over its rects
  void RenderLibass(SComposition& comp);
  bool Same(const SComposition& a, const SComposition& b) const;
  //! false: no buffer free now
  bool CanDraw(bool redraw) const;
  bool DrawTimed(SComposition& comp);
  bool SubmitClear(uint64_t serial);
  CGraphicsRasterAML::SRegime RouteRegime();
  GRAPHICSTICK::SRects RouteRects(const SComposition& comp) const;

  bool m_routeOn = false;
  bool m_routeClearOwed = false; //!< at the start and at a floor: what the plane shows goes
  uint64_t m_routeClearSerial = 0;
  bool m_routeEndClearOwed = false; //!< the route's end (item 11), retried until submitted
  //! each libass handler's output at its last render: a render reporting no change shows it
  std::map<const CDVDSubtitlesLibass*, std::shared_ptr<const OVERLAY::SQuads>> m_assLast;
  uint64_t m_routeSession = 0;
  std::deque<GRAPHICSTICK::SQueued> m_routeQueue;
  bool m_routeHasPending = false;
  SComposition m_routePending; //!< built from the queue's front, waiting for a buffer
  SComposition m_routeLast; //!< the newest composition taken (drawn or not)
  std::vector<SComposition> m_routeDrawn; //!< submitted from the one of the frame on screen on
  bool m_routeLastVisible = false;
  bool m_routeHaveReleased = false;
  GRAPHICSTICK::SReleased m_routeReleased;
  uint64_t m_routeFloorGen = 0;
  uint64_t m_routeLayoutGen = 0;
  uint64_t m_routeAppliedEpoch = 0;
  float m_routePeak = -1.0f;
  uint64_t m_routeRegimeEpoch = 0; //!< the raster's cache key, one per (applied epoch, peak)
  bool m_routeHidden = false;
  std::atomic<bool> m_rasterReady{false};

  static constexpr int WIDTH = 1920;
  static constexpr int HEIGHT = 1080;

  EGLDisplay m_display;
  gbm_device* m_device;
  const int m_drmFd;
  CAMLDisplay* m_amlDisplay;
  CPresentationCoordinator* m_coordinator;
  const bool m_testPattern;

  EGLConfig m_config = nullptr;
  EGLContext m_context = EGL_NO_CONTEXT;
  EGLSurface m_surface = EGL_NO_SURFACE;
  gbm_surface* m_gbmSurface = nullptr;
  uint32_t m_format = 0;
  std::unique_ptr<KODI::UTILS::EGL::CEGLFence> m_fence;

  CEvent m_wake;
  mutable CCriticalSection m_showSection;
  SHdrGraphics m_next; //!< under m_showSection
  bool m_hasNext = false; //!< under m_showSection
  SHdrGraphics m_shown; //!< what the plane shows, this thread only
  bool m_redraw = false; //!< m_shown not on the plane yet
  std::vector<SUntimedPost> m_untimed; //!< GUNTIMED: posts taken, not yet submitted
  std::map<const CDVDOverlay*, STexture> m_textures;
  bool m_texturesLimited = false;
  GLuint m_program = 0;
  GLint m_posLoc = -1;
  GLint m_texLoc = -1;
  std::atomic<bool> m_ready{false};
  bool m_refusedOff = false; //!< switched off after the driver refused the plane
  int m_locked = 0;
  std::vector<gbm_bo*> m_returned;
  uint32_t m_lastFb = 0; //!< the fb last submitted
  bool m_lastVisible = false;
  unsigned int m_epoch = 0;
  uint64_t m_resubmit = 0; //!< C's last graphics resubmit request taken
  int m_submits = 0; //!< for hotplugworker_gfxrefuse
  int m_gholdMs = 0; //!< test: displaystate_ghold
  int64_t m_gholdNs = 0;

  // 5.G2 GT-3: G's renderer, inert (nothing it draws is submitted before GT-4)
  std::unique_ptr<CGraphicsRasterAML> m_raster;
  bool m_selfTest = false; //!< special://profile/gselftest
  int m_selfTestEvery = 10; //!< the real-content check's N (the file's content)
  bool m_shadowOwed = false; //!< a new m_shown not drawn by the renderer yet
  unsigned int m_shadowComps = 0;
  bool m_shadowNoRegime = false; //!< logged once
};
