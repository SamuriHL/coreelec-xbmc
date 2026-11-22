/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsPlaneAML.h"

#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayLibass.h"
#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayStereoUtils.h"
#include "cores/VideoPlayer/DVDOverlayContainer.h"
#include "cores/VideoPlayer/DVDSubtitles/DVDSubtitlesLibass.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRendererUtil.h"
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "utils/StringUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/amlogic/GraphicsRasterAML.h"
#include "settings/Settings.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/GraphicsRegime.h"
#include "windowing/amlogic/GuiSnapshot.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/ThreadRole.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

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
// GT-5: the compositions G keeps to redraw: the frame on screen's and those ahead of it, one
// per queued frame at most (the video queue held up to 14 in RC8)
constexpr size_t ROUTE_DRAWN_CAP = 32;
// GT-5: the pictures G took, kept from the newest at or below the frame on screen (a safety
// bound only: Released trims it)
constexpr size_t ROUTE_HISTORY_CAP = 512;

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

// test: special://profile/displaystate_ghold holds "<ms>"; G switches the plane off that often
int TestHoldMs()
{
  static const int ms = []
  {
    XFILE::CFile file;
    std::vector<uint8_t> buf;
    if (!XFILE::CFile::Exists("special://profile/displaystate_ghold") ||
        file.LoadFile("special://profile/displaystate_ghold", buf) <= 0)
      return 0;
    const int value = std::atoi(std::string(buf.begin(), buf.end()).c_str());
    GRAPHICSTICK::GTLogF(LOGWARNING,
                         "CGraphicsPlaneAML - test: plane off and re-submitted every {} ms", value);
    return value;
  }();
  return ms;
}

bool SameGraphics(const SHdrGraphics& a, const SHdrGraphics& b)
{
  if (a.images.empty() && b.images.empty())
    return true;
  return a.images == b.images && a.source == b.source && a.dest == b.dest && a.view == b.view &&
         a.width == b.width && a.height == b.height && a.limited == b.limited;
}

GLuint CompileShader(GLenum type, const char* source)
{
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - shader: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
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
    GRAPHICSTICK::GTLogF(LOGWARNING, "CGraphicsPlaneAML - no overlay plane on the GUI's CRTC");
    return false;
  }
  m_gholdMs = TestHoldMs();
  GRAPHICSTICK::SetWake([this] { m_wake.Set(); });
  Create();
  return true;
}

bool CGraphicsPlaneAML::CanShow() const
{
  return m_ready && !m_testPattern && !m_coordinator->GraphicsRefused();
}

void CGraphicsPlaneAML::Show(SHdrGraphics graphics)
{
  {
    std::unique_lock lock(m_showSection);
    // GUNTIMED: posts of a set this thread never took go with the one replacing it
    if (m_hasNext && !m_next.untimed.empty())
      graphics.untimed.insert(graphics.untimed.begin(), m_next.untimed.begin(),
                              m_next.untimed.end());
    m_next = std::move(graphics);
    m_hasNext = true;
  }
  m_wake.Set();
}

void CGraphicsPlaneAML::Stop()
{
  if (!IsRunning())
    return;
  m_bStop = true;
  m_wake.Set();
  StopThread(true);
  GRAPHICSTICK::SetWake(nullptr);
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
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no 8-bit RGBA EGL config");
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
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no GBM surface");
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
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no EGL surface ({:#x})", eglGetError());
    return false;
  }

  // its own context: nothing is shared with the GUI's
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
  if (m_context == EGL_NO_CONTEXT ||
      !eglMakeCurrent(m_display, m_surface, m_surface, m_context))
  {
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no EGL context ({:#x})", eglGetError());
    return false;
  }
  eglSwapInterval(m_display, 0);

  if (CEGLUtils::HasExtension(m_display, "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(m_display, "EGL_KHR_fence_sync"))
    m_fence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(m_display);

  GRAPHICSTICK::GTLogF(LOGINFO, "CGraphicsPlaneAML - {}x{} {:.4s} surface, own context", WIDTH, HEIGHT,
            reinterpret_cast<const char*>(&m_format));
  return true;
}

bool CGraphicsPlaneAML::InitProgram()
{
  // premultiplied texels, placed in plane pixels by the caller
  static const char* vertexSource = "attribute vec2 a_pos;\n"
                                    "attribute vec2 a_tex;\n"
                                    "varying vec2 v_tex;\n"
                                    "void main()\n"
                                    "{\n"
                                    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                                    "  v_tex = a_tex;\n"
                                    "}\n";
  static const char* fragmentSource = "precision mediump float;\n"
                                      "uniform sampler2D u_tex;\n"
                                      "varying vec2 v_tex;\n"
                                      "void main()\n"
                                      "{\n"
                                      "  gl_FragColor = texture2D(u_tex, v_tex);\n"
                                      "}\n";
  const GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
  const GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
  if (vertex && fragment)
  {
    m_program = glCreateProgram();
    glAttachShader(m_program, vertex);
    glAttachShader(m_program, fragment);
    glLinkProgram(m_program);
  }
  if (vertex)
    glDeleteShader(vertex);
  if (fragment)
    glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  if (m_program)
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (!linked)
  {
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no program");
    if (m_program)
      glDeleteProgram(m_program);
    m_program = 0;
    return false;
  }
  m_posLoc = glGetAttribLocation(m_program, "a_pos");
  m_texLoc = glGetAttribLocation(m_program, "a_tex");
  return true;
}

void CGraphicsPlaneAML::DeinitGL()
{
  DeinitRing();
  if (m_context != EGL_NO_CONTEXT)
  {
    for (auto& [key, texture] : m_textures)
      glDeleteTextures(1, &texture.texture);
    if (m_program)
      glDeleteProgram(m_program);
    if (m_raster)
      m_raster->Deinit();
  }
  m_raster.reset();
  m_textures.clear();
  m_program = 0;
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
    GRAPHICSTICK::GTLogF(LOGERROR, "CGraphicsPlaneAML - no framebuffer: {}", strerror(errno));
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
  {
    // a ring buffer goes back to the ring, a surface buffer to its surface
    if (RingRelease(bo))
      continue;
    gbm_surface_release_buffer(m_gbmSurface, bo);
    m_locked--;
  }
  m_returned.clear();
}

bool CGraphicsPlaneAML::InitRing()
{
  auto createImage =
      reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  auto targetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  auto targetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
      eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
  const char* why = nullptr;
  if (!createImage || !targetTexture ||
      !CEGLUtils::HasExtension(m_display, "EGL_EXT_image_dma_buf_import"))
    why = "no dma-buf EGL image import";
  for (int i = 0; i < RING_SIZE && !why; i++)
  {
    SRingBuffer buffer;
    // a scanout buffer as the surface's are (linear, the plane's format), allocated by the same
    // GBM device, so from the same heap (heap-fb, then heap-gfx)
    // linear, as the surface's (InitGL) and as the EGL import, which passes no modifier
    const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    buffer.bo = gbm_bo_create_with_modifiers(m_device, WIDTH, HEIGHT, m_format, &modifier, 1);
    if (!buffer.bo)
      buffer.bo = gbm_bo_create(m_device, WIDTH, HEIGHT, m_format,
                                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!buffer.bo)
    {
      why = "gbm_bo_create";
      break;
    }
    const int fd = gbm_bo_get_fd(buffer.bo);
    const EGLint attribs[] = {EGL_WIDTH,
                              WIDTH,
                              EGL_HEIGHT,
                              HEIGHT,
                              EGL_LINUX_DRM_FOURCC_EXT,
                              static_cast<EGLint>(m_format),
                              EGL_DMA_BUF_PLANE0_FD_EXT,
                              fd,
                              EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                              0,
                              EGL_DMA_BUF_PLANE0_PITCH_EXT,
                              static_cast<EGLint>(gbm_bo_get_stride(buffer.bo)),
                              EGL_NONE};
    buffer.image = fd >= 0 ? createImage(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                         nullptr, attribs)
                           : EGL_NO_IMAGE_KHR;
    if (fd >= 0)
      close(fd); // the image holds its own reference
    if (buffer.image == EGL_NO_IMAGE_KHR)
    {
      gbm_bo_destroy(buffer.bo);
      why = "EGL image";
      break;
    }
    while (glGetError() != GL_NO_ERROR)
    {
    }
    // a texture target first; Mali may import a dma-buf as sampled only, so a renderbuffer
    // (GL_OES_EGL_image's render target) next (RC7 S6: GRING off (framebuffer))
    glGenTextures(1, &buffer.texture);
    glBindTexture(GL_TEXTURE_2D, buffer.texture);
    targetTexture(GL_TEXTURE_2D, buffer.image);
    const GLenum texError = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &buffer.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, buffer.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, buffer.texture,
                           0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum texStatus = status;
    GLenum rbError = GL_NO_ERROR;
    if (status != GL_FRAMEBUFFER_COMPLETE && targetRenderbuffer)
    {
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
      glDeleteTextures(1, &buffer.texture);
      buffer.texture = 0;
      glGenRenderbuffers(1, &buffer.renderbuffer);
      glBindRenderbuffer(GL_RENDERBUFFER, buffer.renderbuffer);
      targetRenderbuffer(GL_RENDERBUFFER, buffer.image);
      rbError = glGetError();
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                buffer.renderbuffer);
      status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_ring.push_back(buffer);
    const uint64_t boModifier = gbm_bo_get_modifier(buffer.bo);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
      GRAPHICSTICK::GTLogF(LOGWARNING,
                "GRING buffer {}: FBO incomplete: texture {:#x} (target error {:#x}), "
                "renderbuffer {:#x} (target error {:#x}{}); format {:.4s} modifier {:#x} stride {}",
                i, texStatus, texError, status, rbError,
                targetRenderbuffer ? "" : ", no glEGLImageTargetRenderbufferStorageOES",
                reinterpret_cast<const char*>(&m_format), boModifier, gbm_bo_get_stride(buffer.bo));
      why = "FBO incomplete";
    }
    else if (!FbFromBo(buffer.bo))
    {
      // FbFromBo logged AddFB2's errno; its flags: DRM_MODE_FB_MODIFIERS only for a modifier
      // other than 0 (LINEAR) or INVALID, as for the surface's buffers
      GRAPHICSTICK::GTLogF(LOGWARNING, "GRING buffer {}: no fb; format {:.4s} modifier {:#x} stride {}", i,
                reinterpret_cast<const char*>(&m_format), boModifier, gbm_bo_get_stride(buffer.bo));
      why = "addfb";
    }
    else if (i == 0)
      GRAPHICSTICK::GTLogF(LOGINFO, "GRING buffer 0: {} target, format {:.4s} modifier {:#x} stride {}",
                buffer.renderbuffer ? "renderbuffer" : "texture",
                reinterpret_cast<const char*>(&m_format), boModifier, gbm_bo_get_stride(buffer.bo));
  }
  if (why)
  {
    DeinitRing();
    GRAPHICSTICK::GTLogF(LOGWARNING, "GRING off ({}): the route draws on the surface ({} buffers)", why,
              LOCK_CAP);
    return false;
  }
  GRAPHICSTICK::GTLogF(LOGINFO, "GRING on: {} scanout buffers {}x{} ({:.1f} MB), G's own", RING_SIZE, WIDTH,
            HEIGHT, RING_SIZE * WIDTH * HEIGHT * 4 / 1048576.0);
  return true;
}

void CGraphicsPlaneAML::DeinitRing()
{
  if (m_ring.empty())
    return;
  auto destroyImage =
      reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  for (SRingBuffer& buffer : m_ring)
  {
    if (buffer.fbo)
      glDeleteFramebuffers(1, &buffer.fbo);
    if (buffer.texture)
      glDeleteTextures(1, &buffer.texture);
    if (buffer.renderbuffer)
      glDeleteRenderbuffers(1, &buffer.renderbuffer);
    if (buffer.image != EGL_NO_IMAGE_KHR && destroyImage)
      destroyImage(m_display, buffer.image);
    // its fb goes with it (DestroyPlaneFb, the bo's user data)
    if (buffer.bo)
      gbm_bo_destroy(buffer.bo);
  }
  m_ring.clear();
}

int CGraphicsPlaneAML::RouteFree() const
{
  if (!RingOn())
    return LOCK_CAP - m_locked;
  return static_cast<int>(
      std::count_if(m_ring.begin(), m_ring.end(), [](const SRingBuffer& b) { return b.free; }));
}

int CGraphicsPlaneAML::RouteCap() const
{
  return RingOn() ? RING_SIZE : LOCK_CAP;
}

bool CGraphicsPlaneAML::RouteBeginDraw(int& slot)
{
  slot = -1;
  if (!RingOn())
  {
    if (m_locked >= LOCK_CAP)
      return false;
    m_raster->SetTargetFlipped(false);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
  }
  for (int i = 0; i < static_cast<int>(m_ring.size()); i++)
    if (m_ring[i].free)
    {
      slot = i;
      m_raster->SetTargetFlipped(true);
      glBindFramebuffer(GL_FRAMEBUFFER, m_ring[i].fbo);
      return true;
    }
  return false;
}

bool CGraphicsPlaneAML::RouteTakeFrame(
    int slot, gbm_bo*& bo, uint32_t& fb, drmModeAtomicReqPtr& req, int& fence)
{
  if (slot < 0)
    return TakeFrame(bo, fb, req, fence);
  fence = -1;
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    m_fence->CreateGPUFence();
#endif
  glFlush();
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    fence = m_fence->FlushFence();
#endif
  // without a fence (none made, or its fd could not be duplicated: FlushFence -1) C would scan
  // out an unfinished draw
  if (fence < 0)
    glFinish();
  // the self-test and the shadow draw render for read-back, unflipped (also after a clear)
  m_raster->SetTargetFlipped(false);
  bo = m_ring[slot].bo;
  fb = FbFromBo(bo);
  const int refuseAt = OUTPUT::TestGraphicsRefuseAt();
  const bool refused = refuseAt > 0 && fb && (m_submits >= refuseAt || ++m_submits >= refuseAt);
  req = fb ? m_amlDisplay->BuildOverlayRequest(fb, WIDTH, HEIGHT, refused) : nullptr;
  if (!req)
  {
    if (fence >= 0)
      close(fence);
    bo = nullptr;
    fence = -1;
    return false;
  }
  m_ring[slot].free = false;
  return true;
}

bool CGraphicsPlaneAML::RingRelease(gbm_bo* bo)
{
  for (SRingBuffer& buffer : m_ring)
    if (buffer.bo == bo)
    {
      buffer.free = true;
      return true;
    }
  return false;
}

bool CGraphicsPlaneAML::SubmitFrame(bool visible, int64_t nowNs, double pts)
{
  // R5: never more than four buffers out of the surface
  if (m_locked >= LOCK_CAP)
    return false;

  glViewport(0, 0, WIDTH, HEIGHT);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  if (visible && !m_testPattern)
    DrawGraphics();
  else if (visible && m_testPattern)
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

  gbm_bo* bo = nullptr;
  uint32_t fb = 0;
  drmModeAtomicReqPtr req = nullptr;
  int fence = -1;
  if (!TakeFrame(bo, fb, req, fence))
    return false;
  const uint64_t seq =
      m_coordinator->Submit(CPresentationCoordinator::PLANE_GRAPHICS, bo, fb, req, fence, pts);
  m_lastFb = fb;
  m_lastVisible = visible;
  m_coordinator->WaitTaken(seq, 50ms);
  return true;
}

bool CGraphicsPlaneAML::TakeFrame(gbm_bo*& bo,
                                  uint32_t& fb,
                                  drmModeAtomicReqPtr& req,
                                  int& fence)
{
  fence = -1;
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    m_fence->CreateGPUFence();
#endif
  if (!eglSwapBuffers(m_display, m_surface))
    GRAPHICSTICK::GTLogF(LOGDEBUG, "CGraphicsPlaneAML - swap failed ({:#x})", eglGetError());
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_fence)
    fence = m_fence->FlushFence();
#endif

  bo = gbm_surface_lock_front_buffer(m_gbmSurface);
  fb = bo ? FbFromBo(bo) : 0;
  const int refuseAt = OUTPUT::TestGraphicsRefuseAt();
  const bool refused = refuseAt > 0 && fb && (m_submits >= refuseAt || ++m_submits >= refuseAt);
  req = fb ? m_amlDisplay->BuildOverlayRequest(fb, WIDTH, HEIGHT, refused) : nullptr;
  if (!req)
  {
    if (bo)
      gbm_surface_release_buffer(m_gbmSurface, bo);
    if (fence >= 0)
      close(fence);
    bo = nullptr;
    fence = -1;
    return false;
  }
  m_locked++;
  return true;
}

void CGraphicsPlaneAML::SwitchOff(const char* who)
{
  if (!m_coordinator->SwitchGraphicsOff([this](uint32_t primaryFb)
                                        { return m_amlDisplay->BuildOverlayOffRequest(primaryFb); },
                                        who))
    GRAPHICSTICK::GTLogF(LOGWARNING, "CGraphicsPlaneAML - plane left to go with its buffers");
}

bool CGraphicsPlaneAML::SubmitGeometry()
{
  // a fresh frame rather than a property-only commit on a buffer this thread
  // may already have been handed back
  return SubmitFrame(m_lastVisible, MonotonicNs());
}

bool CGraphicsPlaneAML::TakeShown()
{
  SHdrGraphics next;
  {
    std::unique_lock lock(m_showSection);
    if (!m_hasNext)
      return false;
    next = std::move(m_next);
    m_hasNext = false;
  }
  if (SameGraphics(next, m_shown))
  {
    // GT-1 r1 N2: an A -> B -> A within one wait shows A still; its posts go with a fresh
    // frame of it, not lost (only with guntimed_log, which alone posts)
    if (!next.untimed.empty())
    {
      m_untimed.insert(m_untimed.end(), next.untimed.begin(), next.untimed.end());
      m_redraw = true;
    }
    return false;
  }
  m_shown = std::move(next);
  m_untimed.insert(m_untimed.end(), m_shown.untimed.begin(), m_shown.untimed.end());
  m_shown.untimed.clear();
  m_redraw = true;
  GRAPHICSTICK::GTLogF(LOGDEBUG, "CGraphicsPlaneAML - showing {} images, video pts {:.3f}",
            m_shown.images.size(), m_shown.pts / 1000000.0);
  return true;
}

void CGraphicsPlaneAML::UpdateTextures()
{
  if (m_shown.limited != m_texturesLimited)
  {
    for (auto& [key, texture] : m_textures)
      glDeleteTextures(1, &texture.texture);
    m_textures.clear();
    m_texturesLimited = m_shown.limited;
  }

  for (auto it = m_textures.begin(); it != m_textures.end();)
  {
    if (std::find(m_shown.images.begin(), m_shown.images.end(), it->second.image) ==
        m_shown.images.end())
    {
      glDeleteTextures(1, &it->second.texture);
      it = m_textures.erase(it);
    }
    else
      ++it;
  }

  for (const std::shared_ptr<CDVDOverlay>& image : m_shown.images)
  {
    if (m_textures.count(image.get()))
      continue;
    const auto& o = static_cast<const CDVDOverlayImage&>(*image);
    STexture& texture = m_textures[image.get()];
    texture.image = image;
    if (o.width <= 0 || o.height <= 0 || o.pixels.empty())
      continue;

    // premultiplied, as the GUI plane over it
    std::vector<uint32_t> packed;
    if (o.palette.empty())
    {
      texture.width = o.width;
      texture.height = o.height;
      packed.resize(static_cast<size_t>(o.width) * o.height);
      for (int y = 0; y < o.height; y++)
      {
        const auto* row = reinterpret_cast<const uint32_t*>(o.pixels.data() + y * o.linesize);
        for (int x = 0; x < o.width; x++)
        {
          const uint32_t px = row[x];
          const uint32_t a = (px >> PIXEL_ASHIFT) & 0xff;
          const uint32_t r = (((px >> PIXEL_RSHIFT) & 0xff) * a + 127) / 255;
          const uint32_t g = (((px >> PIXEL_GSHIFT) & 0xff) * a + 127) / 255;
          const uint32_t b = (((px >> PIXEL_BSHIFT) & 0xff) * a + 127) / 255;
          packed[static_cast<size_t>(y) * o.width + x] = (a << PIXEL_ASHIFT) |
                                                         (r << PIXEL_RSHIFT) |
                                                         (g << PIXEL_GSHIFT) | (b << PIXEL_BSHIFT);
        }
      }
    }
    else
    {
      uint32_t lut[256];
      OVERLAY::BuildRGBALut(o.palette, true, lut);
      int x0 = 0;
      int y0 = 0;
      int x1 = o.width;
      int y1 = o.height;
      OVERLAY::FindVisibleBox(o.pixels.data(), o.linesize, o.width, o.height, lut, x0, y0, x1, y1);
      texture.width = x1 - x0;
      texture.height = y1 - y0;
      if (texture.width != o.width || texture.height != o.height)
        texture.crop = CRect(x0, y0, x1, y1);
      packed.resize(static_cast<size_t>(texture.width) * texture.height);
      OVERLAY::ConvertIndices(o.pixels.data() + y0 * o.linesize + x0, o.linesize, texture.width,
                              texture.height, lut, packed.data());
    }

    std::vector<uint8_t> rgba(packed.size() * 4);
    for (size_t i = 0; i < packed.size(); i++)
    {
      const uint32_t px = packed[i];
      const uint32_t a = (px >> PIXEL_ASHIFT) & 0xff;
      uint32_t c[3] = {(px >> PIXEL_RSHIFT) & 0xff, (px >> PIXEL_GSHIFT) & 0xff,
                       (px >> PIXEL_BSHIFT) & 0xff};
      for (uint32_t& v : c)
      {
        if (m_shown.limited)
          v = (v * 219 + a * 16 + 127) / 255;
      }
      rgba[i * 4 + 0] = static_cast<uint8_t>(c[0]);
      rgba[i * 4 + 1] = static_cast<uint8_t>(c[1]);
      rgba[i * 4 + 2] = static_cast<uint8_t>(c[2]);
      rgba[i * 4 + 3] = static_cast<uint8_t>(a);
    }

    glGenTextures(1, &texture.texture);
    glBindTexture(GL_TEXTURE_2D, texture.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texture.width, texture.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

// COverlayTextureGLES's placement followed by COverlay::CRenderer::Render's,
// then scaled from the GUI's space to the plane
CRect CGraphicsPlaneAML::Place(const CDVDOverlayImage& o,
                               const STexture& texture,
                               const SHdrGraphics& g) const
{
  const CRect& rs = g.source;
  const CRect& rd = g.dest;
  const CRect& rv = g.view;
  if (g.width <= 0 || g.height <= 0 || rs.IsEmpty())
    return CRect();

  bool relative = false;
  bool alignVideo = true;
  float x = static_cast<float>(o.x);
  float y = static_cast<float>(o.y);
  float w = static_cast<float>(o.width);
  float h = static_cast<float>(o.height);
  if (o.source_width > 0 && o.source_height > 0)
  {
    relative = true;
    x = (0.5f * o.width + o.x) / o.source_width;
    y = (0.5f * o.height + o.y) / o.source_height;
    const float subRatio = static_cast<float>(o.source_width) / o.source_height;
    const float vidRatio = rs.Width() / rs.Height();
    const bool square = subRatio > 1.22f && subRatio < 1.34f;
    alignVideo = std::fabs(subRatio - vidRatio) < 0.001f || square;
    if (alignVideo)
    {
      w /= o.source_width;
      h /= o.source_height;
      x *= rs.Width();
      y *= rs.Height();
      w *= rs.Width();
      h *= rs.Height();
    }
    else
    {
      const float ratio = std::min(rv.Width() / o.source_width, rv.Height() / o.source_height);
      x *= rv.Width();
      y *= rv.Height();
      w *= ratio;
      h *= ratio;
    }
  }

  if (alignVideo)
  {
    const float scaleX = rd.Width() / rs.Width();
    const float scaleY = rd.Height() / rs.Height();
    x = x * scaleX + rd.x1;
    y = y * scaleY + rd.y1;
    w *= scaleX;
    h *= scaleY;
  }
  else
  {
    x += rv.x1;
    y += rv.y1;
  }

  CRect r = relative ? CRect(x - w * 0.5f, y - h * 0.5f, x + w * 0.5f, y + h * 0.5f)
                     : CRect(x, y, x + w, y + h);
  if (!texture.crop.IsEmpty())
  {
    const float bx = r.Width() / o.width;
    const float by = r.Height() / o.height;
    r = CRect(r.x1 + texture.crop.x1 * bx, r.y1 + texture.crop.y1 * by,
              r.x1 + texture.crop.x2 * bx, r.y1 + texture.crop.y2 * by);
  }

  const float toPlaneX = WIDTH / g.width;
  const float toPlaneY = HEIGHT / g.height;
  return CRect(r.x1 * toPlaneX, r.y1 * toPlaneY, r.x2 * toPlaneX, r.y2 * toPlaneY);
}

void CGraphicsPlaneAML::DrawGraphics()
{
  if (!m_program || m_shown.images.empty())
    return;

  glUseProgram(m_program);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glActiveTexture(GL_TEXTURE0);
  glEnableVertexAttribArray(m_posLoc);
  glEnableVertexAttribArray(m_texLoc);

  for (const std::shared_ptr<CDVDOverlay>& image : m_shown.images)
  {
    const auto it = m_textures.find(image.get());
    if (it == m_textures.end() || !it->second.texture)
      continue;
    const CRect r = Place(static_cast<const CDVDOverlayImage&>(*image), it->second, m_shown);
    if (r.IsEmpty())
      continue;

    // GL's y runs up, the plane's rows down
    const GLfloat left = r.x1 / WIDTH * 2.0f - 1.0f;
    const GLfloat right = r.x2 / WIDTH * 2.0f - 1.0f;
    const GLfloat top = 1.0f - r.y1 / HEIGHT * 2.0f;
    const GLfloat bottom = 1.0f - r.y2 / HEIGHT * 2.0f;
    const GLfloat pos[] = {left, top, right, top, left, bottom, right, bottom};
    const GLfloat tex[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    glBindTexture(GL_TEXTURE_2D, it->second.texture);
    glVertexAttribPointer(m_posLoc, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(m_texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  glDisableVertexAttribArray(m_posLoc);
  glDisableVertexAttribArray(m_texLoc);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
  glUseProgram(0);
}

void CGraphicsPlaneAML::InitRaster()
{
  const bool tick = XFILE::CFile::Exists("special://profile/graphicstick_on");
  m_selfTest = XFILE::CFile::Exists("special://profile/gselftest");
  // M5: without either file no new program is compiled and G is GT-2's
  if (!tick && !m_selfTest)
    return;
  const bool failCompile = XFILE::CFile::Exists("special://profile/gselftest_failcompile");
  if (m_selfTest)
  {
    XFILE::CFile file;
    std::vector<uint8_t> buf;
    if (file.LoadFile("special://profile/gselftest", buf) > 0)
    {
      const int every = std::atoi(std::string(buf.begin(), buf.end()).c_str());
      if (every > 0)
        m_selfTestEvery = every;
    }
  }
  GRAPHICSTICK::GTLogF(LOGINFO, "GPROG on graphicstick_on={} gselftest={} every={} failcompile={}",
            tick ? 1 : 0, m_selfTest ? 1 : 0, m_selfTestEvery, failCompile ? 1 : 0);

  // the LUTs follow the regime's peak from the first draw; this one only seeds them
  const DISPLAYSTATE::GraphicsRegime regime = DISPLAYSTATE::GetRegime();
  m_raster = std::make_unique<CGraphicsRasterAML>(WIDTH, HEIGHT);
  if (!m_raster->Init(regime.epoch ? regime.peak : 1.0f, failCompile))
  {
    // route C (UpdateTextures/DrawGraphics) is untouched; the new route cannot start
    m_raster.reset();
    GRAPHICSTICK::GTLogF(LOGWARNING, "GTICK off (programs)");
  }
  m_rasterReady = m_raster != nullptr;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, WIDTH, HEIGHT);
}

void CGraphicsPlaneAML::ShadowDraw()
{
  const DISPLAYSTATE::GraphicsRegime published = DISPLAYSTATE::GetRegime();
  if (!published.epoch)
  {
    if (!m_shadowNoRegime)
      GRAPHICSTICK::GTLogF(LOGINFO, "GSELFTEST real: no published regime (gregime_off?), not drawn");
    m_shadowNoRegime = true;
    return;
  }
  if (m_shown.images.empty())
  {
    m_raster->Retain({}, {});
    return;
  }

  const int64_t start = MonotonicNs();
  CGraphicsRasterAML::SRegime regime;
  regime.epoch = published.epoch;
  regime.encoding = published.encoding;
  regime.limited = published.limited;
  regime.peak = published.peak;
  m_raster->ResetCounters();
  const bool cold = m_raster->SetRegime(regime) || m_shadowComps == 0;
  std::vector<std::shared_ptr<CDVDOverlay>> images;
  std::vector<CGraphicsRasterAML::SImage> placed;
  for (const std::shared_ptr<CDVDOverlay>& image : m_shown.images)
  {
    const CGraphicsRasterAML::STexture* texture = m_raster->Expand(image);
    if (!texture)
      continue;
    // Place reads the texture's box only
    STexture box;
    box.width = texture->width;
    box.height = texture->height;
    box.crop = texture->crop;
    const CRect rect = Place(static_cast<const CDVDOverlayImage&>(*image), box, m_shown);
    if (rect.IsEmpty())
      continue;
    images.push_back(image);
    placed.push_back({texture, rect});
  }
  m_raster->Retain(m_shown.images, {});
  const int64_t expanded = MonotonicNs();
  const GLuint target = m_raster->DrawCheck(placed, {});
  // offscreen and never submitted: the wait is this test's, the route will not make it
  glFinish();
  const int64_t end = MonotonicNs();
  const unsigned int comp = ++m_shadowComps;
  GRAPHICSTICK::GTLogF(LOGINFO,
            "GDRAW comp={} regime={} epoch={} peak={:.4f} n={} misses={} px={} cold={} "
            "expand_us={} draw_us={} total_us={}",
            comp, DISPLAYSTATE::GuiEncodingName(regime.encoding), regime.epoch, regime.peak,
            placed.size(), m_raster->Misses(), m_raster->ExpandedTexels(), cold ? 1 : 0,
            (expanded - start) / 1000, (end - expanded) / 1000, (end - start) / 1000);
  if (target && comp % static_cast<unsigned int>(m_selfTestEvery) == 0)
  {
    // against route C's own bytes for the same composition (raw, main's limited), not the
    // renderer's rule: a regime whose class or limited differs from what main applies fails
    // as why=encode_rule(...)
    CGraphicsRasterAML::SEncode routeC;
    routeC.limited = m_shown.limited;
    routeC.peak = regime.peak;
    const std::vector<CGraphicsRasterAML::SEncode> expected(images.size(), routeC);
    m_raster->CheckComposition("real/" + std::to_string(comp), images, placed, expected, {},
                               CGraphicsRasterAML::SEncode());
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, WIDTH, HEIGHT);
}

const char* CGraphicsPlaneAML::TickRefusal() const
{
  if (m_testPattern)
    return "graphicsplane test pattern";
  if (!m_ready)
    return "plane not ready";
  if (m_coordinator->GraphicsRefused())
    return "plane refused";
  if (!m_rasterReady)
    return "programs";
  return nullptr;
}

namespace
{
// the GUI's space the video rects are in: W's committed GUI row, else the one main drew in
void GuiSize(float& width, float& height)
{
  width = 0.0f;
  height = 0.0f;
  // G (RC23): lock-free, valid until G's next GetRT
  if (const auto* state = DISPLAYSTATE::GetRT(DISPLAYSTATE::RT_G))
  {
    width = static_cast<float>(state->wRow.width);
    height = static_cast<float>(state->wRow.height);
  }
  GUISNAPSHOT::SSnapshot snapshot;
  if ((width <= 0.0f || height <= 0.0f) && GUISNAPSHOT::Read(snapshot) && snapshot.valid)
  {
    width = static_cast<float>(snapshot.row.width);
    height = static_cast<float>(snapshot.row.height);
  }
}

bool IsLibass(const CDVDOverlay& o)
{
  return o.IsOverlayType(DVDOVERLAY_TYPE_TEXT) || o.IsOverlayType(DVDOVERLAY_TYPE_SSA);
}
} // namespace

CGraphicsRasterAML::SRegime CGraphicsPlaneAML::RouteRegime()
{
  const DISPLAYSTATE::MainApplied applied = DISPLAYSTATE::GetMainApplied();
  // main's GUI reads the peak live from the mirror (GetGuiSdrPeakLuminance's rule)
  const float peak = DISPLAYSTATE::GuiPeakCode(OUTPUT::CSettingsMirror::Get().GetInt(
      CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE));
  if (applied.epoch != m_routeAppliedEpoch || peak != m_routePeak)
  {
    m_routeAppliedEpoch = applied.epoch;
    m_routePeak = peak;
    m_routeRegimeEpoch++;
    for (SComposition& comp : m_routeDrawn)
      comp.redraw = true;
    GRAPHICSTICK::GTLogF(LOGINFO, "GREGIME G draws in main's applied epoch={} class={} limited={} peak={:.4f}",
              applied.epoch, DISPLAYSTATE::GuiEncodingName(applied.encoding),
              applied.limited ? 1 : 0, peak);
  }
  CGraphicsRasterAML::SRegime regime;
  regime.epoch = m_routeRegimeEpoch;
  regime.encoding = applied.encoding;
  regime.limited = applied.limited;
  regime.peak = peak;
  return regime;
}

GRAPHICSTICK::SRects CGraphicsPlaneAML::RouteRects(const SComposition& comp) const
{
  // the area is the display's, not the picture's: C's newest
  if (m_routeHaveReleased && m_routeReleased.rects.valid)
    return m_routeReleased.rects;
  return comp.rects;
}

void CGraphicsPlaneAML::RenderLibass(SComposition& comp)
{
  const int64_t start = MonotonicNs();
  RenderLibassTimed(comp);
  comp.assUs = (MonotonicNs() - start) / 1000; // GT-D15 measurement (GPLACE ass_us)
}

void CGraphicsPlaneAML::RenderLibassTimed(SComposition& comp)
{
  comp.glyphs.clear();
  const OVERLAY::SSubtitleLayout layout = OVERLAY::PublishedSubtitleLayout();
  const GRAPHICSTICK::SRects rects = RouteRects(comp);
  for (const OVERLAY::SFrameOverlay& o : comp.overlays)
  {
    if (!o.overlay || !IsLibass(*o.overlay))
      continue;
    auto& ass = static_cast<CDVDOverlayLibass&>(*o.overlay);
    const std::shared_ptr<CDVDSubtitlesLibass> handler = ass.GetLibassHandler();
    if (!handler || !layout.style || !rects.valid || rects.view.IsEmpty() ||
        !KODI::VIDEO::SUBTITLES::ShouldRenderStereoOverlay(o.overlay->m_stereoView,
                                                           RenderStereoView::OFF,
                                                           layout.stereomode))
    {
      comp.glyphs.emplace_back();
      continue;
    }
    const KODI::SUBTITLES::STYLE::renderOpts opts =
        OVERLAY::LibassRenderOpts(layout, ass, rects.source, rects.dest, rects.view);
    // the images are libass's until its next render, on any thread
    std::unique_lock lock(handler->RenderSection());
    int changes = 0;
    ASS_Image* images = handler->RenderImage(o.pts, opts, layout.style, &changes);
    std::shared_ptr<const OVERLAY::SQuads>& last = m_assLast[handler.get()];
    if (!images)
      last.reset();
    else if (changes > 0 || !last)
    {
      auto quads = std::make_shared<OVERLAY::SQuads>();
      if (OVERLAY::convert_quad(images, *quads, static_cast<int>(opts.frameWidth)))
        last = std::move(quads);
      else
        last.reset();
    }
    comp.glyphs.push_back(last);
  }
}

bool CGraphicsPlaneAML::Same(const SComposition& a, const SComposition& b) const
{
  return a.images == b.images && a.glyphs == b.glyphs && a.hidden == b.hidden &&
         a.untimedIds == b.untimedIds;
}

bool CGraphicsPlaneAML::CanDraw(bool redraw) const
{
  // depth LOCK_CAP - 2 ahead: the buffer on screen and the one in flight are C's too. A redraw
  // replaces C's buffer of its serial, or the one on screen (review rB M-2: paused with two drawn
  // ahead, a setting change still shows). GT-5 (item 2): with an untimed state up, one ahead,
  // so a buffer stays for the frame on screen's untimed entry (item 5 (e))
  // GT-D15 ring: on screen, in flight, then RouteCap() - 3 drawn ahead (3 of 6), one buffer
  // left to draw the next; with an untimed state up one fewer (2 of 6), for the frame on screen's
  // untimed entry
  const int cap = RouteCap();
  const int depth = cap - (m_untimedOnG && UntimedUp() ? 4 : 3) + (RingOn() ? 0 : 1);
  return RouteFree() > 0 && (redraw || m_coordinator->TimedHeld() < depth);
}

bool CGraphicsPlaneAML::DrawTimed(SComposition& comp)
{
  if (!CanDraw(comp.redraw))
    return false;
  std::vector<CPresentationCoordinator::STimedBuffer> unit(1);
  if (!DrawComposition(comp, unit.front()))
    return false;
  m_coordinator->SubmitTimed(unit);
  return true;
}

void CGraphicsPlaneAML::RetainRoute(std::vector<std::shared_ptr<CDVDOverlay>>& images,
                                    std::vector<CGraphicsRasterAML::SGlyphs>& glyphs) const
{
  // GT-5: a redraw (item 5 (d), a regime or geometry change) of an entry G still holds, and the
  // untimed pages, are pass 2 only: their textures and glyph atlases are not dropped and made
  // again at every draw of another entry (GT-D15's per-frame redraw cost)
  const auto keepImages = [&](const std::vector<std::shared_ptr<CDVDOverlay>>& list)
  { images.insert(images.end(), list.begin(), list.end()); };
  for (const SComposition& comp : m_routeDrawn)
  {
    for (const OVERLAY::SFrameOverlay& o : comp.overlays)
      if (o.overlay && o.overlay->IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
        images.push_back(o.overlay);
    keepImages(comp.untimed);
    for (const std::shared_ptr<const OVERLAY::SQuads>& quads : comp.glyphs)
      if (quads)
      {
        CGraphicsRasterAML::SGlyphs item;
        item.quads = quads;
        glyphs.push_back(std::move(item));
      }
  }
  keepImages(m_uCur.images);
  keepImages(m_uWait.images);
}

bool CGraphicsPlaneAML::DrawComposition(SComposition& comp,
                                        CPresentationCoordinator::STimedBuffer& out)
{
  if (RouteFree() <= 0)
    return false;
  const int64_t drawStart = MonotonicNs(); // GT-D15 measurement (GPLACE draw_us)
  const CGraphicsRasterAML::SRegime regime = RouteRegime();
  m_raster->SetRegime(regime);
  GRAPHICSTICK::SRects rects = RouteRects(comp);
  // GT-5: a menu-only screen has no video rects; main draws it over the whole GUI then
  // (RenderWithoutPicture), and so does G
  if (!rects.valid && comp.bridge && m_untimedOnG)
  {
    float width = 0.0f;
    float height = 0.0f;
    GuiSize(width, height);
    if (width > 0.0f && height > 0.0f)
    {
      rects.source = rects.dest = rects.view = CRect(0.0f, 0.0f, width, height);
      rects.valid = true;
    }
  }
  comp.rects = rects;
  SHdrGraphics area;
  area.source = rects.source;
  area.dest = rects.dest;
  area.view = rects.view;
  GuiSize(area.width, area.height);

  std::vector<std::shared_ptr<CDVDOverlay>> kept;
  std::vector<CGraphicsRasterAML::SImage> placed;
  std::vector<CGraphicsRasterAML::SGlyphs> glyphs;
  bool visible = false;
  const std::string stereomode = OVERLAY::PublishedSubtitleLayout().stereomode;
  const auto place = [&](const std::shared_ptr<CDVDOverlay>& image, const SHdrGraphics& g)
  {
    // review rB m-4: a 3D source's eye halves, as main's Render picks them (mono output: the
    // route is off in a stereo output mode)
    if (!KODI::VIDEO::SUBTITLES::ShouldRenderStereoOverlay(image->m_stereoView,
                                                           RenderStereoView::OFF, stereomode))
      return false;
    const CGraphicsRasterAML::STexture* texture = m_raster->Expand(image);
    if (!texture)
      return false;
    STexture box;
    box.width = texture->width;
    box.height = texture->height;
    box.crop = texture->crop;
    const CRect rect = Place(static_cast<const CDVDOverlayImage&>(*image), box, g);
    if (rect.IsEmpty())
      return false;
    kept.push_back(image);
    placed.push_back({texture, rect});
    return true;
  };
  if (!comp.hidden && rects.valid && area.width > 0 && area.height > 0)
  {
    for (const OVERLAY::SFrameOverlay& o : comp.overlays)
      if (o.overlay && o.overlay->IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
        visible |= place(o.overlay, area);
    // GT-5: the state comp carries, over the frame's timed overlays, in every regime (item 6)
    if (m_untimedOnG)
    {
      for (const std::shared_ptr<CDVDOverlay>& image : comp.untimed)
        if (image->IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
          visible |= place(image, area);
    }
    // the untimed bridge (guntimed_off): main's present-latest images, over the frame's, in
    // Composite only (in Srgb and Scalar main draws them on the GUI plane)
    else if (regime.encoding == DISPLAYSTATE::GuiEncoding::Composite)
      for (const std::shared_ptr<CDVDOverlay>& image : m_shown.images)
        place(image, m_shown);
    const float toPlaneX = WIDTH / area.width;
    const float toPlaneY = HEIGHT / area.height;
    for (const std::shared_ptr<const OVERLAY::SQuads>& quads : comp.glyphs)
    {
      if (!quads)
        continue;
      CGraphicsRasterAML::SGlyphs item;
      item.quads = quads;
      item.frameWidth = rects.view.Width();
      item.frameHeight = rects.view.Height();
      item.view = CRect(rects.view.x1 * toPlaneX, rects.view.y1 * toPlaneY,
                        rects.view.x2 * toPlaneX, rects.view.y2 * toPlaneY);
      glyphs.push_back(std::move(item));
      visible = true;
    }
  }
  if (m_untimedOnG)
  {
    std::vector<std::shared_ptr<CDVDOverlay>> keepImages = kept;
    std::vector<CGraphicsRasterAML::SGlyphs> keepGlyphs = glyphs;
    RetainRoute(keepImages, keepGlyphs);
    m_raster->Retain(keepImages, keepGlyphs);
  }
  else
    m_raster->Retain(kept, glyphs);

  int slot = -1;
  if (!RouteBeginDraw(slot))
    return false;
  glViewport(0, 0, WIDTH, HEIGHT);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  m_raster->Draw(placed, glyphs, slot >= 0 ? m_ring[slot].fbo : 0);
  // the self-test and the shadow draw render for read-back, unflipped
  m_raster->SetTargetFlipped(false);

  gbm_bo* bo = nullptr;
  uint32_t fb = 0;
  drmModeAtomicReqPtr req = nullptr;
  int fence = -1;
  if (!RouteTakeFrame(slot, bo, fb, req, fence))
    return false;
  out = CPresentationCoordinator::STimedBuffer();
  out.bo = bo;
  out.fb = fb;
  out.req = req;
  out.fence = fence;
  out.pts = comp.pts;
  out.serial = comp.serial;
  out.visible = visible;
  out.bridge = comp.bridge;
  out.session = m_routeSession;
  out.u = comp.u;
  out.firstPage = comp.firstPage;
  out.regimeEpoch = regime.epoch;
  out.untimedRoute = m_untimedOnG;
  out.assUs = comp.assUs;
  out.submitNs = MonotonicNs();
  out.drawUs = (out.submitNs - drawStart) / 1000;
  m_lastFb = fb;
  m_lastVisible = !placed.empty() || !glyphs.empty();
  m_routeLastVisible = m_lastVisible;
  comp.redraw = false;
  comp.untimedStale = false;
  return true;
}

bool CGraphicsPlaneAML::SubmitClear(uint64_t serial)
{
  int slot = -1;
  if (RouteFree() <= 0 || !RouteBeginDraw(slot))
    return false;
  glViewport(0, 0, WIDTH, HEIGHT);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  gbm_bo* bo = nullptr;
  uint32_t fb = 0;
  drmModeAtomicReqPtr req = nullptr;
  int fence = -1;
  if (!RouteTakeFrame(slot, bo, fb, req, fence))
    return false;
  std::vector<CPresentationCoordinator::STimedBuffer> unit(1);
  CPresentationCoordinator::STimedBuffer& buffer = unit.front();
  buffer.bo = bo;
  buffer.fb = fb;
  buffer.req = req;
  buffer.fence = fence;
  buffer.serial = serial;
  buffer.clear = true;
  buffer.session = m_routeSession;
  m_coordinator->SubmitTimed(unit);
  m_lastFb = fb;
  m_lastVisible = false;
  m_routeLastVisible = false;
  return true;
}

void CGraphicsPlaneAML::RouteStart(uint64_t floorGen)
{
  if (!m_ringTried)
  {
    m_ringTried = true;
    InitRing();
  }
  m_routeOn = true;
  m_routeQueue.clear();
  m_routeHasPending = false;
  m_routePending = SComposition();
  m_routeLast = SComposition();
  m_routeDrawn.clear();
  m_routeHaveReleased = false;
  m_routeFloorGen = floorGen;
  m_routeLayoutGen = 0;
  m_assLast.clear();
  // route C's images (main's whole set until now) leave the plane
  m_shown = SHdrGraphics();
  m_untimed.clear();
  m_redraw = false;
  m_routeEndClearOwed = false;
  m_routeClearOwed = true;
  m_routeClearSerial = 0;
  m_routeClearAtFloor = false;
  GRAPHICSTICK::NoteGSide(m_routeSession, true);
  GRAPHICSTICK::GTLogF(LOGINFO, "GTICK G on: the timed graphics are drawn here, C picks them per tick");
  // GT-5: the untimed graphics too, unless guntimed_off keeps GT-4's bridge
  m_uCur = SUntimedPage();
  m_uWait = SUntimedPage();
  m_uReleasedSegment = 0;
  m_uNotedSegment = 0;
  m_uFirstPageGen = 0;
  m_uFirstPageSerial = 0;
  m_uExpired = false;
  m_uDeferredU = 0;
  m_uTakenU = 0;
  m_uAdvancing = true;
  m_uFramelessShown = false;
  m_uScreenOwed = false;
  m_routeHistory.clear();
  m_routeTimedCleared = true; // the start's clear
  m_uSegFirst = SComposition();
  m_uPageGenSeen = 0;
  m_uLostSeen = GRAPHICSTICK::GraphicsLost();
  m_uFramelessIds.clear();
  m_untimedOnG = !GRAPHICSTICK::UntimedOff();
  if (m_untimedOnG)
  {
    GRAPHICSTICK::SetUntimedHook(true);
    // what the container shows now (main stops selecting it at its next pass)
    GRAPHICSTICK::WithContainer([](CDVDOverlayContainer& container)
                                { container.PostUntimedSnapshot(); });
    GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED G on: the untimed graphics are taken here, newest first{}",
              GRAPHICSTICK::NewestOff() ? " (gnewest_off: every state, in post order)" : "");
  }
}

void CGraphicsPlaneAML::RouteEnd()
{
  m_routeOn = false;
  m_routeQueue.clear();
  m_routeHasPending = false;
  m_routePending = SComposition();
  m_routeLast = SComposition();
  m_routeDrawn.clear();
  m_assLast.clear();
  m_raster->Retain({}, {});
  m_shown = SHdrGraphics();
  m_untimed.clear();
  m_routeClearOwed = false;
  // item 11: the stop's clear is G's (VP's HasVisibleOverlay-gated clear reads empty here)
  m_routeEndClearOwed = true;
  // main renders the timed libass overlays again from here (review rA M1)
  GRAPHICSTICK::NoteGSide(m_routeSession, false);
  GRAPHICSTICK::GTLogF(LOGINFO, "GTICK G off: the timed graphics are main's again");
  if (m_untimedOnG)
  {
    // main selects the untimed composition again from its next pass
    GRAPHICSTICK::SetUntimedHook(false);
    m_untimedOnG = false;
    m_uCur = SUntimedPage();
    m_uWait = SUntimedPage();
    m_uExpiredPage = SUntimedPage();
    GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED G off: the untimed graphics are main's again");
  }
}

bool CGraphicsPlaneAML::RouteStep()
{
  GRAPHICSTICK::SInbox in;
  GRAPHICSTICK::Take(in);
  if (in.session != m_routeSession)
  {
    m_routeSession = in.session;
    if (m_routeOn)
      RouteEnd();
    if (in.on)
      RouteStart(in.floor.gen);
  }
  if (!m_routeOn)
    return false;

  if (m_coordinator->GraphicsRefused())
  {
    GRAPHICSTICK::EndRoute("plane refused");
    RouteEnd();
    return false;
  }
  GUISNAPSHOT::SSnapshot snapshot;
  if (GUISNAPSHOT::Read(snapshot) && snapshot.valid)
  {
    // D-G2-2: a stereo output mode hands the graphics back to main
    if (snapshot.stereoMode != static_cast<int>(RenderStereoMode::OFF))
    {
      GRAPHICSTICK::EndRoute("stereo");
      RouteEnd();
      return false;
    }
    // D-G2-1: no video window shown, no graphics
    const bool hidden = !snapshot.fullScreenVideo && snapshot.videoRect.IsEmpty();
    if (hidden != m_routeHidden)
    {
      m_routeHidden = hidden;
      for (SComposition& comp : m_routeDrawn)
      {
        comp.hidden = hidden;
        comp.redraw = true;
      }
      m_routeLast.hidden = hidden;
    }
  }

  for (GRAPHICSTICK::SQueued& queued : in.queued)
    m_routeQueue.push_back(std::move(queued));

  // M2: a flush's floor; the plane shows nothing at or below it from C's next slot
  if (in.floor.gen != m_routeFloorGen)
  {
    m_routeFloorGen = in.floor.gen;
    const uint64_t floor = in.floor.floor;
    while (!m_routeQueue.empty() && m_routeQueue.front().serial <= floor)
      m_routeQueue.pop_front();
    if (m_routeHasPending && m_routePending.serial <= floor)
      m_routeHasPending = false;
    m_routeDrawn.erase(std::remove_if(m_routeDrawn.begin(), m_routeDrawn.end(),
                                      [floor](const SComposition& c) { return c.serial <= floor; }),
                       m_routeDrawn.end());
    // the plane is cleared: the next picture is drawn whatever it shows (review rB M-1)
    const bool hidden = m_routeLast.hidden;
    m_routeLast = SComposition();
    m_routeLast.hidden = hidden;
    // pictures at or below a floor are gone, and so is every timed composition from the plane
    m_routeTimedCleared = true;
    m_routeHistory.erase(std::remove_if(m_routeHistory.begin(), m_routeHistory.end(),
                                        [floor](const SComposition& c) { return c.serial <= floor; }),
                         m_routeHistory.end());
    m_routeClearOwed = true;
    m_routeClearSerial = floor;
    m_routeClearAtFloor = true;
  }

  if (in.released)
  {
    const GRAPHICSTICK::SRects& was = m_routeReleased.rects;
    const GRAPHICSTICK::SRects& now = in.last.rects;
    const bool moved = !m_routeHaveReleased || was.valid != now.valid || was.rgen != now.rgen ||
                       was.source != now.source || was.dest != now.dest || was.view != now.view;
    m_routeReleased = in.last;
    m_routeHaveReleased = true;
    // the history keeps the newest picture at or below the frame on screen and all after it
    {
      size_t keep = 0;
      bool any = false;
      for (size_t i = 0; i < m_routeHistory.size(); i++)
        if (m_routeHistory[i].serial <= in.last.serial)
        {
          keep = i;
          any = true;
        }
      m_routeHistory.erase(m_routeHistory.begin(),
                           m_routeHistory.begin() + static_cast<long>(keep));
      // the plane shows a picture's timed composition again once one after the clear is out
      if (any)
        m_routeTimedCleared = false;
    }
    // compositions before the frame on screen are no longer anyone's
    size_t current = 0;
    for (size_t i = 0; i < m_routeDrawn.size(); i++)
      if (m_routeDrawn[i].serial <= in.last.serial)
        current = i;
    m_routeDrawn.erase(m_routeDrawn.begin(), m_routeDrawn.begin() + current);
    if (moved)
      for (SComposition& comp : m_routeDrawn)
        comp.redraw = true;
    // GT-5 (item 5): the page gate moves to C's release. A page of this segment waiting in the
    // container becomes current now; G drew it ahead with the segment's frames, and C commits it
    // with the first of them (that frame's slot is the next tick's)
    if (m_untimedOnG && in.last.segmentGen > m_uNotedSegment)
    {
      // the page counts as released when the container's snapshot says so (taken below), so no
      // frame is drawn with the old page in between
      const unsigned int gen = in.last.segmentGen;
      m_uNotedSegment = gen;
      GRAPHICSTICK::WithContainer([gen](CDVDOverlayContainer& container)
                                  { container.NotePresentedSegment(gen); });
    }
  }
  // bounded, but never the frame on screen's entry: it carries an untimed change to C's next
  // slot. Trimmed from the front, a deep video queue (RC8 Superman BD-J: 14 frames queued, a
  // state per frame) took it with the oldest and a menu change then waited for an entry's own
  // frame, the queue's depth later (u=34..42: 12 vblanks). The bound covers the deepest queue
  if (m_routeDrawn.size() > ROUTE_DRAWN_CAP)
  {
    const uint64_t onScreen = m_routeHaveReleased ? m_routeReleased.serial : 0;
    size_t screen = 0;
    for (size_t i = 0; i < m_routeDrawn.size(); i++)
      if (m_routeDrawn[i].serial <= onScreen)
        screen = i;
    // the entries G may still redraw: the frame on screen's and every later one, then the
    // oldest of those later ones if still over
    m_routeDrawn.erase(m_routeDrawn.begin(), m_routeDrawn.begin() + static_cast<long>(screen));
    if (m_routeDrawn.size() > ROUTE_DRAWN_CAP)
    {
      GRAPHICSTICK::GTLogF(LOGWARNING, "GTICK drawn over {}: {} entries ahead dropped from G's books",
                           ROUTE_DRAWN_CAP, m_routeDrawn.size() - ROUTE_DRAWN_CAP);
      m_routeDrawn.erase(m_routeDrawn.begin() + 1,
                         m_routeDrawn.begin() + 1 +
                             static_cast<long>(m_routeDrawn.size() - ROUTE_DRAWN_CAP));
    }
  }

  const uint64_t layoutGen = OVERLAY::PublishedSubtitleLayout().gen;
  if (layoutGen != m_routeLayoutGen)
  {
    m_routeLayoutGen = layoutGen;
    for (SComposition& comp : m_routeDrawn)
      if (!comp.glyphs.empty())
        comp.redraw = true;
  }
  // GT-5: the newest state is taken before the timed draws, so a frame drawn now carries it
  // (item 5 (c)); its re-draws wait for the timed work (RedrawUntimed, below)
  if (m_untimedOnG)
  {
    // review rB M2: C stopped picking frame entries (paused, a title's end with RM unconfigured,
    // video detached): what C last picked may be a frame entry it will never commit, so the
    // frame on screen's composition is re-drawn (as an untimed-only entry) with what is up
    const bool advancing = GRAPHICSTICK::CAdvancing();
    if (advancing != m_uAdvancing)
    {
      m_uAdvancing = advancing;
      m_uFramelessShown = false;
      if (!advancing && UntimedUp())
        MarkScreenStale();
    }
    // review rB m9: a commit carrying the plane was lost; a static menu gets no further post
    const uint64_t lost = GRAPHICSTICK::GraphicsLost();
    if (lost != m_uLostSeen)
    {
      m_uLostSeen = lost;
      for (SComposition& comp : m_routeDrawn)
        comp.redraw = true;
      m_uFramelessShown = false;
      if (UntimedUp())
        MarkScreenStale();
    }
    TakeUntimedStates();
    CheckKeepAlive();
  }
  RouteRegime();
  // GT-5: main hands nothing untimed over (its sets are empty while G holds the hook)
  // (on the route m_redraw and m_untimed are route C's, unused: GT-4 forwards no GUNTIMED post)
  if (m_untimedOnG)
  {
    TakeShown();
    m_untimed.clear();
    m_redraw = false;
  }
  // the untimed bridge (guntimed_off): main's composition changed
  else if (!TakeShown())
    m_redraw = false;
  else
  {
    m_redraw = false;
    m_untimed.clear();
    for (SComposition& comp : m_routeDrawn)
      comp.redraw = true;
    if (m_routeDrawn.empty() && !m_routeHasPending && m_routeQueue.empty())
    {
      // nothing timed drawn since the start or a floor: the untimed images alone, at C's next
      // slot and under no floor (review rA M2)
      SComposition comp = m_routeLast;
      comp.bridge = true;
      comp.redraw = true;
      m_routeDrawn.push_back(std::move(comp));
    }
  }

  if (m_routeClearOwed)
  {
    // review rB m3: a floor (a seek) under a menu clears the timed graphics only; the menu stays
    // in the clear itself, so it never blinks off until a bridge or the first post-seek frame.
    // Only at a floor (r2 N1): the route's start clear goes before the start's hold (a clear is
    // picked at the first slot), so a menu up then waits for the hold as an untimed-only entry
    if (m_untimedOnG && m_routeClearAtFloor && !m_uCur.ids.empty())
    {
      SComposition comp;
      comp.serial = m_routeClearSerial;
      comp.hidden = m_routeHidden;
      comp.bridge = true; // its images only, no frame; the whole GUI without video rects
      AssignPage(comp);
      std::vector<CPresentationCoordinator::STimedBuffer> unit(1);
      if (!DrawComposition(comp, unit.front()))
        return true;
      unit.front().clear = true;
      unit.front().bridge = false;
      m_coordinator->SubmitTimed(unit);
      m_routeLast.untimedIds = comp.untimedIds;
      // r2 N1: kept as an untimed-only entry, so a regime, geometry or layout redraw (and a
      // newer state) reaches the menu the clear carries; drawn again only for those
      comp.redraw = false;
      comp.untimedStale = false;
      m_routeDrawn.push_back(std::move(comp));
    }
    else if (!SubmitClear(m_routeClearSerial))
      return true;
    m_routeClearOwed = false;
  }
  // the draw-ahead depth or the buffers stop the timed draws; GT-5's untimed work still runs
  // then (the frames within the depth have their entries, item 5 (c))
  bool timedWaits = false;
  for (SComposition& comp : m_routeDrawn)
  {
    // GT-5: an entry owing a newer untimed state is redrawn by RedrawUntimed's unit, in order
    if (!comp.redraw || comp.untimedStale)
      continue;
    if (!comp.glyphs.empty())
      RenderLibass(comp);
    if (!DrawTimed(comp))
    {
      timedWaits = true;
      break;
    }
  }
  // timed first: in serial order, a draw only where the composition changed
  while (!timedWaits && (m_routeHasPending || !m_routeQueue.empty()))
  {
    if (!m_routeHasPending)
    {
      GRAPHICSTICK::SQueued queued = std::move(m_routeQueue.front());
      m_routeQueue.pop_front();
      SComposition comp;
      comp.serial = queued.serial;
      comp.pts = queued.pts;
      comp.overlays = std::move(queued.overlays);
      comp.rects = queued.rects;
      comp.hidden = m_routeHidden;
      comp.segmentGen = queued.segmentGen;
      for (const OVERLAY::SFrameOverlay& o : comp.overlays)
        if (o.overlay && o.overlay->IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
          comp.images.push_back(o.overlay.get());
      // GT-5: the newest state whose images G has (item 5 (c)); a segment's frames its page
      if (m_untimedOnG)
        AssignPage(comp);
      RenderLibass(comp);
      // review rB M3: each segment's first picture, drawn or not (a Same frame has no entry): a
      // page of that segment arriving later still goes on it
      if (m_untimedOnG && comp.segmentGen > m_uSegFirst.segmentGen)
        m_uSegFirst = comp;
      // every picture taken, drawn or Same: the frame C picks for next may have no entry, and a
      // menu change needs one at or below it (RC13 B1_m3gan: GTICK no pick, every held entry
      // 10-11 frames ahead of the target)
      if (m_untimedOnG)
      {
        // bounded by Released (only the newest picture at or below the frame on screen is
        // kept), not by a count: V can take several pictures per frame released and run 14
        // frames ahead (RC15 B2_m3gan: the 32 newest were all queued frames, none at or below
        // the frame on screen, so no entry could be made there)
        m_routeHistory.push_back(comp);
        if (m_routeHistory.size() > ROUTE_HISTORY_CAP)
          m_routeHistory.pop_front();
      }
      if (Same(comp, m_routeLast))
      {
        // the bridge's serial: the newest picture, never one under a later floor
        m_routeLast.serial = comp.serial;
        GRAPHICSTICK::NoteProcessed(comp.serial);
        continue;
      }
      m_routePending = std::move(comp);
      m_routeHasPending = true;
    }
    // GT-5: a composition held back for a buffer goes with the state up when it is drawn (a
    // waiting page that arrived meanwhile, review rB M3)
    if (m_untimedOnG)
      AssignPage(m_routePending);
    if (!DrawTimed(m_routePending))
    {
      timedWaits = true;
      break;
    }
    GRAPHICSTICK::NoteProcessed(m_routePending.serial);
    // a bridge is only for while nothing timed is drawn
    m_routeDrawn.erase(std::remove_if(m_routeDrawn.begin(), m_routeDrawn.end(),
                                      [](const SComposition& c) { return c.bridge; }),
                       m_routeDrawn.end());
    m_routeLast = m_routePending;
    m_routeDrawn.push_back(std::move(m_routePending));
    m_routePending = SComposition();
    m_routeHasPending = false;
  }
  // GT-5: untimed work only once every frame within the draw-ahead has its timed entry (item 5
  // (c), timed first)
  const bool untimedWaits = m_untimedOnG && RedrawUntimed();
  if (m_untimedOnG)
    NoteScreenOwed(untimedWaits);
  return untimedWaits || timedWaits;
}

void CGraphicsPlaneAML::NoteScreenOwed(bool waits)
{
  // measurement (log only, at most 5 lines a second): a new untimed state still not drawn at or
  // below the frame on screen after a route step, with what G holds (RC15-RC17: on M3GAN the
  // same frames, 584..653, never got the change; neither the owed entry nor the bridge fired)
  // with no video yet (a menu before its first picture) the change goes as an untimed-only
  // entry: no signal
  if (!m_uScreenOwed || !m_routeHaveReleased || (m_uOwedLogged && m_uOwedLoggedU == m_uCur.u))
    return;
  const int64_t now = MonotonicNs();
  if (now - m_uOwedWindowNs >= 1000000000)
  {
    m_uOwedWindowNs = now;
    m_uOwedLines = 0;
  }
  if (++m_uOwedLines > 5)
    return;
  m_uOwedLogged = true;
  m_uOwedLoggedU = m_uCur.u;
  std::string drawn;
  for (const SComposition& c : m_routeDrawn)
    drawn += StringUtils::Format(" {}:u{}{}{}", c.serial, c.u, c.untimedStale ? "s" : "",
                                 c.bridge ? "b" : "");
  GRAPHICSTICK::GTLogF(
      LOGINFO,
      "GUNTIMED owed u={} released={} advancing={} free={} waits={} history={}..{}({}) queue={} "
      "pending={} drawn:{}",
      m_uCur.u, m_routeHaveReleased ? static_cast<int64_t>(m_routeReleased.serial) : -1,
      m_uAdvancing ? 1 : 0, RouteFree(), waits ? 1 : 0,
      m_routeHistory.empty() ? 0 : m_routeHistory.front().serial,
      m_routeHistory.empty() ? 0 : m_routeHistory.back().serial, m_routeHistory.size(),
      m_routeQueue.size(), m_routeHasPending ? static_cast<int64_t>(m_routePending.serial) : -1,
      drawn);
}

const CGraphicsPlaneAML::SUntimedPage& CGraphicsPlaneAML::PageFor(const SComposition& comp) const
{
  // item 5: a frame of a segment not yet released shows that segment's newest waiting page; an
  // untimed-only entry never does (a page waits for its picture)
  if (!comp.bridge && !m_uWait.ids.empty() && m_uWait.gen > m_uReleasedSegment &&
      comp.segmentGen >= m_uWait.gen)
    return m_uWait;
  return m_uCur;
}

void CGraphicsPlaneAML::AssignPage(SComposition& comp)
{
  const SUntimedPage& page = PageFor(comp);
  comp.u = page.u;
  // item 5 (f): the first page is the one committed with the segment's first frame; later
  // frames of the segment carry the same page as an ordinary entry
  comp.firstPage = false;
  if (&page == &m_uWait)
  {
    if (m_uFirstPageGen != m_uWait.gen)
    {
      m_uFirstPageGen = m_uWait.gen;
      m_uFirstPageSerial =
          m_uSegFirst.segmentGen == m_uWait.gen ? m_uSegFirst.serial : comp.serial;
    }
    comp.firstPage = comp.serial == m_uFirstPageSerial;
  }
  comp.untimed = page.images;
  comp.untimedIds = page.ids;
}

void CGraphicsPlaneAML::MarkScreenStale()
{
  const uint64_t onScreen = m_routeHaveReleased ? m_routeReleased.serial : 0;
  int screen = -1;
  for (size_t i = 0; i < m_routeDrawn.size(); i++)
    if (m_routeDrawn[i].bridge || (m_routeHaveReleased && m_routeDrawn[i].serial <= onScreen))
      screen = static_cast<int>(i);
  if (screen >= 0)
    m_routeDrawn[screen].untimedStale = true;
  else
    m_routeLast.untimedIds.clear(); // nothing held: the untimed-only entry is made again
}

void CGraphicsPlaneAML::MarkUntimedStale()
{
  for (SComposition& comp : m_routeDrawn)
    if (PageFor(comp).ids != comp.untimedIds)
      comp.untimedStale = true;
  // the change must reach C at or below the frame it picks for next, not only in entries for
  // frames still in the video queue
  m_uScreenOwed = true;
}

void CGraphicsPlaneAML::TakeUntimedStates()
{
  // item 5 (j), gnewest_off: the next state only once C has committed (or dropped) the one
  // before, never waiting for it (timed draws go on); 500 ms bounds a state C never gets
  if (GRAPHICSTICK::NewestOff() && m_uTakenU != 0 && GRAPHICSTICK::UntimedCommitted() < m_uTakenU &&
      MonotonicNs() - m_uTakenNs < 500000000)
    return;
  GRAPHICSTICK::SUntimedState state;
  size_t depth = 0;
  if (!GRAPHICSTICK::TakeUntimed(state, depth))
    return;
  GRAPHICSTICK::CountMax(GRAPHICSTICK::UntimedCount::NEWEST_DEPTH_MAX, static_cast<int>(depth));
  GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::STATES);
  ApplyUntimed(state);
}

void CGraphicsPlaneAML::ApplyUntimed(GRAPHICSTICK::SUntimedState& state)
{
  const auto ids = [](const std::vector<std::shared_ptr<CDVDOverlay>>& images)
  {
    std::vector<const CDVDOverlay*> out;
    for (const auto& image : images)
      out.push_back(image.get());
    return out;
  };
  std::vector<SUntimedPost> posts;
  for (const GRAPHICSTICK::SUntimedPosted& skipped : state.skipped)
  {
    SUntimedPost post;
    post.u = skipped.u;
    post.postNs = skipped.postNs;
    post.route = 'G';
    post.cleared = skipped.cleared;
    post.skipped = true;
    posts.push_back(post);
  }
  std::vector<const CDVDOverlay*> current = ids(state.current);
  std::vector<const CDVDOverlay*> waiting = ids(state.waiting);
  bool changed = false;
  // the waiting page became current (its picture was released, or the P-side fallback): it is
  // the state G already drew with the segment's frames, under its own u
  if (!m_uWait.ids.empty() && current == m_uWait.ids)
  {
    // review rB m8: released without its picture's Released (the P-side 3 s fallback)
    if (state.releasedSegment > m_uNotedSegment)
    {
      GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::PAGE_FALLBACK);
      GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED page u={} of segment {} released by the fallback", m_uWait.u,
                m_uWait.gen);
    }
    m_uCur = m_uWait;
    m_uCur.gen = 0;
    m_uWait = SUntimedPage();
    m_uExpired = false;
    changed = true;
  }
  else if (current != m_uCur.ids || (m_uExpired && !current.empty()))
  {
    // review rB m8: a segment's page posted after that segment's picture was released never
    // waited; it is current at once, late
    if (state.currentGen && state.currentGen > m_uPageGenSeen &&
        state.currentGen <= m_uNotedSegment)
    {
      GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::PAGE_LATE_POSTED);
      GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED page u={} of segment {} posted after its picture", state.u,
                state.currentGen);
    }
    m_uPageGenSeen = std::max(m_uPageGenSeen, state.currentGen);
    m_uCur.u = state.u;
    m_uCur.postNs = state.postNs;
    m_uCur.images = std::move(state.current);
    m_uCur.ids = std::move(current);
    m_uCur.gen = 0;
    m_uExpired = false;
    SUntimedPost post;
    post.u = state.u;
    post.postNs = state.postNs;
    post.route = 'G';
    post.cleared = m_uCur.ids.empty();
    posts.push_back(post);
    changed = true;
  }
  if (waiting != m_uWait.ids || (!waiting.empty() && state.waitingGen != m_uWait.gen))
  {
    if (waiting.empty())
      m_uWait = SUntimedPage();
    else
    {
      m_uWait.u = state.u;
      m_uWait.postNs = state.postNs;
      m_uWait.images = std::move(state.waiting);
      m_uWait.ids = std::move(waiting);
      m_uWait.gen = state.waitingGen;
      SUntimedPost post;
      post.u = state.u;
      post.postNs = state.postNs;
      post.route = 'G';
      post.firstPage = true;
      posts.push_back(post);
      GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::FIRST_PAGES);
      m_uPageGenSeen = std::max(m_uPageGenSeen, m_uWait.gen);
      PlaceFirstPage();
    }
    changed = true;
  }
  m_uReleasedSegment = std::max(m_uReleasedSegment, state.releasedSegment);
  if (!posts.empty() && CPresentationCoordinator::UntimedLog())
    m_coordinator->NoteUntimedRoute(std::move(posts));
  if (changed)
  {
    m_uTakenU = state.u;
    m_uTakenNs = MonotonicNs();
    MarkUntimedStale();
  }
}

void CGraphicsPlaneAML::PlaceFirstPage()
{
  // review rB M3: the segment's first picture was taken already, maybe with no entry (Same as
  // the frame before): an entry at its serial carries the page, so C commits it with that frame
  if (m_uSegFirst.segmentGen != m_uWait.gen || !m_uSegFirst.serial)
    return;
  const uint64_t released = m_routeHaveReleased ? m_routeReleased.serial : 0;
  if (m_uSegFirst.serial <= released)
  {
    // its picture is on screen already: the page is late (it shows at the next slot)
    GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::PAGE_LATE_POSTED);
    GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED page u={} of segment {} posted after its first picture", m_uWait.u,
              m_uWait.gen);
    return;
  }
  m_uFirstPageGen = m_uWait.gen;
  m_uFirstPageSerial = m_uSegFirst.serial;
  const bool held = std::any_of(m_routeDrawn.begin(), m_routeDrawn.end(),
                                [this](const SComposition& c)
                                { return !c.bridge && c.serial == m_uSegFirst.serial; }) ||
                    (m_routeHasPending && m_routePending.serial == m_uSegFirst.serial);
  if (held)
    return; // MarkUntimedStale re-draws it with the page (the pending one is assigned at its draw)
  SComposition comp = m_uSegFirst;
  comp.redraw = false;
  comp.bridge = false;
  comp.untimedStale = true;
  // in serial order, before any untimed-only entry (those go last)
  auto at = m_routeDrawn.begin();
  while (at != m_routeDrawn.end() && !at->bridge && at->serial < comp.serial)
    ++at;
  m_routeDrawn.insert(at, std::move(comp));
}

void CGraphicsPlaneAML::CheckKeepAlive()
{
  // VPV's rule for timed overlays, on G for the untimed composition: a BD-J composition the Xlet
  // keeps alive by re-posts goes about 1 s after they stop (BD-J sends no clear). Not while C is
  // PAUSED: the repaint loop may pause too
  constexpr int64_t KEEPALIVE_TTL_MS = 1000;
  const int64_t stamp = GRAPHICSTICK::UntimedKeepAlive();
  const uint64_t posts = GRAPHICSTICK::UntimedPosts();
  const bool stale = stamp != 0 && CurrentHostCounter() - stamp >
                                       CurrentHostFrequency() * KEEPALIVE_TTL_MS / 1000;
  // review rA m3, rB m4 (b): the player's pause, as main's IsPausedPlayback (its speed), not
  // C's state (HELD, DISPLAY_LOST or IDLE on a menu-only screen while paused)
  const bool paused = CDataCacheCore::GetInstance().GetSpeed() == 0.0f;
  if (!m_uExpired && stale && !paused && !m_uCur.ids.empty())
  {
    m_uExpired = true;
    m_uExpiredStamp = stamp;
    m_uExpiredPosts = posts;
    m_uExpiredPage = m_uCur;
    m_uCur = SUntimedPage();
    m_uCur.u = GRAPHICSTICK::NextUntimedU();
    m_uCur.postNs = MonotonicNs();
    GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::KEEPALIVE_EXPIRED);
    GRAPHICSTICK::GTLogF(LOGINFO, "GUNTIMED keep-alive expired u={} (was u={})", m_uCur.u,
              m_uExpiredPage.u);
  }
  // the Xlet re-posts the same composition again: it comes back (the container's repost of the
  // same images is no new state)
  // review rB m4 (a): a re-post with a zero stamp (no streak) never expires, as main showed it
  else if (m_uExpired && !stale && posts != m_uExpiredPosts &&
           (stamp == 0 || stamp != m_uExpiredStamp) && m_uCur.ids.empty())
  {
    m_uExpired = false;
    m_uCur = m_uExpiredPage;
    m_uCur.u = GRAPHICSTICK::NextUntimedU();
    m_uCur.postNs = MonotonicNs();
    m_uExpiredPage = SUntimedPage();
  }
  else
    return;
  if (CPresentationCoordinator::UntimedLog())
  {
    SUntimedPost post;
    post.u = m_uCur.u;
    post.postNs = m_uCur.postNs;
    post.route = 'G';
    post.cleared = m_uCur.ids.empty();
    m_coordinator->NoteUntimedRoute({post});
  }
  MarkUntimedStale();
}

bool CGraphicsPlaneAML::RedrawUntimed()
{
  // nothing drawn to carry the state (a menu-only screen, or a floor or the start before the
  // first frame): the state alone, at C's next slot, above a floor's clear
  if (m_routeDrawn.empty() && !m_routeHasPending && m_routeQueue.empty() &&
      m_uCur.ids != m_routeLast.untimedIds)
  {
    SComposition comp = m_routeLast;
    comp.serial = std::max(comp.serial, m_routeClearSerial);
    comp.bridge = true;
    comp.redraw = true;
    comp.untimedStale = true;
    m_routeDrawn.push_back(std::move(comp));
  }
  const uint64_t onScreen = m_routeHaveReleased ? m_routeReleased.serial : 0;
  // an untimed change with no entry at or below the frame on screen (every frame since the last
  // one drawn was Same, and the entries drawn since are for frames still queued): the frame on
  // screen's composition from G's history becomes that entry, so the change reaches C's next
  // slot (picked for a frame at or above it) instead of riding the queue to its own frame
  // ... one C can take: not below the serial it last picked, which its stale filter drops
  const uint64_t picked = GRAPHICSTICK::PickedSerial(m_routeSession);
  if (m_uScreenOwed && m_routeHaveReleased && m_uAdvancing &&
      std::none_of(m_routeDrawn.begin(), m_routeDrawn.end(),
                   [onScreen, picked](const SComposition& c)
                   { return c.bridge || (c.serial <= onScreen && c.serial >= picked); }))
  {
    // newest at or below the frame on screen; one below C's picked serial would be dropped by
    // C and re-inserted here at every pass, so then the bridge (picked runs ahead of released
    // only across a race with the release)
    const auto it = std::find_if(m_routeHistory.rbegin(), m_routeHistory.rend(),
                                 [onScreen](const SComposition& c) { return c.serial <= onScreen; });
    if (it != m_routeHistory.rend() && it->serial >= picked)
    {
      SComposition comp = *it;
      comp.redraw = false;
      comp.bridge = false;
      comp.untimedStale = true;
      auto at = m_routeDrawn.begin();
      while (at != m_routeDrawn.end() && !at->bridge && at->serial < comp.serial)
        ++at;
      m_routeDrawn.insert(at, std::move(comp));
    }
    else
    {
      // nothing taken at or below it: the state alone as an untimed-only entry at the frame on
      // screen's serial, which C takes at its next slot and which sorts below the entries ahead
      // (they stay C's). Its timed part is empty, which is what the plane shows then: the history
      // loses its last picture at or below the frame on screen only at a floor or a route start
      // (Released always keeps one), and the floor's or start's clear has taken every timed
      // composition at or below it off the plane (M2), until the first picture after it is
      // released, which the history then holds. Anything else would drop what is on screen:
      // counted apart (screenBridgeNoTimed) and logged
      SComposition comp;
      comp.serial = onScreen;
      comp.hidden = m_routeHidden;
      comp.bridge = true;
      comp.untimedStale = true;
      auto at = m_routeDrawn.begin();
      while (at != m_routeDrawn.end() && !at->bridge && at->serial <= comp.serial)
        ++at;
      m_routeDrawn.insert(at, std::move(comp));
      if (m_routeTimedCleared)
        GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::SCREEN_BRIDGE);
      else
      {
        GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::SCREEN_BRIDGE_NO_TIMED);
        GRAPHICSTICK::GTLogF(LOGWARNING,
                             "GUNTIMED screenBridgeNoTimed u={} released={}: no picture at or below "
                             "the frame on screen outside a floor or start",
                             m_uCur.u, onScreen);
      }
    }
  }
  std::vector<size_t> ahead;
  int screen = -1;
  for (size_t i = 0; i < m_routeDrawn.size(); i++)
  {
    const SComposition& comp = m_routeDrawn[i];
    if (comp.bridge || (m_routeHaveReleased && comp.serial <= onScreen))
      screen = static_cast<int>(i);
    else if (comp.untimedStale)
      ahead.push_back(i);
  }
  // an entry below C's picked serial is no frame-on-screen entry C can take: the owed one
  // (inserted above, at or above it) is the last at or below the frame on screen, so it is this
  if (screen >= 0 && !m_routeDrawn[screen].bridge && m_routeDrawn[screen].serial < picked)
    screen = -1;
  if (screen >= 0 && !m_routeDrawn[screen].untimedStale)
    m_uScreenOwed = false;
  // measurement (log only, once per state): a new state that cannot reach C's next slot, and
  // why (the RC4 B1 untimed stalls: 13 vblanks with no graphics flip)
  const auto noteWait = [&](const char* why)
  {
    if (m_uWaitLogged && m_uWaitLoggedU == m_uCur.u)
      return;
    m_uWaitLogged = true;
    m_uWaitLoggedU = m_uCur.u;
    // at most WAIT_LINES_PER_S lines a second (BD-J can post a state per vblank); the rest are
    // counted (waitSuppressed in the GUNTIMED report)
    constexpr int WAIT_LINES_PER_S = 5;
    const int64_t now = MonotonicNs();
    if (now - m_uWaitWindowNs >= 1000000000)
    {
      m_uWaitWindowNs = now;
      m_uWaitLines = 0;
    }
    if (++m_uWaitLines > WAIT_LINES_PER_S)
    {
      GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::WAIT_SUPPRESSED);
      return;
    }
    GRAPHICSTICK::GTLogF(LOGINFO,
              "GUNTIMED wait u={} why={} free={} drawn={} ahead={} screen={} released={} "
              "advancing={}",
              m_uCur.u, why, RouteFree(), m_routeDrawn.size(), ahead.size(),
              screen >= 0 ? static_cast<int64_t>(m_routeDrawn[screen].serial) : -1,
              m_routeHaveReleased ? static_cast<int64_t>(m_routeReleased.serial) : -1,
              m_uAdvancing ? 1 : 0);
  };
  // review rA M1, rB M2: C picks no frame entry now (paused, RM unconfigured between titles,
  // video detached; C's own state, CAdvancing). Neither a frame entry nor the re-draw unit
  // would ever be committed or get its buffers back, so the frame on screen's composition with
  // the new state goes as one untimed-only entry, above every entry G holds (C drops them under
  // it at its pick; the unit re-draws them once C advances again)
  if (!m_uAdvancing)
  {
    const bool stale =
        !ahead.empty() || (screen >= 0 && m_routeDrawn[screen].untimedStale);
    if (!stale)
      return false;
    // with no frame on screen the entry carries no timed overlays (ahead's are not shown yet)
    SComposition comp;
    if (screen >= 0)
      comp = m_routeDrawn[screen];
    comp.hidden = m_routeHidden;
    comp.bridge = true;
    // above the entries of an older state, so C drops them under it at its pick (the unit
    // re-draws them when C advances again); r2 N3: with none stale (no state change, only the
    // edge into not advancing) at the frame on screen's serial, so C keeps the drawn-ahead timed
    // entries, which G will not draw again
    if (!ahead.empty())
    {
      comp.serial = std::max(m_routeLast.serial, m_routeClearSerial);
      for (const SComposition& held : m_routeDrawn)
        comp.serial = std::max(comp.serial, held.serial);
    }
    else
      comp.serial = m_routeDrawn[screen].serial; // stale implies a screen entry here
    AssignPage(comp);
    if (comp.untimedIds == m_uFramelessIds && m_uFramelessShown)
    {
      if (screen >= 0)
        m_routeDrawn[screen].untimedStale = false;
      // R3-3: once C has taken that entry (and dropped the stale ones under it), the drawn-ahead
      // entries are re-drawn with the new state under their own serials while still paused,
      // ready for the resume's first release; C keeps them (above its paused target)
      if (ahead.empty() || GRAPHICSTICK::UntimedCommitted() < comp.u ||
          RouteFree() < static_cast<int>(ahead.size()))
        return false;
    }
    else
    {
      if (!CanDraw(true))
        return true;
      if (!comp.glyphs.empty())
        RenderLibass(comp);
      // a redraw: never held to the draw-ahead depth (DrawTimed's CanDraw(comp.redraw))
      comp.redraw = true;
      if (!DrawTimed(comp))
        return true;
      m_uFramelessIds = comp.untimedIds;
      m_uFramelessShown = true;
      if (screen >= 0 && m_routeDrawn[screen].bridge)
        m_routeDrawn.erase(m_routeDrawn.begin() + screen);
      else if (screen >= 0)
        m_routeDrawn[screen].untimedStale = false;
      m_routeLast.untimedIds = comp.untimedIds;
      // in serial order, so a Released erases only the entries below it (r2 N3)
      auto at = m_routeDrawn.begin();
      while (at != m_routeDrawn.end() && at->serial <= comp.serial)
        ++at;
      m_routeDrawn.insert(at, std::move(comp));
      return false;
    }
    // the unit below re-draws the ahead entries; the frame on screen's entry is done
    screen = -1;
  }
  // (d): the entries C holds above the frame on screen, re-drawn with the new state under their
  // own serials and handed over together, or none of them: C never holds the newer state at a
  // lower serial than an entry of the older one
  if (!ahead.empty())
  {
    if (RouteFree() < static_cast<int>(ahead.size()))
    {
      noteWait("unit deferred");
      if (m_uDeferredU != m_uCur.u)
      {
        m_uDeferredU = m_uCur.u;
        GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::UNIT_DEFERRED);
        GRAPHICSTICK::GTLogF(LOGDEBUG, "GUNTIMED gUnitDeferred u={} entries={} free={}", m_uCur.u,
                  ahead.size(), RouteFree());
      }
      // ended by a buffer back (C advances: its flip or pick wakes G) or by the next frame's
      // entry, which carries the new state at a higher serial
      return true;
    }
    std::vector<CPresentationCoordinator::STimedBuffer> unit;
    bool drawn = true;
    for (size_t i : ahead)
    {
      SComposition& comp = m_routeDrawn[i];
      AssignPage(comp);
      if (!comp.glyphs.empty())
        RenderLibass(comp);
      unit.emplace_back();
      if (!DrawComposition(comp, unit.back()))
      {
        unit.pop_back();
        drawn = false;
        break;
      }
    }
    // review rA m2, rB m6: all or none; a part would leave C the new state below the old one
    if (!drawn)
    {
      for (CPresentationCoordinator::STimedBuffer& buffer : unit)
        ReleaseUnsubmitted(buffer);
      for (size_t i : ahead)
        m_routeDrawn[i].untimedStale = true;
      return true;
    }
    m_coordinator->SubmitTimed(unit);
    // the unit's hand-over replaced C's entries of the older state: their buffers are back at
    // once, for the frame on screen's entry below. Without this, under a menu animating every
    // frame (a state per vblank), every unit took the buffers and the frame on screen's entry
    // never got one: the change waited for an ahead entry's own frame, up to the queue's depth
    // later (RC4 am9pro B1_m3gan: 13 vblanks with no graphics flip, untimedReplaced 16,
    // gUnitDeferred 0)
    ReleaseReturned();
    if (m_uDeferredU == m_uCur.u)
      GRAPHICSTICK::GTLogF(LOGDEBUG, "GUNTIMED gUnitDeferred u={} ended (buffers)", m_uCur.u);
  }
  // then the frame on screen's composition with the new state, picked at the next slot (an
  // untimed change is not frame-paced, item 3); never while an older entry waits above it
  const bool staleAny = std::any_of(m_routeDrawn.begin(), m_routeDrawn.end(),
                                    [](const SComposition& c) { return c.untimedStale; });
  if (staleAny && screen < 0)
    noteWait("no frame on screen's entry");
  if (screen >= 0 && m_routeDrawn[screen].untimedStale)
  {
    SComposition& comp = m_routeDrawn[screen];
    if (!CanDraw(true))
    {
      noteWait("no buffer for the frame on screen's entry");
      return true;
    }
    AssignPage(comp);
    if (!comp.glyphs.empty())
      RenderLibass(comp);
    // a redraw: DrawTimed's CanDraw(comp.redraw) held a not-redraw entry to the draw-ahead
    // depth, which the ahead entries fill (RC19 B1_m3gan_5: "GUNTIMED owed ... free=2 waits=1
    // drawn: 656:u133s 668:u133 669:u133": the frame on screen's entry stayed stale with two
    // buffers free, because the two ahead entries were the depth)
    comp.redraw = true;
    if (!DrawTimed(comp))
      return true;
    m_uScreenOwed = false;
    if (comp.bridge)
      m_routeLast.untimedIds = comp.untimedIds;
  }
  return false;
}

void CGraphicsPlaneAML::ReleaseUnsubmitted(CPresentationCoordinator::STimedBuffer& buffer)
{
  if (buffer.req)
    drmModeAtomicFree(buffer.req);
  if (buffer.fence >= 0)
    close(buffer.fence);
  if (buffer.bo && !RingRelease(buffer.bo))
  {
    gbm_surface_release_buffer(m_gbmSurface, buffer.bo);
    m_locked--;
  }
  buffer = CPresentationCoordinator::STimedBuffer();
}

void CGraphicsPlaneAML::Process()
{
  OUTPUT::SetThreadRole("G");
  // a stopped G pins no display state (GetRT's slot), whichever way it returns
  struct SReleaseRT
  {
    ~SReleaseRT() { DISPLAYSTATE::ReleaseRT(DISPLAYSTATE::RT_G); }
  } releaseRT;
  if (!InitGL() || (!m_testPattern && !InitProgram()))
  {
    // never ready, so CanShow() keeps the HDR graphics on main's route B
    GRAPHICSTICK::GTLogF(LOGWARNING, "GPLANE off (gl init)");
    DeinitGL();
    return;
  }
  if (!m_testPattern)
    InitRaster();

  m_epoch = aml_presenter_epoch();
  // establishes the plane, enabled with a cleared buffer (R4: idle is never
  // a disable, nor alpha 0)
  SubmitFrame(m_testPattern, MonotonicNs());
  m_ready = true;
  // after the plane is up: the self-test holds G, not its start
  if (m_raster && m_selfTest)
  {
    m_raster->SelfTest();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, WIDTH, HEIGHT);
  }

  while (!m_bStop)
  {
    ReleaseReturned();
    // its submits are dropped from now on, so what it last showed would stay
    if (m_coordinator->GraphicsRefused())
    {
      if (m_routeOn)
      {
        GRAPHICSTICK::EndRoute("plane refused");
        RouteEnd();
        m_routeEndClearOwed = false;
      }
      if (!m_refusedOff)
      {
        SwitchOff("refused");
        m_refusedOff = true;
      }
      m_wake.Wait(100ms);
      continue;
    }
    // test hold (displaystate_ghold): the epoch change below re-submits the plane
    if (m_gholdMs > 0 && MonotonicNs() - m_gholdNs >= m_gholdMs * 1000000LL)
    {
      m_gholdNs = MonotonicNs();
      SwitchOff("ghold");
      // 5.B2-7: C's off takes no hold, so no epoch change brings the plane back
      if (OUTPUT::PlaneOffOnC())
        m_epoch = 0;
    }
    // a display transaction moved or rescaled the CRTC: the plane still has
    // the old rect until committed again (R6)
    // GT-4 on the route: a fresh timed buffer of what is shown
    const auto resubmitShown = [this]
    {
      if (!m_routeOn)
        return SubmitGeometry();
      for (SComposition& comp : m_routeDrawn)
        comp.redraw = true;
      if (m_routeDrawn.empty())
      {
        m_routeClearOwed = true;
        m_routeClearAtFloor = false;
      }
      return true;
    };
    const unsigned int epoch = aml_presenter_epoch();
    if (epoch != m_epoch && (m_testPattern || resubmitShown()))
      m_epoch = epoch;
    // 5.B3-10a: C retries a suspect graphics plane with a fresh frame (a static one sends none)
    const uint64_t resubmit = m_coordinator->GraphicsResubmit();
    if (resubmit != m_resubmit && (m_testPattern || resubmitShown()))
      m_resubmit = resubmit;
    if (m_testPattern)
    {
      if (!SubmitFrame(true, MonotonicNs()))
        m_wake.Wait(16ms);
    }
    else if (m_rasterReady && (m_routeOn || GRAPHICSTICK::RouteSession() != m_routeSession))
    {
      // a redraw's replacement hands C's old buffer back with no event to wake G (review r2 N5)
      bool waits = RouteStep();
      if (waits)
      {
        ReleaseReturned();
        if (RouteFree() > 0)
          waits = RouteStep();
      }
      if (!m_routeOn && m_routeEndClearOwed && SubmitClear(0))
        m_routeEndClearOwed = false;
      // GT-5 (item 12): G's only wait on C is for a free buffer, which the in-flight commit's
      // flip ends; bounded at 50 ms (as WaitTaken off the route), counted gWaitBuffer
      const bool bufferWait = waits && m_routeOn && RouteFree() <= 0;
      if (bufferWait && !m_waitBufferNs)
      {
        m_waitBufferNs = MonotonicNs();
        GRAPHICSTICK::Count(GRAPHICSTICK::UntimedCount::WAIT_BUFFER);
      }
      else if (!bufferWait && m_waitBufferNs)
      {
        GRAPHICSTICK::CountMax(GRAPHICSTICK::UntimedCount::WAIT_BUFFER_MAX_MS,
                               static_cast<int>((MonotonicNs() - m_waitBufferNs) / 1000000));
        m_waitBufferNs = 0;
      }
      // C wakes G for a buffer back or a pick, V for a picture; only the end clear polls
      m_wake.Wait(m_routeEndClearOwed ? 4ms : bufferWait ? 50ms : 100ms);
    }
    else
    {
      // GT-4: the route's end clear goes before route C's next frame
      if (m_routeEndClearOwed && SubmitClear(0))
        m_routeEndClearOwed = false;
      if (TakeShown())
      {
        m_shadowOwed = m_raster && m_selfTest;
        const int64_t start = m_untimed.empty() ? 0 : MonotonicNs();
        UpdateTextures();
        if (start)
        {
          const int64_t updUs = (MonotonicNs() - start) / 1000;
          for (SUntimedPost& post : m_untimed)
            if (post.updUs < 0)
              post.updUs = updUs;
        }
      }
      // GUNTIMED: C hands them to this frame's submit
      if (m_redraw && !m_untimed.empty())
        m_coordinator->NoteUntimed(CPresentationCoordinator::PLANE_GRAPHICS,
                                   std::exchange(m_untimed, {}));
      if (m_redraw && SubmitFrame(!m_shown.images.empty(), MonotonicNs(), m_shown.pts))
        m_redraw = false;
      // after route C's own submit, so the test never delays what is shown
      if (m_shadowOwed && !m_redraw)
      {
        m_shadowOwed = false;
        ShadowDraw();
      }
      m_wake.Wait(m_redraw ? 16ms : 100ms);
    }
  }

  m_ready = false;
  if (m_routeOn)
    GRAPHICSTICK::NoteGSide(m_routeSession, false);
  // main selects the untimed composition again
  if (m_untimedOnG)
    GRAPHICSTICK::SetUntimedHook(false);
  SwitchOff("stop");
  ReleaseReturned();
  DeinitGL();
  GRAPHICSTICK::GTLogF(LOGINFO, "CGraphicsPlaneAML - stopped");
}
