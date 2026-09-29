/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/Event.h"
#include "threads/Thread.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <EGL/egl.h>

class CAMLDisplay;
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
 * Step 5a: no content yet; the plane idles enabled at alpha 0, or shows a
 * test pattern.
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

protected:
  void Process() override;

private:
  bool InitGL();
  void DeinitGL();
  //! draws and submits a frame; false when it could not be handed over
  bool SubmitFrame(bool visible, int64_t nowNs);
  //! re-commits what is on screen with the current geometry (R6)
  void SubmitGeometry();
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
  int m_locked = 0;
  std::vector<gbm_bo*> m_returned;
  uint32_t m_lastFb = 0; //!< the fb last submitted
  bool m_lastVisible = false;
  unsigned int m_epoch = 0;
};
