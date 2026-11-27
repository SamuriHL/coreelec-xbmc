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
#include "windowing/amlogic/GraphicsRegime.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/ThreadRole.h"

#include <algorithm>
#include <optional>

namespace OUTPUT
{
namespace
{
// CScopedValue's override, this thread only
thread_local bool t_override = false;
thread_local std::string t_overrideId;
thread_local int t_overrideValue = 0;

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
    // read on the video side (MC-42)
    CSettings::SETTING_BLURAY_PLAYERREGION,
    CSettings::SETTING_BLURAY_SIMULATE3DDISPLAY,
    // 5.V3-3: the geometry's two settings, for C
    CSettings::SETTING_VIDEOPLAYER_ERRORINASPECT,
    CSettings::SETTING_VIDEOPLAYER_STRETCH43,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_AUTO_TRIGGER,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_MODE,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_OSD_UNMASK,
    CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK,
    CSettings::SETTING_VIDEOPLAYER_USEAMCODEC,
    CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264,
    CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2,
    CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG4,
    CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK,
    // 5.E2-5: DECIDE's mode rules on P (mayChooseMode, the 3D ASK predicate)
    CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE,
    CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE,
    // 5.E2-10 9d: the 3D target's other inputs (StereoscopicsManager's)
    CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE,
    CSettings::SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE,
    CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP,
    // 5H-G1 Q-C (user 2026-11-26): every other setting P, V and A read
    // P: stream selection (OpenDefaultStreams), subtitles, captions, the packet loop
    CSettings::SETTING_LOCALE_SUBTITLELANGUAGE,
    CSettings::SETTING_LOCALE_AUDIOLANGUAGE,
    CSettings::SETTING_ACCESSIBILITY_SUBHEARING,
    CSettings::SETTING_ACCESSIBILITY_AUDIOHEARING,
    CSettings::SETTING_ACCESSIBILITY_AUDIOVISUAL,
    CSettings::SETTING_VIDEOPLAYER_PREFERDEFAULTFLAG,
    CSettings::SETTING_SUBTITLES_HIDESAMEAUDIOLANGUAGE,
    CSettings::SETTING_SUBTITLES_PARSECAPTIONS,
    CSettings::SETTING_SUBTITLES_CAPTIONSIMPAIRED,
    CSettings::SETTING_SUBTITLES_CUSTOMPATH,
    CSettings::SETTING_SUBTITLES_OVERRIDEFONTS,
    CSettings::SETTING_SUBTITLES_OVERRIDESTYLES,
    CSettings::SETTING_VIDEOPLAYER_TELETEXTENABLED,
    CSettings::SETTING_COREELEC_DETECT_BROKEN_FILES,
    // P: demuxer and input streams
    CSettings::SETTING_VIDEOPLAYER_MAX_SMOOTH_FF_SPEED,
    CSettings::SETTING_COREELEC_AMLOGIC_DV_DUAL_PRIORITY,
    CSettings::SETTING_NETWORK_BANDWIDTH,
    CSettings::SETTING_DVDS_PLAYERREGION,
    CSettings::SETTING_DVDS_AUTOMENU,
    "pvrplayback.enableradiords",
    "pvrplayback.trafficadvisory",
    "pvrplayback.trafficadvisoryvolume",
    // P: file opens and the cache thread
    CSettings::SETTING_FILECACHE_BUFFERMODE,
    CSettings::SETTING_FILECACHE_MEMORYSIZE,
    CSettings::SETTING_FILECACHE_CHUNKSIZE,
    CSettings::SETTING_FILECACHE_READFACTOR,
    // CDirectory::GetDirectory's hidden-file filter (P lists a disc or a folder at a start)
    CSettings::SETTING_FILELISTS_SHOWHIDDEN,
    // P/A: the passthrough codec's dialnorm
    CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM,
    CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM,
    CSettings::SETTING_COREELEC_AUDIO_TRUEHD_DIALNORM,
    CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM,
    // P/A and the AE thread: ActiveAE's LoadSettings, SupportsRaw/SupportsFormat, the resampler
    CSettings::SETTING_AUDIOOUTPUT_AUDIODEVICE,
    CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGHDEVICE,
    CSettings::SETTING_AUDIOOUTPUT_CONFIG,
    CSettings::SETTING_AUDIOOUTPUT_CHANNELS,
    CSettings::SETTING_AUDIOOUTPUT_SAMPLERATE,
    CSettings::SETTING_AUDIOOUTPUT_STEREOUPMIX,
    CSettings::SETTING_AUDIOOUTPUT_MAINTAINORIGINALVOLUME,
    CSettings::SETTING_AUDIOOUTPUT_GUISOUNDMODE,
    CSettings::SETTING_AUDIOOUTPUT_PASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_AC3PASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_AC3TRANSCODE,
    CSettings::SETTING_AUDIOOUTPUT_EAC3PASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_TRUEHDPASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_DTSPASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_DTSHDPASSTHROUGH,
    CSettings::SETTING_AUDIOOUTPUT_DTSHDCOREFALLBACK,
    CSettings::SETTING_AUDIOOUTPUT_PROCESSQUALITY,
    CSettings::SETTING_AUDIOOUTPUT_ATEMPOTHRESHOLD,
    CSettings::SETTING_AUDIOOUTPUT_STREAMNOISE,
    CSettings::SETTING_AUDIOOUTPUT_STREAMSILENCE,
    CSettings::SETTING_AUDIOOUTPUT_MIXSUBLEVEL,
    CSettings::SETTING_AUDIOOUTPUT_LOWLATENCY,
    "audiooutput.boostcenter",
};

int IntOf(const CSetting& setting)
{
  if (setting.GetType() == SettingType::Boolean)
    return static_cast<const CSettingBool&>(setting).GetValue() ? 1 : 0;
  if (setting.GetType() == SettingType::Integer)
    return static_cast<const CSettingInt&>(setting).GetValue();
  return 0;
}

// a read of an ID the table does not hold, on a non-main thread: the inventory names it
void NoteMiss()
{
  MAINCOUPLING("SettingsMirror miss");
}
} // namespace

CSettingsMirror& CSettingsMirror::Get()
{
  static CSettingsMirror mirror;
  return mirror;
}

CSettingsMirror::CSettingsMirror()
{
  for (const char* id : MIRRORED)
  {
    if (std::ranges::any_of(m_slots, [id](const auto& slot) { return slot->id == id; }))
      continue;
    auto slot = std::make_unique<SSlot>();
    slot->id = id;
    m_slots.push_back(std::move(slot));
  }
  std::ranges::sort(m_slots, {}, [](const auto& slot) -> const std::string& { return slot->id; });
}

const CSettingsMirror::SSlot* CSettingsMirror::Find(const std::string& id) const
{
  if (!m_started.load(std::memory_order_acquire))
    return nullptr;
  const auto it = std::ranges::lower_bound(m_slots, id, {}, [](const auto& slot) -> const std::string&
                                           { return slot->id; });
  if (it == m_slots.end() || (*it)->id != id || !(*it)->present.load(std::memory_order_acquire))
    return nullptr;
  return it->get();
}

void CSettingsMirror::StoreSlot(SSlot& slot, const CSetting& setting)
{
  const SettingType type = setting.GetType();
  slot.type.store(static_cast<int>(type), std::memory_order_relaxed);
  if (type == SettingType::String)
    slot.stringValue.store(std::make_shared<const std::string>(
                               static_cast<const CSettingString&>(setting).GetValue()),
                           std::memory_order_release);
  else if (type == SettingType::Number)
    slot.numberValue.store(static_cast<const CSettingNumber&>(setting).GetValue(),
                           std::memory_order_release);
  else
    slot.intValue.store(IntOf(setting), std::memory_order_release);
  slot.present.store(true, std::memory_order_release);
}

SettingsContainer CSettingsMirror::Load()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  SettingsContainer ids;
  std::optional<int> peakValue;
  for (const auto& slot : m_slots)
  {
    const auto setting = settings->GetSetting(slot->id);
    if (!setting)
    {
      slot->present.store(false, std::memory_order_release);
      CLog::Log(LOGWARNING, "OUTPUT: setting {} not found, not mirrored", slot->id);
      continue;
    }
    StoreSlot(*slot, *setting);
    ids.insert(slot->id);
    if (slot->id == CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE)
      peakValue = slot->intValue.load(std::memory_order_relaxed);
  }
  // 5.G2 GT-2 (M6): a profile load's peak is a GUI-only regime epoch
  if (peakValue)
    DISPLAYSTATE::PublishRegimePeak(*peakValue);
  return ids;
}

void CSettingsMirror::Start()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  std::unique_lock lock(m_mutex);
  if (m_started.load())
    return;
  const SettingsContainer ids = Load();
  m_check.store(XFILE::CFile::Exists("special://profile/settingsmirror_check"));
  m_started.store(true, std::memory_order_release);
  settings->RegisterCallback(this, ids);
  // a profile load sets every value with the callbacks off
  settings->GetSettingsManager()->RegisterSettingsHandler(this);
  CLog::Log(LOGINFO, "OUTPUT: {} settings mirrored (lock-free reads){}", ids.size(),
            m_check.load() ? ", every read checked against CSettings (settingsmirror_check)"
                           : "");
}

void CSettingsMirror::Stop()
{
  {
    std::unique_lock lock(m_mutex);
    if (!m_started.exchange(false))
      return;
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
  if (!m_started.load())
    return;
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
  const std::string& id = setting->GetId();
  const auto it = std::ranges::lower_bound(m_slots, id, {}, [](const auto& slot) -> const std::string&
                                           { return slot->id; });
  if (it == m_slots.end() || (*it)->id != id)
    return;
  StoreSlot(**it, *setting);
  // 5.G2 GT-2 (M6): main's peak read takes this value, so the published peak changes with it
  if (id == CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE)
    DISPLAYSTATE::PublishRegimePeak((*it)->intValue.load(std::memory_order_relaxed));
}

CSettingsMirror::CScopedValue::CScopedValue(const std::string& id, int value)
{
  t_override = true;
  t_overrideId = id;
  t_overrideValue = value;
}

CSettingsMirror::CScopedValue::~CScopedValue()
{
  t_override = false;
}

bool CSettingsMirror::IsMirrored(const std::string& id) const
{
  return Find(id) != nullptr;
}

void CSettingsMirror::Check(const std::string& id, const SSlot& slot) const
{
  // never on W: a setting's writer may hold its lock while main waits on W
  if (!m_check.load(std::memory_order_relaxed) || COutputWorker::Get().OnWorker())
    return;
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  const auto setting = settings ? settings->GetSetting(id) : nullptr;
  if (!setting)
    return;
  const auto type = static_cast<SettingType>(slot.type.load(std::memory_order_relaxed));
  if (type == SettingType::String)
  {
    const auto mine = slot.stringValue.load(std::memory_order_acquire);
    const std::string actual = setting->GetType() == SettingType::String
                                   ? static_cast<const CSettingString&>(*setting).GetValue()
                                   : std::string();
    if (!mine || *mine != actual)
      CLog::Log(LOGWARNING, "OUTPUT: settings mirror DIFF {}: mirror '{}' CSettings '{}'", id,
                mine ? *mine : std::string(), actual);
  }
  else if (type == SettingType::Number)
  {
    const double mine = slot.numberValue.load(std::memory_order_acquire);
    const double actual = setting->GetType() == SettingType::Number
                              ? static_cast<const CSettingNumber&>(*setting).GetValue()
                              : 0.0;
    if (mine != actual)
      CLog::Log(LOGWARNING, "OUTPUT: settings mirror DIFF {}: mirror {} CSettings {}", id, mine,
                actual);
  }
  else
  {
    const int mine = slot.intValue.load(std::memory_order_acquire);
    const int actual = IntOf(*setting);
    if (mine != actual)
      CLog::Log(LOGWARNING, "OUTPUT: settings mirror DIFF {}: mirror {} CSettings {}", id, mine,
                actual);
  }
}

int CSettingsMirror::GetInt(const std::string& id) const
{
  if (t_override && t_overrideId == id)
    return t_overrideValue;
  if (const SSlot* slot = Find(id))
  {
    Check(id, *slot);
    // as before: a bool reads 0/1, a string or number 0
    return slot->intValue.load(std::memory_order_acquire);
  }
  // before Start, or a setting not in the list: today's read
  if (m_started.load(std::memory_order_relaxed))
    NoteMiss();
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  const auto setting = settings ? settings->GetSetting(id) : nullptr;
  return setting ? IntOf(*setting) : 0;
}

bool CSettingsMirror::GetBool(const std::string& id) const
{
  return GetInt(id) != 0;
}

std::string CSettingsMirror::GetString(const std::string& id) const
{
  if (const SSlot* slot = Find(id))
  {
    Check(id, *slot);
    if (static_cast<SettingType>(slot->type.load(std::memory_order_relaxed)) !=
        SettingType::String)
      return {};
    const auto value = slot->stringValue.load(std::memory_order_acquire);
    return value ? *value : std::string();
  }
  if (m_started.load(std::memory_order_relaxed))
    NoteMiss();
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings ? settings->GetString(id) : std::string();
}

double CSettingsMirror::GetNumber(const std::string& id) const
{
  if (const SSlot* slot = Find(id))
  {
    Check(id, *slot);
    if (static_cast<SettingType>(slot->type.load(std::memory_order_relaxed)) !=
        SettingType::Number)
      return 0.0;
    return slot->numberValue.load(std::memory_order_acquire);
  }
  if (m_started.load(std::memory_order_relaxed))
    NoteMiss();
  const auto component = CServiceBroker::GetSettingsComponent();
  const auto settings = component ? component->GetSettings() : nullptr;
  return settings ? settings->GetNumber(id) : 0.0;
}

} // namespace OUTPUT
