/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <string.h>
#include <float.h>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "platform/linux/FDEventMonitor.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "guilib/DispResource.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <poll.h>
#include <unistd.h>

#include "system_egl.h"

using namespace KODI;

std::unique_ptr<CAMLDisplay> CWinSystemAmlogic::m_amlDisplay = nullptr;

CWinSystemAmlogic::CWinSystemAmlogic()
:  m_libinput(new CLibInputHandler)
,  m_force_mode_switch(false)
,  m_fdMonitorId(-1)
,  m_udev(NULL)
,  m_callback_data(NULL, NULL)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  m_amlDisplay = std::make_unique<CAMLDisplay>();
  m_amlGBMUtils = std::make_unique<CAMLGBMUtils>(m_amlDisplay->aml_get_Device_handle());

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;
  m_stereo_mode = RenderStereoMode::OFF;
  m_delayDispReset = false;

  m_nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  m_libinput->Start();
}

CWinSystemAmlogic::~CWinSystemAmlogic()
{
  MonitorStop();
}

void CWinSystemAmlogic::SettingOptionsComponentsFiller(const SettingConstPtr& setting,
                                                 std::vector<IntegerSettingOption>& list,
                                                 int& current)
{
  if (m_amlDisplay->aml_display_support_dv())
  {
    const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    CHDRCapabilities dv_cap = m_amlDisplay->GetHDRCaps();

    if (dv_cap.SupportsDVTVLED())
      list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(14426),
                        AML_DV_TV_LED);

    if (dv_cap.SupportsDVPlayerLED())
      list.emplace_back(CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(14427),
                        AML_DV_PLAYER_LED);

    AML_DISPLAY_DV_LED old_value = static_cast<AML_DISPLAY_DV_LED>(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED));
    AML_DISPLAY_DV_LED new_value = old_value;

    if (old_value == AML_DV_TV_LED && !dv_cap.SupportsDVTVLED())
      new_value = static_cast<AML_DISPLAY_DV_LED>(dv_cap.SupportsDVPlayerLED() ? AML_DV_PLAYER_LED : -1);

    if (old_value == AML_DV_PLAYER_LED && !dv_cap.SupportsDVPlayerLED())
      new_value = static_cast<AML_DISPLAY_DV_LED>(dv_cap.SupportsDVTVLED()? AML_DV_TV_LED : -1);

    if (new_value != -1)
      current = new_value;
  }
}

void CWinSystemAmlogic::MonitorStart()
{
  int err;

  if (!m_udev && m_fdMonitorId == -1)
  {
    m_udev = udev_new();
    if (!m_udev)
    {
      CLog::Log(LOGWARNING, "CWinSystemAmlogic::Start - Unable to open udev handle");
      return;
    }

    m_callback_data.udevMonitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_callback_data.udevMonitor)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_new_from_netlink() failed");
      goto err_unref_udev;
    }

    err = udev_monitor_filter_add_match_subsystem_devtype(m_callback_data.udevMonitor, "drm", NULL);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_filter_add_match_subsystem_devtype() failed");
      goto err_unref_udev;
    }

    err = udev_monitor_enable_receiving(m_callback_data.udevMonitor);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_enable_receiving() failed");
      goto err_unref_udev;
    }

    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    m_callback_data.object = this;
    m_fdMonitorId = 0;
    eventMonitor->AddFD(
        CFDEventMonitor::MonitoredFD(udev_monitor_get_fd(m_callback_data.udevMonitor),
                                     POLLIN, FDEventCallback, (void *)&m_callback_data),
        m_fdMonitorId);
  }

  return;

err_unref_udev:
  MonitorStop();
}

void CWinSystemAmlogic::MonitorStop()
{
  if (m_fdMonitorId != -1)
  {
    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    eventMonitor->RemoveFD(m_fdMonitorId);
    m_fdMonitorId = -1;
  }

  if (m_callback_data.udevMonitor)
  {
    udev_monitor_unref(m_callback_data.udevMonitor);
    m_callback_data.udevMonitor = NULL;
  }

  if (m_udev)
  {
    udev_unref(m_udev);
    m_udev = NULL;
  }
}

bool CWinSystemAmlogic::MessagePump()
{
  if (m_hotplugPending.exchange(false))
    HotplugEvent();

  return false;
}

void CWinSystemAmlogic::HotplugEvent()
{
  SetPresentationReady(false);

  // A different panel may now be attached, so the cached VSVDB - which
  // describes one specific display - can no longer be trusted.
  aml_display_vsvdb_invalidate();

  m_amlDisplay->aml_init_drmDevice();
  drmModeConnection connection;
  int mode_count = m_amlDisplay->aml_get_display_modes_count(&connection);

  RefreshDisplayCapabilities();

  if (connection == DRM_MODE_DISCONNECTED && mode_count == 1)
  {
    CLog::Log(LOGWARNING,
      "CWinSystemAmlogic - HotplugEvent mode switch ignored while HDMI DRM connector is not ready ({:d} modes)",
      mode_count);
    return;
  }

  std::string preferred_mode = m_amlDisplay->aml_get_preferred_mode();
  RESOLUTION res = static_cast<RESOLUTION>(RES_DESKTOP);

  CDisplaySettings::GetInstance().ClearCustomResolutions();
  RefreshResolutions();
  CDisplaySettings::GetInstance().ApplyCalibrations();

  if (!preferred_mode.empty())
  {
    for (size_t resolution = RES_DESKTOP; resolution < CDisplaySettings::GetInstance().ResolutionInfoSize(); resolution++)
    {
      RESOLUTION_INFO resinfo = CDisplaySettings::GetInstance().GetResolutionInfo(resolution);
      if (StringUtils::EqualsNoCase(resinfo.strId, preferred_mode))
      {
        res = static_cast<RESOLUTION>(resolution);
        break;
      }
    }

    CLog::Log(LOGDEBUG, "CWinSystemAmlogic - HotplugEvent, preferred mode: {}, display mode: {}",
      preferred_mode, CDisplaySettings::GetInstance().GetResolutionInfo(res).strId);
  }
  else
    CLog::Log(LOGWARNING, "CWinSystemAmlogic - HotplugEvent, no preferred mode defined, use display mode: {}",
      CDisplaySettings::GetInstance().GetResolutionInfo(res).strId);

  m_amlDisplay->SetHotPlug();
  CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, true);
  CServiceBroker::GetWinSystem()->GetGfxContext().ApplyModeChange(res);
}

void CWinSystemAmlogic::FDEventCallback(int id, int fd, short revents, void *data)
{
  struct callback_data *callbackData = (struct callback_data *)data;
  if (!callbackData || !callbackData->udevMonitor || !callbackData->object)
    return;

  struct udev_monitor *udevMonitor = callbackData->udevMonitor;
  CWinSystemAmlogic *winSystem = callbackData->object;
  struct udev_device *device;

  while ((device = udev_monitor_receive_device(udevMonitor)) != NULL)
  {
    const char* action = udev_device_get_action(device);
    const char* syspath = udev_device_get_syspath(device);
    const char* devpath = udev_device_get_devpath(device);
    const char* hotplug = udev_device_get_property_value(device, "HOTPLUG");
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic - FDEventCallback (\"{}\", \"{}\"), action: {}",
      syspath ? syspath : "<null>", devpath ? devpath : "<null>", action ? action : "<null>");

    const bool isHotplug = action && hotplug && StringUtils::EqualsNoCase(action, "change") &&
                           strcmp(hotplug, "1") == 0;

    udev_device_unref(device);

    if (isHotplug)
      winSystem->m_hotplugPending.store(true);
  }
}

namespace
{
// Dolby Vision VS10 engine: option fillers for the per-source-type output-mode
// spinners. Offered outputs are gated on real display capability (HDR10/DV);
// there is no force-modes override on CE22, so unsupported outputs are hidden.
std::string vs10_label(int id)
{
  return CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(id);
}

// SDR8 / SDR10 sources.
void VS10SdrFiller(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current)
{
  list.clear();
  list.emplace_back(vs10_label(60063), DOLBY_VISION_OUTPUT_MODE_BYPASS); // Off
  list.emplace_back(vs10_label(60064), DOLBY_VISION_OUTPUT_MODE_SDR10);  // SDR
  if (aml_display_support_hdr_pq()) list.emplace_back(vs10_label(60065), DOLBY_VISION_OUTPUT_MODE_HDR10);
  if (aml_display_support_dv())     list.emplace_back(vs10_label(60066), DOLBY_VISION_OUTPUT_MODE_IPT);
}

// HDR10 / HDR10+ sources (Off only offered when the display can take HDR10).
void VS10Hdr10Filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current)
{
  list.clear();
  if (aml_display_support_hdr_pq()) list.emplace_back(vs10_label(60063), DOLBY_VISION_OUTPUT_MODE_BYPASS);
  list.emplace_back(vs10_label(60064), DOLBY_VISION_OUTPUT_MODE_SDR10);
  if (aml_display_support_hdr_pq()) list.emplace_back(vs10_label(60065), DOLBY_VISION_OUTPUT_MODE_HDR10);
  if (aml_display_support_dv())     list.emplace_back(vs10_label(60066), DOLBY_VISION_OUTPUT_MODE_IPT);
}

// HLG sources.
void VS10HdrHlgFiller(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current)
{
  list.clear();
  if (aml_display_support_hdr_hlg()) list.emplace_back(vs10_label(60063), DOLBY_VISION_OUTPUT_MODE_BYPASS);
  list.emplace_back(vs10_label(60064), DOLBY_VISION_OUTPUT_MODE_SDR10);
  if (aml_display_support_hdr_pq())  list.emplace_back(vs10_label(60065), DOLBY_VISION_OUTPUT_MODE_HDR10);
  if (aml_display_support_dv())      list.emplace_back(vs10_label(60066), DOLBY_VISION_OUTPUT_MODE_IPT);
}

// DV sources (Off == native DV tunnel via IPT).
void VS10DvFiller(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current)
{
  list.clear();
  list.emplace_back(vs10_label(60064), DOLBY_VISION_OUTPUT_MODE_SDR10); // SDR
  if (aml_display_support_hdr_pq()) list.emplace_back(vs10_label(60065), DOLBY_VISION_OUTPUT_MODE_HDR10); // HDR10
  if (aml_display_support_dv()) list.emplace_back(vs10_label(60063), DOLBY_VISION_OUTPUT_MODE_IPT); // Off = native DV
}
} // namespace

bool CWinSystemAmlogic::InitWindowSystem()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  RefreshDisplayCapabilities();

  if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- disabling noise reduction");
     CSysfsPath("/sys/module/aml_media/parameters/nr2_en", 0);
  }

  if (!IsHDRDisplay())
  {
    CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", 0);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", 0);
    CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 1);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 1);
  }

  if (!aml_support_dolby_vision())
  {
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE, false);
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV, false);
    settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV, false);
    settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED, AML_DV_TV_LED);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_DOVIZEROLEVEL5, true);
  }

  const bool box_supports_dv = aml_support_dolby_vision();

  // Setting VISIBILITY for everything Dolby Vision now lives in
  // RefreshDisplayCapabilities(), which upstream re-runs on every display event -
  // so a sink change re-evaluates the rows instead of leaving the boot-time
  // verdict frozen for the session. What stays here is what must happen exactly
  // once: value clamps keyed on SoC capability (which cannot change at runtime),
  // and the option-filler / callback registrations.
  // (the !aml_support_dolby_vision() block above already zeroes L5 for that case,
  // which is the fallback while dolbyvision.l5.mode is hidden.)
  if (box_supports_dv)
  {
    auto* dvMgr = CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager();

    // Registered whenever the SoC can do DV, not only when the boot-time sink
    // could: RefreshDisplayCapabilities() can re-show dv_led later on a sink
    // change, and its spinner needs a filler to populate.
    dvMgr->RegisterSettingOptionsFiller("dv_led_modes", SettingOptionsComponentsFiller);

    // Dolby Vision VS10 engine per-source-type output-mode fillers.
    dvMgr->RegisterSettingOptionsFiller("DolbyVisionVS10SDR8", VS10SdrFiller);
    dvMgr->RegisterSettingOptionsFiller("DolbyVisionVS10SDR10", VS10SdrFiller);
    dvMgr->RegisterSettingOptionsFiller("DolbyVisionVS10HDR10", VS10Hdr10Filler);
    dvMgr->RegisterSettingOptionsFiller("DolbyVisionVS10HDRHLG", VS10HdrHlgFiller);
    dvMgr->RegisterSettingOptionsFiller("DolbyVisionVS10DV", VS10DvFiller);

    // Live-apply the VSVDB max-luminance override when the shared display-peak
    // value or the force toggle changes during DV playback, and the CMv4.0
    // append mode / Smart threshold when those change (both used to be latched
    // at stream open, so changing them mid-playback appeared to do nothing).
    dvMgr->RegisterCallback(this, {CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_TARGET_MINLUM,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD});
  }

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceGLES::Register();

  auto setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (setting)
  {
    setting->SetVisible(false);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK, false);
  }

  // Close the OpenVFD splash and switch the display into time mode.
  CSysfsPath("/tmp/openvfd_service", 0);

  drmModeConnection connection;
  int mode_count = m_amlDisplay->aml_get_display_modes_count(&connection);

  if (connection == DRM_MODE_DISCONNECTED)
  {
    if (mode_count > 1)
    {
      CLog::Log(LOGWARNING,
        "CWinSystemAmlogic::InitWindowSystem HDMI modes are present but DRM connector is not ready, defer hotplug");
      m_hotplugPending.store(true);
    }
    else if (mode_count == 1)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem Looks like no display is connected, wait for hotplug");
    }
  }

  MonitorStart();

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (settingsComponent)
    settingsComponent->GetSettings()->GetSettingsManager()->UnregisterCallback(this);
  return true;
}

void CWinSystemAmlogic::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting)
    return;

  const std::string& settingId = setting->GetId();

  // CMv4.0 append mode / Smart threshold: publish to the codec, which re-pushes
  // them to its bitstream converter on the next packet. Unconditional - the
  // generation counter is inert when nothing is decoding, and a stream opening
  // later reads the settings directly anyway. display.maxnits feeds BOTH this
  // and the VSVDB inject below; bumping it here is what keeps the Smart bypass
  // threshold and the peak the amdv core tone-maps to from diverging when the
  // slider moves mid-playback.
  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS)
    aml_dv_cmv40_settings_changed();

  if (settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_TARGET_MINLUM &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE)
    return;

  // Only re-apply live while a DV stream is decoding (dolby_vision_enable == Y);
  // otherwise the new value is picked up at the next decoder open. aml_dv_apply_vsvdb
  // re-injects the updated block and re-latches it via an EDID re-parse.
  CSysfsPath dv_enable{"/sys/module/aml_media/parameters/dolby_vision_enable"};
  if (dv_enable.Exists() &&
      StringUtils::EqualsNoCase(dv_enable.Get<std::string>().value_or("N"), "Y"))
  {
    // DM target overrides (peak + reference black) for the active VS10 mode.
    aml_dv_apply_target_overrides(aml_dv_get_vs10_pending());
    if (settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_TARGET_MINLUM)
      aml_dv_apply_vsvdb();
  }
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  bool ret;

  SetPresentationReady(false);
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  if ((ret = m_amlDisplay->set_native_resolution(res, m_framebuffer_name, m_stereo_mode,
                                           m_force_mode_switch, m_hotplug_mode_switch)))
  {
    m_bWindowCreated = true;
  }

  m_force_mode_switch = false;
  m_hotplug_mode_switch = false;
  return ret;
}

bool CWinSystemAmlogic::DestroyWindow()
{
  SetPresentationReady(false);
  m_bWindowCreated = false;
  return true;
}

void CWinSystemAmlogic::RefreshResolutions()
{
  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!m_amlDisplay->aml_probe_resolutions(resolutions) || resolutions.empty())
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);

  // get all resolutions supported by connected device
  if (m_amlDisplay->aml_get_native_resolution(&curDisplay))
    resDesktop = curDisplay;

  for (auto& res : resolutions)
  {
    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz",
      res.iWidth,
      res.iHeight,
      res.iScreenWidth,
      res.iScreenHeight,
      res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      res.fRefreshRate);

    // add new custom resolution
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
    CDisplaySettings::GetInstance().AddResolutionInfo(res);

    // check if resolution match current mode
    if(resDesktop.iWidth == res.iWidth &&
       resDesktop.iHeight == res.iHeight &&
       resDesktop.iScreenWidth == res.iScreenWidth &&
       resDesktop.iScreenHeight == res.iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       fabs(resDesktop.fRefreshRate - res.fRefreshRate) < FLT_EPSILON)
    {
      // update desktop resolution
      CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;
    }
  }
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RefreshResolutions();
}

void CWinSystemAmlogic::RefreshDisplayCapabilities()
{
  m_amlDisplay->aml_refresh_display_caps();

  const bool device_dv = aml_support_dolby_vision();
  const bool sink_dv = device_dv && m_amlDisplay->aml_display_support_dv();

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  // VISIBILITY ONLY below this point. This function runs on every display event,
  // so it must be idempotent: forcing setting VALUES here would clobber the user's
  // choices on each sink change (unplug to a non-DV panel and back, and an
  // sdr2dv/hdr2dv the user had enabled would never come back). The one-time value
  // clamps live in InitWindowSystem, keyed on SoC capability, which cannot change.

  // The native Dolby Vision tunnel (TV-LED / Player-LED output, SDR->DV, HDR->DV)
  // drives the sink over HDMI as a DV stream, so it needs BOTH a DV-capable SoC and
  // a DV-capable display. Upstream keys these three on the SoC alone so a disabled
  // row keeps its cause on screen; this build hides them, because on a non-DV sink
  // the VS10 engine - not the DV tunnel - is what carries DV content, and leaving
  // SDR->DV / HDR->DV offered there invites a setting that cannot do anything.
  auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);
  if (setting)
    setting->SetVisible(sink_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV);
  if (setting)
    setting->SetVisible(sink_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV);
  if (setting)
    setting->SetVisible(sink_dv);

  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED);
  if (setting)
    setting->SetVisible(sink_dv);

  // The stock videoplayer.dovizerolevel5 boolean is superseded on Amlogic by the
  // richer dolbyvision.l5.mode (Source/Zero/Auto-detect), so it is always hidden.
  setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_DOVIZEROLEVEL5);
  if (setting)
    setting->SetVisible(false);

  // The VS10 engine, VSVDB override, Smart CMv4.0, L5 active-area and HDR10+ ->
  // DV conversion all rewrite the RPU before output mapping, so they are useful
  // whenever the SoC can process DV -- including on non-DV displays, where VS10
  // maps DV -> HDR10/SDR. Keyed on SoC capability alone, NOT on the sink (the
  // per-source fillers self-limit which output options the display can take).
  for (const auto& dvId : {CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10_OSD_BRIGHTNESS,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_TARGET_MINLUM,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_MODE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_OSD_UNMASK,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_NONDV_STOCKCONVERT})
  {
    setting = settings->GetSetting(dvId);
    if (setting)
      setting->SetVisible(device_dv);
  }

  // Clamp the DV output mode to something this sink actually advertises. Sink
  // dependent, so it re-runs on a display change rather than only at boot.
  if (sink_dv)
  {
    const int dv_cap = m_amlDisplay->aml_get_drmProperty("dv_cap", DRM_MODE_OBJECT_CONNECTOR);
    const AML_DISPLAY_DV_LED old_value = static_cast<AML_DISPLAY_DV_LED>(
      settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED));
    AML_DISPLAY_DV_LED new_value = old_value;

    if (old_value == AML_DV_TV_LED && !(dv_cap & DV_RGB_444_8BIT))
      new_value = static_cast<AML_DISPLAY_DV_LED>((dv_cap & LL_YCbCr_422_12BIT) != 0 ? AML_DV_PLAYER_LED : -1);

    if (old_value == AML_DV_PLAYER_LED && !(dv_cap & LL_YCbCr_422_12BIT))
      new_value = static_cast<AML_DISPLAY_DV_LED>((dv_cap & DV_RGB_444_8BIT) != 0 ? AML_DV_TV_LED : -1);

    if (new_value != old_value)
      settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED, new_value);
  }

  if (IsHDRDisplay())
  {
    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR)
        ->SetVisible(true);

    int sdr2hdr = CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
    if (sdr2hdr)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting sdr2hdr mode to {:d}", __FUNCTION__, sdr2hdr);
      CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", sdr2hdr);
      CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 0);
      CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 0);
    }

    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR)
        ->SetVisible(true);

    int hdr2sdr = CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
    if (hdr2sdr)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting hdr2sdr mode to {:d}", __FUNCTION__, hdr2sdr);
      CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", hdr2sdr);
    }
  }
  else
  {
    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
    if (setting)
      setting->SetVisible(false);

    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
    if (setting)
      setting->SetVisible(false);
  }
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  CHDRCapabilities caps = m_amlDisplay->GetHDRCaps();
  return (caps.SupportsHDR10() | caps.SupportsHDR10Plus() | caps.SupportsHLG() |
         (caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE));
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  return m_amlDisplay->GetHDRCaps();
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int guiSdrPeak = settings->GetInt(CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE);

  return ((0.7f * guiSdrPeak + 30.0f) / 100.0f);
}

HDR_STATUS CWinSystemAmlogic::GetOSHDRStatus()
{
  return (IsHDRDisplay() ? HDR_STATUS::HDR_ON : HDR_STATUS::HDR_UNSUPPORTED);
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
