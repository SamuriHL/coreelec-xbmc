/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SettingsMirror.h"

#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "settings/lib/SettingsManager.h"
#include "utils/log.h"

namespace OUTPUT
{
namespace
{
// not DISABLEGUISCALING: CDisplaySettings acts on it inside its own OnSettingChanging, which can
// run before the mirror's
const char* const MIRRORED[] = {
    CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_GUI_SINK_PEAK,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_LED,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_NONDV_STOCKCONVERT,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10_OSD_BRIGHTNESS,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MINLUM,
    CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV,
    CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV,
    CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR,
    CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR,
    CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION,
    CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE,
    "videoscreen.delayrefreshchange",
};

int ValueOf(const CSetting& setting)
{
  if (setting.GetType() == SettingType::Boolean)
    return static_cast<const CSettingBool&>(setting).GetValue() ? 1 : 0;
  if (setting.GetType() == SettingType::Integer)
    return static_cast<const CSettingInt&>(setting).GetValue();
  return 0;
}
} // namespace

CSettingsMirror& CSettingsMirror::Get()
{
  static CSettingsMirror mirror;
  return mirror;
}

SettingsContainer CSettingsMirror::Load()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  SettingsContainer ids;
  std::unique_lock lock(m_mutex);
  for (const char* id : MIRRORED)
  {
    const auto setting = settings->GetSetting(id);
    if (!setting)
    {
      CLog::Log(LOGWARNING, "OUTPUT: setting {} not found, not mirrored", id);
      continue;
    }
    m_values[id] = ValueOf(*setting);
    ids.insert(id);
  }
  return ids;
}

void CSettingsMirror::Start()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  {
    std::unique_lock lock(m_mutex);
    if (m_started)
      return;
  }
  const SettingsContainer ids = Load();
  {
    std::unique_lock lock(m_mutex);
    m_check = XFILE::CFile::Exists("special://profile/settingsmirror_check");
    m_started = true;
  }
  settings->RegisterCallback(this, ids);
  // a profile load sets every value with the callbacks off
  settings->GetSettingsManager()->RegisterSettingsHandler(this);
  CLog::Log(LOGINFO, "OUTPUT: {} settings mirrored{}", ids.size(),
            m_check ? ", every read checked against CSettings (settingsmirror_check)" : "");
}

void CSettingsMirror::Stop()
{
  {
    std::unique_lock lock(m_mutex);
    if (!m_started)
      return;
    m_started = false;
  }
  if (const auto component = CServiceBroker::GetSettingsComponent())
    if (const auto settings = component->GetSettings())
    {
      settings->UnregisterCallback(this);
      settings->GetSettingsManager()->UnregisterSettingsHandler(this);
    }
}

void CSettingsMirror::OnSettingsLoaded()
{
  {
    std::unique_lock lock(m_mutex);
    if (!m_started)
      return;
  }
  Load();
  CLog::Log(LOGINFO, "OUTPUT: settings mirror reloaded");
}

bool CSettingsMirror::OnSettingChanging(const std::shared_ptr<const CSetting>& setting)
{
  if (setting)
    Store(setting);
  return true;
}

void CSettingsMirror::Store(const std::shared_ptr<const CSetting>& setting)
{
  const int value = ValueOf(*setting);
  std::unique_lock lock(m_mutex);
  m_values[setting->GetId()] = value;
}

int CSettingsMirror::GetInt(const std::string& id) const
{
  int value = 0;
  bool mirrored = false;
  bool check = false;
  {
    std::unique_lock lock(m_mutex);
    const auto it = m_values.find(id);
    if (m_started && it != m_values.end())
    {
      value = it->second;
      mirrored = true;
      check = m_check;
    }
  }
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  if (!mirrored)
  {
    // before Start, or a setting not in the list: today's read
    const auto setting = settings ? settings->GetSetting(id) : nullptr;
    return setting ? ValueOf(*setting) : 0;
  }
  if (check && settings)
  {
    const auto setting = settings->GetSetting(id);
    const int actual = setting ? ValueOf(*setting) : 0;
    if (actual != value)
      CLog::Log(LOGWARNING, "OUTPUT: settings mirror DIFF {}: mirror {} CSettings {}", id, value,
                actual);
  }
  return value;
}

bool CSettingsMirror::GetBool(const std::string& id) const
{
  return GetInt(id) != 0;
}

} // namespace OUTPUT
