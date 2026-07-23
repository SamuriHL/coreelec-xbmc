/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "WinSystemAmlogicGLESContext.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

using namespace KODI;
using namespace KODI::WINDOWING::AML;

CWinSystemAmlogicGLESContext::CWinSystemAmlogicGLESContext()
: m_pGLContext(new CEGLContextUtils(EGL_PLATFORM_GBM_MESA, "EGL_EXT_platform_base"))
{
}

void CWinSystemAmlogicGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "aml");
}

std::unique_ptr<CWinSystemBase> CWinSystemAmlogicGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemAmlogicGLESContext>();
}

bool CWinSystemAmlogicGLESContext::InitWindowSystem()
{
  if (!CWinSystemAmlogic::InitWindowSystem())
  {
    return false;
  }

  if (!m_pGLContext->CreatePlatformDisplay(m_amlGBMUtils->GetDevice(), m_amlGBMUtils->GetDevice()))
  {
    m_pGLContext->Destroy();
    return false;
  }

  if (!m_pGLContext->InitializeDisplay(EGL_OPENGL_ES_API))
  {
    m_pGLContext->Destroy();
    return false;
  }

  EGLint renderableType{EGL_OPENGL_ES3_BIT};
  if (!m_pGLContext->ChooseConfig(renderableType))
  {
    renderableType = EGL_OPENGL_ES2_BIT;
    if (!m_pGLContext->ChooseConfig(renderableType))
    {
      m_pGLContext->Destroy();
      return false;
    }
  }

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, (renderableType == EGL_OPENGL_ES3_BIT) ? 3 : 2}});

  if (!m_pGLContext->CreateContext(contextAttribs))
  {
    m_pGLContext->Destroy();
    return false;
  }

  if (CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_KHR_fence_sync"))
  {
    m_eglFence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(GetEGLDisplay());
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  m_amlDisplay->aml_set_drmDevice_active(false);

  m_pGLContext->DestroyContext();
  m_pGLContext->Destroy();
  return CWinSystemAmlogic::DestroyWindowSystem();
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  const RenderStereoMode stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = m_amlDisplay->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR);

  bool nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  StreamHdrType hdrType = CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType();
  bool force_mode_switch_by_dv = false;
  bool force_mode_switch_by_hotplug = m_amlDisplay->GetHotPlug();
  if (aml_dolby_vision_enabled() &&
     ((m_hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION && hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION) ||
      (m_hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION && hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)))
      force_mode_switch_by_dv = true;

  // get current used resolution
  if (!m_amlDisplay->aml_get_native_resolution(&current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    return false;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(hdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(m_hdrType);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "m_bWindowCreated: {}, "
    "frac rate {:d}({:d}), "
    "hdrType: {}({}), force mode switch: {}",
    __FUNCTION__,
    m_bWindowCreated,
    fractional_rate, cur_fractional_rate,
    new_hdrStr.empty() ? "none" : new_hdrStr, old_hdrStr.empty() ? "none" : old_hdrStr,
    force_mode_switch_by_dv ? "by DV" : force_mode_switch_by_hotplug ? "by HotPlug" : "no");
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags, m_nativeGUI);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags, nativeGUI);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_stereo_mode == stereo_mode && m_bWindowCreated &&
      !force_mode_switch_by_dv && !force_mode_switch_by_hotplug &&
      (fractional_rate == cur_fractional_rate) &&
      nativeGUI == m_nativeGUI)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();

  // check if a forced mode switch is required
  if (force_mode_switch_by_hotplug)
  {
    m_force_mode_switch = true;
  }
  else
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      MathUtils::FloatEquals(current_resolution.fRefreshRate, res.fRefreshRate, 0.06f))
  {
    // same resolution, check frac rate and other parameter
    if ((cur_fractional_rate != fractional_rate) || force_mode_switch_by_dv || (m_stereo_mode != stereo_mode))
      m_force_mode_switch = true;
  }

  if (m_force_mode_switch)
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);

  // refresh backup data
  m_hdrType = hdrType;
  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;
  m_nativeGUI = nativeGUI;

  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  uint32_t format = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  if (!m_amlGBMUtils->CreateSurface(res.iWidth, res.iHeight, format))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create GBM surface", __FUNCTION__);
    return false;
  }

  if (!m_pGLContext->CreatePlatformSurface(
          m_amlGBMUtils->GetSurface(),
          reinterpret_cast<EGLNativeWindowType>(m_amlGBMUtils->GetSurface())))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create CreatePlatformSurface", __FUNCTION__);
    return false;
  }

  if (!m_pGLContext->BindContext())
  {
    return false;
  }

  if (!m_delayDispReset)
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  m_pGLContext->DestroySurface();
  return CWinSystemAmlogic::DestroyWindow();
}

bool CWinSystemAmlogicGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAmlogicGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  CreateNewWindow("", fullScreen, res);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAmlogicGLESContext::SetVSyncImpl(bool enable)
{
  if (!m_pGLContext->SetVSync(enable))
  {
    CLog::Log(LOGERROR, "{},Could not set egl vsync", __FUNCTION__);
  }
}

void CWinSystemAmlogicGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  SetVSync(true);
  if (rendered)
  {
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_amlDisplay->TakeOutFenceFd();
      if (fd != -1)
      {
        m_eglFence->CreateKMSFence(fd);
        m_eglFence->WaitSyncGPU();
      }

      m_eglFence->CreateGPUFence();
    }
#endif

    // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
    // there is probably nothing we can do about it
    m_pGLContext->TrySwapBuffers();

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_eglFence->FlushFence();
      m_amlDisplay->SetInFenceFd(fd);

      m_eglFence->WaitSyncCPU();
    }
#endif

    if (m_amlGBMUtils && m_amlGBMUtils->LockFrontBuffer(m_amlDisplay->aml_get_Device_handle()))
      m_amlDisplay->FlipPage(m_amlGBMUtils->GetFBId());
  }
  else if (!videoLayer)
  {
    m_amlDisplay->aml_drmDevice_vsync();
  }

  if (m_delayDispReset && m_dispResetTimer.IsTimePast())
  {
    m_delayDispReset = false;
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }
}

// GUI HDR/DV compositing. The GUI/OSD is rendered into an sRGB FBO, then a
// single full-screen pass transforms it sRGB -> BT.709->BT.2020 -> ST2084 PQ
// (CGuiCompositeShaderGLES) into the OSD plane back buffer. This does the
// transfer/gamut/PQ conversion ONCE, post-blend, so anti-aliased and
// translucent GUI edges blend correctly in sRGB rather than in PQ space (the
// per-primitive shader encode did the latter -> aliased menu text). Ported
// from CWinSystemGbmGLESContext; Amlogic has no Kodi-managed DRM planes but is
// structurally dual-plane - the OSD/GBM plane is alpha-composited over the
// separate amvideo hardware plane by the VPP - so the GBM D2P branches are
// taken unconditionally here.
bool CWinSystemAmlogicGLESContext::SetGuiCompositing(int colorTransfer)
{
  m_guiCompositing = (colorTransfer != 0);

  if (m_guiCompositing)
  {
    if (!m_compositeShader)
    {
      std::string defines;
      if (UseLimitedColor())
        defines += "#define KODI_LIMITED_RANGE 1\n";
      m_compositeShader = std::make_unique<CGuiCompositeShaderGLES>(defines);
      if (!m_compositeShader->CompileAndLink())
      {
        CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to compile GUI composite shader");
        m_compositeShader.reset();
        m_guiCompositing = false;
        return false;
      }
    }

    if (!m_compositeShader->CreateLUTs(colorTransfer))
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to create LUTs");
      m_compositeShader.reset();
      m_guiCompositing = false;
      return false;
    }
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemAmlogicGLESContext::BeginGuiComposite(bool guiWillRender)
{
  if (!m_guiCompositing)
    return false;

  m_guiWillRender = guiWillRender;

  int width = m_nWidth;
  int height = m_nHeight;

  // create or recreate FBO if size changed
  if (!m_guiFbo.IsValid() || m_guiFboWidth != width || m_guiFboHeight != height)
  {
    m_guiFbo.Cleanup();

    if (!m_guiFbo.Initialize())
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to initialize GUI FBO");
      return false;
    }

    if (!m_guiFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to create GUI FBO texture {}x{}",
                width, height);
      m_guiFbo.Cleanup();
      return false;
    }

    if (GetEnabledFrontToBackRendering() && !m_guiFbo.AttachDepthBuffer(width, height))
    {
      CLog::Log(LOGERROR,
                "CWinSystemAmlogicGLESContext: failed to attach depth buffer to GUI FBO {}x{}",
                width, height);
      m_guiFbo.Cleanup();
      return false;
    }

    m_guiFboWidth = width;
    m_guiFboHeight = height;
    m_guiFboClean = false; // fresh FBO is undefined, force a clear
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext: created GUI FBO {}x{}", width, height);
  }

  // GUI render skipped this frame: leave the FBO bind/clear out. The prior
  // sRGB GUI content is implicitly preserved and the cached composited OSD
  // front buffer keeps being scanned out (PresentRender skips the swap).
  if (!guiWillRender)
    return true;

  if (!m_guiFbo.BeginRender())
    return false;

  // Clear only when the FBO holds stale content; idle frames are already clean.
  if (!m_guiFboClean)
  {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    m_guiFboClean = true;
  }

  return true;
}

void CWinSystemAmlogicGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();

  // When the GUI didn't render this frame the cached OSD front buffer is
  // reused and PresentRender skips the swap, so clearing the back buffer is
  // wasted (dual-plane: nothing else draws into it).
  if (!m_guiWillRender)
    return;

  // Clear the back buffer before video renders. In the FBO compositing path,
  // video renders with clear=false, so DrawBlackBars is never called; without
  // this, letterbox areas retain stale swap-chain content.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
}

// CompositeGui is the last GL operation in the frame (called just before the
// swap). GL state is not restored afterward; the next frame sets its own.
void CWinSystemAmlogicGLESContext::CompositeGui()
{
  if (!m_guiFbo.IsValid() || !m_guiFbo.IsBound() || !m_compositeShader)
    return;

  // m_guiFboClean tracks whether the FBO is empty AND the cached OSD front
  // buffer is valid; only update it when the GUI actually rendered this frame.
  if (m_guiWillRender)
  {
    const bool guiEmpty = (GetGUIElementCount() == 0);
    m_guiFboClean = guiEmpty;
    if (guiEmpty)
      return;
  }
  else if (m_guiFboClean)
  {
    return;
  }

  // GUI didn't re-render: the cached composited PQ frame is still in the OSD
  // plane front buffer. Skip the shader pass entirely; PresentRender skips the
  // swap (hasRendered==false) and the display HW keeps scanning it out while
  // the amvideo plane updates independently.
  if (!m_guiWillRender)
    return;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_guiFbo.Texture());

  glEnable(GL_BLEND);

  // The OSD plane is alpha-composited over the amvideo plane by the VPP. The
  // default blend also multiplies the stored alpha (leaving src.a^2); the
  // hardware composite then reads that squared alpha and translucent GUI
  // pixels render at the wrong opacity. Replace the stored alpha with src.a.
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

  // orthographic projection (screen coords, Y-down)
  float w = static_cast<float>(m_guiFboWidth);
  float h = static_cast<float>(m_guiFboHeight);

  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};

  m_compositeShader->SetProjection(proj);
  m_compositeShader->Enable();

  GLint posLoc = m_compositeShader->GetPosLoc();
  GLint texLoc = m_compositeShader->GetTexLoc();

  GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  GLubyte idx[4] = {0, 1, 3, 2};

  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);

  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);

  m_compositeShader->Disable();
}

EGLDisplay CWinSystemAmlogicGLESContext::GetEGLDisplay() const
{
  return m_pGLContext->GetEGLDisplay();
}

EGLSurface CWinSystemAmlogicGLESContext::GetEGLSurface() const
{
  return m_pGLContext->GetEGLSurface();
}

EGLContext CWinSystemAmlogicGLESContext::GetEGLContext() const
{
  return m_pGLContext->GetEGLContext();
}

EGLConfig  CWinSystemAmlogicGLESContext::GetEGLConfig() const
{
  return m_pGLContext->GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAmlogicGLESContext::GetVideoSync(CVideoReferenceClock *clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAML(clock));
  return pVSync;
}

bool CWinSystemAmlogicGLESContext::SupportsStereo(const RenderStereoMode mode) const
{
  if (aml_display_support_3d() &&
      mode == RenderStereoMode::HARDWAREBASED) {
    // yes, we support hardware based MVC decoding
    return true;
  }

  return CRenderSystemGLES::SupportsStereo(mode);
}
