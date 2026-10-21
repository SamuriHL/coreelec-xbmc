/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "VideoSyncTimeline.h"
#include "WinSystemAmlogicGLESContext.h"
#include "GraphicsPlaneAML.h"
#include "Timekeeper.h"
#include "utils/PresentationTimeline.h"
#include "filesystem/File.h"
#include "cores/VideoPlayer/VideoRenderers/HdrGraphics.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
#include "settings/AdvancedSettings.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/ThreadRole.h"
#include "threads/SingleLock.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/WireClassProbe.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>

using namespace KODI;
using namespace KODI::WINDOWING::AML;
using namespace std::chrono_literals;

namespace KODI::WINDOWING::AML
{
// 5.B2-5a: main's mode set request, read on main and taken by value, with main's decision and
// W's shadow of it
struct SDriveShadow
{
  struct Decision
  {
    bool valid{true}; //!< the current mode was read
    bool noop{false};
    bool hdr{false};
    bool frac{false};
    bool stereo{false};
    bool hotplug{false};
    bool wire{false};
    bool force{false};
    bool fracReclock{false};
    bool notCommitted{false};
    const char* reply{"failed"};
  };
  uint64_t id{0};
  RESOLUTION_INFO res;
  bool fullScreen{false};
  RenderStereoMode stereo{RenderStereoMode::OFF};
  StreamHdrType hdrType{StreamHdrType::HDR_TYPE_NONE};
  bool nativeGUI{false};
  bool hotplug{false};
  bool fracRequest{false};
  uint64_t jobsStarted{0};
  uint64_t backupsVersion{0};
  bool posted{false};
  bool raced{false};
  bool wasNativeGUI{false};
  Decision main;
  Decision w;
};

// 5.B2-5b: one mode set of main's. The request is read on main and taken by value (W reads no
// CSettings and no plain member of main's); the reply is W's, read by main after the job
struct SDriveJob
{
  struct Backups
  {
    StreamHdrType hdrType{StreamHdrType::HDR_TYPE_NONE};
    RenderStereoMode stereo{RenderStereoMode::OFF};
    bool fullScreen{false};
    bool nativeGUI{false};
    bool windowCreated{false};
  };

  // the request (design §4.5)
  uint64_t id{0};
  RESOLUTION_INFO res;
  int resIndex{-1}; //!< GC's RESOLUTION, for a late reply's GC update
  std::string fbName;
  bool fullScreen{false};
  RenderStereoMode stereo{RenderStereoMode::OFF};
  StreamHdrType hdrType{StreamHdrType::HDR_TYPE_NONE};
  bool nativeGUI{false}; //!< DISABLEGUISCALING, read on main
  bool hotplug{false}; //!< GetHotPlug, read (and cleared) on main
  bool fracRequest{false}; //!< RM's re-clock request (its scope ends with RM's set call)
  bool originRm{false};
  int delay{0}; //!< videoscreen.delayrefreshchange, 0.1 s units
  uint32_t uiFormat{0};
  OUTPUT::Lane lane{OUTPUT::Lane::SETTINGS};
  Backups backups; //!< main's (m_hdrType, m_stereo_mode, m_bFullScreen, m_nativeGUI, created)
  CPresentationCoordinator* coordinator{nullptr};
  CGraphicsPlaneAML* graphicsPlane{nullptr};

  // the reply
  OUTPUT::DriveReply kind{OUTPUT::DriveReply::FAILED};
  bool armDelay{false}; //!< today's set reaches WSA's delay arming (r2 N4)
  bool tornDown{false}; //!< planes off and the reclaim ran (or, unparked, is main's to run)
  bool parkRetried{false}; //!< the park was not acked in 2 s; it was waited for 500 ms more
  bool unparked{false}; //!< nor then: the set ran unparked (S1' with the plane off, no cbuf)
  bool lostWalked{false}; //!< the lost walk ran: its LEDGER set is ledgerSet
  uint64_t ledgerSet{0};
  int guiLocked{0}; //!< main's m_guiLocked when it posted the request
  bool offRefused{false}; //!< S1''s GUI plane off was refused (the SB3 state)
  uint64_t uiEpoch{0}; //!< the UI surface epoch the job started
  Backups result;
  std::chrono::steady_clock::time_point modeSetAt{};
  int64_t parkMs{-1}, offMs{-1}, lostMs{-1}, commitMs{-1}, engageMs{-1};

  // the hand-over: main waits, bounded; a reply it gave up on goes to the late list
  std::mutex mutex;
  std::condition_variable cond;
  bool done{false};
  bool abandoned{false};
};
} // namespace KODI::WINDOWING::AML

namespace
{
// W's copy of the set backups (m_hdrType, m_stereo_mode, m_bFullScreen, m_nativeGUI,
// m_bWindowCreated), written from main's after each set that ran the job
struct ShadowBackups
{
  StreamHdrType hdrType{StreamHdrType::HDR_TYPE_NONE};
  RenderStereoMode stereo{RenderStereoMode::OFF};
  bool fullScreen{false};
  bool nativeGUI{false};
  bool windowCreated{false};
};

std::mutex s_shadowMutex;
ShadowBackups s_shadowBackups;
uint64_t s_shadowBackupsVersion{0}; // 0: not seeded
uint64_t s_shadowSets{0};
uint64_t s_shadowMatch{0};
uint64_t s_shadowDiff{0};
uint64_t s_shadowRaced{0};
uint64_t s_shadowRacedDiff{0};

bool DriveShadowOn()
{
  static const bool on = []
  {
    const bool result = OUTPUT::OutputDriveOn() && !OUTPUT::OutputDriveSkip("B2-5a");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-5a: W shadows the mode set decision");
    return result;
  }();
  return on;
}

// on W, with no write: CreateNewWindow's no-op test, force reasons and re-clock request, from the
// request, W's backups and W's own reads (never GetHotPlug, which clears)
void ShadowDecide(CAMLDisplay& display, SDriveShadow& s)
{
  ShadowBackups b;
  {
    std::unique_lock lock(s_shadowMutex);
    b = s_shadowBackups;
    s.raced = s_shadowBackupsVersion != s.backupsVersion;
  }
  // the job this runs in is the one started since main's reads
  s.raced = s.raced || OUTPUT::COutputWorker::Get().JobsStarted() - 1 != s.jobsStarted;
  s.wasNativeGUI = b.nativeGUI;

  SDriveShadow::Decision& d = s.w;
  const RESOLUTION_INFO& res = s.res;
  const int fractional = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  d.frac = display.aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR) != fractional;
  d.hdr = b.hdrType != s.hdrType;
  d.stereo = b.stereo != s.stereo;
  d.hotplug = s.hotplug;
  d.wire = display.aml_output_wire_stale();

  RESOLUTION_INFO cur;
  cur.iWidth = cur.iHeight = 0;
  if (!display.aml_get_native_resolution(&cur, s.nativeGUI))
  {
    d.valid = false;
    return;
  }

  const bool sameSize = cur.iWidth == res.iWidth && cur.iHeight == res.iHeight &&
                        cur.iScreenWidth == res.iScreenWidth &&
                        cur.iScreenHeight == res.iScreenHeight;
  d.noop = sameSize && b.fullScreen == s.fullScreen && cur.fRefreshRate == res.fRefreshRate &&
           (cur.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
           b.windowCreated && s.nativeGUI == b.nativeGUI && !d.hdr && !d.hotplug && !d.frac &&
           !d.stereo && !d.wire;
  if (d.noop)
  {
    d.reply = "done(noChange)";
    return;
  }

  if (d.hotplug || d.stereo)
    d.force = true;
  else if (sameSize && MathUtils::FloatEquals(cur.fRefreshRate, res.fRefreshRate, 0.06f))
    d.force = d.frac || d.hdr || d.wire;
  const float familyRate = std::round(res.fRefreshRate);
  d.fracReclock = d.frac && s.fracRequest &&
                  (familyRate == 24.0f || familyRate == 30.0f || familyRate == 60.0f);
  // posted, it runs beside main, whose connector cache it must not read (r1 M5)
  if (s.posted)
    d.reply = "commit";
  else
    d.notCommitted = !display.aml_get_display_connected() &&
                     !(d.hotplug && display.aml_hotplug_set_commits());
}

// on W, after main's commit in the same job
void ShadowCommitted(SDriveShadow& s, bool committed, bool notConnected)
{
  s.main.notCommitted = committed && notConnected;
  s.main.reply = !committed ? "failed" : s.main.notCommitted ? "not-committed" : "done";
  if (s.w.valid && !s.w.noop)
    s.w.reply = s.w.notCommitted ? "not-committed" : committed ? "done" : "failed";
}

void ShadowLog(const SDriveShadow& s, uint64_t ledgerSet, const char* handler)
{
  const SDriveShadow::Decision& m = s.main;
  const SDriveShadow::Decision& w = s.w;
  std::string diff;
  const auto field = [&diff](const char* name, bool differs)
  {
    if (differs)
      diff += (diff.empty() ? "" : ",") + std::string(name);
  };
  field("valid", m.valid != w.valid);
  field("noop", m.noop != w.noop);
  field("hdr", m.hdr != w.hdr);
  field("frac", m.frac != w.frac);
  field("stereo", m.stereo != w.stereo);
  field("hotplug", m.hotplug != w.hotplug);
  field("wire", m.wire != w.wire);
  field("force", m.force != w.force);
  field("reclock", m.fracReclock != w.fracReclock);
  field("reply", std::string(m.reply) != w.reply);

  const auto describe = [](const SDriveShadow::Decision& d)
  {
    std::string reasons;
    for (const auto& [name, on] : {std::pair{"hdr", d.hdr}, std::pair{"frac", d.frac},
                                   std::pair{"stereo", d.stereo}, std::pair{"hotplug", d.hotplug},
                                   std::pair{"wire", d.wire}})
      if (on)
        reasons += (reasons.empty() ? "" : ",") + std::string(name);
    return fmt::format("{} force({})={} reclock={} reply={}",
                       !d.valid ? "failed" : d.noop ? "noop" : "commit",
                       reasons.empty() ? "-" : reasons, d.force ? 1 : 0, d.fracReclock ? 1 : 0,
                       d.reply);
  };

  std::unique_lock lock(s_shadowMutex);
  ++s_shadowSets;
  if (s.raced)
  {
    ++s_shadowRaced;
    if (!diff.empty())
      ++s_shadowRacedDiff;
  }
  else if (diff.empty())
    ++s_shadowMatch;
  else
    ++s_shadowDiff;
  CLog::Log(LOGINFO,
            "OUTPUTDRIVE shadow set={} {}{} {} ledger={} w: {} main: {} nativeGUI={} was={} "
            "handler={} diff={}",
            s.id, diff.empty() ? "MATCH" : "DIFF", s.raced ? " raced" : "",
            s.posted ? "posted" : "job", ledgerSet, describe(w), describe(m), s.nativeGUI ? 1 : 0,
            s.wasNativeGUI ? 1 : 0, handler, diff.empty() ? "-" : diff);
  CLog::Log(LOGINFO,
            "OUTPUTDRIVE shadow count sets={} match={} diff={} raced={} raced_diff={}",
            s_shadowSets, s_shadowMatch, s_shadowDiff, s_shadowRaced, s_shadowRacedDiff);
}

// 5.B2-5b: replies main gave up on (after 15 s), for MessagePump
std::mutex s_lateMutex;
// sets run today's way after two unacked parks (W only; gate target 0)
int s_unparkedSets = 0;
std::vector<std::shared_ptr<SDriveJob>> s_late;

// a set with no mode set job: W shadows it after main's decision, on the lane its job would use
void PostDriveShadow(const std::shared_ptr<SDriveShadow>& shadow, CAMLDisplay* display)
{
  shadow->posted = true;
  OUTPUT::COutputWorker::Get().Post(
      shadow->hotplug ? OUTPUT::Lane::RESTORE : OUTPUT::Lane::SETTINGS, "drive shadow",
      [shadow, display]
      {
        ShadowDecide(*display, *shadow);
        ShadowLog(*shadow, 0, "-");
      });
}
} // namespace

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
  OUTPUT::StartAccounting();
  OUTPUT::CSettingsMirror::Get().Start();
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

  const bool coordinator =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoPresentationCoordinator;
  // On validated hardware the timekeeper owns display time and drives the
  // coordinator's video ticks. The timeline clock needs it; the shadow
  // comparison runs with it.
  m_tickOwner = coordinator && aml_presentation_validated() &&
                !XFILE::CFile::Exists("special://profile/tickowner_off");
  m_timelineClock = (aml_presentation_validated() &&
                     !XFILE::CFile::Exists("special://profile/timelineclock_off")) ||
                    XFILE::CFile::Exists("special://profile/timeline_clock");
  m_timekeeperShadow =
      m_timelineClock || XFILE::CFile::Exists("special://profile/timekeeper_shadow");
  StartTimekeeper();

  if (coordinator)
  {
    const bool ticks = m_tickOwner && m_timekeeper;
    m_coordinator = std::make_unique<CPresentationCoordinator>(
        m_amlDisplay->aml_get_Device_handle(), ticks ? m_timekeeper->TickFd() : -1,
        ticks ? m_timekeeper->CrtcId() : 0);
    if (!m_coordinator->Start())
      m_coordinator.reset();
  }
  OUTPUT::NoteCoordinatorStarted(m_coordinator != nullptr);

  const int graphicsPlane =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoGraphicsPlane;
  if (m_coordinator && graphicsPlane > 0)
  {
    m_graphicsPlane = std::make_unique<CGraphicsPlaneAML>(
        GetEGLDisplay(), m_amlGBMUtils->GetDevice(), m_amlDisplay->aml_get_Device_handle(),
        m_amlDisplay.get(), m_coordinator.get(), graphicsPlane == 2);
    if (!m_graphicsPlane->Start())
      m_graphicsPlane.reset();
  }

  WIRECLASS::CWireClassProbe::Get().Start();
  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  WIRECLASS::CWireClassProbe::Get().Stop();
  // the shutdown writes below run inline on main
  OUTPUT::COutputWorker::Get().Stop();

  // off while the coordinator runs and the GUI plane is still up to carry it
  if (m_graphicsPlane)
  {
    m_graphicsPlane->Stop();
    m_graphicsPlane.reset();
  }

  if (m_coordinator)
  {
    DetachGuiSurface();
    m_coordinator->Stop();
    m_coordinator.reset();
  }

  if (IsPresentationReady())
  {
    SetPresentationReady(false);
    m_amlDisplay->aml_set_drmDevice_active(false);
  }

  // normally gone at DestroyRenderSystem already, with the context current
  m_compositeShader.reset();

  m_pGLContext->DestroyContext();
  m_pGLContext->Destroy();
  const bool destroyed = CWinSystemAmlogic::DestroyWindowSystem();
  OUTPUT::CSettingsMirror::Get().Stop();
  OUTPUT::StopAccounting();
  return destroyed;
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  MAINCOUPLING("GC GetStereoMode");
  const RenderStereoMode stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = m_amlDisplay->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR);

  bool nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  MAINCOUPLING("GC GetHDRType");
  StreamHdrType hdrType = CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType();
  bool force_mode_switch_by_hdr = (m_hdrType != hdrType);
  bool force_mode_switch_by_stereo_mode = (m_stereo_mode != stereo_mode);
  bool force_mode_switch_by_fractional_rate = (cur_fractional_rate != fractional_rate);
  // 5.B2-5a: before GetHotPlug clears the flag
  const std::shared_ptr<SDriveShadow> shadow =
      m_outputWorkerStarted && DriveShadowOn()
          ? BeginDriveShadow(res, fullScreen, stereo_mode, hdrType, nativeGUI)
          : nullptr;
  bool force_mode_switch_by_hotplug = m_amlDisplay->GetHotPlug();
  bool force_mode_switch_by_wire = m_amlDisplay->aml_output_wire_stale();
  if (shadow)
  {
    SDriveShadow::Decision& d = shadow->main;
    d.hdr = force_mode_switch_by_hdr;
    d.frac = force_mode_switch_by_fractional_rate;
    d.stereo = force_mode_switch_by_stereo_mode;
    d.hotplug = force_mode_switch_by_hotplug;
    d.wire = force_mode_switch_by_wire;
  }

  // get current used resolution
  if (!m_amlDisplay->aml_get_native_resolution(&current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    if (shadow)
    {
      shadow->main.valid = false;
      PostDriveShadow(shadow, m_amlDisplay.get());
    }
    SetPresentationReady(false);
    return false;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(hdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(m_hdrType);
  CLog::Log(LOGDEBUG,
            "CWinSystemAmlogicGLESContext::{}: "
            "m_bWindowCreated: {}, "
            "hdrType: {}({}), "
            "force mode switch by - hdr: {}, frac rate: {}, stereo mode: {}, hotplug: {}, wire: {}",
            __FUNCTION__, m_bWindowCreated, new_hdrStr.empty() ? "none" : new_hdrStr,
            old_hdrStr.empty() ? "none" : old_hdrStr, force_mode_switch_by_hdr,
            force_mode_switch_by_fractional_rate, force_mode_switch_by_stereo_mode,
            force_mode_switch_by_hotplug, force_mode_switch_by_wire);
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
      current_resolution.iScreenWidth == res.iScreenWidth &&
      current_resolution.iScreenHeight == res.iScreenHeight && m_bFullScreen == fullScreen &&
      current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) ==
          (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_bWindowCreated && nativeGUI == m_nativeGUI && !force_mode_switch_by_hdr &&
      !force_mode_switch_by_hotplug && !force_mode_switch_by_fractional_rate &&
      !force_mode_switch_by_stereo_mode && !force_mode_switch_by_wire)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    if (shadow)
    {
      shadow->main.noop = true;
      shadow->main.reply = "done(noChange)";
      PostDriveShadow(shadow, m_amlDisplay.get());
    }
    aml_dv_engage_pending_disc_session(hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION);
    WIRECLASS::CWireClassProbe::Get().Notify("no mode set", WIRECLASS::FromHdrType(hdrType));
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();
  OUTPUT::TestStopAt("3");

  // check if a forced mode switch is required
  if (force_mode_switch_by_hotplug)
  {
    m_force_mode_switch = true;
    m_hotplug_mode_switch = true;
  }
  else if (force_mode_switch_by_stereo_mode)
  {
    m_force_mode_switch = true;
  }
  else
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      MathUtils::FloatEquals(current_resolution.fRefreshRate, res.fRefreshRate, 0.06f))
  {
    // same resolution, check frac rate and other parameter
    if (force_mode_switch_by_fractional_rate || force_mode_switch_by_hdr ||
        force_mode_switch_by_wire)
      m_force_mode_switch = true;
  }

  if (m_force_mode_switch)
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);

  // a content start takes the content's 1000/1001 rate (24/30/60 families only)
  const float familyRate = std::round(res.fRefreshRate);
  m_frac_reclock = force_mode_switch_by_fractional_rate && aml_frac_reclock_request() &&
                   (familyRate == 24.0f || familyRate == 30.0f || familyRate == 60.0f);
  if (shadow)
  {
    shadow->main.force = m_force_mode_switch;
    shadow->main.fracReclock = m_frac_reclock;
    CAMLDisplay* const display = m_amlDisplay.get();
    m_shadowBefore = [shadow, display] { ShadowDecide(*display, *shadow); };
    m_shadowAfter = [shadow](bool committed, bool notConnected)
    { ShadowCommitted(*shadow, committed, notConnected); };
    m_driveShadow = shadow;
  }

  // refresh backup data
  m_hdrType = hdrType;
  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;
  m_nativeGUI = nativeGUI;

  const bool afterBootSet = m_outputWorkerStarted;
  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  // the disc-session DV engage waits for this mode set, so the sink locks once
  aml_dv_engage_pending_disc_session(hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION);
  WIRECLASS::CWireClassProbe::Get().Notify("mode set", WIRECLASS::FromHdrType(hdrType));
  OUTPUT::TestStopAt("5");

  uint32_t format = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  // 5.B2-4: the new surface takes the epoch its detach started (never main's own)
  const bool surfaceEpoch = afterBootSet && SurfaceEpochOn() && m_coordinator;
  if (surfaceEpoch)
    m_surfaceEpoch = m_coordinator->UiEpoch();
  if (!m_amlGBMUtils->CreateSurface(
          res.iWidth, res.iHeight, format, afterBootSet,
          surfaceEpoch ? std::optional<uint64_t>(m_surfaceEpoch) : std::nullopt))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create GBM surface", __FUNCTION__);
    DestroyWindow();
    return false;
  }

  if (!m_pGLContext->CreatePlatformSurface(
          m_amlGBMUtils->GetSurface(),
          reinterpret_cast<EGLNativeWindowType>(m_amlGBMUtils->GetSurface())))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create CreatePlatformSurface", __FUNCTION__);
    DestroyWindow();
    return false;
  }

  if (!m_pGLContext->BindContext())
  {
    DestroyWindow();
    return false;
  }

  // The new surface has no cached OSD frame for the dirty-driven GUI skip to
  // keep on screen.
  m_guiRepaintFrames = 3;

  if (!m_delayDispReset && DisplayResetAllowed())
    ResetDisplayNow();

  if (m_amlDisplay->aml_get_display_connected())
    SetPresentationReady(true);

  // booted without a display: the timekeeper starts with the first CRTC
  if (!m_timekeeper && StartTimekeeper() && m_coordinator && m_tickOwner)
    m_coordinator->SetTickSource(m_timekeeper->TickFd(), m_timekeeper->CrtcId());

  return true;
}

void CWinSystemAmlogicGLESContext::ResetDisplayNow()
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  // tell any shared resources
  for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
  {
    (*i)->OnResetDisplay();
    LedgerReset(*i, 0ms);
  }
  LedgerResetDone();
  CloseLostSpan();
  lock.unlock();
  OUTPUT::TestStopAt("8");
}

bool CWinSystemAmlogicGLESContext::StartTimekeeper()
{
  const uint32_t crtc = m_amlDisplay->aml_get_Device_crtc_id();
  if (!(m_tickOwner || m_timekeeperShadow) || !crtc)
    return false;
  m_timekeeper =
      std::make_unique<CTimekeeper>(m_amlDisplay->aml_get_Device_handle(), crtc);
  if (!m_timekeeper->Start(m_timekeeperShadow))
  {
    m_timekeeper.reset();
    return false;
  }
  if (m_timelineClock)
    PRESENTATION::TimelineClockActive() = true;
  return true;
}

bool CWinSystemAmlogicGLESContext::InitRenderSystem()
{
  if (!CRenderSystemGLES::InitRenderSystem())
    return false;

  // Build the GUI composite program now, with the context current and no
  // render-manager lock held: built at the first composite Configure instead,
  // its 19-41 ms compile would stall the presentation coordinator's tick
  // (design §16.14). Kept for the session on validated hardware.
  if (aml_presentation_validated())
  {
    const auto start = std::chrono::steady_clock::now();
    const bool built = BuildCompositeShader();
    CLog::Log(LOGINFO, "CWinSystemAmlogicGLESContext: GUI composite program {} in {} ms",
              built ? "built" : "FAILED",
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
  }
  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyRenderSystem()
{
  // the last point with the context and surface current (DestroyWindow unbinds it)
  m_compositeShader.reset();
  return CRenderSystemGLES::DestroyRenderSystem();
}

bool CWinSystemAmlogicGLESContext::BuildCompositeShader()
{
  // the range is compiled in: a kept program built for the other range is rebuilt
  const bool limited = UseLimitedColor();
  if (m_compositeShader && m_compositeShaderLimited == limited)
    return true;

  std::string defines;
  if (limited)
    defines += "#define KODI_LIMITED_RANGE 1\n";
  m_compositeShaderLimited = limited;
  m_compositeShader = std::make_unique<CGuiCompositeShaderGLES>(defines);
  // a new program has no LUTs
  m_guiCompositeTransfer = 0;
  if (!m_compositeShader->CompileAndLink())
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to compile GUI composite shader");
    m_compositeShader.reset();
    return false;
  }
  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  // the GUI plane still shows its last buffer, which carries the off commit;
  // at a mode switch the plane comes back with the next graphics frame
  if (m_graphicsPlane)
    m_graphicsPlane->SwitchOff();
  DetachGuiSurface();
  if (!SB3PROBE::On())
  {
    m_pGLContext->DestroySurface();
    return CWinSystemAmlogic::DestroyWindow();
  }
  // 5.B2 M5 (SB3 probe, log only): whether this destroy's RmFB took the UI plane off
  const auto steadyNs = []
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  SB3PROBE::ArmRmFb();
  const int64_t destroyStart = steadyNs();
  m_pGLContext->DestroySurface();
  const int64_t destroyEnd = steadyNs();
  const SB3PROBE::RmFbResult rmFb = SB3PROBE::TakeRmFb();
  CLog::Log(LOGINFO,
            "SB3 egl destroyed mono={} took_us={} rmfb={} first_fb={} first_at_us={} first_us={}",
            destroyEnd, (destroyEnd - destroyStart) / 1000, rmFb.count, rmFb.firstFb,
            rmFb.count ? (rmFb.firstNs - destroyStart) / 1000 : -1, rmFb.firstUs);
  return CWinSystemAmlogic::DestroyWindow();
}

bool CWinSystemAmlogicGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAmlogicGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  // 5.B2-5b: after the boot set, which stays inline (ruling 4, MC-40)
  if (OUTPUT::ModeSetsOnW())
    return SetFullScreenOnW(fullScreen, res);

  const uint64_t ledgerSet = m_ledgerSet;
  const int guiLocked = m_guiLocked;
  m_guiDiscarded = 0;
  const bool created = CreateNewWindow("", fullScreen, res);
  if (const auto shadow = std::exchange(m_driveShadow, nullptr))
  {
    NoteDriveShadowBackups();
    ShadowLog(*shadow, m_ledgerSet, created ? "ok" : "failed");
  }
  // 5.B2-1: once per mode set that reached its lost walk
  if (m_ledgerSet != ledgerSet)
  {
    if (!created)
      LedgerSetFailed();
    CLog::Log(LOGINFO, "OUTPUTDRIVE set={} {} guiLocked {} discarded {}", m_ledgerSet.load(),
              created ? "ok" : "failed", guiLocked, m_guiDiscarded);
    m_guiLockedLogSet = created ? m_ledgerSet.load() : 0;
  }
  if (!created)
    return false;

  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

std::shared_ptr<SDriveShadow> CWinSystemAmlogicGLESContext::BeginDriveShadow(
    const RESOLUTION_INFO& res,
    bool fullScreen,
    RenderStereoMode stereo,
    StreamHdrType hdrType,
    bool nativeGUI)
{
  static uint64_t id = 0;
  bool seeded;
  {
    std::unique_lock lock(s_shadowMutex);
    seeded = s_shadowBackupsVersion != 0;
  }
  if (!seeded)
    NoteDriveShadowBackups();

  auto shadow = std::make_shared<SDriveShadow>();
  shadow->id = ++id;
  shadow->res = res;
  shadow->fullScreen = fullScreen;
  shadow->stereo = stereo;
  shadow->hdrType = hdrType;
  shadow->nativeGUI = nativeGUI;
  shadow->hotplug = m_amlDisplay->PeekHotPlug();
  // the request's RAII scope ends with RM's set call, so it is taken here
  shadow->fracRequest = aml_frac_reclock_request();
  shadow->jobsStarted = OUTPUT::COutputWorker::Get().JobsStarted();
  std::unique_lock lock(s_shadowMutex);
  shadow->backupsVersion = s_shadowBackupsVersion;
  return shadow;
}

void CWinSystemAmlogicGLESContext::NoteDriveShadowBackups()
{
  std::unique_lock lock(s_shadowMutex);
  s_shadowBackups = {m_hdrType, m_stereo_mode, m_bFullScreen, m_nativeGUI, m_bWindowCreated};
  ++s_shadowBackupsVersion;
}

bool CWinSystemAmlogicGLESContext::SetFullScreenOnW(bool fullScreen, RESOLUTION_INFO& res)
{
  m_guiDiscarded = 0;

  // the request, read on main as CreateNewWindow reads it (r2 N2: W reads no CSettings)
  static uint64_t s_requests = 0;
  auto job = std::make_shared<SDriveJob>();
  job->id = ++s_requests;
  job->res = res;
  job->resIndex = OUTPUT::DriveResolution();
  job->fbName = m_framebuffer_name;
  job->fullScreen = fullScreen;
  MAINCOUPLING("GC GetStereoMode");
  job->stereo = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();
  job->nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
      CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
  MAINCOUPLING("GC GetHDRType");
  job->hdrType = CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType();
  job->hotplug = m_amlDisplay->GetHotPlug();
  job->fracRequest = aml_frac_reclock_request();
  job->originRm = OUTPUT::DriveOriginRm();
  job->delay = OUTPUT::CSettingsMirror::Get().GetInt("videoscreen.delayrefreshchange");
  job->uiFormat = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  job->backups = {m_hdrType, m_stereo_mode, m_bFullScreen, m_nativeGUI, m_bWindowCreated};
  job->coordinator = m_coordinator.get();
  job->graphicsPlane = m_graphicsPlane.get();
  job->guiLocked = m_guiLocked;
  m_drivePosted = job->id;
  // r1 L7: MidPlay for RM's decision, Restore for the hotplug set, Settings otherwise
  job->lane = job->hotplug    ? OUTPUT::Lane::RESTORE
              : job->originRm ? OUTPUT::Lane::MIDPLAY
                              : OUTPUT::Lane::SETTINGS;

  const auto posted = std::chrono::steady_clock::now();
  OUTPUT::COutputWorker::Get().Post(job->lane, "mode set",
                                    [this, job]
                                    {
                                      // a throw replies failed, and main is answered
                                      try
                                      {
                                        RunModeSetJob(*job);
                                      }
                                      catch (const std::exception& e)
                                      {
                                        CLog::Log(LOGERROR, "OUTPUTDRIVE request {} threw: {}",
                                                  job->id, e.what());
                                        job->kind = OUTPUT::DriveReply::FAILED;
                                      }
                                      catch (...)
                                      {
                                        CLog::Log(LOGERROR, "OUTPUTDRIVE request {} threw",
                                                  job->id);
                                        job->kind = OUTPUT::DriveReply::FAILED;
                                      }
                                      bool late;
                                      {
                                        std::unique_lock lock(job->mutex);
                                        job->done = true;
                                        late = job->abandoned;
                                      }
                                      job->cond.notify_all();
                                      if (late)
                                      {
                                        std::unique_lock lock(s_lateMutex);
                                        s_late.push_back(job);
                                      }
                                    });

  // design §4.5: the gfx lock released across the wait (every recursion level); ERROR at 5 s,
  // given up at 15 s
  bool gaveUp = false;
  {
    CSingleExit gfxExit(CServiceBroker::GetWinSystem()->GetGfxContext());
    std::unique_lock lock(job->mutex);
    const auto done = [&job] { return job->done; };
    if (!job->cond.wait_for(lock, 5s, done))
    {
      CLog::Log(LOGERROR, "OUTPUTDRIVE request {} still waiting after 5000 ms", job->id);
      if (!job->cond.wait_for(lock, 10s, done))
      {
        job->abandoned = true;
        gaveUp = true;
      }
    }
  }
  const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - posted)
                          .count();

  bool created = false;
  if (gaveUp)
  {
    // the outcome is unknown, not a failure: RM does not retry on it (review S1/S2); main's
    // ledger lines wait for the reply, which is applied when it comes if no request followed
    CLog::Log(LOGERROR,
              "OUTPUTDRIVE request {} given up after 15000 ms; its reply is applied when it "
              "comes, if it is still the latest request",
              job->id);
    SetPresentationReady(false);
    // the wire is unknown to main now: the next set runs in full
    m_bWindowCreated = false;
    OUTPUT::NoteDriveReply(OUTPUT::DriveReply::GAVE_UP);
  }
  else
  {
    const auto handlerStart = std::chrono::steady_clock::now();
    created = ApplyDriveReply(*job);
    OUTPUT::NoteDriveReply(job->kind);
    ReportDriveSet(*job, created, waitMs,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - handlerStart)
                       .count(),
                   false);
  }
  if (!created)
    return false;

  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAmlogicGLESContext::ReportDriveSet(
    const SDriveJob& job, bool created, int64_t waitMs, int64_t handlerMs, bool late)
{
  CLog::Log(late ? LOGERROR : LOGINFO,
            "OUTPUTDRIVE set request={} ledger={} origin={} reply={} handler={} park={} park_ms={} "
            "off_ms={} lost_ms={} commit_ms={} engage_ms={} wait_ms={} handler_ms={} "
            "uiepoch={} off={} guiLocked {}{}",
            job.id, job.ledgerSet, job.originRm ? "rm" : job.hotplug ? "hotplug" : "main",
            OUTPUT::DriveReplyName(job.kind), created ? "ok" : "failed",
            job.unparked ? "unacked" : job.parkRetried ? "retried" : "acked", job.parkMs,
            job.offMs, job.lostMs, job.commitMs, job.engageMs, waitMs, handlerMs, job.uiEpoch,
            !job.tornDown ? "-" : job.offRefused ? "refused" : "done", job.guiLocked,
            late ? " (late)" : "");
  // 5.B2-1: once per mode set that reached its lost walk, from the job's own record (never a
  // compare of the live counter, which a job main gave up on may still move)
  if (job.lostWalked)
  {
    if (!created)
      LedgerSetFailed();
    CLog::Log(LOGINFO, "OUTPUTDRIVE set={} {} guiLocked {} discarded {}", job.ledgerSet,
              created ? "ok" : "failed", job.guiLocked, m_guiDiscarded);
    m_guiLockedLogSet = created ? job.ledgerSet : 0;
  }
}

void CWinSystemAmlogicGLESContext::RunModeSetJob(SDriveJob& job)
{
  // On W. Reads no CSettings and no plain member of main's: main waits (or gave up, an ERROR
  // path), so its state comes in the request and goes back in the reply. Today's
  // CreateNewWindow sequence, in today's order (5.B2-5c: the lost walk moved to after the park).
  const auto start = std::chrono::steady_clock::now();
  const auto sinceStart = [&start]
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
  };
  CAMLDisplay* const display = m_amlDisplay.get();
  const RESOLUTION_INFO& res = job.res;
  job.result = job.backups;

  // the decision (GLES CreateNewWindow), from the request and W's own reads (D1)
  const int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  const int cur_fractional_rate =
      display->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR);
  const bool force_mode_switch_by_hdr = job.backups.hdrType != job.hdrType;
  const bool force_mode_switch_by_stereo_mode = job.backups.stereo != job.stereo;
  const bool force_mode_switch_by_fractional_rate = cur_fractional_rate != fractional_rate;
  const bool force_mode_switch_by_hotplug = job.hotplug;
  const bool force_mode_switch_by_wire = display->aml_output_wire_stale();

  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  if (!display->aml_get_native_resolution(&current_resolution, job.nativeGUI))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution",
              __FUNCTION__);
    job.kind = OUTPUT::DriveReply::FAILED;
    return;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(job.hdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(job.backups.hdrType);
  CLog::Log(LOGDEBUG,
            "CWinSystemAmlogicGLESContext::{}: "
            "m_bWindowCreated: {}, "
            "hdrType: {}({}), "
            "force mode switch by - hdr: {}, frac rate: {}, stereo mode: {}, hotplug: {}, wire: {}",
            __FUNCTION__, job.backups.windowCreated, new_hdrStr.empty() ? "none" : new_hdrStr,
            old_hdrStr.empty() ? "none" : old_hdrStr, force_mode_switch_by_hdr,
            force_mode_switch_by_fractional_rate, force_mode_switch_by_stereo_mode,
            force_mode_switch_by_hotplug, force_mode_switch_by_wire);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags, job.backups.nativeGUI);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags, job.nativeGUI);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth &&
      current_resolution.iScreenHeight == res.iScreenHeight &&
      job.backups.fullScreen == job.fullScreen &&
      current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) ==
          (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      job.backups.windowCreated && job.nativeGUI == job.backups.nativeGUI &&
      !force_mode_switch_by_hdr && !force_mode_switch_by_hotplug &&
      !force_mode_switch_by_fractional_rate && !force_mode_switch_by_stereo_mode &&
      !force_mode_switch_by_wire)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window",
              __FUNCTION__);
    // D7: parked only for a disc engage the set carries (today's engage runs parked); no
    // not-ready, no lost walk
    const bool dvPicture = job.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION;
    {
      std::optional<CAmlPresenterHold> hold;
      // its own site: not a set's PARK record, not counted by outputdrive_cstall's n (N5)
      if (dvPicture && aml_dv_disc_engage_pending())
        hold.emplace("mode set no-op");
      aml_dv_engage_pending_disc_session(dvPicture);
    }
    WIRECLASS::CWireClassProbe::Get().Notify("no mode set", WIRECLASS::FromHdrType(job.hdrType));
    job.kind = OUTPUT::DriveReply::DONE_NO_CHANGE;
    return;
  }

  // design §4.2 step 1: the park (not-ready is main's, from the reply). Not acked in 2 s: W
  // waits 500 ms more on the same held epoch (no release, no second PARK record). Not acked then,
  // the set runs unparked as today's park timeout does, and still takes the GUI plane off before
  // the commit, as today's unacked DetachSurface and EGL destroy do; only C's cleared buffer is
  // left out for that set
  CAmlPresenterHold hold("mode set");
  if (!hold.Parked())
  {
    CLog::Log(LOGWARNING, "OUTPUTDRIVE request {}: PARK not acked in 2 s, waiting 500 ms more",
              job.id);
    job.parkRetried = true;
    hold.Rewait(500);
  }
  job.parkMs = sinceStart();
  OUTPUT::TestStopAt("1");
  job.armDelay = true;
  job.unparked = !hold.Parked();
  if (job.unparked)
    CLog::Log(LOGERROR,
              "PARK not acked twice: set runs unparked, plane off before the commit (request {}, "
              "n={})",
              job.id, ++s_unparkedSets);

  // WSA CreateNewWindow's lost walk. 5.B2-5c: right after the park (design §4.2 step 2), before
  // the planes off; outputdrive_order "today" and the skip keep B2-5b's order, after them
  const bool lostFirst = LostFirstSwitch();
  const auto lostWalk = [&]
  {
    LostWalk();
    job.lostWalked = true;
    job.ledgerSet = m_ledgerSet.load();
    job.lostMs = sinceStart();
    CLog::Log(LOGINFO, "OUTPUTDRIVE B2-5c order={} set={}", lostFirst ? "lost-first" : "today",
              job.ledgerSet);
  };
  if (lostFirst)
    lostWalk();

  // step 3, at DestroyWindow's place in today's order: the graphics plane off, then rule S1'
  // (X1's GUI plane off, every GUI buffer back, the UI surface epoch); the EGL surface stays
  // until main's handler
  if (job.graphicsPlane)
    job.graphicsPlane->SwitchOff();
  uint64_t epoch = job.coordinator->UiEpoch() + 1;
  job.offRefused = !job.coordinator->ReclaimUiForModeSet(
      epoch, display->BuildUiPlaneOffRequestLocked(), job.uiFormat, !job.unparked);
  job.uiEpoch = epoch;
  job.tornDown = true;
  job.result.windowCreated = false;
  job.offMs = sinceStart();
  OUTPUT::TestStopAt("3");

  // check if a forced mode switch is required
  bool force_mode_switch = false;
  bool hotplug_mode_switch = false;
  if (force_mode_switch_by_hotplug)
  {
    force_mode_switch = true;
    hotplug_mode_switch = true;
  }
  else if (force_mode_switch_by_stereo_mode)
  {
    force_mode_switch = true;
  }
  else if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
           current_resolution.iScreenWidth == res.iScreenWidth &&
           current_resolution.iScreenHeight == res.iScreenHeight &&
           MathUtils::FloatEquals(current_resolution.fRefreshRate, res.fRefreshRate, 0.06f))
  {
    // same resolution, check frac rate and other parameter
    if (force_mode_switch_by_fractional_rate || force_mode_switch_by_hdr ||
        force_mode_switch_by_wire)
      force_mode_switch = true;
  }

  if (force_mode_switch)
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);

  // a content start takes the content's 1000/1001 rate (24/30/60 families only)
  const float familyRate = std::round(res.fRefreshRate);
  const bool frac_reclock = force_mode_switch_by_fractional_rate && job.fracRequest &&
                            (familyRate == 24.0f || familyRate == 30.0f || familyRate == 60.0f);

  // refresh backup data
  job.result = {job.hdrType, job.stereo, job.fullScreen, job.nativeGUI, false};

  // WSA CreateNewWindow: the lost walk (B2-5b's order), then the commit
  if (!lostFirst)
    lostWalk();

  const bool committed = display->set_native_resolution(res, job.fbName, job.stereo,
                                                        force_mode_switch, hotplug_mode_switch,
                                                        frac_reclock);
  // today's true with no display and no hotplug set (r1 M5)
  const bool notConnected = committed && display->aml_set_not_connected();
  OUTPUT::TestStopAt("4");
  if (committed)
  {
    DISPLAYSTATE::PublishMode(*display, res, !job.fullScreen);
    // N3 / r1 B1: counted, and the timing bumped, before the unpark
    DISPLAYSTATE::NoteCommit();
    aml_timing_inputs_bump();
    // unparked: C may still be presenting; its cleared buffer is left out for this set
    if (!job.unparked)
      job.coordinator->PrepareClearedUi(res.iWidth, res.iHeight, epoch,
                                        [display](uint32_t fb, int& w, int& h)
                                        { return display->BuildFlipRequestLocked(fb, w, h); });
  }
  OUTPUT::TestStopAt("6");
  // where main took the time after its commit job: the delay counts from here
  job.modeSetAt = std::chrono::steady_clock::now();
  job.commitMs = sinceStart();

  if (committed)
  {
    job.result.windowCreated = true;
    // the disc-session DV engage waits for this mode set, so the sink locks once
    aml_dv_engage_pending_disc_session(job.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION);
    WIRECLASS::CWireClassProbe::Get().Notify("mode set", WIRECLASS::FromHdrType(job.hdrType));
    OUTPUT::TestStopAt("5");
  }
  job.engageMs = sinceStart();
  job.kind = !committed      ? OUTPUT::DriveReply::FAILED
             : notConnected ? OUTPUT::DriveReply::NOT_COMMITTED
                            : OUTPUT::DriveReply::DONE;
  // step 7: the unpark, at the hold's end
}

bool CWinSystemAmlogicGLESContext::ApplyDriveReply(SDriveJob& job)
{
  m_driveApplied = std::max(m_driveApplied, job.id);
  // W's backups into main's members (main writes its own, r1 M9)
  m_hdrType = job.result.hdrType;
  m_stereo_mode = job.result.stereo;
  m_bFullScreen = job.result.fullScreen;
  m_nativeGUI = job.result.nativeGUI;
  if (job.tornDown)
  {
    m_nWidth = job.res.iWidth;
    m_nHeight = job.res.iHeight;
    m_fRefreshRate = job.res.fRefreshRate;
  }
  // today's not-ready: WSA CreateNewWindow and DestroyWindow, or the failed decision
  if (job.kind != OUTPUT::DriveReply::DONE_NO_CHANGE)
    SetPresentationReady(false);
  // r2 N4 / rev 3.1: armed for every reply that today reaches WSA's arming; after a failure
  // presentation stays not-ready, so the armed reset waits for a successful set's
  if (job.armDelay && job.delay > 0)
    ArmDelayedReset(job.delay,
                    job.tornDown ? job.modeSetAt : std::chrono::steady_clock::now());

  switch (job.kind)
  {
    case OUTPUT::DriveReply::DONE_NO_CHANGE:
      return true;
    case OUTPUT::DriveReply::DONE:
    case OUTPUT::DriveReply::NOT_COMMITTED:
      // R4-L2: not-committed ran the reclaim too, so it rebuilds
      return RebuildGuiSurface(job);
    default:
      m_bWindowCreated = job.result.windowCreated;
      // Q9 (a): the film stays paused, the GUI frozen, until a set succeeds
      if (job.tornDown)
        CLog::Log(LOGERROR,
                  "OUTPUTDRIVE request {} {}: presentation not ready until a set succeeds",
                  job.id, OUTPUT::DriveReplyName(job.kind));
      return false;
  }
}

bool CWinSystemAmlogicGLESContext::RebuildGuiSurface(const SDriveJob& job)
{
  // rule S1', main's handler
  CPresentationCoordinator* const coordinator = m_coordinator.get();
  const uint64_t newEpoch = job.uiEpoch;
  const auto start = std::chrono::steady_clock::now();
  bool forgot = false;
  {
    // 1. the asserts first: every buffer main locked is in C's returned list, and C holds no
    //    buffer below the epoch (C's own excluded); nothing else is applied before (R4-M3, SM2)
    const auto drained = [&]
    {
      coordinator->TakeReturned(m_guiReturned, m_guiReturnedEpochs);
      return !coordinator->HoldsUiEpochBelow(newEpoch) &&
             static_cast<int>(m_guiReturned.size()) == m_guiLocked;
    };
    bool ok = drained();
    // 2. bounded by the lost-flip bound (FLIP_LOST_NS), inside main's own request (D3). After
    //    S1' everything is returned before the reply, so a wait here detects an m_guiLocked leak
    //    (a count with no buffer behind it), not a flip still landing
    while (!ok && std::chrono::steady_clock::now() - start < 500ms)
    {
      KODI::TIME::Sleep(10ms);
      ok = drained();
    }
    if (!ok)
    {
      // 3. no retry, nothing left unbounded
      CLog::Log(LOGERROR, "UI epoch not drained: guiLocked {} returned {} at epoch {}", m_guiLocked,
                m_guiReturned.size(), newEpoch);
      coordinator->ForgetUiEpoch(newEpoch);
      coordinator->TakeReturned(m_guiReturned, m_guiReturnedEpochs);
      forgot = true;
    }

    // 4. into the still-old surface: only buffers below the epoch
    int released = 0;
    for (size_t i = 0; i < m_guiReturned.size(); i++)
    {
      if (m_guiReturnedEpochs[i] >= newEpoch)
        CLog::Log(LOGERROR, "UIEPOCH ASSERT a buffer of epoch {} back at the rebuild for epoch {}",
                  m_guiReturnedEpochs[i], newEpoch);
      else if (m_amlGBMUtils->Release(m_guiReturned[i], m_guiReturnedEpochs[i]))
        released++;
    }
    m_guiReturned.clear();
    m_guiReturnedEpochs.clear();
    m_guiLocked -= released;
    if (m_guiLocked != 0)
    {
      CLog::Log(LOGERROR, "UIEPOCH ASSERT guiLocked {} at the EGL destroy (epoch {})", m_guiLocked,
                newEpoch);
      m_guiDiscarded += m_guiLocked;
      m_guiLocked = 0;
    }
  }

  // 5. the EGL destroy: the GUI plane is already off (S1'), so normally no RmFB disables it
  const bool planeMayBeUp = job.offRefused || forgot;
  if (!SB3PROBE::On())
    m_pGLContext->DestroySurface();
  else
  {
    SB3PROBE::ArmRmFb();
    const auto destroyStart = std::chrono::steady_clock::now();
    m_pGLContext->DestroySurface();
    const auto destroyEnd = std::chrono::steady_clock::now();
    const SB3PROBE::RmFbResult rmFb = SB3PROBE::TakeRmFb();
    const int64_t destroyStartNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       destroyStart.time_since_epoch())
                                       .count();
    CLog::Log(LOGINFO, "SB3 egl destroyed (handler) took_us={} rmfb={} first_fb={} first_at_us={} "
                       "first_us={}",
              std::chrono::duration_cast<std::chrono::microseconds>(destroyEnd - destroyStart)
                  .count(),
              rmFb.count, rmFb.firstFb,
              rmFb.count ? (rmFb.firstNs - destroyStartNs) / 1000 : -1, rmFb.firstUs);
  }
  if (planeMayBeUp)
    CLog::Log(LOGERROR, "PLANE ui may go off at the EGL destroy's RmFB (epoch {}, {})", newEpoch,
              forgot ? "epoch forgotten" : "off refused");
  CWinSystemAmlogic::DestroyWindow();

  // 6. the new surface, with the job's epoch (never main's own)
  uint32_t format = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
  m_surfaceEpoch = newEpoch;
  // SM6: a failure here is "done, handler failed": no surface, presentation not ready, C's
  // buffer up, GC restored by the caller (no WINDOW_RESIZE); no detach or hold (R4-L3)
  bool created = m_amlGBMUtils->CreateSurface(job.res.iWidth, job.res.iHeight, format, true,
                                              newEpoch);
  if (!created)
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create GBM surface",
              __FUNCTION__);
  else if (!m_pGLContext->CreatePlatformSurface(
               m_amlGBMUtils->GetSurface(),
               reinterpret_cast<EGLNativeWindowType>(m_amlGBMUtils->GetSurface())))
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create CreatePlatformSurface",
              __FUNCTION__);
    created = false;
  }
  else if (!m_pGLContext->BindContext())
    created = false;
  if (!created)
  {
    m_pGLContext->DestroySurface();
    CLog::Log(LOGERROR, "OUTPUTDRIVE request {} done, handler failed: no GUI surface", job.id);
    return false;
  }
  m_bWindowCreated = job.result.windowCreated;

  // The new surface has no cached OSD frame for the dirty-driven GUI skip to
  // keep on screen.
  m_guiRepaintFrames = 3;

  // at delay 0, after the unpark (5.B2-5b), in today's place after the surface
  if (!m_delayDispReset && DisplayResetAllowed())
    ResetDisplayNow();

  if (m_amlDisplay->aml_get_display_connected())
    SetPresentationReady(true);

  // booted without a display: the timekeeper starts with the first CRTC
  if (!m_timekeeper && StartTimekeeper() && m_coordinator && m_tickOwner)
    m_coordinator->SetTickSource(m_timekeeper->TickFd(), m_timekeeper->CrtcId());
  return true;
}

bool CWinSystemAmlogicGLESContext::MessagePump()
{
  // 5.B2-5b (r1 L11): a reply main gave up on, applied normally when it comes (GC goes old ->
  // old -> new), only if no request was posted since, and only from the top-level loop, never
  // from a modal dialog's nested pump
  std::vector<std::shared_ptr<SDriveJob>> late;
  if (!CServiceBroker::GetGUI() || !CServiceBroker::GetGUI()->GetWindowManager().IsNested())
  {
    std::unique_lock lock(s_lateMutex);
    late.swap(s_late);
  }
  for (const auto& job : late)
  {
    if (job->id != m_drivePosted)
    {
      CLog::Log(LOGERROR, "OUTPUTDRIVE request {} late reply {} not applied: request {} since",
                job->id, OUTPUT::DriveReplyName(job->kind), m_drivePosted);
      continue;
    }
    CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
    std::unique_lock gfxLock(gfx);
    const auto start = std::chrono::steady_clock::now();
    DISPLAYSTATE::GcSetBegins();
    m_guiDiscarded = 0;
    const bool created = ApplyDriveReply(*job);
    if (created)
    {
      CRenderSystemGLES::ResetRenderSystem(job->res.iWidth, job->res.iHeight);
      // GC and WINDOW_RESIZE, as at the end of today's set (FinishModeChange is a no-op here)
      if (job->resIndex >= 0)
        gfx.ApplyModeChange(static_cast<RESOLUTION>(job->resIndex));
    }
    DISPLAYSTATE::GcSetEnds(created);
    ReportDriveSet(*job, created, -1,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count(),
                   true);
  }
  return CWinSystemAmlogic::MessagePump();
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
  if (IsHotplugPending() || !IsPresentationReady())
  {
    KODI::TIME::Sleep(10ms);
    return;
  }

  if (m_guiRepaintFrames > 0)
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();

  aml_note_present(rendered);
  SetVSync(true);
  // a frame without a video pass (or without the graphics) takes them off
  if (rendered && !m_hdrGraphicsThisFrame && m_hdrGraphicsShown)
    PresentHdrGraphics(SHdrGraphics());
  bool paced = false;
  if (m_coordinator)
  {
    ReleaseReturnedGuiBuffers();
    if (rendered)
      paced = SubmitGuiFrame();
  }
  else if (rendered)
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
    {
      m_amlDisplay->FlipPage(m_amlGBMUtils->GetFBId());
      if (m_guiRepaintFrames > 0)
        m_guiRepaintFrames--;
    }
  }
  // The loop paces on the vblank unless the video release's poll or the wait
  // for the GUI frame's commit does.
  if (!paced && (m_coordinator || !rendered) && (!videoLayer || aml_video_presenter_active()))
  {
    // no vblank to wait on (disconnected, CRTC off): pace as the video
    // release's 50 ms poll did, not at 100% CPU
    if (!m_amlDisplay->aml_drmDevice_vsync() && (m_coordinator || aml_video_presenter_active()))
      KODI::TIME::Sleep(50ms);
  }

  // polled every frame so the lock reads count from the mode set
  const bool locked = m_delayDispReset && SinkLockedAfterModeSet();
  const auto sinceModeSet = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - m_modeSetAt);
  // a link that never reports lock must not hold the start past the delay + 3 s
  if (m_delayDispReset && m_dispResetTimer.IsTimePast() &&
      (locked || sinceModeSet > m_resetDelay + 3000ms) && DisplayResetAllowed())
  {
    m_delayDispReset = false;
    CLog::Log(LOGINFO, "CWinSystemAmlogic: display reset {} ms after the mode set ({})",
              sinceModeSet.count(), locked ? "delay and sink lock" : "sink never locked");
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnResetDisplay();
      LedgerReset(*i, m_resetDelay);
    }
    LedgerResetDone();
    CloseLostSpan();
    lock.unlock();
    OUTPUT::TestStopAt("8");
  }
}

bool CWinSystemAmlogicGLESContext::SubmitGuiFrame()
{
  // R5: an exhausted Mali surface crashes libMali; keep this frame in the
  // back buffer and draw it again
  constexpr int GUI_LOCK_CAP = 4;
  if (m_guiLocked >= GUI_LOCK_CAP)
  {
    // 5.B2-4 gate (P5-38): counted under on
    if (m_outputWorkerStarted && SurfaceEpochOn() &&
        (++m_guiLockCapStops <= 3 || m_guiLockCapStops % 100 == 0))
      CLog::Log(LOGINFO, "UIEPOCH GUI_LOCK_CAP stop {}", m_guiLockCapStops);
    CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
    return false;
  }

  int fence = -1;
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_eglFence)
    m_eglFence->CreateGPUFence();
#endif

  // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
  // there is probably nothing we can do about it
  m_pGLContext->TrySwapBuffers();

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
  if (m_eglFence)
    fence = m_eglFence->FlushFence();
#endif

  uint32_t fbId = 0;
  gbm_bo* bo = m_amlGBMUtils->LockFront(m_amlDisplay->aml_get_Device_handle(), fbId);
  drmModeAtomicReqPtr req = bo ? m_amlDisplay->BuildFlipRequest(fbId) : nullptr;
  if (!req)
  {
    if (bo)
      m_amlGBMUtils->Release(bo);
    if (fence >= 0)
      close(fence);
    return false;
  }

  m_guiLocked++;
  const uint64_t seq = m_coordinator->SubmitUi(bo, fbId, req, fence, m_surfaceEpoch);
  if (!seq)
  {
    // 5.B2-4 (SM4): refused, a buffer of a surface C no longer takes
    if (m_amlGBMUtils->Release(bo, m_surfaceEpoch))
      m_guiLocked--;
    return false;
  }
  if (m_guiRepaintFrames > 0)
    m_guiRepaintFrames--;
  if (m_guiLockedLogSet && m_guiRepaintFrames == 0)
  {
    CLog::Log(LOGINFO, "OUTPUTDRIVE set={} guiLocked {} after the repaint frames", m_guiLockedLogSet,
              m_guiLocked);
    m_guiLockedLogSet = 0;
  }

  // At most one frame ahead: this one is committed once the previous flip
  // lands, as the render thread's own flip used to wait for it. Rendering the
  // next frame overlaps that flip.
  m_coordinator->WaitTaken(seq, 50ms);
  return true;
}

void CWinSystemAmlogicGLESContext::ReleaseReturnedGuiBuffers()
{
  m_coordinator->TakeReturned(m_guiReturned, m_guiReturnedEpochs);
  if (!m_outputWorkerStarted || !SurfaceEpochOn())
  {
    for (gbm_bo* bo : m_guiReturned)
      m_amlGBMUtils->Release(bo);
    m_guiLocked -= static_cast<int>(m_guiReturned.size());
  }
  else
  {
    // 5.B2-4 (SM1): filtered on C's epoch before the bo is touched
    int released = 0;
    for (size_t i = 0; i < m_guiReturned.size(); i++)
    {
      if (m_guiReturnedEpochs[i] != m_surfaceEpoch)
        CLog::Log(LOGERROR, "UIEPOCH ASSERT a buffer of epoch {} returned at surface epoch {}",
                  m_guiReturnedEpochs[i], m_surfaceEpoch);
      else if (m_amlGBMUtils->Release(m_guiReturned[i], m_guiReturnedEpochs[i]))
        released++;
    }
    m_guiLocked -= released;
    if (m_guiLocked < 0)
    {
      CLog::Log(LOGERROR, "UIEPOCH ASSERT m_guiLocked {}", m_guiLocked);
      m_guiLocked = 0;
    }
  }
  m_guiReturned.clear();
  m_guiReturnedEpochs.clear();
}

void CWinSystemAmlogicGLESContext::DetachGuiSurface()
{
  if (!m_coordinator)
    return;
  // 5.B2-4: the UI epoch steps here until 5.B2-5 moves the reclaim into W's park (r1 L9)
  const bool surfaceEpoch = m_outputWorkerStarted && SurfaceEpochOn();
  uint64_t epoch = 0;
  if (surfaceEpoch)
    epoch = m_coordinator->DetachUiSurfaceNewEpoch(
        m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID));
  else
    m_coordinator->DetachUiSurface();
  ReleaseReturnedGuiBuffers();
  if (surfaceEpoch && m_coordinator->HoldsUiEpochBelow(epoch))
    CLog::Log(LOGERROR, "UIEPOCH ASSERT C still holds a GUI buffer below epoch {}", epoch);
  m_guiDiscarded += m_guiLocked;
  // anything still counted belongs to the old surface and never comes back
  m_guiLocked = 0;
}

// Clear the OSD/overlay plane to opaque black at playback teardown. On this
// dual-plane box the OSD/GBM plane keeps scanning out its cached front buffer
// until a real cleared present swaps in (see the compositing notes above); when
// a BD menu / subtitle overlay is still up at stop, the overlay renderer is
// destroyed and no normal render pass overdraws those pixels, so the stale menu
// lingers. Present a genuine cleared buffer through the vetted PresentRender
// path (guarded LockFrontBuffer + fence handshake), once per buffer in the
// swap chain, paced on vblank so each NONBLOCK atomic commit settles before the
// next buffer is reused. Caller guarantees the rendering thread + current
// context.
bool CWinSystemAmlogicGLESContext::HdrGraphicsOnPlane() const
{
  return m_guiCompositing && m_graphicsPlane && m_graphicsPlane->CanShow();
}

void CWinSystemAmlogicGLESContext::PresentHdrGraphics(const SHdrGraphics& graphics)
{
  if (!m_graphicsPlane)
    return;
  SHdrGraphics shown = graphics;
  shown.limited = UseLimitedColor();
  m_hdrGraphicsThisFrame = true;
  m_hdrGraphicsShown = !shown.images.empty();
  m_graphicsPlane->Show(std::move(shown));
}

void CWinSystemAmlogicGLESContext::ClearOverlayPlane()
{
  if (m_hdrGraphicsShown)
    PresentHdrGraphics(SHdrGraphics());

  if (!m_pGLContext || !m_amlGBMUtils)
    return;

  // Bind the default framebuffer: on a DV/HDR (composite) disc the GUI FBO may
  // still be bound, and a raw glClear would clear that offscreen texture rather
  // than the back buffer that is presented.
  if (m_guiFbo.IsValid() && m_guiFbo.IsBound())
    m_guiFbo.EndRender();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);

  // Opaque black: makes the OSD plane itself black regardless of the amvideo
  // plane teardown (a transparent clear would let residual video show through).
  // black is 0 in every transfer function, so this is correct on the PQ
  // composite plane too, without running the composite shader.
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

  for (int i = 0; i < 3; i++)
  {
    glClear(GL_COLOR_BUFFER_BIT);
    PresentRender(true, false);
    m_amlDisplay->aml_drmDevice_vsync();
  }

  m_guiFboClean = true;
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
    if (!BuildCompositeShader())
    {
      m_guiCompositing = false;
      return false;
    }

    // GUI reference white follows videoscreen.guipeakluminance instead of the
    // composite's hardcoded 203 nits, so the setting is a working brightness
    // control here too and means the same thing as on the per-primitive path.
    // At the shipped default this is ~199 nits, i.e. a <2% change from before.
    const float peak(CGuiCompositeShaderGLES::PeakFromPQCode(GetGuiSdrPeakLuminance()));
    if (colorTransfer == m_guiCompositeTransfer && peak == m_guiCompositePeak)
      return true;
    m_compositeShader->SetSdrPeak(peak);

    if (!m_compositeShader->CreateLUTs(colorTransfer))
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext: failed to create LUTs");
      m_compositeShader.reset();
      m_guiCompositing = false;
      return false;
    }
    m_guiCompositeTransfer = colorTransfer;
    m_guiCompositePeak = peak;
  }
  else
  {
    m_guiFbo.Cleanup();
    m_guiFboWidth = 0;
    m_guiFboHeight = 0;
    m_hdrFbo.Cleanup();
    m_hdrFboWidth = 0;
    m_hdrFboHeight = 0;
    m_hdrFboHasContent = false;
    m_hdrFboUnavailable = false;
    // Compiling the program takes 19-41 ms (design §16.14), inside
    // CRenderManager::Configure under the locks the presentation coordinator's
    // tick needs: every SDR -> composite segment change cost the tick one or two
    // vblanks. On validated hardware the program is kept for the next composite
    // segment (its LUTs are rebuilt there, under 1 ms); only the FBOs go.
    if (!aml_presentation_validated())
      m_compositeShader.reset();
  }

  return m_guiCompositing;
}

bool CWinSystemAmlogicGLESContext::BeginRender()
{
  if (!CRenderSystemGLES::BeginRender())
    return false;

  // The HDR overlay FBO holds content only for the frame whose video pass drew
  // it (EndHdrOverlayRender sets this again). Without the reset, a window with
  // no video pass - the user backing out to a skin window without a
  // videowindow, or RenderWithoutPicture returning early - kept compositing the
  // last subtitle/menu frame under the GUI until playback stopped.
  m_hdrFboHasContent = false;
  m_hdrGraphicsThisFrame = false;

  // CApplication::Render runs the video pass (RenderEx) before the GUI here. HDR
  // overlays go to their own FBO when it can be created (BeginHdrOverlayRender)
  // and fall back to this back buffer otherwise. Clear before that pass rather
  // than after it, or the composite would present a fallback draw wiped - and
  // letterbox areas would keep stale swap-chain content otherwise.
  if (m_guiCompositing)
  {
    // the scissor box still holds whatever the previous frame's GUI left
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
  }

  return true;
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

  // Pick up a live guipeakluminance change so GUI brightness can be dialled in
  // while a disc is playing, rather than only at the next stream start. One
  // settings read per composited frame - cheaper than the per-primitive path,
  // which already reads it on every shader enable (CGLESShader::OnEnabled) - and
  // the LUT is rebuilt only when the value actually moves.
  if (m_compositeShader)
  {
    const float peak(CGuiCompositeShaderGLES::PeakFromPQCode(GetGuiSdrPeakLuminance()));
    if (peak != m_guiCompositePeak)
    {
      m_compositeShader->SetSdrPeak(peak);
      if (!m_compositeShader->CreateLUTs(m_guiCompositeTransfer))
      {
        // CreateLUTs commits only on success, so the previous LUTs are still
        // live and the GUI keeps rendering correctly at the old reference white.
        // Put the shader's peak back in step with them, but still record the
        // requested value so a failed rebuild is not retried on every frame.
        CLog::Log(LOGWARNING, "CWinSystemAmlogicGLESContext: GUI peak luminance change "
                              "rejected, keeping the previous reference white");
        m_compositeShader->SetSdrPeak(m_guiCompositePeak);
      }
      m_guiCompositePeak = peak;
    }
  }

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

bool CWinSystemAmlogicGLESContext::BeginHdrOverlayRender()
{
  if (!m_guiCompositing)
    return false;

  const int width = m_nWidth;
  const int height = m_nHeight;
  if (!m_hdrFbo.IsValid() || m_hdrFboWidth != width || m_hdrFboHeight != height)
  {
    m_hdrFbo.Cleanup();
    if (!m_hdrFbo.Initialize() ||
        !m_hdrFbo.CreateAndBindToTexture(GL_TEXTURE_2D, width, height, GL_RGBA))
    {
      CLog::Log(LOGERROR,
                "CWinSystemAmlogicGLESContext: failed to create HDR overlay FBO {}x{}, "
                "drawing HDR overlays onto the back buffer",
                width, height);
      m_hdrFbo.Cleanup();
      m_hdrFboWidth = 0;
      m_hdrFboHeight = 0;
      m_hdrFboHasContent = false;
      m_hdrFboUnavailable = true;
      return false;
    }
    m_hdrFboWidth = width;
    m_hdrFboHeight = height;
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext: created HDR overlay FBO {}x{}", width,
              height);
  }

  if (!m_hdrFbo.BeginRender())
  {
    m_hdrFboUnavailable = true;
    return false;
  }
  m_hdrFboUnavailable = false;

  // the scissor box still holds whatever the previous frame's GUI left
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  return true;
}

void CWinSystemAmlogicGLESContext::EndHdrOverlayRender(bool drawn)
{
  // drawn == true only ever follows a successful BeginHdrOverlayRender
  if (drawn)
    m_hdrFbo.EndRender();
  m_hdrFboHasContent = drawn && m_hdrFbo.IsValid();
}

void CWinSystemAmlogicGLESContext::EndGuiComposite()
{
  if (m_guiWillRender)
    m_guiFbo.EndRender();
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
  // pixels render at the wrong opacity. Composite the stored alpha "over"
  // (src.a + dst.a * (1 - src.a)) instead: the back buffer is cleared to 0, so
  // this is exactly src.a except under HDR overlays already drawn by the video
  // pass, which keep their coverage.
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  // orthographic projection (screen coords, Y-down)
  float w = static_cast<float>(m_guiFboWidth);
  float h = static_cast<float>(m_guiFboHeight);

  GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};

  m_compositeShader->SetProjection(proj);
  m_compositeShader->SetHdrTexture(m_hdrFboHasContent ? m_hdrFbo.Texture() : 0);
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
  // the timeline clock, once the timekeeper has published (before its first
  // vblank the board is empty, and the kernel's vblanks are the source)
  int64_t tl[PRESENTATION::TL_COUNT];
  if (PRESENTATION::TimelineClockActive() && PRESENTATION::TimelineBoard().Read(tl))
    return std::make_unique<CVideoSyncTimeline>(clock);
  return std::make_unique<CVideoSyncAML>(clock);
}

bool CWinSystemAmlogicGLESContext::SupportsStereo(const RenderStereoMode mode) const
{
  if (m_amlDisplay->aml_display_support_3d() &&
      mode == RenderStereoMode::HARDWAREBASED) {
    // yes, we support hardware based MVC decoding
    return true;
  }

  return CRenderSystemGLES::SupportsStereo(mode);
}
