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
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cxxabi.h>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <utility>

#include "ServiceBroker.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
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
#include "filesystem/File.h"
#include "guilib/DispResource.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/AMLUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/ThreadRole.h"
#include "threads/SingleLock.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>

#include "system_egl.h"

using namespace KODI;
using namespace std::chrono_literals;

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
    if (start != std::string::npos)
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

    // Kept as int: AML_DISPLAY_DV_LED only has the values 0 and 1, so a -1
    // sentinel is outside the enum's range and casting to it is undefined. When
    // the sink advertises neither mode there is no valid choice to move to, and
    // the option list above is empty anyway, so leave the value alone.
    const int old_value = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED);
    int new_value = old_value;

    if (old_value == AML_DV_TV_LED && !dv_cap.SupportsDVTVLED() && dv_cap.SupportsDVPlayerLED())
      new_value = AML_DV_PLAYER_LED;
    else if (old_value == AML_DV_PLAYER_LED && !dv_cap.SupportsDVPlayerLED() &&
             dv_cap.SupportsDVTVLED())
      new_value = AML_DV_TV_LED;

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
  if (!m_wHotplug && OUTPUT::HotplugOnW())
    StartHotplugOnW();

  if (m_wHotplug)
  {
    TestSetAfter();
    // a receipt the FD thread took on HEAD's path before the switch was live: handed to W
    if (m_hotplugPending.exchange(false))
      ReceiveHotplug(false);
    ApplyHotplugReplies();
  }
  else
  {
    // a span whose reconnect did not end it (the mode set failed): try again,
    // backing off, while the output reads connected and no reset is pending
    // (5.B2-6: W's SETTLE publishes its own pending flag)
    if (m_lostOpen && !m_hotplugPending && !m_disconnectedSinceMs &&
        !((m_delayDispReset || m_resetPending) && IsPresentationReady()) &&
        m_lostRetry.IsTimePast())
    {
      CLog::Log(LOGWARNING, "CWinSystemAmlogic: display back but still lost, retrying the mode set");
      m_lostRetry.Set(m_lostRetryDelay);
      m_lostRetryDelay = std::min(m_lostRetryDelay * 2, std::chrono::milliseconds(30000));
      m_hotplugPending.store(true);
    }

    if (m_hotplugPending.exchange(false))
      HotplugEvent();
  }

  DISPLAYSTATE::HandleOutputChanged();
  // 5.B3-3 (hotplugworker only): playback's GuiInputs, published when they change
  DISPLAYSTATE::PublishGuiPlayback();
  // 5.B3-7b: main's sets are applied and their GuiInputs published. A late reply is taken only
  // at a top-level pump; a nested one (a modal) publishes GC as it stands, as HEAD's nested
  // HotplugEvent read it, so the hotplug waits at most one pump there
  if (m_wHotplug && m_mainRequests == 0 &&
      (m_lateReplies == 0 || (CServiceBroker::GetGUI() &&
                              CServiceBroker::GetGUI()->GetWindowManager().IsNested())))
    m_guiInputsStale = false;
  DISPLAYSTATE::Tick();
  return false;
}

void CWinSystemAmlogic::HotplugEvent()
{
  if (m_wHotplug)
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogic::{} called with the hotplug on W: ignored", __FUNCTION__);
    return;
  }
  SetPresentationReady(false);

  // a display coming back during playback gets the video's mode, not the
  // desktop mode; an unplug moves the output to the desktop mode first
  const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (!appPlayer || !appPlayer->IsPlayingVideo())
    m_videoModeAtHotplug.reset();
  else if (!m_videoModeAtHotplug)
    m_videoModeAtHotplug = CDisplaySettings::GetInstance().GetResolutionInfo(
        CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution());

  // A different panel may now be attached, so the cached VSVDB - which
  // describes one specific display - can no longer be trusted.
  OUTPUT::COutputWorker::Get().Run(OUTPUT::Lane::RESTORE, "vsvdb refill", [] {
    aml_display_vsvdb_invalidate();
    aml_display_vsvdb_refill();
  });

  DISPLAYSTATE::CapsWriteBegins();
  try
  {
    m_amlDisplay->aml_init_drmDevice();
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogic::{} - display rebuild failed: {}", __FUNCTION__,
              e.what());
    LogHotplugRead(DRM_MODE_UNKNOWNCONNECTION, -1);
    // not known disconnected: an open span retries the rebuild, and the time
    // stops counting towards the pause
    {
      // 5.B3-7a (O3): the stamp is kept with the span and the lost generation
      std::unique_lock<CCriticalSection> lock(m_resourceSection, std::defer_lock);
      if (OUTPUT::OrderingOn())
        lock.lock();
      m_disconnectedSinceMs = 0;
    }
    PublishDisplayCaps();
    return;
  }

  drmModeConnection connection;
  int mode_count = m_amlDisplay->aml_get_display_modes_count(&connection);
  LogHotplugRead(connection, mode_count);

  {
    // 5.B3-7a (O3): the stamp and the span's opening under one hold of the lock, so a reset's
    // re-check sees both or neither
    std::unique_lock<CCriticalSection> lock(m_resourceSection, std::defer_lock);
    if (OUTPUT::OrderingOn())
      lock.lock();
    if (connection == DRM_MODE_CONNECTED)
      m_disconnectedSinceMs = 0;
    else if (m_lostAtDisconnect)
    {
      if (!m_disconnectedSinceMs)
        m_disconnectedSinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
      OpenLostSpan();
    }
  }

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

  DISPLAYSTATE::CapsWriteBegins();
  CDisplaySettings::GetInstance().ClearCustomResolutions();
  RefreshResolutions();
  aml_timing_inputs_bump();
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

  if (m_videoModeAtHotplug && connection == DRM_MODE_CONNECTED)
  {
    for (size_t i = RES_DESKTOP; i < CDisplaySettings::GetInstance().ResolutionInfoSize(); i++)
    {
      const RESOLUTION_INFO& info = CDisplaySettings::GetInstance().GetResolutionInfo(i);
      if (StringUtils::EqualsNoCase(info.strId, m_videoModeAtHotplug->strId) &&
          std::abs(info.fRefreshRate - m_videoModeAtHotplug->fRefreshRate) < 0.01f &&
          info.dwFlags == m_videoModeAtHotplug->dwFlags)
      {
        res = static_cast<RESOLUTION>(i);
        CLog::Log(LOGINFO, "CWinSystemAmlogic - HotplugEvent, restoring the video's mode {} @ {:.3f}",
                  info.strId, info.fRefreshRate);
        // kept until applied: a display may come back with fewer modes first
        m_videoModeAtHotplug.reset();
        break;
      }
    }
  }

  m_amlDisplay->SetHotPlug();
  CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, true);
  CServiceBroker::GetWinSystem()->GetGfxContext().ApplyModeChange(res);
}

namespace
{
std::string HpdReadText(const HpdRead& read)
{
  return StringUtils::Format("{}/{}/{}", read.valid ? std::to_string(read.state) : "invalid",
                             aml_hpd_source_name(read.source), read.seq);
}
} // namespace

namespace
{
// 5.B3-6 (r1 M6): the HDMI connector's encoder_id from the job's own ioctls, both freed here;
// never m_connector, which main's re-init may be freeing. -1 when it cannot be read
int ShadowEncoderId(int fd)
{
  drmModeResPtr resources = drmModeGetResources(fd);
  if (!resources)
    return -1;
  int encoderId = -1;
  for (int i = 0; i < resources->count_connectors && encoderId < 0; i++)
  {
    drmModeConnectorPtr connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
    if (!connector)
      continue;
    if (connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)
      encoderId = static_cast<int>(connector->encoder_id);
    drmModeFreeConnector(connector);
  }
  drmModeFreeResources(resources);
  return encoderId;
}
} // namespace

void CWinSystemAmlogic::PostDetectShadow()
{
  // 5.B3-7a (O4): a receipt after W's stop is dropped, never run inline on the FD thread (Post
  // runs fn on its caller once W is stopping; Active() then Post was a check-then-act)
  if (OUTPUT::DetectShadowOn() && OUTPUT::OrderingOn())
  {
    m_shadowReceipts.fetch_add(1);
    if (m_shadowQueued.exchange(true))
      return;
    if (!OUTPUT::COutputWorker::Get().PostIfRunning(
            OUTPUT::Lane::RESTORE, "hotplug shadow",
            [this, posted = std::chrono::steady_clock::now()] { RunDetectShadow(posted); }))
    {
      m_shadowQueued.store(false);
      CLog::Log(LOGINFO, "HOTPLUG shadow: hotplug after W stop: dropped (main's flag is set)");
    }
    return;
  }
  if (!OUTPUT::DetectShadowOn() || !OUTPUT::COutputWorker::Get().Active())
    return;
  m_shadowReceipts.fetch_add(1);
  // one job per collapse, as the receipts collapse into main's flag
  if (m_shadowQueued.exchange(true))
    return;
  OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::RESTORE, "hotplug shadow",
                                    [this, posted = std::chrono::steady_clock::now()]
                                    { RunDetectShadow(posted); });
}

void CWinSystemAmlogic::RunDetectShadow(std::chrono::steady_clock::time_point posted)
{
  try
  {
    // receipts after this belong to the next job
    m_shadowQueued.store(false);
    // r5 #18: how long the job queued behind W's running job; its reads are that late
    const auto queueMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - posted)
                             .count();
    const uint64_t receipts = m_shadowReceipts.load();
    // hotplugworker_hold (test only): W sleeps between the job's receipt count and its read, so
    // an edge inside the hold is read without its receipt counted (plan §22 item 17's case)
    if (const int holdMs = OUTPUT::TestHotplugHoldMs(); holdMs > 0)
    {
      CLog::Log(LOGINFO, "HOTPLUG shadow test hold {} ms", holdMs);
      KODI::TIME::Sleep(std::chrono::milliseconds(holdMs));
    }
    HpdRead read;
    const bool valid = aml_hpd_read(read, !OUTPUT::TestHotplugNoHpdCb());
    const int encoderId = ShadowEncoderId(m_amlDisplay->aml_get_Device_handle());
    int state = read.state;
    const char* cls;
    if (!valid)
      cls = "invalid-read";
    else
    {
      // no hdmitx node either: today's predicate, on the job's own read
      if (read.source == HpdSource::DRM)
        state = encoderId > 0 ? 1 : 0;
      if (read.source == HpdSource::CB && read.seq != 0 && read.seq == m_shadowLastAct)
        cls = "echo";
      else if (read.source == HpdSource::HPD_STATE && state == 1 && m_shadowDisconnectedRead &&
               receipts <= m_shadowDisconnectedAt)
        // no event since the disconnected read: W will wait for one (up to 1 s); the shadow
        // waits for nothing and lets the next event decide
        cls = "wait-confirm";
      else
      {
        cls = state == 0 ? "loss" : (m_shadowSpan ? "restore" : "reassert");
        m_shadowSpan = state == 0;
        m_shadowLastAct = read.seq;
      }
      if (read.source == HpdSource::HPD_STATE && state == 0)
      {
        // r5 #17: the receipts as they stand after the read, so only an event received after
        // the disconnected read confirms a later connected one
        m_shadowDisconnectedRead = true;
        m_shadowDisconnectedAt = m_shadowReceipts.load();
      }
      else if (read.source == HpdSource::HPD_STATE && state == 1 &&
               strcmp(cls, "wait-confirm") != 0)
        m_shadowDisconnectedRead = false;
    }
    CLog::Log(LOGINFO,
              "HOTPLUG shadow class={} source={} state={} seq={} last={} span={} encoder_id={} "
              "tries={} queue_ms={}",
              cls, aml_hpd_source_name(read.source), state, read.seq, m_shadowLastAct,
              IsLostSpanOpen() ? 1 : 0, encoderId, read.tries, queueMs);
  }
  catch (...)
  {
  }
}

void CWinSystemAmlogic::LogStartupLink()
{
  // 5.B3-1 (log only, every session): the link the init found, before any hotplug
  // a log line never aborts the init (HEAD has no throw here)
  try
  {
    HpdRead read;
    aml_hpd_read(read);
    uint32_t encoder = 0;
    uint32_t crtc = 0;
    std::string mode;
    bool modeValid = false;
    m_amlDisplay->aml_get_display_link(encoder, crtc, mode, modeValid);
    CLog::Log(LOGINFO, "startup link hpd={} encoder_id={} crtc={} mode={} valid={}",
              HpdReadText(read), encoder, crtc, mode.empty() ? "-" : mode, modeValid ? 1 : 0);
  }
  catch (...)
  {
  }
}

void CWinSystemAmlogic::LogHotplugRead(drmModeConnection connection, int modes)
{
  // 5.B3-1 (log only): today's event against the HPD reads, sysfs only (design §6, A0)
  // a log line never aborts the event (HEAD has no throw here; one call is in a catch)
  try
  {
    const bool cbPresent = aml_hpd_cb_present();
    HpdRead read;
    const bool cb = cbPresent && aml_hpd_read(read) && read.source == HpdSource::CB;
    const char* const noCb = cbPresent ? "invalid" : "absent";
    uint32_t encoder = 0;
    uint32_t crtc = 0;
    std::string mode;
    bool modeValid = false;
    m_amlDisplay->aml_get_display_link(encoder, crtc, mode, modeValid);
    const int hpdState = aml_hdmitx_hpd_state();
    CSysfsPath statusNode{"/sys/class/drm/card0-HDMI-A-1/status"};
    const std::string status =
        statusNode.Exists() ? statusNode.Get<std::string>().value_or("-") : "absent";
    CLog::Log(LOGINFO,
              "HOTPLUG event hpd_state={} cb_state={} cb_seq={} drm_status={} encoder_id={} "
              "connection={} modes={}",
              hpdState < 0 ? "absent" : std::to_string(hpdState),
              cb ? std::to_string(read.state) : noCb, cb ? std::to_string(read.seq) : noCb,
              status, encoder,
              modes < 0 ? "-"
                        : (connection == DRM_MODE_CONNECTED ? "connected" : "disconnected"),
              modes < 0 ? "-" : std::to_string(modes));
  }
  catch (...)
  {
  }
}

void CWinSystemAmlogic::OpenLostSpan()
{
  // 5.B3-7a (O3): the test inside the lock, so no reset fans out between it and the span
  const bool ordering = OUTPUT::OrderingOn();
  if (!ordering && m_lostOpen)
    return;
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  if (ordering && m_lostOpen)
    return;
  m_lostOpen = true;
  // 5.B3-7b: W's job arms its own P5-9 timer
  if (!m_wHotplug)
  {
    m_lostRetryDelay = std::chrono::milliseconds(1000);
    m_lostRetry.Set(m_lostRetryDelay);
  }
  CLog::Log(LOGINFO, "CWinSystemAmlogic: output disconnected, display lost until it is back");
  LedgerBegin("lost span");
  // 5.B3-7a (O3): the span's fan-out is a new lost generation; a reset armed before it drops
  if (ordering)
    CLog::Log(LOGINFO, "LEDGER set={} lost generation {} (lost span)", m_ledgerSet.load(),
              ++m_lostGen);
  for (IDispResource* resource : m_resources)
  {
    resource->OnLostDisplay();
    LedgerLost(resource);
  }
}

void CWinSystemAmlogic::CloseLostSpan()
{
  if (m_lostOpen.exchange(false))
    CLog::Log(LOGINFO, "CWinSystemAmlogic: display back, the lost span ends");
}

bool CWinSystemAmlogic::DisplayResetAllowed() const
{
  // cleared only by a hotplug that reads the output connected (a mode set
  // inside the span would make the connector read connected)
  return !m_lostOpen || !m_disconnectedSinceMs;
}

int64_t CWinSystemAmlogic::DisconnectedForMs() const
{
  const int64_t since = m_disconnectedSinceMs;
  if (!since)
    return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
             .count() -
         since;
}

bool CWinSystemAmlogic::HoldIfDisplayLost(IDispResource* resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  if (!m_lostOpen)
    return false;
  resource->OnLostDisplay();
  LedgerLost(resource);
  return true;
}

namespace
{
std::string ListenerName(IDispResource* resource)
{
  const char* mangled = typeid(*resource).name();
  int status = 0;
  char* name = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  std::string result = status == 0 && name ? name : mangled;
  free(name);
  return result;
}
} // namespace

void CWinSystemAmlogic::LedgerBegin(const char* kind)
{
  if (const int owed = m_ledgerLost.load())
    CLog::Log(LOGINFO, "LEDGER set={} lost {} owed into set={}", m_ledgerSet.load(), owed,
              m_ledgerSet.load() + 1);
  m_ledgerFailed = false;
  m_ledgerAt = std::chrono::steady_clock::now();
  CLog::Log(LOGINFO, "LEDGER set={} begins ({})", ++m_ledgerSet, kind);
}

void CWinSystemAmlogic::LedgerLost(IDispResource* resource)
{
  ++m_ledgerLost;
  CLog::Log(LOGINFO, "LEDGER lost {} set={}", ListenerName(resource), m_ledgerSet.load());
}

void CWinSystemAmlogic::LedgerReset(IDispResource* resource, std::chrono::milliseconds delay)
{
  ++m_ledgerReset;
  CLog::Log(LOGINFO, "LEDGER reset {} set={} after {} ms delay={} after_failed={}",
            ListenerName(resource), m_ledgerSet.load(),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_ledgerAt)
                .count(),
            delay.count(), m_ledgerFailed.load() ? 1 : 0);
}

void CWinSystemAmlogic::LedgerResetDone()
{
  CLog::Log(LOGINFO, "LEDGER set={} lost {} reset {}", m_ledgerSet.load(), m_ledgerLost.load(),
            m_ledgerReset);
  m_ledgerLost = 0;
  m_ledgerReset = 0;
}

void CWinSystemAmlogic::LedgerSetFailed()
{
  m_ledgerFailed = true;
  CLog::Log(LOGINFO, "LEDGER set={} failed, lost {} owed", m_ledgerSet.load(), m_ledgerLost.load());
}

bool CWinSystemAmlogic::ResetStillArmedLocked(uint64_t lostGen, const char* reset)
{
  // the caller's test ran outside the lock (H6): a span opened, or a set walked, since
  const bool allowed = DisplayResetAllowed();
  if (allowed && lostGen == m_lostGen)
  {
    CLog::Log(LOGINFO, "LEDGER set={} reset in lost generation {} ({})", m_ledgerSet.load(),
              lostGen, reset);
    return true;
  }
  // nothing is reset: m_ledgerLost keeps the lost calls, owed into the next reset (union)
  CLog::Log(LOGINFO,
            "LEDGER set={} reset dropped: lost generation {} superseded (now {}, {}; {} reset): "
            "lost {} owed into the next reset",
            m_ledgerSet.load(), lostGen, m_lostGen,
            allowed ? "a lost fan-out since" : "the span is stamped", reset, m_ledgerLost.load());
  return false;
}

const char* SHotplugReply::KindName(Kind kind)
{
  switch (kind)
  {
    case Kind::ECHO:
      return "echo";
    case Kind::INVALID_READ:
      return "invalid-read";
    case Kind::WAIT_TIMEOUT:
      return "wait-timeout";
    case Kind::THROW:
      return "throw";
    case Kind::LOSS:
      return "loss";
    case Kind::LOSS_CAPSFAIL:
      return "loss-capsfail";
    case Kind::RESTORE:
      return "restore";
    case Kind::REASSERT:
      return "reassert";
    case Kind::RETRY_NOOP:
      return "retry-noop";
    case Kind::RETRY_RESTORE:
      return "retry-restore";
  }
  return "?";
}

namespace
{
int64_t SteadyMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Has2160(const std::vector<RESOLUTION_INFO>& table)
{
  return std::any_of(table.begin(), table.end(),
                     [](const RESOLUTION_INFO& res) { return res.iScreenHeight >= 2160; });
}

// RES_DESKTOP's entry names a mode of a display (not the disconnected placeholder)
bool RealSlot(const RESOLUTION_INFO& slot)
{
  return !slot.strId.empty() && !StringUtils::EqualsNoCase(slot.strId, "dummy_l");
}

bool SameMode(const RESOLUTION_INFO& res, const std::string& strId, float refresh, uint32_t flags)
{
  return StringUtils::EqualsNoCase(res.strId, strId) && std::abs(res.fRefreshRate - refresh) < 0.01f &&
         res.dwFlags == flags;
}

// 5.B3-8 (§4.7): a mode by its facts: the mode string (any case), the screen size, the refresh
// within 0.01 Hz and the flags; never iWidth/iHeight (the GUI size follows DISABLEGUISCALING)
bool SameFacts(const RESOLUTION_INFO& res, const DISPLAYSTATE::ModeFacts& facts)
{
  return res.iScreenWidth == facts.screenWidth && res.iScreenHeight == facts.screenHeight &&
         SameMode(res, facts.strId, facts.refresh, facts.dwFlags);
}

// the Restore's rows (rows[0] RES_DESKTOP's entry, then the probe's): the first row with these
// facts, when the probe has them (a slot kept from the old table is no evidence of the new one)
std::optional<size_t> FactsRow(const std::vector<RESOLUTION_INFO>& rows,
                               const DISPLAYSTATE::ModeFacts& facts)
{
  if (facts.strId.empty() ||
      std::none_of(rows.begin() + std::min<size_t>(1, rows.size()), rows.end(),
                   [&facts](const RESOLUTION_INFO& res) { return SameFacts(res, facts); }))
    return std::nullopt;
  for (size_t i = 0; i < rows.size(); ++i)
    if (SameFacts(rows[i], facts))
      return i;
  return std::nullopt;
}

// the connector's PREFERRED mode on the Restore's rows: the probe's first row of that mode
// string, then FactsRow's first row with its facts
std::optional<size_t> PreferredRow(const std::vector<RESOLUTION_INFO>& rows,
                                   const std::string& preferred)
{
  if (preferred.empty())
    return std::nullopt;
  for (size_t i = 1; i < rows.size(); ++i)
    if (StringUtils::EqualsNoCase(rows[i].strId, preferred))
      return FactsRow(rows, DISPLAYSTATE::FactsOf(rows[i]));
  return std::nullopt;
}

// the boot vout mode on the Restore's rows: the probe's row of that string at resolution.ini's
// frac (the 1000/1001 row at 1), else PreferredRow's first row of the string, as off's
std::optional<size_t> VoutRow(const std::vector<RESOLUTION_INFO>& rows,
                              const std::string& vout,
                              int frac)
{
  if (!vout.empty() && frac >= 0)
    for (size_t i = 1; i < rows.size(); ++i)
      if (StringUtils::EqualsNoCase(rows[i].strId, vout) &&
          (rows[i].fRefreshRate != std::floor(rows[i].fRefreshRate)) == (frac == 1))
        return FactsRow(rows, DISPLAYSTATE::FactsOf(rows[i]));
  return PreferredRow(rows, vout);
}
} // namespace

void CWinSystemAmlogic::StartHotplugOnW()
{
  // main, holding nothing; no W hotplug job exists before m_wHotplug
  LatchRestoreInputs();
  PublishMainSlot();
  if (OUTPUT::RestoreRow2On())
  {
    // 5.B3-8 (§4.7): the session's desktop mode, once; W's desktopMode until the first reconnect
    std::unique_lock lock(m_slotMutex);
    m_sessionDesktopMode = DISPLAYSTATE::FactsOf(m_mainSlot);
    m_wDesktopMode = m_sessionDesktopMode;
    m_wDesktopModeWhy = "the session's desktop mode";
    CLog::Log(LOGINFO, "HOTPLUG session desktop mode {}",
              DISPLAYSTATE::FactsString(m_sessionDesktopMode));
    // a session whose live mode at start was not the boot vout mode (resolution.ini's; an
    // unrouted or reduced-EDID boot, a kernel fallback) takes the vout's at the first reconnect
    // whose table has it, at the boot frac_rate_policy (VoutRow)
    const std::string vout = m_amlDisplay->aml_get_boot_vout_mode();
    m_wSessionVout.clear();
    if (!vout.empty() && !StringUtils::EqualsNoCase(m_sessionDesktopMode.strId, vout))
    {
      m_wSessionVout = vout;
      m_wSessionVoutFrac = m_amlDisplay->aml_get_boot_frac_rate_policy();
      const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
      std::string voutRow = "not in the table";
      for (size_t i = RES_DESKTOP; i < displaySettings.ResolutionInfoSize(); ++i)
        if (StringUtils::EqualsNoCase(displaySettings.GetResolutionInfo(i).strId, vout))
        {
          voutRow = DISPLAYSTATE::FactsString(
              DISPLAYSTATE::FactsOf(displaySettings.GetResolutionInfo(i)));
          break;
        }
      CLog::Log(LOGINFO,
                "HOTPLUG session desktop mode {} is not the boot vout mode {} ({}): the first "
                "reconnect whose table has the vout mode takes it",
                DISPLAYSTATE::FactsString(m_sessionDesktopMode), vout, voutRow);
    }
  }
  {
    const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
    std::vector<RESOLUTION_INFO> table;
    for (size_t i = RES_DESKTOP; i < displaySettings.ResolutionInfoSize(); ++i)
      table.push_back(displaySettings.GetResolutionInfo(i));
    m_wHas2160 = Has2160(table);
  }
  // the hooks W reads, read here so no hotplug job probes a file
  OUTPUT::TestHotplugHoldMs();
  OUTPUT::TestHotplugRaceGapMs();
  OUTPUT::TestHotplugRetryMs();
  OUTPUT::TestHotplugSetAfterMs();
  OUTPUT::TestHotplugApplyThrowAt();
  OUTPUT::PrimeTestHotplugCapsFail();
  m_wHotplug = true;
  OUTPUT::NoteHotplugJobsOnW();
}

void CWinSystemAmlogic::PublishMainSlot()
{
  const RESOLUTION_INFO slot = CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP);
  std::unique_lock lock(m_slotMutex);
  m_mainSlot = slot;
}

void CWinSystemAmlogic::ReceiveHotplug(bool fromFd)
{
  // hotplugworker_setafter (test only): a plug-in's receipt is followed by main's own set
  if (const int afterMs = OUTPUT::TestHotplugSetAfterMs(); fromFd && afterMs >= 0)
  {
    HpdRead read;
    if (aml_hpd_read(read, !OUTPUT::TestHotplugNoHpdCb()) && read.state == 1)
      m_setAfterDueMs = SteadyMs() + afterMs;
  }
  // O1: the receipt before the flag, so a queued job's read covers it
  m_receiptSeq.fetch_add(1);
  if (m_jobQueued.exchange(true))
    return;
  if (!OUTPUT::COutputWorker::Get().PostIfRunning(OUTPUT::Lane::RESTORE, "hotplug",
                                                  [this] { RunHotplugJob(false); }))
  {
    m_jobQueued = false;
    CLog::Log(LOGINFO, "HOTPLUG hotplug after W stop: dropped");
  }
}

void CWinSystemAmlogic::TestSetAfter()
{
  int64_t due = m_setAfterDueMs.load();
  if (!due || SteadyMs() < due || !m_setAfterDueMs.compare_exchange_strong(due, 0))
    return;
  const RESOLUTION res = CDisplaySettings::GetInstance().GetCurrentResolution();
  CLog::Log(LOGWARNING, "HOTPLUGWORKER: test settings resolution change to {} after a plug-in",
            static_cast<int>(res));
  CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SETVIDEORESOLUTION, res, 1);
}

void CWinSystemAmlogic::RunHotplugJob(bool retry, std::optional<DISPLAYSTATE::ModeFacts> recover)
{
  SHotplugReply reply;
  // HEAD's order: no hotplug between main's set and main's apply of it, nor before its GuiInputs
  // publish (the video's mode, P13). The job stays queued (m_jobQueued set, its receipts not
  // read), so the receipts after this still collapse into it
  if (!retry && (m_mainRequests > 0 || m_guiInputsStale))
  {
    if (!std::exchange(m_wDeferred, true))
      CLog::Log(LOGINFO, "HOTPLUG job deferred: main's mode set is on W or not yet applied");
    if (!OUTPUT::COutputWorker::Get().PostAfter(OUTPUT::Lane::RESTORE, "hotplug", 10ms,
                                                [this] { RunHotplugJob(false); }))
    {
      m_jobQueued = false;
      CLog::Log(LOGINFO, "HOTPLUG hotplug after W stop: dropped");
    }
    return;
  }
  if (!retry)
  {
    m_wDeferred = false;
    // O1 (r2 N2): the flag cleared before the receipts are read
    m_jobQueued = false;
    if (const int gapMs = OUTPUT::TestHotplugRaceGapMs(); gapMs > 0)
    {
      CLog::Log(LOGINFO, "HOTPLUG job test gap {} ms", gapMs);
      KODI::TIME::Sleep(std::chrono::milliseconds(gapMs));
    }
    m_wUpTo = m_receiptSeq.load();
  }
  reply.upTo = m_wUpTo;
  reply.replyId = ++m_wReplies;
  // O4: W stopping drains its queue; main applies no reply after DestroyWindowSystem
  if (!OUTPUT::COutputWorker::Get().Active())
  {
    CLog::Log(LOGINFO, "HOTPLUG job {}: hotplug after W stop: dropped", reply.replyId);
    return;
  }
  try
  {
    HotplugJob(reply, retry, recover);
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "HOTPLUG job {} threw: {}", reply.replyId, e.what());
    reply.kind = SHotplugReply::Kind::THROW;
  }
  catch (...)
  {
    CLog::Log(LOGERROR, "HOTPLUG job {} threw", reply.replyId);
    reply.kind = SHotplugReply::Kind::THROW;
  }
  // ends a caps write the throw left begun (PublishCaps catches its own failure)
  if (reply.kind == SHotplugReply::Kind::THROW)
    DISPLAYSTATE::PublishCaps(*m_amlDisplay, m_wHas2160,
                              OUTPUT::RestoreRow2On() ? &m_wDesktopMode : nullptr);
  CLog::Log(LOGINFO,
            "HOTPLUG job reply={} upTo={} kind={}{} source={} state={} seq={} last={} span={} "
            "claim=no-op park_ms={} parkRetried={} modes={} table={} restore={}",
            reply.replyId, reply.upTo, SHotplugReply::KindName(reply.kind), retry ? " (retry)" : "",
            aml_hpd_source_name(reply.read.source), reply.read.state, reply.read.seq, m_wLastAct,
            m_lostOpen ? 1 : 0, reply.parkMs, reply.parkRetried ? 1 : 0, reply.modes,
            reply.hasTable ? static_cast<int>(reply.table.size()) : -1,
            reply.restoreJob ? "yes" : "no");
  std::unique_lock lock(m_replyMutex);
  m_replies.push_back(std::move(reply));
}

void CWinSystemAmlogic::HotplugJob(SHotplugReply& reply,
                                   bool retry,
                                   const std::optional<DISPLAYSTATE::ModeFacts>& recover)
{
  using Kind = SHotplugReply::Kind;
  // hotplugworker_hold (test only): an edge inside the hold is read by this job, its receipt
  // posts the next one
  if (const int holdMs = OUTPUT::TestHotplugHoldMs(); holdMs > 0 && !retry)
  {
    CLog::Log(LOGINFO, "HOTPLUG job test hold {} ms", holdMs);
    KODI::TIME::Sleep(std::chrono::milliseconds(holdMs));
  }

  // step 1: the read, before any claim or park (design §6 Detection)
  HpdRead& read = reply.read;
  const bool valid = aml_hpd_read(read, !OUTPUT::TestHotplugNoHpdCb());
  if (valid && read.source == HpdSource::DRM)
    read.state = ShadowEncoderId(m_amlDisplay->aml_get_Device_handle()) > 0 ? 1 : 0;

  // HotplugEvent's video mode, at every event of a playback: recorded once, kept until main has
  // applied a Restore to it (D5: HEAD resets it at the match, its apply being the same call; a
  // reply main drops before its adopt leaves it recorded for the retry's Restore)
  const DISPLAYSTATE::GuiInputs gui = DISPLAYSTATE::GetGuiInputs();
  if (!gui.videoPlaying || (m_wVideoModeReply && m_videoModeApplied.load() >= m_wVideoModeReply))
  {
    m_wVideoMode.reset();
    m_wVideoModeReply = 0;
  }
  // never the facts of no row (GC at RES_INVALID, or its row past main's table, before D5's hold)
  if (gui.videoPlaying && !m_wVideoMode && !gui.videoResolution.strId.empty())
    m_wVideoMode = gui.videoResolution;

  if (!valid)
  {
    reply.kind = Kind::INVALID_READ;
    // B3-1: one re-read 50 ms later, as a receipt of its own
    OUTPUT::COutputWorker::Get().PostAfter(OUTPUT::Lane::RESTORE, "hotplug re-read", 50ms,
                                           [this] { ReceiveHotplug(false); });
    return;
  }
  if (!retry && read.source == HpdSource::CB && read.seq != 0 && read.seq == m_wLastAct)
  {
    CLog::Log(LOGINFO, "HOTPLUG hpd echo: seq {} already acted on", read.seq);
    reply.kind = Kind::ECHO;
    return;
  }
  if (!retry && read.source == HpdSource::HPD_STATE && read.state == 1 && m_wDisconnectedRead &&
      reply.upTo <= m_wDisconnectedAt)
  {
    // no event since the disconnected read: an event received after it confirms the connected
    // one (up to 1 s), and its own job acts
    const auto until = std::chrono::steady_clock::now() + 1s;
    while (m_receiptSeq.load() <= m_wDisconnectedAt && std::chrono::steady_clock::now() < until)
      KODI::TIME::Sleep(10ms);
    if (m_receiptSeq.load() > m_wDisconnectedAt)
      CLog::Log(LOGINFO, "HOTPLUG hpd confirmed by a later event: its job acts");
    else
      CLog::Log(LOGWARNING, "HOTPLUG hpd ahead of event: connected read with no event after the "
                            "disconnected one");
    reply.kind = Kind::WAIT_TIMEOUT;
    return;
  }
  const bool connected = read.state == 1;
  if (retry && !connected)
  {
    reply.kind = Kind::RETRY_NOOP;
    return;
  }
  m_wLastAct = read.seq;
  if (read.source == HpdSource::HPD_STATE)
  {
    m_wDisconnectedRead = !connected;
    if (!connected)
      m_wDisconnectedAt = m_receiptSeq.load();
  }

  // step 2: the claim (a no-op before 5.E2's display word), then the park (B2-5b's rule)
  const auto start = std::chrono::steady_clock::now();
  CAmlPresenterHold hold("hotplug");
  if (!hold.Parked())
  {
    CLog::Log(LOGWARNING, "HOTPLUG job {}: PARK not acked in 2 s, waiting 500 ms more",
              reply.replyId);
    reply.parkRetried = true;
    hold.Rewait(500);
  }
  reply.parkMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();

  // step 3: the stamp and the Lost fan-out, before the re-init; a re-assert (connected with no
  // span) takes the fan-out unstamped
  bool spanWasOpen;
  bool spanOpened;
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // read with the resets' span close excluded (O3)
    spanWasOpen = m_lostOpen;
    if (!connected && m_lostAtDisconnect)
    {
      if (!m_disconnectedSinceMs)
      {
        // review #7: a loss inside an open span is a new generation, so a reset armed before it
        // cannot fire once a later connected read clears the stamp
        if (m_lostOpen)
          CLog::Log(LOGINFO, "LEDGER set={} lost generation {} (re-stamp)", m_ledgerSet.load(),
                    ++m_lostGen);
        m_disconnectedSinceMs = SteadyMs();
      }
      OpenLostSpan();
    }
    else if (connected)
    {
      m_disconnectedSinceMs = 0;
      if (!retry && !m_lostOpen && m_lostAtDisconnect)
        OpenLostSpan();
    }
    spanOpened = !spanWasOpen && m_lostOpen;
  }
  // P5-9's timer for the span this job opened, outside the resource lock
  if (spanOpened)
    ArmHotplugRetry();

  // the VSVDB describes one display (today's order: before the re-init)
  aml_display_vsvdb_invalidate();
  aml_display_vsvdb_refill();

  // step 4: the re-init under the park, connection from the read
  DISPLAYSTATE::CapsWriteBegins();
  try
  {
    m_amlDisplay->aml_init_drmDevice(read.source == HpdSource::DRM ? std::nullopt
                                                                   : std::optional<bool>(connected));
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogic::{} - display rebuild failed: {}", __FUNCTION__,
              e.what());
    LogHotplugRead(DRM_MODE_UNKNOWNCONNECTION, -1);
    {
      std::unique_lock<CCriticalSection> lock(m_resourceSection);
      m_disconnectedSinceMs = 0;
    }
    // the caps are published with the reply (RunHotplugJob)
    reply.kind = Kind::THROW;
    return;
  }
  drmModeConnection connection;
  reply.modes = m_amlDisplay->aml_get_display_modes_count(&connection);
  LogHotplugRead(connection, reply.modes);
  if (connected && OUTPUT::TestHotplugCapsFail())
    reply.modes = 1;

  // step 5: the caps, all before any commit (H5)
  m_amlDisplay->aml_refresh_display_caps();
  reply.deviceDv = aml_support_dolby_vision();
  reply.sinkDv = reply.deviceDv && m_amlDisplay->aml_display_support_dv();
  const CHDRCapabilities caps = m_amlDisplay->GetHDRCaps();
  reply.hdrDisplay = caps.SupportsHDR10() || caps.SupportsHDR10Plus() || caps.SupportsHLG() ||
                     caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
  if (reply.sinkDv)
    reply.dvLed = DvLedClamp(
        OUTPUT::CSettingsMirror::Get().GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED),
        m_amlDisplay->aml_get_drmProperty("dv_cap", DRM_MODE_OBJECT_CONNECTOR));
  reply.hasCaps = true;
  if (reply.hdrDisplay)
    aml_hdr_conversion_refresh("display caps");

  RESOLUTION_INFO slot;
  if (reply.modes > 1)
  {
    // the probe into a by-value table, with main's DISABLEGUISCALING (H4)
    if (!m_amlDisplay->aml_probe_resolutions(reply.table, gui.nativeGui) || reply.table.empty())
      CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.", __FUNCTION__);
    RESOLUTION_INFO mainSlot;
    {
      std::unique_lock lock(m_slotMutex);
      mainSlot = m_mainSlot;
    }
    // P14: a disconnected read is "dummy_l", never the display's mode; RES_DESKTOP keeps a real
    // mode until the Restore (a session that has none takes HEAD's rule)
    RESOLUTION_INFO live;
    if (!connected && RealSlot(mainSlot))
      CLog::Log(LOGINFO, "HOTPLUG slot kept {}: the live read is dummy_l (disconnected)",
                mainSlot.strId);
    else if (m_amlDisplay->aml_get_native_resolution(&live, gui.nativeGui))
      for (size_t i = 0; i < reply.table.size(); ++i)
      {
        const RESOLUTION_INFO& res = reply.table[i];
        if (live.iWidth == res.iWidth && live.iHeight == res.iHeight &&
            live.iScreenWidth == res.iScreenWidth && live.iScreenHeight == res.iScreenHeight &&
            (live.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
            fabs(live.fRefreshRate - res.fRefreshRate) < FLT_EPSILON)
          reply.liveIndex = static_cast<int>(i);
      }
    reply.hasTable = true;
    slot = reply.liveIndex >= 0 ? reply.table[reply.liveIndex] : mainSlot;
    m_wHas2160 = Has2160(reply.table) || slot.iScreenHeight >= 2160;
  }
  const bool row2 = OUTPUT::RestoreRow2On();
  // 5.B3-8: a reconnect's caps step decides desktopMode, on the rows the Restore takes
  if (row2 && connected && reply.modes > 1)
  {
    std::vector<RESOLUTION_INFO> rows{slot};
    for (const RESOLUTION_INFO& res : reply.table)
      rows.push_back(CDisplaySettings::AddedResolutionInfo(res));
    UpdateDesktopMode(rows);
  }
  DISPLAYSTATE::PublishCaps(*m_amlDisplay, m_wHas2160, row2 ? &m_wDesktopMode : nullptr);

  // step 6: no commit at a loss (§1 step 4's plug-out commit is gone, fc30 C3)
  if (!connected)
  {
    reply.kind = reply.modes > 1 ? Kind::LOSS : Kind::LOSS_CAPSFAIL;
    return;
  }
  if (reply.modes <= 1)
  {
    // the span stays open for P5-9's timer
    CLog::Log(LOGWARNING,
              "CWinSystemAmlogic - HotplugEvent mode switch ignored while HDMI DRM connector is "
              "not ready ({:d} modes)",
              reply.modes);
    reply.kind = retry ? Kind::RETRY_NOOP : Kind::LOSS_CAPSFAIL;
    return;
  }

  // step 7: the Restore at §6's target on W's table (row 1, HotplugEvent's, under the skip; row 2
  // under B3-8): RES_DESKTOP's entry, then the probe's as CDisplaySettings stores them
  std::vector<RESOLUTION_INFO> rows{slot};
  for (const RESOLUTION_INFO& res : reply.table)
    rows.push_back(CDisplaySettings::AddedResolutionInfo(res));
  size_t target = 0;
  if (!row2)
  {
    const std::string preferred_mode = m_amlDisplay->aml_get_preferred_mode();
    if (!preferred_mode.empty())
    {
      for (size_t i = 0; i < rows.size(); ++i)
        if (StringUtils::EqualsNoCase(rows[i].strId, preferred_mode))
        {
          target = i;
          break;
        }
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic - HotplugEvent, preferred mode: {}, display mode: {}",
                preferred_mode, rows[target].strId);
    }
    else
      CLog::Log(LOGWARNING,
                "CWinSystemAmlogic - HotplugEvent, no preferred mode defined, use display mode: {}",
                rows[target].strId);
  }
  std::optional<size_t> videoRow;
  if (m_wVideoMode)
  {
    for (size_t i = 0; i < rows.size(); ++i)
      if (SameMode(rows[i], m_wVideoMode->strId, m_wVideoMode->refresh, m_wVideoMode->dwFlags))
      {
        target = i;
        videoRow = i;
        CLog::Log(LOGINFO, "CWinSystemAmlogic - HotplugEvent, restoring the video's mode {} @ {:.3f}",
                  rows[i].strId, rows[i].fRefreshRate);
        break;
      }
  }
  // 5.B3-8 (§6 row 2): the video's mode is HotplugEvent's record (D5), GC's video row at the
  // playback's first event (recorded once per playback) and kept until applied, so the committed
  // mode during playback is off's; with none (no player, no video, or its mode not on the table)
  // §4.7's no-player target
  if (row2 && !videoRow)
  {
    std::string why;
    target = NoPlayerTarget(rows, gui, why);
    if (m_wVideoMode)
    {
      // the one playback case where on differs from off: off keeps HotplugEvent's preferred rule,
      // whose boot vout string the table may lack, leaving RES_DESKTOP's entry (P10's kept slot,
      // with Y the film's mode)
      size_t offTarget = 0;
      const std::string preferred_mode = m_amlDisplay->aml_get_preferred_mode();
      for (size_t i = 0; i < rows.size() && !preferred_mode.empty(); ++i)
        if (StringUtils::EqualsNoCase(rows[i].strId, preferred_mode))
        {
          offTarget = i;
          break;
        }
      CLog::Log(LOGINFO,
                "HOTPLUG restore target {}: playing, the video's mode {} not in the table: row 2's "
                "target ({}) (off would target the {} {})",
                DISPLAYSTATE::FactsString(DISPLAYSTATE::FactsOf(rows[target])),
                DISPLAYSTATE::FactsString(*m_wVideoMode), why,
                offTarget == 0 ? "slot" : "preferred mode's row",
                DISPLAYSTATE::FactsString(DISPLAYSTATE::FactsOf(rows[offTarget])));
    }
    else
      CLog::Log(LOGINFO, "HOTPLUG restore target {}: {}",
                DISPLAYSTATE::FactsString(DISPLAYSTATE::FactsOf(rows[target])), why);
  }
  // the throw recovery: the mode the dropped Restore put on the wire, as main never adopted it
  bool recovered = false;
  if (recover)
  {
    const auto row = std::find_if(rows.begin(), rows.end(), [&recover](const RESOLUTION_INFO& res)
                                  { return SameMode(res, recover->strId, recover->refresh,
                                                    recover->dwFlags); });
    if (row != rows.end())
    {
      target = static_cast<size_t>(row - rows.begin());
      recovered = true;
    }
    CLog::Log(LOGINFO, "HOTPLUG retry target {} @ {:.3f}: {}", rows[target].strId,
              rows[target].fRefreshRate,
              row != rows.end() ? "the dropped Restore's committed mode"
                                : "the committed mode is not in the table, today's target");
  }
  RunRestoreOnW(reply, rows[target], reply.dvLed);
  reply.kind = retry ? Kind::RETRY_RESTORE : spanWasOpen ? Kind::RESTORE : Kind::REASSERT;
  // kept until applied: a display may come back with fewer modes first (a Restore off the
  // record leaves it HEAD's kept record, reset only by a matched Restore main applies, or a stop),
  // and main may drop this reply (its ApplyHotplugReply marks it applied). The retry's committed
  // mode consumes it, as the recovery set does, only when the dropped Restore was the record's
  // (at FRAC GC's row and the wire's can differ); a dropped Restore off the record keeps it
  if (m_wVideoMode)
  {
    const bool pending = m_wVideoModeReply != 0;
    reply.videoMode = (videoRow && target == *videoRow) || (recovered && pending);
    m_wVideoModeReply = reply.videoMode ? reply.replyId : 0;
  }
  // step 8: the unpark at the hold's end, then the reply
}

void CWinSystemAmlogic::UpdateDesktopMode(const std::vector<RESOLUTION_INFO>& rows)
{
  DISPLAYSTATE::ModeFacts session;
  {
    std::unique_lock lock(m_slotMutex);
    if (const auto vout = VoutRow(rows, m_wSessionVout, m_wSessionVoutFrac))
    {
      m_sessionDesktopMode = DISPLAYSTATE::FactsOf(rows[*vout]);
      CLog::Log(LOGINFO,
                "HOTPLUG session desktop mode {}: the boot vout mode {} (frac_rate_policy {}), in "
                "this table",
                DISPLAYSTATE::FactsString(m_sessionDesktopMode), m_wSessionVout,
                m_wSessionVoutFrac);
      m_wSessionVout.clear();
    }
    session = m_sessionDesktopMode;
  }
  // §4.7: the session's desktop mode when the new table has it (an empty one is absent), else
  // the PREFERRED mode, so a reduced EDID gives 1080p60 and a later full one the session's back
  if (FactsRow(rows, session))
  {
    m_wDesktopMode = session;
    m_wDesktopModeWhy = "the session's desktop mode";
  }
  else if (const auto preferred = PreferredRow(rows, m_amlDisplay->aml_get_drm_preferred_mode()))
  {
    m_wDesktopMode = DISPLAYSTATE::FactsOf(rows[*preferred]);
    m_wDesktopModeWhy = StringUtils::Format("PREFERRED, the session's desktop mode {} is not in "
                                            "the table",
                                            DISPLAYSTATE::FactsString(session));
  }
  else
  {
    // neither: kept, and the target below falls back to today's rule
    CLog::Log(LOGWARNING, "HOTPLUG desktopMode kept {}: neither the session's desktop mode {} nor "
                          "a PREFERRED mode is in the table",
              DISPLAYSTATE::FactsString(m_wDesktopMode), DISPLAYSTATE::FactsString(session));
    return;
  }
  CLog::Log(LOGINFO, "HOTPLUG desktopMode {}: {}", DISPLAYSTATE::FactsString(m_wDesktopMode),
            m_wDesktopModeWhy);
}

size_t CWinSystemAmlogic::NoPlayerTarget(const std::vector<RESOLUTION_INFO>& rows,
                                         const DISPLAYSTATE::GuiInputs& gui,
                                         std::string& why)
{
  // §4.7's no-player target: the GUI resolution's facts, "desktop" as desktopMode; else the
  // connector's PREFERRED mode (1080p60 on both boxes, R1)
  std::string missing;
  if (gui.guiResolution)
  {
    if (const auto row = FactsRow(rows, *gui.guiResolution))
    {
      why = "the GUI resolution";
      return *row;
    }
    missing = StringUtils::Format("the GUI resolution {} is not in the table",
                                  DISPLAYSTATE::FactsString(*gui.guiResolution));
  }
  else if (const auto row = FactsRow(rows, m_wDesktopMode))
  {
    why = StringUtils::Format("desktop, desktopMode ({})", m_wDesktopModeWhy);
    return *row;
  }
  else
    missing = StringUtils::Format("desktop, desktopMode {} is not in the table",
                                  DISPLAYSTATE::FactsString(m_wDesktopMode));
  if (const auto row = PreferredRow(rows, m_amlDisplay->aml_get_drm_preferred_mode()))
  {
    why = "PREFERRED, " + missing;
    return *row;
  }
  // no PREFERRED mode on the table (not seen on S6 or G12B): today's rule, the boot vout or the
  // DRM PREFERRED mode by its string, else RES_DESKTOP's entry
  size_t target = 0;
  const std::string preferred_mode = m_amlDisplay->aml_get_preferred_mode();
  for (size_t i = 0; i < rows.size() && !preferred_mode.empty(); ++i)
    if (StringUtils::EqualsNoCase(rows[i].strId, preferred_mode))
    {
      target = i;
      break;
    }
  if (rows[target].strId.empty())
    // r17a H2: an empty target would read "already set" on a dark CRTC and commit nothing
    CLog::Log(LOGERROR, "HOTPLUG restore target empty: {}, and no PREFERRED mode in the table",
              missing);
  else
    CLog::Log(LOGWARNING, "HOTPLUG restore: no PREFERRED mode in the table, today's target");
  why = "today's target, " + missing;
  return target;
}

void CWinSystemAmlogic::ArmHotplugRetry()
{
  const int firstMs = OUTPUT::TestHotplugRetryMs();
  m_wRetryDelay = std::chrono::milliseconds(firstMs > 0 ? firstMs : 1000);
  PostHotplugRetryTick(++m_wRetryGen, m_wRetryDelay);
}

void CWinSystemAmlogic::PostHotplugRetryTick(uint64_t gen, std::chrono::milliseconds after)
{
  if (!OUTPUT::COutputWorker::Get().PostAfter(OUTPUT::Lane::RESTORE, "hotplug retry", after,
                                              [this, gen] { HotplugRetryTick(gen); }))
    CLog::Log(LOGINFO, "HOTPLUG retry: W stopped, not retried");
}

void CWinSystemAmlogic::HotplugRetryTick(uint64_t gen)
{
  // HEAD's predicate (WSA MessagePump), on its live terms. HEAD evaluates it only on main, never
  // between a hotplug's take and its set, nor inside main's own set: so also not while a reply
  // is queued or being applied, nor while main's request is on W or being applied
  if (gen != m_wRetryGen || !m_lostOpen)
    return;
  bool replyQueued;
  {
    std::unique_lock lock(m_replyMutex);
    replyQueued = !m_replies.empty();
  }
  if (IsHotplugPending() || m_disconnectedSinceMs ||
      ((m_delayDispReset || m_resetPending) && IsPresentationReady()) || replyQueued ||
      m_applyingReply || m_mainRequests > 0 || m_guiInputsStale || m_lateReplies > 0)
  {
    PostHotplugRetryTick(gen, 50ms);
    return;
  }
  CLog::Log(LOGWARNING, "CWinSystemAmlogic: display back but still lost, retrying the mode set");
  const std::chrono::milliseconds wait = m_wRetryDelay;
  m_wRetryDelay = std::min(m_wRetryDelay * 2, std::chrono::milliseconds(30000));
  RunHotplugJob(true);
  if (gen == m_wRetryGen && m_lostOpen)
    PostHotplugRetryTick(gen, wait);
}

void CWinSystemAmlogic::PostHotplugRetry(const DISPLAYSTATE::ModeFacts& committed)
{
  if (!OUTPUT::COutputWorker::Get().PostIfRunning(OUTPUT::Lane::RESTORE, "hotplug retry",
                                                  [this, committed]
                                                  { RunHotplugJob(true, committed); }))
    CLog::Log(LOGINFO, "HOTPLUG retry: W stopped, not retried");
}

int CWinSystemAmlogic::FindResolution(const std::string& strId, float refresh, uint32_t flags)
{
  const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  for (size_t i = RES_DESKTOP; i < displaySettings.ResolutionInfoSize(); i++)
    if (SameMode(displaySettings.GetResolutionInfo(i), strId, refresh, flags))
      return static_cast<int>(i);
  return -1;
}

void CWinSystemAmlogic::ApplyHotplugTable(const SHotplugReply& reply, bool throwInside)
{
  // HotplugEvent's rebuild (ClearCustomResolutions, RefreshResolutions, ApplyCalibrations) from
  // W's probe
  CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  displaySettings.ClearCustomResolutions();
  if (throwInside)
    throw std::runtime_error("test throw inside the table step");
  for (size_t i = 0; i < reply.table.size(); ++i)
  {
    RESOLUTION_INFO res = reply.table[i];
    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz", res.iWidth,
              res.iHeight, res.iScreenWidth, res.iScreenHeight,
              res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", res.fRefreshRate);
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
    displaySettings.AddResolutionInfo(res);
    if (static_cast<int>(i) == reply.liveIndex)
      displaySettings.GetResolutionInfo(RES_DESKTOP) = res;
  }
  displaySettings.ApplyCalibrations();
  PublishMainSlot();
}

void CWinSystemAmlogic::ApplyHotplugCaps(const SHotplugReply& reply)
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  SetDvVisibility(reply.deviceDv, reply.sinkDv);
  // the value W's Restore already used; only the settings mirror listens (no set starts)
  if (reply.dvLed && settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED) != *reply.dvLed)
    settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED, *reply.dvLed);
  for (const char* id :
       {CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR, CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR})
    if (const auto setting = settings->GetSetting(id))
      setting->SetVisible(reply.hdrDisplay);
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

    if (isHotplug && winSystem->m_wHotplug)
      winSystem->ReceiveHotplug(true);
    else if (isHotplug)
    {
      // the caps may change from here: HotplugEvent publishes them on every path
      DISPLAYSTATE::CapsWriteBegins();
      winSystem->m_hotplugPending.store(true);
      winSystem->PostDetectShadow();
    }
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

  m_lostAtDisconnect = aml_presentation_validated() &&
                       !XFILE::CFile::Exists("special://profile/lostatdisconnect_off");

  // Before anything touches the display: undo a DV core left engaged by a
  // previous run that died before its disc-session teardown. Nothing else can -
  // the kernel's own self-heal hooks enable_amdv(0), which a stale session never
  // reaches.
  aml_dv_recover_stale_disc_session();

  // 5.B3-2, every session, whatever the switches: the kernel restores the link at a plug-in
  // again (hpd_restore Y, its default), so no session inherits an N an earlier one left (written
  // by hand, or by a session that lost its DRM master). Touches only the parameter
  // (the sysfs calls cannot abort the init: HEAD has no throw here)
  try
  {
    if (aml_hpd_restore_present())
    {
      const auto text = [](std::optional<bool> value)
      { return value ? (*value ? "Y" : "N") : "unreadable"; };
      const std::optional<bool> before = aml_hpd_restore();
      aml_set_hpd_restore(true);
      const std::optional<bool> after = aml_hpd_restore();
      if (after && *after)
        CLog::Log(LOGINFO, "hpdRestore Y (init, was {})", text(before));
      else
        CLog::Log(LOGERROR, "hpdRestore not Y after the init's Y write (reads {}, was {})",
                  text(after), text(before));
    }
    else
      CLog::Log(LOGINFO, "hpdRestore absent");
  }
  catch (const std::exception& e)
  {
    CLog::Log(LOGERROR, "hpdRestore init write failed: {}", e.what());
  }

  RefreshDisplayCapabilities();

  if (OUTPUT::CSettingsMirror::Get().GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
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
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MINLUM,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER,
                                   CSettings::SETTING_COREELEC_AMLOGIC_DV_GUI_SINK_PEAK});
  }
  // D10: applied without a restart (outputworker_off: at init and hotplug only, as before)
  if (!OUTPUT::COutputWorker::SwitchedOff())
    CServiceBroker::GetSettingsComponent()->GetSettings()->GetSettingsManager()->RegisterCallback(
        this, {CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR,
               CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR});
  aml_dv_apply_graphics_from_sink();

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
  LogStartupLink();

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
  DISPLAYSTATE::Tick();
  DISPLAYSTATE::LogCounters();
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

  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR)
  {
    OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::SETTINGS, "hdr conversion",
                                      [] { aml_hdr_conversion_refresh("setting"); });
    return;
  }

  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_GUI_SINK_PEAK)
  {
    OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::SETTINGS, "gui sink peak",
                                      [] { aml_dv_apply_graphics_from_sink(); });
    return;
  }

  // CMv4.0 append mode / Smart threshold: publish to the codec, which re-pushes
  // them to its bitstream converter on the next packet. Unconditional - the
  // generation counter is inert when nothing is decoding, and a stream opening
  // later reads the settings directly anyway. display.maxnits feeds both this
  // and the VSVDB inject below, so bumping it here keeps the two in step when the
  // slider moves mid-playback.
  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER ||
      settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS)
    aml_dv_cmv40_settings_changed();

  if (settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE &&
      settingId != CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MINLUM)
    return;

  // Only re-apply live while a DV stream is decoding (dolby_vision_enable == Y);
  // otherwise the new value is picked up at the next decoder open. aml_dv_apply_vsvdb
  // re-injects the updated block and re-latches it via an EDID re-parse.
  OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::SETTINGS, "vsvdb setting", [] {
    CSysfsPath dv_enable{"/sys/module/aml_media/parameters/dolby_vision_enable"};
    if (dv_enable.Exists() &&
        StringUtils::EqualsNoCase(dv_enable.Get<std::string>().value_or("N"), "Y"))
    {
      // Keyed on the mode the sink receives: the pending VS10 mode stays BYPASS on
      // the converted-to-DV paths and is not updated by a live VS10 switch.
      aml_dv_apply_target_overrides(aml_dv_get_output_mode());
      aml_dv_apply_vsvdb();
    }
  });
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  bool ret;
  // this set's only: cleared before anything can throw
  const bool fracReclock = std::exchange(m_frac_reclock, false);

  SetPresentationReady(false);
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  int delay = OUTPUT::CSettingsMirror::Get().GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  m_windowLostGen = LostWalk();
  // 5.B3-7a (O3): an armed poll resets for this set too (today's, at delay 0 as well)
  if (m_delayDispReset)
    m_pollLostGen = m_windowLostGen;

  // the commit job (5.B1-7): W sets the mode; main keeps the settings reads, the display
  // resources and the GL work
  {
    auto committed = std::make_shared<bool>(false);
    CAMLDisplay* display = m_amlDisplay.get();
    // 5.B2-3: the commit is counted and bumps the timing inputs before the unpark; the boot set
    // stays today's (before W, and before display state's init)
    const bool commitTiming = m_outputWorkerStarted && DISPLAYSTATE::CommitTimingOn();
    // 5.B2-4: C's cleared buffer for the epoch the surface detach started
    CPresentationCoordinator* const coordinator =
        m_outputWorkerStarted && SurfaceEpochOn() ? CPresentationCoordinator::Get() : nullptr;
    const uint64_t uiEpoch = coordinator ? coordinator->UiEpoch() : 0;
    OUTPUT::COutputWorker::Get().Run(
        m_hotplug_mode_switch ? OUTPUT::Lane::RESTORE : OUTPUT::Lane::SETTINGS, "mode set",
        [committed, display, res, name = m_framebuffer_name, stereo = m_stereo_mode,
         force = m_force_mode_switch, hotplug = m_hotplug_mode_switch, frac = fracReclock,
         windowed = !fullScreen, commitTiming, coordinator, uiEpoch,
         shadowBefore = std::exchange(m_shadowBefore, nullptr),
         shadowAfter = std::exchange(m_shadowAfter, nullptr)]
        {
          if (shadowBefore)
            shadowBefore();
          *committed = display->set_native_resolution(res, name, stereo, force, hotplug, frac);
          if (shadowAfter)
            shadowAfter(*committed, display->aml_set_not_connected());
          OUTPUT::TestStopAt("4");
          if (*committed)
          {
            DISPLAYSTATE::PublishMode(*display, res, windowed);
            if (commitTiming)
            {
              DISPLAYSTATE::NoteCommit();
              aml_timing_inputs_bump();
            }
            if (coordinator)
              coordinator->PrepareClearedUi(
                  res.iWidth, res.iHeight, uiEpoch, [display](uint32_t fb, int& w, int& h)
                  { return display->BuildFlipRequestLocked(fb, w, h); });
          }
          OUTPUT::TestStopAt("6");
        });
    ret = *committed;
    if (ret && !commitTiming)
      DISPLAYSTATE::NoteCommit();
  }
  if (ret)
    m_bWindowCreated = true;

  // W starts after the boot mode set, which stays inline (ruling 4)
  if (!m_outputWorkerStarted)
  {
    m_outputWorkerStarted = true;
    aml_presenter_set_test_cstall(OUTPUT::TestCStallMs(), OUTPUT::TestCStallNth());
    aml_displaystate_init();
    OUTPUT::COutputWorker::Get().Start();
    // 5.B2-5b: from here on W runs main's mode sets
    OUTPUT::NoteModeSetsOnW(OUTPUT::COutputWorker::Get().Active() && ModeSetsOnWSwitch());
    if (OUTPUT::ModeSetsOnW())
    {
      LostFirstSwitch();
      DISPLAYSTATE::DvModePublishOn();
      OUTPUT::SettleOnW();
    }
    // 5.B3: the hotplug switches, read once (log only until W takes the hotplug)
    try
    {
      OUTPUT::LatchHotplugWorker();
      OUTPUT::DetectShadowOn();
      // the B3-7a gate hook, read here so the first SETTLE reset makes no file probe
      if (OUTPUT::OrderingOn())
        OUTPUT::TestHotplugSettleGapMs();
      // 5.B3-7b's switch line; the move itself starts at main's first pump after this set
      OUTPUT::HotplugOnW();
      // 5.B3-8's, read here so no settings thread's GuiResolution publish reads the file
      OUTPUT::RestoreRow2On();
      OUTPUT::DarkRoutingOn();
      OUTPUT::LinkRequestOn();
      OUTPUT::RefusalConfirmOn();
      OUTPUT::TestGraphicsRefuseAt();
    }
    catch (const std::exception& e)
    {
      // sysfs and profile-file reads only; a throw leaves hotplugworker off (HEAD's path)
      CLog::Log(LOGERROR, "HOTPLUGWORKER switch read failed, off: {}", e.what());
    }
    if (aml_displaystate_on())
      OUTPUT::COutputWorker::Get().Run(
          OUTPUT::Lane::RESTORE, "display state seed",
          [display = m_amlDisplay.get(), res, ret, windowed = !fullScreen]
          { DISPLAYSTATE::PublishSeed(*display, ret ? &res : nullptr, windowed); });
    // GuiInputs: the stereo mode main set before the switch was read
    DISPLAYSTATE::PublishGuiInputs(
        {CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode()});
    // 5.B3-3: W's hotplug inputs, DISABLEGUISCALING's live value
    if (OUTPUT::HotplugWorkerOn())
      DISPLAYSTATE::PublishGuiInputsAtStart(
          CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
              CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING));
    OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::RESTORE, "vsvdb refill",
                                      [] { aml_display_vsvdb_refill(); });
    // 5.B3-4's gate hook (hotplugworker_reinit), a no-op without its file
    StartTestReinit();
  }

  // the delay is for the display to lock onto the new mode, so count it from
  // when the mode is set
  if (delay > 0 && m_delayDispReset)
  {
    m_resetDelay = std::chrono::milliseconds(static_cast<unsigned int>(delay * 100));
    m_dispResetTimer.Set(m_resetDelay);
    m_modeSetAt = std::chrono::steady_clock::now();
    m_sinkLockedReads = 0;
    m_sinkLockPoll.Set(0ms);
  }

  m_force_mode_switch = false;
  m_hotplug_mode_switch = false;
  return ret;
}

namespace
{
// hotplugworker_reinit (B3-4's gate): W parks C, runs today's re-init (the one HotplugEvent
// runs on main) and publishes the caps, every period until the deadline
void PostTestReinit(CAMLDisplay* display,
                    int periodMs,
                    std::chrono::steady_clock::time_point until,
                    uint64_t n)
{
  OUTPUT::COutputWorker::Get().PostAfter(
      OUTPUT::Lane::SETTINGS, "test re-init", std::chrono::milliseconds(periodMs),
      [display, periodMs, until, n]
      {
        {
          CAmlPresenterHold hold("test re-init");
          DISPLAYSTATE::CapsWriteBegins();
          try
          {
            display->aml_init_drmDevice();
          }
          catch (const std::exception& e)
          {
            CLog::Log(LOGERROR, "HOTPLUGWORKER: test re-init {} failed: {}", n, e.what());
          }
          // 5.B3-7b (H4): the table is main's, unchanged by this re-init
          const auto state = DISPLAYSTATE::Get();
          DISPLAYSTATE::PublishCaps(*display, OUTPUT::HotplugOnW() && state
                                                  ? std::optional<bool>(state->has2160)
                                                  : std::nullopt);
        }
        if (std::chrono::steady_clock::now() < until)
          PostTestReinit(display, periodMs, until, n + 1);
        else
          CLog::Log(LOGWARNING, "HOTPLUGWORKER: test re-init done, {} runs", n);
      });
}
} // namespace

void CWinSystemAmlogic::StartTestReinit()
{
  const auto [periodMs, seconds] = OUTPUT::TestHotplugReinit();
  // a W re-init races main's readers only under hotplugworker; off, they read m_crtc live
  if (periodMs <= 0 || !OUTPUT::HotplugWorkerOn())
    return;
  CLog::Log(LOGWARNING, "HOTPLUGWORKER: test re-init on W started");
  PostTestReinit(m_amlDisplay.get(), periodMs,
                 std::chrono::steady_clock::now() + std::chrono::seconds(seconds), 1);
}

uint64_t CWinSystemAmlogic::LostWalk()
{
  uint64_t lostGen;
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    LedgerBegin("mode set");
    // 5.B3-7a (O3): this walk's generation; a reset armed for an older one is dropped
    if (OUTPUT::OrderingOn())
      CLog::Log(LOGINFO, "LEDGER set={} lost generation {} (mode set)", m_ledgerSet.load(),
                ++m_lostGen);
    lostGen = m_lostGen;
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
      LedgerLost(*i);
    }
  }
  OUTPUT::ArmDemuxBlock();
  OUTPUT::TestStopAt("2");
  return lostGen;
}

void CWinSystemAmlogic::ArmDelayedReset(int delay, std::chrono::steady_clock::time_point modeSetAt)
{
  // WSA::CreateNewWindow's arming, on main from W's reply: counted from W's commit
  m_delayDispReset = true;
  m_resetDelay = std::chrono::milliseconds(static_cast<unsigned int>(delay * 100));
  const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - modeSetAt);
  m_dispResetTimer.Set(std::max(m_resetDelay - since, std::chrono::milliseconds(0)));
  m_modeSetAt = modeSetAt;
  m_sinkLockedReads = 0;
  m_sinkLockPoll.Set(0ms);
}

bool CWinSystemAmlogic::ModeSetsOnWSwitch()
{
  static const bool on = []
  {
    if (!OUTPUT::OutputDriveOn() || OUTPUT::OutputDriveSkip("B2-5b"))
      return false;
    // S1' needs B2-4's epoch and C's buffer; the timing after W's commit is B2-3's
    if (!SurfaceEpochOn() || !DISPLAYSTATE::CommitTimingOn())
    {
      CLog::Log(LOGWARNING, "OUTPUTDRIVE B2-5b off: it needs B2-3 and B2-4 (skipped)");
      return false;
    }
    CLog::Log(LOGINFO, "OUTPUTDRIVE B2-5b: W runs main's mode sets");
    return true;
  }();
  return on;
}

bool CWinSystemAmlogic::LostFirstSwitch()
{
  static const bool on = []
  {
    if (OUTPUT::OutputDriveSkip("B2-5c") || OUTPUT::TestOrderToday())
      return false;
    CLog::Log(LOGINFO, "OUTPUTDRIVE B2-5c: W's lost walk right after the park");
    return true;
  }();
  return on;
}

bool CWinSystemAmlogic::SurfaceEpochOn()
{
  static const bool on = []
  {
    const bool result = OUTPUT::OutputDriveOn() && !OUTPUT::OutputDriveSkip("B2-4");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-4: surface epoch and C's cleared UI buffer");
    return result;
  }();
  return on;
}

// The display reset waits for the later of the user's refresh-change delay and
// the sink the box drives reporting lock over SCDC. Without HDCP a source cannot
// see a display behind a repeater; the delay is the user's knowledge of their
// chain, and the lock covers a display slower than it. Two locked reads 50 ms
// apart, the first no sooner than 100 ms after the mode set so a lock left over
// from the old signal is not taken. An unreadable lock leaves the delay alone.
bool CWinSystemAmlogic::SinkLockedAfterModeSet()
{
  if (m_sinkLockedReads >= 2)
    return true;
  if (std::chrono::steady_clock::now() - m_modeSetAt < 100ms || !m_sinkLockPoll.IsTimePast())
    return false;
  m_sinkLockPoll.Set(50ms);
  const int locked = aml_hdmi_sink_locked();
  if (locked < 0)
    m_sinkLockedReads = 2;
  else if (locked == 1)
    ++m_sinkLockedReads;
  else
    m_sinkLockedReads = 0;
  return m_sinkLockedReads >= 2;
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

  DISPLAYSTATE::CapsWriteBegins();

  if (!m_amlDisplay->aml_probe_resolutions(resolutions) || resolutions.empty())
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);

  // get all resolutions supported by connected device
  if (m_amlDisplay->aml_get_native_resolution(&curDisplay))
    resDesktop = curDisplay;
  // 5.B3-7b (P14): a disconnected read ("dummy_l") keeps the last real RES_DESKTOP mode, taken
  // from this probe (its sizes follow DISABLEGUISCALING; a GUI-scaling change cleared the table)
  if (m_wHotplug && StringUtils::EqualsNoCase(curDisplay.strId, "dummy_l"))
  {
    RESOLUTION_INFO slot;
    {
      std::unique_lock lock(m_slotMutex);
      slot = m_mainSlot;
    }
    if (RealSlot(slot))
    {
      const auto match = std::find_if(
          resolutions.begin(), resolutions.end(),
          [&slot](const RESOLUTION_INFO& res)
          {
            return StringUtils::EqualsNoCase(res.strId, slot.strId) &&
                   res.iScreenWidth == slot.iScreenWidth &&
                   res.iScreenHeight == slot.iScreenHeight &&
                   (res.dwFlags & D3DPRESENTFLAG_MODEMASK) ==
                       (slot.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
                   fabs(res.fRefreshRate - slot.fRefreshRate) < FLT_EPSILON;
          });
      if (match != resolutions.end())
      {
        CLog::Log(LOGINFO, "HOTPLUG slot kept {}: the live read is dummy_l (disconnected)",
                  slot.strId);
        resDesktop = *match;
      }
      else
        CLog::Log(LOGWARNING,
                  "HOTPLUG slot {} not in the probe: the live read dummy_l takes RES_DESKTOP",
                  slot.strId);
    }
  }

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
  if (m_wHotplug)
    PublishMainSlot();
  PublishDisplayCaps();
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RefreshResolutions();
  aml_timing_inputs_bump();
}

int CWinSystemAmlogic::DvLedClamp(int value, int dvCap)
{
  // Only ever move BETWEEN the two real modes. A sink that advertises neither
  // has nothing to switch to: the previous -1 sentinel was outside
  // AML_DISPLAY_DV_LED's range, and because dv_led_modes is an options FILLER
  // rather than a static list, CSettingInt::CheckValidity() does not validate
  // it - so the -1 was stored and persisted as a spinner value no option
  // matches. Every consumer tests `== AML_DV_PLAYER_LED`, so leaving the value
  // untouched behaves identically for the DV pipeline.
  if (value == AML_DV_TV_LED && !(dvCap & DV_RGB_444_8BIT) && (dvCap & LL_YCbCr_422_12BIT))
    return AML_DV_PLAYER_LED;
  if (value == AML_DV_PLAYER_LED && !(dvCap & LL_YCbCr_422_12BIT) && (dvCap & DV_RGB_444_8BIT))
    return AML_DV_TV_LED;
  return value;
}

void CWinSystemAmlogic::SetDvVisibility(bool device_dv, bool sink_dv)
{
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

  // Retired: dovi.ko never reads the DM target min, so this setting cannot affect
  // the picture. Kept defined so stored values load; "Black level (VSVDB)" is the
  // working control for player-led DV.
  setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_TARGET_MINLUM);
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
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MINLUM,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_MODE,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_OSD_UNMASK,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_GUI_SINK_PEAK,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT,
                           CSettings::SETTING_COREELEC_AMLOGIC_DV_NONDV_STOCKCONVERT})
  {
    setting = settings->GetSetting(dvId);
    if (setting)
      setting->SetVisible(device_dv);
  }
}

void CWinSystemAmlogic::RefreshDisplayCapabilities()
{
  DISPLAYSTATE::CapsWriteBegins();
  m_amlDisplay->aml_refresh_display_caps();

  const bool device_dv = aml_support_dolby_vision();
  const bool sink_dv = device_dv && m_amlDisplay->aml_display_support_dv();

  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  SetDvVisibility(device_dv, sink_dv);

  // Clamp the DV output mode to something this sink actually advertises. Sink
  // dependent, so it re-runs on a display change rather than only at boot.
  if (sink_dv)
  {
    const int dv_cap = m_amlDisplay->aml_get_drmProperty("dv_cap", DRM_MODE_OBJECT_CONNECTOR);
    const int old_value = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED);
    const int new_value = DvLedClamp(old_value, dv_cap);

    if (new_value != old_value)
      settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_LED, new_value);
  }

  if (IsHDRDisplay())
  {
    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR)
        ->SetVisible(true);

    CServiceBroker::GetSettingsComponent()
        ->GetSettings()
        ->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR)
        ->SetVisible(true);

    if (!OUTPUT::COutputWorker::SwitchedOff())
    {
      // inline at boot (W not started yet)
      OUTPUT::COutputWorker::Get().Run(OUTPUT::Lane::RESTORE, "hdr conversion",
                                       [] { aml_hdr_conversion_refresh("display caps"); });
    }
    else
    {
      int sdr2hdr = OUTPUT::CSettingsMirror::Get().GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
      if (sdr2hdr)
      {
        CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting sdr2hdr mode to {:d}", __FUNCTION__, sdr2hdr);
        CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", sdr2hdr);
        CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 0);
        CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 0);
      }

      int hdr2sdr = OUTPUT::CSettingsMirror::Get().GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
      if (hdr2sdr)
      {
        CLog::Log(LOGDEBUG, "CWinSystemAmlogic::{} -- setting hdr2sdr mode to {:d}", __FUNCTION__, hdr2sdr);
        CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", hdr2sdr);
      }
    }
  }
  else
  {
    auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
    if (setting)
      setting->SetVisible(false);

    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
    if (setting)
      setting->SetVisible(false);
  }
  PublishDisplayCaps();
}

void CWinSystemAmlogic::PublishDisplayCaps()
{
  if (!aml_displaystate_on())
    return;
  CAMLDisplay* display = m_amlDisplay.get();
  // 5.B3-7b (H4): main reads its own table, W does not
  std::optional<bool> has2160;
  if (m_wHotplug)
  {
    const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
    has2160 = false;
    for (size_t i = RES_DESKTOP; i < displaySettings.ResolutionInfoSize() && !*has2160; ++i)
      has2160 = displaySettings.GetResolutionInfo(i).iScreenHeight >= 2160;
  }
  OUTPUT::COutputWorker::Get().Run(OUTPUT::Lane::RESTORE, "display state caps",
                                   [display, has2160]
                                   { DISPLAYSTATE::PublishCaps(*display, has2160); });
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  MAINCOUPLING_ACCESSOR("WinSystem IsHDRDisplay");
  CHDRCapabilities caps = m_amlDisplay->GetHDRCaps();
  return (caps.SupportsHDR10() | caps.SupportsHDR10Plus() | caps.SupportsHLG() |
         (caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE));
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  MAINCOUPLING_ACCESSOR("WinSystem GetDisplayHDRCapabilities");
  return m_amlDisplay->GetHDRCaps();
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const int guiSdrPeak = OUTPUT::CSettingsMirror::Get().GetInt(CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE);

  return ((0.7f * guiSdrPeak + 30.0f) / 100.0f);
}

HDR_STATUS CWinSystemAmlogic::GetOSHDRStatus()
{
  MAINCOUPLING_ACCESSOR("WinSystem GetOSHDRStatus");
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

uint32_t CWinSystemAmlogic::GetDRMCrtcId() const
{
  if (const auto state = DISPLAYSTATE::LinkState())
    return state->crtcId;
  return m_amlDisplay ? m_amlDisplay->aml_get_Device_crtc_id() : 0;
}

bool CWinSystemAmlogic::GetDRMConnected() const
{
  if (const auto state = DISPLAYSTATE::LinkState())
    return state->connected;
  return m_amlDisplay && m_amlDisplay->aml_get_display_connected();
}

bool CWinSystemAmlogic::DrmVsync()
{
  if (const auto state = DISPLAYSTATE::LinkState())
    return m_amlDisplay->aml_drmDevice_vsync(state->connected);
  return m_amlDisplay->aml_drmDevice_vsync();
}

float CWinSystemAmlogic::GetFrameLatencyAdjustment()
{
  MAINCOUPLING_ACCESSOR("WinSystem GetFrameLatencyAdjustment");
  // Milliseconds since the last hardware vblank. The render manager subtracts
  // it from the display latency so a frame-phase sample is referenced to the
  // vblank, not to whenever the render loop reached it.
  int fd = GetDRMDeviceFd();
  // 5.B3-4 (C's tick, V): under hotplugworker the live id comes from its lock-free mirror, not
  // m_crtc, which main's re-init frees; DrmIds then picks as HEAD does (the published ids when
  // connected and no caps write is pending, else the live one: a reconnect's re-init gives the
  // new CRTC before its caps publish, as HEAD's live read does)
  uint32_t crtc = DISPLAYSTATE::LinkState() ? m_amlDisplay->aml_get_Device_crtc_id_live()
                                            : GetDRMCrtcId();
  DISPLAYSTATE::DrmIds(fd, crtc);
  if (fd < 0 || crtc == 0)
    return 0.0f;

  uint64_t sequence = 0;
  uint64_t vblankNs = 0;
  if (drmCrtcGetSequence(fd, crtc, &sequence, &vblankNs) != 0)
    return 0.0f;

  struct timespec now = {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const int64_t sinceNs =
      static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec - static_cast<int64_t>(vblankNs);
  // a stale sequence (CRTC off, mid mode set) is not a phase
  if (sinceNs < 0 || sinceNs > 100000000)
    return 0.0f;
  return static_cast<float>(sinceNs) / 1000000.0f;
}
