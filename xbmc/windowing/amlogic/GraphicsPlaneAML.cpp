/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsPlaneAML.h"

#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRendererUtil.h"
#include "utils/AMLUtils.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "utils/log.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - shader: {}", log);
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
    CLog::Log(LOGWARNING, "CGraphicsPlaneAML - no overlay plane on the GUI's CRTC");
    return false;
  }
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
    CLog::Log(LOGERROR, "CGraphicsPlaneAML - no program");
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
  if (m_context != EGL_NO_CONTEXT)
  {
    for (auto& [key, texture] : m_textures)
      glDeleteTextures(1, &texture.texture);
    if (m_program)
      glDeleteProgram(m_program);
  }
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
    return false;
  m_shown = std::move(next);
  m_redraw = true;
  CLog::Log(LOGDEBUG, "CGraphicsPlaneAML - showing {} images, video pts {:.3f}",
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
CRect CGraphicsPlaneAML::Place(const CDVDOverlayImage& o, const STexture& texture) const
{
  const CRect& rs = m_shown.source;
  const CRect& rd = m_shown.dest;
  const CRect& rv = m_shown.view;
  if (m_shown.width <= 0 || m_shown.height <= 0 || rs.IsEmpty())
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

  const float toPlaneX = WIDTH / m_shown.width;
  const float toPlaneY = HEIGHT / m_shown.height;
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
    const CRect r = Place(static_cast<const CDVDOverlayImage&>(*image), it->second);
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

void CGraphicsPlaneAML::Process()
{
  if (!InitGL() || (!m_testPattern && !InitProgram()))
  {
    DeinitGL();
    return;
  }

  m_epoch = aml_presenter_epoch();
  // establishes the plane, enabled and transparent (R4: idle is alpha 0,
  // never a disable)
  SubmitFrame(m_testPattern, MonotonicNs());
  m_ready = true;

  while (!m_bStop)
  {
    ReleaseReturned();
    // its submits are dropped from now on, so what it last showed would stay
    if (m_coordinator->GraphicsRefused())
    {
      if (!m_refusedOff)
      {
        SwitchOff();
        m_refusedOff = true;
      }
      m_wake.Wait(100ms);
      continue;
    }
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
    {
      if (TakeShown())
        UpdateTextures();
      if (m_redraw && SubmitFrame(!m_shown.images.empty(), MonotonicNs()))
        m_redraw = false;
      m_wake.Wait(m_redraw ? 16ms : 100ms);
    }
  }

  m_ready = false;
  SwitchOff();
  ReleaseReturned();
  DeinitGL();
  CLog::Log(LOGINFO, "CGraphicsPlaneAML - stopped");
}
