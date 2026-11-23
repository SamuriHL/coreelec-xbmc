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
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
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
#include <EGL/eglext.h>

class CAMLDisplay;
class CDVDOverlayImage;
class CDVDSubtitlesLibass;
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
  //! 5.G2 GT-3: builds G's renderer when the route is requested or gselftest asks for it (M5)
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
    // GT-5: the untimed state it carries (item 5 (a)); part of its identity
    unsigned int segmentGen = 0; //!< of its picture
    uint64_t u = 0;
    bool firstPage = false; //!< its segment's waiting page (item 5 (f))
    std::vector<std::shared_ptr<CDVDOverlay>> untimed;
    std::vector<const CDVDOverlay*> untimedIds;
    bool untimedStale = false; //!< submitted with an older state: item 5 (d)'s re-draw is owed
    int64_t assUs = -1; //!< GT-D15 measurement: its last libass render
  };
  //! GT-5: an untimed state as G holds it: the current composition or a segment's waiting page
  struct SUntimedPage
  {
    uint64_t u = 0;
    int64_t postNs = 0;
    std::vector<std::shared_ptr<CDVDOverlay>> images;
    std::vector<const CDVDOverlay*> ids;
    unsigned int gen = 0; //!< a waiting page: its segment
  };
  //! one route step; true when work is left that waits for a buffer
  bool RouteStep();
  void RouteStart(uint64_t floorGen);
  void RouteEnd();
  //! the libass output of comp's overlays at its pts over its rects
  void RenderLibass(SComposition& comp);
  void RenderLibassTimed(SComposition& comp);
  bool Same(const SComposition& a, const SComposition& b) const;
  //! false: no buffer free now
  bool CanDraw(bool redraw) const;
  //! draws comp and takes its buffer into out; false: no buffer (nothing locked)
  bool DrawComposition(SComposition& comp, CPresentationCoordinator::STimedBuffer& out);
  //! DrawComposition, handed to C alone
  bool DrawTimed(SComposition& comp);
  // GT-5 (design §10.2 item 5): untimed graphics on G
  //! takes the newest posted state (gnewest_off: the next one, once C has the one before)
  void TakeUntimedStates();
  void ApplyUntimed(GRAPHICSTICK::SUntimedState& state);
  //! the state comp's frame shows: its segment's waiting page, or the current composition
  const SUntimedPage& PageFor(const SComposition& comp) const;
  void AssignPage(SComposition& comp);
  //! marks every drawn entry whose state is no longer its frame's
  void MarkUntimedStale();
  //! the frame on screen's entry (or the untimed-only one) is drawn again with what is up
  void MarkScreenStale();
  //! review rB M3: the waiting page's entry on its segment's first picture
  void PlaceFirstPage();
  //! a drawn buffer never handed to C goes back to the surface
  void ReleaseUnsubmitted(CPresentationCoordinator::STimedBuffer& buffer);
  //! item 5 (d): the stale entries ahead of the frame on screen as one unit, then the frame on
  //! screen's; true when it waits for buffers
  bool RedrawUntimed();
  //! the keep-alive TTL (BD-J's abandoned compositions), from C's PAUSED state
  void CheckKeepAlive();
  //! an untimed state is up (G draws one frame entry ahead, item 2)
  bool UntimedUp() const { return !m_uCur.ids.empty() || !m_uWait.ids.empty(); }
  //! images and glyphs of what G may still redraw: their textures stay cached
  void RetainRoute(std::vector<std::shared_ptr<CDVDOverlay>>& images,
                   std::vector<CGraphicsRasterAML::SGlyphs>& glyphs) const;
  bool SubmitClear(uint64_t serial);
  CGraphicsRasterAML::SRegime RouteRegime();
  GRAPHICSTICK::SRects RouteRects(const SComposition& comp) const;

  // GT-5 (GT-D15, user ruling 2026-11-22): G's own ring of scanout buffers for the route, not
  // the Mali GBM surface (exhausting that surface segfaulted libMali on the S6; it stays for
  // route C and the test pattern). One size: 6, up to 8 allowed (about 8.3 MB each)
  static constexpr int RING_SIZE = 6;
  struct SRingBuffer
  {
    gbm_bo* bo = nullptr;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLuint renderbuffer = 0; //!< when the driver cannot render to the image as a texture
    GLuint fbo = 0;
    bool free = true;
    int dmabuf = -1; //!< its heap-fb dma-buf, kept while GBM's import of it lives; -1: GBM's own
  };
  std::vector<SRingBuffer> m_ring;
  bool m_ringTried = false;
  int m_ringRetries = 0; //!< GT-D22: one per route end while the ring is off
  //! GT-D22: when G, idle after a route end, tries the ring again (0: none owed). The delay lets
  //! the player release the decoder's buffers first, and keeps the CMA cost off any start
  int64_t m_ringRetryAtNs = 0;
  static constexpr int64_t RING_RETRY_DELAY_NS = 2000000000;
  int m_ringAttempts = 0; //!< for the test file gring_fail
  //! the ring, made at G's start (the route requested), retried after a route end while off;
  //! false: G's route draws on the surface (LOCK_CAP)
  bool InitRing();
  //! one ring buffer from /dev/dma_heap/heap-fb, imported into GBM (and by it into DRM); nullptr
  //! when that fails (GBM's own then: heap-gfx only)
  gbm_bo* RingBoFromHeap(int& fd, uint32_t& stride);
  void LogHeapState(const char* why) const;
  void DeinitRing();
  //! GT-D22: one more attempt, logged with the heaps before and the outcome and time after
  void RetryRing();
  bool RingOn() const { return !m_ring.empty(); }
  //! the route's free buffers and its capacity (ring or surface)
  int RouteFree() const;
  int RouteCap() const;
  //! binds a free buffer to draw a route composition into; slot -1: the surface's back buffer
  bool RouteBeginDraw(int& slot);
  //! after the draw: its buffer, fb, request and GPU fence; false leaves nothing locked
  bool RouteTakeFrame(int slot, gbm_bo*& bo, uint32_t& fb, drmModeAtomicReqPtr& req, int& fence);
  //! a ring buffer back (C returned it or it was never handed over); false: not the ring's
  bool RingRelease(gbm_bo* bo);

  bool m_routeOn = false;
  bool m_routeClearOwed = false; //!< at the start and at a floor: what the plane shows goes
  uint64_t m_routeClearSerial = 0;
  bool m_routeClearAtFloor = false; //!< GT-5 (r2 N1): the owed clear is a floor's
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
  uint8_t m_routeKnobTag = 0; //!< GT-6: the tag of the regime drawn in (0: main's, untagged)
  bool m_routeHidden = false;
  std::atomic<bool> m_rasterReady{false};
  // GT-5, this thread only
  bool m_untimedOnG = false; //!< this route session: G takes the container's states
  SUntimedPage m_uCur;
  SUntimedPage m_uWait;
  unsigned int m_uReleasedSegment = 0; //!< as the container's last snapshot says
  unsigned int m_uNotedSegment = 0; //!< the newest segment G called NotePresentedSegment for
  unsigned int m_uFirstPageGen = 0; //!< item 5 (f): the segment whose first frame is ...
  uint64_t m_uFirstPageSerial = 0; //!< ... this picture
  bool m_uExpired = false; //!< the keep-alive TTL took the current composition away
  SUntimedPage m_uExpiredPage;
  int64_t m_uExpiredStamp = 0;
  uint64_t m_uDeferredU = 0; //!< gUnitDeferred logged for this state
  uint64_t m_uWaitLoggedU = 0; //!< GUNTIMED wait logged for this state (m_uWaitLogged: any yet)
  bool m_uWaitLogged = false;
  int64_t m_uWaitWindowNs = 0; //!< GUNTIMED wait: lines in the current second
  int m_uWaitLines = 0;
  uint64_t m_uTakenU = 0; //!< gnewest_off: the last state taken, and when
  int64_t m_uTakenNs = 0;
  int64_t m_waitBufferNs = 0; //!< item 12: G waits on C for a free buffer since
  bool m_uAdvancing = true; //!< C picks frame entries (GRAPHICSTICK::CAdvancing), as last seen
  //! the state the last untimed-only entry drawn while C did not advance carries
  std::vector<const CDVDOverlay*> m_uFramelessIds;
  bool m_uFramelessShown = false;
  //! an untimed change not yet in an entry at or below the frame on screen
  bool m_uScreenOwed = false;
  //! GUNTIMED owed (log only): once per state, at most 5 lines a second
  void NoteScreenOwed(bool waits);
  bool m_uOwedLogged = false;
  uint64_t m_uOwedLoggedU = 0;
  int64_t m_uOwedWindowNs = 0;
  int m_uOwedLines = 0;
  //! the pictures G took (drawn or Same), newest last: the frame on screen's composition when
  //! it has no entry of its own
  std::deque<SComposition> m_routeHistory;
  //! since a floor or the route's start no picture after it was released: the plane carries
  //! no timed composition
  bool m_routeTimedCleared = true;
  SComposition m_uSegFirst; //!< the newest segment's first picture, drawn or not
  unsigned int m_uPageGenSeen = 0; //!< the newest segment a page was seen for
  uint64_t m_uLostSeen = 0; //!< GRAPHICSTICK::GraphicsLost, as last seen
  uint64_t m_clearedSeen = 0; //!< GT-6: GRAPHICSTICK::PlaneCleared, as last seen
  uint64_t m_uExpiredPosts = 0; //!< GRAPHICSTICK::UntimedPosts at the keep-alive expiry

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
