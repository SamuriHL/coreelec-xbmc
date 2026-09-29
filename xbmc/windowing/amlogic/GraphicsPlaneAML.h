/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/HdrGraphics.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"
#include "threads/Thread.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <GLES2/gl2.h>

#include <EGL/egl.h>

class CAMLDisplay;
class CDVDOverlayImage;
class CPresentationCoordinator;
struct gbm_bo;
struct gbm_device;
struct gbm_surface;

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
 * or a test pattern; with nothing to show it idles enabled at alpha 0.
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
  //! the commit; the next frame switches it on again.
  void SwitchOff();
  //! true once it can take graphics over from the GUI plane
  bool CanShow() const;
  //! shows these graphics from the next frame on (an empty set hides the plane)
  void Show(SHdrGraphics graphics);

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
  //! where the image goes on the plane, in plane pixels
  CRect Place(const CDVDOverlayImage& image, const STexture& texture) const;
  //! draws and submits a frame; false when it could not be handed over
  bool SubmitFrame(bool visible, int64_t nowNs);
  //! re-commits what is on screen with the current geometry (R6)
  bool SubmitGeometry();
  void ReleaseReturned();
  uint32_t FbFromBo(gbm_bo* bo);

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
};
