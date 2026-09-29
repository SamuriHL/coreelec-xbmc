/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsPlaneAML.h"

#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
#include "utils/AMLUtils.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "utils/log.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <chrono>
#include <cstring>

#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>

using namespace std::chrono_literals;

namespace
{
constexpr int LOCK_CAP = 4; // R5

int64_t MonotonicNs()
{
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct PlaneFb
{
  uint32_t id = 0;
  int fd = -1;
};

void DestroyPlaneFb(gbm_bo* bo, void* data)
{
  auto* fb = static_cast<PlaneFb*>(data);
  if (fb->id)
    drmModeRmFB(fb->fd, fb->id);
  delete fb;
}
} // namespace

CGraphicsPlaneAML::CGraphicsPlaneAML(EGLDisplay display,
                                     gbm_device* device,
                                     int drmFd,
                                     CAMLDisplay* amlDisplay,
                                     CPresentationCoordinator* coordinator,
                                     bool testPattern)
  : CThread("GraphicsPlane"),
    m_display(display),
    m_device(device),
    m_drmFd(drmFd),
    m_amlDisplay(amlDisplay),
    m_coordinator(coordinator),
    m_testPattern(testPattern)
{
}

CGraphicsPlaneAML::~CGraphicsPlaneAML()
{
  Stop();
}

bool CGraphicsPlaneAML::Start()
{
  if (!m_amlDisplay->HasOverlayPlane())
  {
    CLog::Log(LOGWARNING, "CGraphicsPlaneAML - no overlay plane on the GUI's CRTC");
    return false;
  }
  Create();
  return true;
}

void CGraphicsPlaneAML::Stop()
{
  if (!IsRunning())
    return;
  m_bStop = true;
  m_wake.Set();
  StopThread(true);
}

bool CGraphicsPlaneAML::InitGL()
{
  const EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                            EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_RENDERABLE_TYPE,
                            EGL_OPENGL_ES2_BIT, EGL_NONE};
  EGLint count = 0;
  if (!eglChooseConfig(m_display, attribs, nullptr, 0, &count) || count <= 0)
    return false;
  std::vector<EGLConfig> configs(count);
  if (!eglChooseConfig(m_display, attribs, configs.data(), count, &count))
    return false;
  // the GUI plane's format first, so both planes carry the same bytes
  for (const uint32_t wanted : {GBM_FORMAT_ABGR8888, GBM_FORMAT_ARGB8888})
  {
    for (EGLint i = 0; i < count && !m_config; i++)
    {
      EGLint visual = 0;
      if (eglGetConfigAttrib(m_display, configs[i], EGL_NATIVE_VISUAL_ID, &visual) &&
          static_cast<uint32_t>(visual) == wanted)
      {
        m_config = configs[i];
        m_format = wanted;
      }
    }
  }
  if (!m_config)
  {
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no 8-bit RGBA EGL config");
    return false;
  }

  // linear, as the GUI plane's surface
  const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  m_gbmSurface =
      gbm_surface_create_with_modifiers(m_device, WIDTH, HEIGHT, m_format, &modifier, 1);
  if (!m_gbmSurface)
    m_gbmSurface = gbm_surface_create(m_device, WIDTH, HEIGHT, m_format,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!m_gbmSurface)
  {
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no GBM surface");
    return false;
  }

  auto createPlatformSurface = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(
      eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
  m_surface = createPlatformSurface
                  ? createPlatformSurface(m_display, m_config, m_gbmSurface, nullptr)
                  : eglCreateWindowSurface(m_display, m_config,
                                           reinterpret_cast<EGLNativeWindowType>(m_gbmSurface),
                                           nullptr);
  if (m_surface == EGL_NO_SURFACE)
  {
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no EGL surface ({:#x})", eglGetError());
    return false;
  }

  // its own context: nothing is shared with the GUI's
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
  if (m_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
  {
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no EGL context ({:#x})", eglGetError());
    return false;
  }
  eglSwapInterval(m_display, 0);

  if (CEGLUtils::HasExtension(m_display, "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(m_display, "EGL_KHR_fence_sync"))
    m_fence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(m_display);

  CLog::Log(LOGINFO, "CGraphicsPlaneAML - {}x{} {:.4s} surface, own context", WIDTH, HEIGHT,
            reinterpret_cast<const char*>(&m_format));
  return true;
}

void CGraphicsPlaneAML::DeinitGL()
{
  m_fence.reset();
  if (m_context != EGL_NO_CONTEXT)
  {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_display, m_context);
    m_context = EGL_NO_CONTEXT;
  }
  if (m_surface != EGL_NO_SURFACE)
  {
    eglDestroySurface(m_display, m_surface);
    m_surface = EGL_NO_SURFACE;
  }
  // its buffers, and their fbs with them; the display itself is the GUI's
  if (m_gbmSurface)
  {
    gbm_surface_destroy(m_gbmSurface);
    m_gbmSurface = nullptr;
  }
  eglReleaseThread();
}

uint32_t CGraphicsPlaneAML::FbFromBo(gbm_bo* bo)
{
  if (auto* fb = static_cast<PlaneFb*>(gbm_bo_get_user_data(bo)))
    return fb->id;

  const uint32_t handles[4] = {gbm_bo_get_handle(bo).u32};
  const uint32_t strides[4] = {gbm_bo_get_stride(bo)};
  const uint32_t offsets[4] = {};
  const uint64_t modifier = gbm_bo_get_modifier(bo);
  const uint64_t modifiers[4] = {modifier};
  const uint32_t flags =
      modifier && modifier != DRM_FORMAT_MOD_INVALID ? DRM_MODE_FB_MODIFIERS : 0;
  auto* fb = new PlaneFb;
  fb->fd = m_drmFd;
  if (drmModeAddFB2WithModifiers(m_drmFd, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                                 gbm_bo_get_format(bo), handles, strides, offsets, modifiers,
                                 &fb->id, flags) != 0)
  {
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no framebuffer: {}", strerror(errno));
    delete fb;
    return 0;
  }
  gbm_bo_set_user_data(bo, fb, DestroyPlaneFb);
  return fb->id;
}

void CGraphicsPlaneAML::ReleaseReturned()
{
  m_coordinator->TakeReturned(CPresentationCoordinator::PLANE_GRAPHICS, m_returned);
  for (gbm_bo* bo : m_returned)
    gbm_surface_release_buffer(m_gbmSurface, bo);
  m_locked -= static_cast<int>(m_returned.size());
  m_returned.clear();
}

bool CGraphicsPlaneAML::SubmitFrame(bool visible, int64_t nowNs)
{
  // R5: never more than four buffers out of the surface
  if (m_locked >= LOCK_CAP)
    return false;

  glViewport(0, 0, WIDTH, HEIGHT);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (visible && m_testPattern)
  {
    // a half-transparent box crossing the top of the screen every 4 s, and a
    // fixed one bottom left: movement shows the plane updates, the fixed box
    // its placement and scaling
    const int64_t period = 4000000000;
    const int x = static_cast<int>((nowNs % period) * (WIDTH - 240) / period);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, HEIGHT - 100 - 135, 240, 135);
    glClearColor(0.5f, 0.25f, 0.0f, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(40, 40, 160, 90);
    glClearColor(0.0f, 0.4f, 0.4f, 0.8f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
  }

  int fence = -1;
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    m_fence->CreateGPUFence();
#endif
  if (!eglSwapBuffers(m_display, m_surface))
    CLog::Log(LOGDEBUG, "CGraphicsPlaneAML - swap failed ({:#x})", eglGetError());
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    fence = m_fence->FlushFence();
#endif

  gbm_bo* bo = gbm_surface_lock_front_buffer(m_gbmSurface);
  const uint32_t fb = bo ? FbFromBo(bo) : 0;
  drmModeAtomicReqPtr req = fb ? m_amlDisplay->BuildOverlayRequest(fb, WIDTH, HEIGHT, visible)
                               : nullptr;
  if (!req)
  {
    if (bo)
      gbm_surface_release_buffer(m_gbmSurface, bo);
    if (fence >= 0)
      close(fence);
    return false;
  }

  m_locked++;
  const uint64_t seq =
      m_coordinator->Submit(CPresentationCoordinator::PLANE_GRAPHICS, bo, fb, req, fence);
  m_lastFb = fb;
  m_lastVisible = visible;
  m_coordinator->WaitTaken(seq, 50ms);
  return true;
}

void CGraphicsPlaneAML::SwitchOff()
{
  if (!m_coordinator->DisableGraphicsPlane([this](uint32_t primaryFb)
                                           { return m_amlDisplay->BuildOverlayOffRequest(primaryFb); }))
    CLog::Log(LOGWARNING, "CGraphicsPlaneAML - plane left to go with its buffers");
}

bool CGraphicsPlaneAML::SubmitGeometry()
{
  // a fresh frame rather than a property-only commit on a buffer this thread
  // may already have been handed back
  return SubmitFrame(m_lastVisible, MonotonicNs());
}

void CGraphicsPlaneAML::Process()
{
  if (!InitGL())
  {
    DeinitGL();
    return;
  }

  m_epoch = aml_presenter_epoch();
  // establishes the plane, enabled and transparent (R4: idle is alpha 0,
  // never a disable)
  SubmitFrame(m_testPattern, MonotonicNs());

  while (!m_bStop)
  {
    ReleaseReturned();
    // a display transaction moved or rescaled the CRTC: the plane still has
    // the old rect until committed again (R6)
    const unsigned int epoch = aml_presenter_epoch();
    if (epoch != m_epoch && (m_testPattern || SubmitGeometry()))
      m_epoch = epoch;
    if (m_testPattern)
    {
      if (!SubmitFrame(true, MonotonicNs()))
        m_wake.Wait(16ms);
    }
    else
      m_wake.Wait(100ms);
  }

  SwitchOff();
  ReleaseReturned();
  DeinitGL();
  CLog::Log(LOGINFO, "CGraphicsPlaneAML - stopped");
}
