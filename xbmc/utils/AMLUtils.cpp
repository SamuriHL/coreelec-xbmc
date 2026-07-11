/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <fcntl.h>
#include <regex>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "AMLUtils.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "ServiceBroker.h"
#include "utils/RegExp.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "platform/linux/SysfsPath.h"
#include "windowing/amlogic/WinSystemAmlogic.h"
#include "windowing/WinSystem.h"

// Dolby Vision L5 active-area detection (background software decode + luma scan)
#include "URL.h"
#include "application/Application.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "filesystem/File.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <amcodec/codec.h>

int aml_get_cpufamily_id()
{
  static int aml_cpufamily_id = -1;
  if (aml_cpufamily_id == -1)
  {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::regex re(".*: (.*)$");

    for (std::string line; std::getline(cpuinfo, line);)
    {
      if (line.find("Serial") != std::string::npos)
      {
        std::smatch match;

        if (std::regex_match(line, match, re) && match.size() == 2)
        {
          std::ssub_match value = match[1];
          std::string cpu_family = value.str().substr(0, 2);
          aml_cpufamily_id = std::stoi(cpu_family, nullptr, 16);
          break;
        }
      }
    }
  }
  return aml_cpufamily_id;
}

std::string aml_get_cpufamily_name(int cpuid)
{
  switch(cpuid)
  {
    case AML_G12A:
      return "G12A";
    case AML_G12B:
      return "G12B";
    case AML_SM1:
      return "SM1";
    case AML_SC2:
      return "SC2";
    case AML_T7:
      return "T7";
    case AML_S4:
      return "S4";
    case AML_S5:
      return "S5";
    case AML_S7:
      return "S7";
    case AML_S7D:
      return "S7D";
    case AML_S6:
      return "S6";
    default:
      return aml_get_cpufamily_name(aml_get_cpufamily_id());
  }
  return "Unknown";
}

static bool aml_support_vcodec_profile(const char *regex)
{
  int profile = 0;
  CRegExp regexp;
  regexp.RegComp(regex);
  std::string valstr;
  CSysfsPath vcodec_profile{"/sys/class/amstream/vcodec_profile"};
  if (vcodec_profile.Exists())
  {
    valstr = vcodec_profile.Get<std::string>().value();
    profile = (regexp.RegFind(valstr) >= 0) ? 1 : 0;
  }

  return profile;
}

bool aml_support_hevc()
{
  static int has_hevc = -1;

  if (has_hevc == -1)
      has_hevc = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):");

  return (has_hevc == 1);
}

bool aml_support_hevc_4k2k()
{
  static int has_hevc_4k2k = -1;

  if (has_hevc_4k2k == -1)
    has_hevc_4k2k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*(4k|8k)");

  return (has_hevc_4k2k == 1);
}

bool aml_support_hevc_8k4k()
{
  static int has_hevc_8k4k = -1;

  if (has_hevc_8k4k == -1)
    has_hevc_8k4k = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*8k");

  return (has_hevc_8k4k == 1);
}

bool aml_support_hevc_10bit()
{
  static int has_hevc_10bit = -1;

  if (has_hevc_10bit == -1)
    has_hevc_10bit = aml_support_vcodec_profile("(\\bhevc\\b|\\bhevc_fb\\b):(?!\\;).*10bit");

  return (has_hevc_10bit == 1);
}

bool aml_support_h266()
{
  static int has_h266 = -1;

  if (has_h266 == -1)
    has_h266 = aml_support_vcodec_profile("\\bh266\\b:");

  return (has_h266 == 1);
}

AML_SUPPORT_H264_4K2K aml_support_h264_4k2k()
{
  static AML_SUPPORT_H264_4K2K has_h264_4k2k = AML_SUPPORT_H264_4K2K_UNINIT;

  if (has_h264_4k2k == AML_SUPPORT_H264_4K2K_UNINIT)
  {
    has_h264_4k2k = AML_NO_H264_4K2K;

    if (aml_support_vcodec_profile("(\\bh264\\b|\\bmh264\\b):4k"))
      has_h264_4k2k = AML_HAS_H264_4K2K_SAME_PROFILE;
    else if (aml_support_vcodec_profile("\\bh264_4k2k\\b:"))
      has_h264_4k2k = AML_HAS_H264_4K2K;
  }
  return has_h264_4k2k;
}

bool aml_support_vp9()
{
  static int has_vp9 = -1;

  if (has_vp9 == -1)
    has_vp9 = aml_support_vcodec_profile("(\\bvp9\\b|\\bvp9_fb\\b):(?!\\;).*compressed");

  return (has_vp9 == 1);
}

bool aml_support_av1()
{
  static int has_av1 = -1;

  if (has_av1 == -1)
    has_av1 = aml_support_vcodec_profile("(\\bav1\\b|\\bav1_fb\\b):(?!\\;).*compressed");

  return (has_av1 == 1);
}

bool aml_support_avs2()
{
  static int has_avs2 = -1;

  if (has_avs2 == -1)
    has_avs2 = aml_support_vcodec_profile("(\\bavs2\\b|\\bavs2_fb\\b):(?!\\;).*compressed");

  return (has_avs2 == 1);
}

bool aml_support_avs3()
{
  static int has_avs3 = -1;

  if (has_avs3 == -1)
    has_avs3 = aml_support_vcodec_profile("\\bavs3\\b:(?!\\;).*compressed");

  return (has_avs3 == 1);
}

bool aml_support_dolby_vision()
{
  static int support_dv = -1;

  if (support_dv == -1)
  {
    CSysfsPath support_info{"/sys/class/amdolby_vision/support_info"};
    support_dv = 0;
    if (support_info.Exists())
    {
      support_dv = (int)((support_info.Get<int>().value() & 7) == 7);
      if (support_dv == 1) {
        CSysfsPath ko_info{"/sys/class/amdolby_vision/ko_info"};
        if (ko_info.Exists())
          CLog::Log(LOGINFO, "Amlogic Dolby Vision info: {}", ko_info.Get<std::string>().value().c_str());
      }
    }
  }

  return (support_dv == 1);
}

bool aml_dolby_vision_enabled()
{
  bool dv_user_enabled(!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));

  bool dv_enabled = (!!aml_support_dolby_vision() &&
                     static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
                         ->GetAmlDisplay()->aml_display_support_dv());

  return ((dv_enabled && !!dv_user_enabled) == 1);
}

bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType)
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  bool dv_user_enabled(!settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE));
  bool user_convert_to_dv;

  if (hdrType == StreamHdrType::HDR_TYPE_NONE)
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2DV);
  else
    user_convert_to_dv = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2DV);

  bool convert_to_dv = (!!aml_support_dolby_vision() &&
                        static_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem())
                            ->GetAmlDisplay()->aml_display_support_dv());

  return ((convert_to_dv && !!user_convert_to_dv && !!dv_user_enabled) == 1);
}

// --- Dolby Vision VS10 engine ------------------------------------------------
// The VS10 engine forces the amdolby_vision output mode so incoming HDR flavors
// are converted to a chosen output. CE22 drives this the same way its native
// SDR2DV/HDR2DV conversion does (see CAMLCodec::OpenDecoder/CloseDecoder):
// dolby_vision_enable=Y, dolby_vision_policy=AMDV_FORCE_OUTPUT_MODE, then the
// amdolby_vision/dv_mode node with the kernel's (mode + 1) % 6 encoding. The
// module params live under aml_media on CE22 (driver is linked into aml_media.ko).
#define AMDV_FOLLOW_SOURCE      (unsigned int)(1)
#define AMDV_FORCE_OUTPUT_MODE  (unsigned int)(2)

bool aml_display_support_hdr_pq()
{
  bool support = false;
  CSysfsPath hdr_cap{"/sys/class/amhdmitx/amhdmitx0/hdr_cap"};
  if (hdr_cap.Exists())
    support = (hdr_cap.Get<std::string>().value().find("SMPTE ST 2084: 1") != std::string::npos);
  return support;
}

bool aml_display_support_hdr_hlg()
{
  bool support = false;
  CSysfsPath hdr_cap{"/sys/class/amhdmitx/amhdmitx0/hdr_cap"};
  if (hdr_cap.Exists())
    support = (hdr_cap.Get<std::string>().value().find("Hybrid Log-Gamma: 1") != std::string::npos);
  return support;
}

// VS10 output mode resolved at stream-open and consumed in CAMLCodec::OpenDecoder.
static unsigned int s_vs10_pending_mode = DOLBY_VISION_OUTPUT_MODE_BYPASS;
void aml_dv_set_vs10_pending(unsigned int mode) { s_vs10_pending_mode = mode; }
unsigned int aml_dv_get_vs10_pending() { return s_vs10_pending_mode; }

unsigned int aml_vs10_by_setting(const std::string& setting)
{
  return CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(setting);
}

unsigned int aml_vs10_by_hdrtype(StreamHdrType hdrType, unsigned int bitDepth)
{
  switch (hdrType)
  {
    case StreamHdrType::HDR_TYPE_NONE:
      return aml_vs10_by_setting(bitDepth == 10 ? CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10
                                                : CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8);
    case StreamHdrType::HDR_TYPE_HDR10:
      return aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10);
    case StreamHdrType::HDR_TYPE_HDR10PLUS:
      return aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS);
    case StreamHdrType::HDR_TYPE_HLG:
      return aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG);
    case StreamHdrType::HDR_TYPE_DOLBYVISION:
      return aml_vs10_by_setting(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV);
    default:
      return DOLBY_VISION_OUTPUT_MODE_BYPASS;
  }
}

unsigned int aml_dv_dolby_vision_mode()
{
  CSysfsPath dolby_vision_mode{"/sys/module/aml_media/parameters/dolby_vision_mode"};
  if (dolby_vision_mode.Exists())
    return dolby_vision_mode.Get<unsigned int>().value();
  return DOLBY_VISION_OUTPUT_MODE_BYPASS;
}

void aml_dv_set_vs10_mode(unsigned int mode)
{
  CSysfsPath dolby_vision_enable{"/sys/module/aml_media/parameters/dolby_vision_enable"};
  CSysfsPath dolby_vision_policy{"/sys/module/aml_media/parameters/dolby_vision_policy"};
  bool dv_enabled(dolby_vision_enable.Exists() &&
                  StringUtils::EqualsNoCase(dolby_vision_enable.Get<std::string>().value(), "Y"));

  if (mode == DOLBY_VISION_OUTPUT_MODE_BYPASS)
  {
    // Stop forcing an output mode: let the pipeline follow the source. Mirrors
    // CAMLCodec::CloseDecoder's disable path.
    if (dv_enabled && dolby_vision_policy.Exists() &&
        dolby_vision_policy.Get<int>().value() == static_cast<int>(AMDV_FORCE_OUTPUT_MODE))
      dolby_vision_policy.Set(AMDV_FOLLOW_SOURCE);
    CSysfsPath("/sys/class/amdolby_vision/dv_mode", (DOLBY_VISION_OUTPUT_MODE_BYPASS + 1) % 6);
    CLog::Log(LOGINFO, "AMLUtils::{} - VS10 bypass (follow source)", __FUNCTION__);
    return;
  }

  // Force the requested VS10 output mode.
  dolby_vision_enable.Set('Y');
  dolby_vision_policy.Set(AMDV_FORCE_OUTPUT_MODE);
  CSysfsPath("/sys/class/amdolby_vision/dv_mode", (mode + 1) % 6);
  CLog::Log(LOGINFO, "AMLUtils::{} - VS10 output mode {} (dv_mode {})",
            __FUNCTION__, mode, (mode + 1) % 6);
}

void aml_dv_set_hdr10_osd_brightness(int nits)
{
  // OSD graphics peak luminance for VS10 HDR10 output. The donor wrote
  // dolby_vision_graphic_max; CE22's kernel exposes amdv_graphic_max instead.
  CSysfsPath("/sys/module/aml_media/parameters/dv_graphic_blend_test", 0);
  CSysfsPath("/sys/module/aml_media/parameters/amdv_graphic_max", nits);
}

// --- Dolby Vision VSVDB max-luminance override -------------------------------
// The display advertises its DV capabilities (primaries + max/min luminance) in
// a VSVDB block via EDID. CE22 can inject a replacement block through the stock
// aml_media params force_vsvdb (0/1) + vsvdb_data (comma-separated decimal bytes).
// We read the display's own v2 VSVDB, patch only its 5-bit "Maximum Luminance
// (PQ)" field to the user's target, and re-inject it - so every other field
// (version/primaries) is preserved exactly. v2 layout matches
// DVDVideoCodecAmlogic::GetDisplayVsvdbMaxNits(): block = [0xEB, 0x01, OUI(3)]
// then payload at b[5]; version = (b[5]>>5)&7; max-lum index = (b[7]>>3)&0x1F.

// VSVDB v2 5-bit max-luminance (PQ) index -> nits (Dolby table).
static const int vsvdb_v2_max_lum_lut[32] = {
    96,   113,  132,  155,  181,  211,  245,  285,  332,  385,  447,
    518,  601,  696,  807,  934,  1082, 1252, 1450, 1678, 1943, 2250,
    2607, 3020, 3501, 4060, 4710, 5467, 6351, 7382, 8588, 10000};

// Read the display's advertised VSVDB block (hex after "VSVDB: " in dv_cap) into
// bytes[]; returns the byte count (0 on failure).
static size_t aml_read_display_vsvdb(int bytes[], size_t max)
{
  CSysfsPath dv_cap{"/sys/class/amhdmitx/amhdmitx0/dv_cap"};
  if (!dv_cap.Exists())
    return 0;
  std::string cap = dv_cap.Get<std::string>().value();
  const std::string tag = "VSVDB: ";
  size_t p = cap.find(tag);
  if (p == std::string::npos)
    return 0;
  size_t start = p + tag.size();
  size_t end = cap.find_first_of(" \t\r\n", start);
  std::string hex = cap.substr(start, (end == std::string::npos ? cap.size() : end) - start);
  size_t n = 0;
  for (size_t i = 0; (i + 1 < hex.size()) && (n < max); i += 2)
  {
    try { bytes[n++] = static_cast<int>(std::stoul(hex.substr(i, 2), nullptr, 16)); }
    catch (...) { return 0; }
  }
  return n;
}

void aml_dv_clear_vsvdb()
{
  CSysfsPath("/sys/module/aml_media/parameters/force_vsvdb", 0);
}

void aml_dv_apply_vsvdb()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  if (!settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_MAXLUM_OVERRIDE))
  {
    aml_dv_clear_vsvdb();
    return;
  }

  // Shared display peak value (0 = auto): nothing to force onto the panel, so
  // leave its real advertised VSVDB in place.
  int nits = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS);
  if (nits <= 0)
  {
    CLog::Log(LOGINFO, "AMLUtils::{} - display peak is auto (0) - not injecting", __FUNCTION__);
    aml_dv_clear_vsvdb();
    return;
  }

  int b[16];
  size_t n = aml_read_display_vsvdb(b, 16);
  // Need the v2 payload byte b[7]; version is in b[5] bits 7:5.
  if (n < 8 || (((b[5] >> 5) & 0x07) != 2))
  {
    CLog::Log(LOGINFO, "AMLUtils::{} - no v2 VSVDB from display ({} bytes) - not injecting",
              __FUNCTION__, static_cast<int>(n));
    aml_dv_clear_vsvdb();
    return;
  }

  // Snap the requested nits to the nearest Dolby PQ max-luminance step.
  int idx = 0, best = 0x7fffffff;
  for (int i = 0; i < 32; i++)
  {
    int d = vsvdb_v2_max_lum_lut[i] - nits;
    if (d < 0) d = -d;
    if (d < best) { best = d; idx = i; }
  }

  // Patch the 5-bit "Maximum Luminance (PQ)" field (b[7] bits 7:3), keep the rest.
  b[7] = (b[7] & 0x07) | ((idx & 0x1F) << 3);

  // Optional colour-space / primary override: replace only the v2 primary fields
  // (Gx/Gy/Rx/Bx/Ry/By in bytes 8-11), preserving the display's version, DM-version,
  // capability and dv-type bits. 0 = keep the display's advertised primaries (max-lum
  // patch only). Values are the Dolby VSVDB v2 primary fields (coord minus per-channel
  // base, x256), i.e. Gx: 43=BT.2020 / 67=DCI-P3 / 76=BT.709.
  const int cs = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_COLOURSPACE);
  const char* csName = "display";
  if (cs >= 1 && cs <= 3 && n >= 12)
  {
    static const int primaries[3][6] = {
        // Rx, Ry, Gx, Gy, Bx, By
        {14, 17, 67, 48, 6, 7}, // DCI-P3
        {21, 10, 43, 76, 1, 3}, // BT.2020
        {3, 20, 76, 25, 6, 7},  // BT.709
    };
    static const char* const csNames[3] = {"DCI-P3", "BT.2020", "BT.709"};
    const int* p = primaries[cs - 1];
    csName = csNames[cs - 1];
    b[8] = ((p[2] & 0x7F) << 1) | (b[8] & 0x01);  // Gx (keep 12-bit 4:4:4 bit)
    b[9] = ((p[3] & 0x7F) << 1) | (b[9] & 0x01);  // Gy (keep 10-bit 4:4:4 bit)
    b[10] = ((p[0] & 0x1F) << 3) | (p[4] & 0x07); // Rx | Bx
    b[11] = ((p[1] & 0x1F) << 3) | (p[5] & 0x07); // Ry | By
  }

  // Emit the full block as comma-separated decimals for vsvdb_data, enable inject.
  std::string data;
  for (size_t i = 0; i < n; i++)
  {
    if (i) data += ",";
    data += std::to_string(b[i] & 0xff);
  }
  CSysfsPath("/sys/module/aml_media/parameters/vsvdb_data", data);
  // Toggle force_vsvdb 0->1 so the DV core re-latches the new block. This makes
  // the override apply live mid-stream (setting change), not just at decoder
  // open; at open force_vsvdb is already 0 so the toggle is a harmless no-op.
  CSysfsPath("/sys/module/aml_media/parameters/force_vsvdb", 0);
  CSysfsPath("/sys/module/aml_media/parameters/force_vsvdb", 1);
  CLog::Log(LOGINFO, "AMLUtils::{} - VSVDB override -> {} nits (idx {}), primaries {}, data [{}]",
            __FUNCTION__, vsvdb_v2_max_lum_lut[idx], idx, csName, data);
}

// --- Dolby Vision L5 active-area detection ----------------------------------
// A background thread software-decodes the playing file at spread positions and
// finds the letterbox/pillarbox bars from luma. The detected offsets are published
// via atomics; CBitstreamConverter injects them into the DV RPU
// (dovi_rpu_set_active_area_offsets) - no kernel params, unlike the donor builds.
// Algorithm ported from pannal's DetectActiveAreaFromFile (adaptive threshold /
// consensus / AR-snap / IMAX guard / cache-aware throttle), with the sysfs writes
// and DataCacheCore coupling removed (RPU-side source-L5 precedence lives in
// CBitstreamConverter).

static std::atomic<bool> s_detectStable{false};
static std::atomic<int> s_detectState{DV_DETECT_FAILED};
static std::atomic<uint16_t> s_detectedTop{0};
static std::atomic<uint16_t> s_detectedBottom{0};
static std::atomic<uint16_t> s_detectedLeft{0};
static std::atomic<uint16_t> s_detectedRight{0};
static std::atomic<bool> s_detectCancel{false};
static std::atomic<bool> s_detectThrottleActive{false};
static std::atomic<bool> s_detectCacheStarved{false};
static std::atomic<int64_t> s_detectLastCacheCheckMs{0};
static std::thread s_detectThread;
static std::string s_detectFilePath;

// Common aspect ratios x1000 for snapping.
static const uint32_t s_commonAR[] = {
    1333, 1370, 1667, 1778, 1850, 1896, 2000, 2200, 2350, 2390, 2400, 2550, 2760};

static constexpr int kDetectCacheCriticalPct = 40; // abort in-flight read below this
static constexpr int kDetectCacheTargetPct = 80;   // only seek when buffer this full
static constexpr int kDetectCacheFloorPct = 50;    // still below after waiting = too slow

static bool aml_dv_detect_active_area_enabled()
{
  return CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
             CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_MODE) == 2; // Auto-detect
}

int aml_dv_detect_active_area_state()
{
  if (!aml_dv_detect_active_area_enabled())
    return DV_DETECT_INACTIVE;
  return s_detectState.load();
}

bool aml_dv_detect_active_area_get(uint16_t& top, uint16_t& bottom, uint16_t& left,
                                   uint16_t& right)
{
  top = s_detectedTop.load();
  bottom = s_detectedBottom.load();
  left = s_detectedLeft.load();
  right = s_detectedRight.load();
  return s_detectState.load() == DV_DETECT_OK;
}

static int64_t detect_steady_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ffmpeg interrupt callback: abort a cancelled scan, or an in-flight read that is
// draining playback's buffer below critical (rate-limited to 150ms).
static int detect_interrupt_cb(void* /*opaque*/)
{
  if (s_detectCancel.load())
    return 1;
  if (s_detectThrottleActive.load())
  {
    const int64_t now = detect_steady_ms();
    if (now - s_detectLastCacheCheckMs.load() >= 150)
    {
      s_detectLastCacheCheckMs.store(now);
      auto& components = CServiceBroker::GetAppComponents();
      const auto appPlayer = components.GetComponent<CApplicationPlayer>();
      if (appPlayer && appPlayer->IsPlaying() &&
          appPlayer->GetCacheLevel() < kDetectCacheCriticalPct)
      {
        s_detectCacheStarved.store(true);
        return 1;
      }
    }
  }
  return 0;
}

// AVIO callbacks for reading through Kodi's VFS (handles nfs://, smb://, etc.)
static int detect_avio_read(void* opaque, uint8_t* buf, int size)
{
  auto* file = static_cast<XFILE::CFile*>(opaque);
  int ret = file->Read(buf, size);
  return (ret == 0) ? AVERROR_EOF : ret;
}

static int64_t detect_avio_seek(void* opaque, int64_t pos, int whence)
{
  auto* file = static_cast<XFILE::CFile*>(opaque);
  if (whence == AVSEEK_SIZE)
    return file->GetLength();
  return file->Seek(pos, whence & ~AVSEEK_FORCE);
}

// Cache-aware throttle: block until the player's buffer reaches targetPct so our
// competing reads don't starve playback. Returns the level reached [0..100], or -1
// if cancelled; caps at maxWaitMs so a marginal source can't hang forever.
static int detect_wait_for_cache(int targetPct, int maxWaitMs)
{
  auto& components = CServiceBroker::GetAppComponents();
  const auto appPlayer = components.GetComponent<CApplicationPlayer>();
  const int step = 100;
  int waited = 0;
  for (;;)
  {
    if (s_detectCancel.load())
      return -1;
    if (!appPlayer || !appPlayer->IsPlaying())
      return 100;
    const int level = appPlayer->GetCacheLevel();
    if (level >= targetPct || waited >= maxWaitMs)
      return level;
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    waited += step;
  }
}

static void DetectActiveAreaFromFile(const std::string& filePath)
{
  AVFormatContext* fmtCtx = nullptr;
  AVCodecContext* codecCtx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* pkt = nullptr;
  AVIOContext* avioCtx = nullptr;
  XFILE::CFile file;
  uint8_t* avioBuf = nullptr;
  const int bufSize = 32768;
  int videoIdx = -1;
  uint16_t detTop = 0, detBottom = 0, detLeft = 0, detRight = 0;

  // Disc playlists (bluray://, dvd://) can't be resolved by the AVIO wrapper.
  if (StringUtils::StartsWithNoCase(filePath, "bluray://") ||
      StringUtils::StartsWithNoCase(filePath, "dvd://"))
  {
    s_detectState.store(DV_DETECT_SKIPPED);
    goto cleanup;
  }

  // Cache-aware throttle: build a safety margin before competing for the device.
  {
    const int lvl = detect_wait_for_cache(kDetectCacheTargetPct, 12000);
    if (lvl < 0)
      goto cleanup; // cancelled
    if (lvl < kDetectCacheFloorPct)
    {
      CLog::Log(LOGINFO, "DetectActiveArea: cache stuck at {}% - too slow to scan, skipping", lvl);
      s_detectState.store(DV_DETECT_SKIPPED);
      goto cleanup;
    }
  }
  s_detectThrottleActive.store(true);

  if (!file.Open(filePath, XFILE::READ_NO_CACHE))
  {
    CLog::Log(LOGWARNING, "DetectActiveArea: failed to open {}", CURL::GetRedacted(filePath));
    goto cleanup;
  }

  avioBuf = static_cast<uint8_t*>(av_malloc(bufSize));
  if (!avioBuf)
    goto cleanup;
  avioCtx = avio_alloc_context(avioBuf, bufSize, 0, &file, detect_avio_read, nullptr,
                               detect_avio_seek);
  if (!avioCtx)
  {
    av_free(avioBuf);
    goto cleanup;
  }
  fmtCtx = avformat_alloc_context();
  if (!fmtCtx)
  {
    avio_context_free(&avioCtx);
    goto cleanup;
  }
  fmtCtx->pb = avioCtx;
  fmtCtx->interrupt_callback.callback = detect_interrupt_cb;
  fmtCtx->interrupt_callback.opaque = nullptr;

  if (s_detectCancel.load())
    goto cleanup;
  if (avformat_open_input(&fmtCtx, filePath.c_str(), nullptr, nullptr) < 0)
    goto cleanup;

  // MPEG-TS/m2ts have no reliable seek index -> stale frames. Skip.
  if (fmtCtx->iformat && fmtCtx->iformat->name &&
      (strstr(fmtCtx->iformat->name, "mpegts") || strstr(fmtCtx->iformat->name, "m2ts")))
  {
    s_detectState.store(DV_DETECT_SKIPPED);
    goto cleanup;
  }

  if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
    goto cleanup;

  for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
  {
    if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
      videoIdx = i;
      break;
    }
  }
  if (videoIdx < 0)
    goto cleanup;

  {
    const AVCodec* codec =
        avcodec_find_decoder(fmtCtx->streams[videoIdx]->codecpar->codec_id);
    if (!codec)
      goto cleanup;
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
      goto cleanup;
    avcodec_parameters_to_context(codecCtx, fmtCtx->streams[videoIdx]->codecpar);
    codecCtx->thread_count = 2;
    codecCtx->skip_frame = AVDISCARD_NONKEY;    // keyframes only - fast
    codecCtx->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
      goto cleanup;
  }

  frame = av_frame_alloc();
  pkt = av_packet_alloc();
  if (!frame || !pkt)
    goto cleanup;

  // Pre-cropped content: encoded resolution significantly non-16:9 => no bars.
  {
    uint32_t refH = (uint32_t)codecCtx->width * 9 / 16;
    uint32_t refW = (uint32_t)codecCtx->height * 16 / 9;
    int tbGap = ((int)refH > codecCtx->height) ? ((int)refH - codecCtx->height) / 2 : 0;
    int lrGap = ((int)refW > codecCtx->width) ? ((int)refW - codecCtx->width) / 2 : 0;
    if (tbGap > 20 || lrGap > 20)
    {
      CLog::Log(LOGINFO, "DetectActiveArea: pre-cropped {}x{} - no bars to scan",
                codecCtx->width, codecCtx->height);
      s_detectState.store(DV_DETECT_SKIP_NON16X9);
      goto cleanup;
    }
  }

  // Sample at 7 spread positions; retry at +1% to escape fades/title cards.
  {
    const int seekPercents[] = {0, 15, 30, 45, 60, 75, 88};
    const int numSeeks = 7;
    const int maxRetries = 8;
    const uint32_t minContrast = 10;
    uint16_t samples_top[7] = {}, samples_bottom[7] = {};
    uint16_t samples_left[7] = {}, samples_right[7] = {};
    int validSamples = 0;
    int lastWidth = 0, lastHeight = 0;
    const int agreeTolerance = 5;
    uint32_t staleRow0 = UINT32_MAX, staleMid = UINT32_MAX, staleLast = UINT32_MAX;
    int staleCount = 0;
    const int maxStale = 5;
    bool staleAbort = false;

    auto pickBest = [](uint16_t* v, int n) -> uint16_t {
      uint16_t best = v[0];
      int bestCount = 0;
      for (int i = 0; i < n; i++)
      {
        int count = 0;
        for (int j = 0; j < n; j++)
          if (v[j] == v[i])
            count++;
        if (count > bestCount)
        {
          bestCount = count;
          best = v[i];
        }
      }
      if (bestCount >= 2)
        return best;
      std::sort(v, v + n);
      return v[n / 2];
    };

    auto countSupport = [&agreeTolerance](uint16_t* v, int n, uint16_t picked) -> int {
      int support = 0;
      for (int i = 0; i < n; i++)
        if (std::abs((int)v[i] - (int)picked) <= agreeTolerance)
          support++;
      return support;
    };

    for (int s = 0; s < numSeeks && validSamples < numSeeks; s++)
    {
      if (s_detectCancel.load() || s_detectCacheStarved.load())
        break;
      if (s > 0 && detect_wait_for_cache(kDetectCacheTargetPct, 8000) < 0)
        break; // cancelled

      bool usable = false;
      for (int retry = 0; retry <= maxRetries && !usable; retry++)
      {
        if (s_detectCacheStarved.load())
          break;
        int seekPct = seekPercents[s] + retry;
        if (seekPct > 90)
          break;
        if (fmtCtx->duration > 0)
        {
          avcodec_flush_buffers(codecCtx);
          av_seek_frame(fmtCtx, -1, fmtCtx->duration * seekPct / 100, AVSEEK_FLAG_BACKWARD);
        }
        else if (s > 0 || retry > 0)
          break; // unseekable - one attempt only

        bool gotFrame = false;
        for (int vidPkts = 0; vidPkts < 50 && !gotFrame;)
        {
          if (av_read_frame(fmtCtx, pkt) < 0)
            break;
          if (pkt->stream_index != videoIdx)
          {
            av_packet_unref(pkt);
            continue;
          }
          vidPkts++;
          avcodec_send_packet(codecCtx, pkt);
          av_packet_unref(pkt);
          if (avcodec_receive_frame(codecCtx, frame) == 0)
            gotFrame = true;
        }
        if (!gotFrame)
        {
          avcodec_send_packet(codecCtx, nullptr);
          if (avcodec_receive_frame(codecCtx, frame) == 0)
            gotFrame = true;
        }
        if (!gotFrame || !frame->data[0] || frame->width < 64 || frame->height < 64)
          continue;

        lastWidth = frame->width;
        lastHeight = frame->height;
        const int stride = frame->linesize[0];
        const uint8_t* yData = frame->data[0];
        const bool isP010 =
            (frame->format == AV_PIX_FMT_P010LE || frame->format == AV_PIX_FMT_P010BE);
        const bool is10bit = isP010 || frame->format == AV_PIX_FMT_YUV420P10LE ||
                             frame->format == AV_PIX_FMT_YUV420P10BE;
        const int shift = isP010 ? 8 : (is10bit ? 2 : 0);
        auto getY = [&](int row, int col) -> uint32_t {
          if (is10bit)
            return reinterpret_cast<const uint16_t*>(yData + row * stride)[col] >> shift;
          return yData[row * stride + col];
        };

        uint32_t row0Y = getY(0, lastWidth / 2);
        uint32_t midY = getY(lastHeight / 2, lastWidth / 2);
        uint32_t lastY = getY(lastHeight - 1, lastWidth / 2);
        uint32_t borderY = std::min(row0Y, lastY);
        uint32_t vContrast = (midY > borderY) ? (midY - borderY) : 0;
        uint32_t col0Y = getY(lastHeight / 2, 0);
        uint32_t colLastY = getY(lastHeight / 2, lastWidth - 1);
        uint32_t colBorderY = std::min(col0Y, colLastY);
        uint32_t hContrast = (midY > colBorderY) ? (midY - colBorderY) : 0;
        uint32_t contrast = std::max(vContrast, hContrast);

        if (row0Y == staleRow0 && midY == staleMid && lastY == staleLast)
        {
          if (++staleCount >= maxStale)
          {
            CLog::Log(LOGWARNING, "DetectActiveArea: identical frames - seeks broken, aborting");
            staleAbort = true;
            break;
          }
        }
        else
        {
          staleCount = 0;
          staleRow0 = row0Y;
          staleMid = midY;
          staleLast = lastY;
        }
        if (contrast < minContrast)
          continue;
        usable = true;
      }

      if (staleAbort)
      {
        s_detectState.store(DV_DETECT_FAILED);
        goto cleanup;
      }
      if (!usable)
        continue;

      const int stride = frame->linesize[0];
      const uint8_t* yData = frame->data[0];
      const bool isP010 =
          (frame->format == AV_PIX_FMT_P010LE || frame->format == AV_PIX_FMT_P010BE);
      const bool is10bit = isP010 || frame->format == AV_PIX_FMT_YUV420P10LE ||
                           frame->format == AV_PIX_FMT_YUV420P10BE;
      const int shift = isP010 ? 8 : (is10bit ? 2 : 0);
      const int sampleW = std::min(64, lastWidth / 2);
      const int sampleStartX = lastWidth / 2 - sampleW / 2;
      auto getY = [&](int row, int col) -> uint32_t {
        if (is10bit)
          return reinterpret_cast<const uint16_t*>(yData + row * stride)[col] >> shift;
        return yData[row * stride + col];
      };

      // Adaptive threshold: midpoint between border and content luma.
      uint32_t borderAvg = 0, contentAvg = 0;
      for (int i = 0; i < sampleW; i++)
        borderAvg += getY(0, sampleStartX + i);
      for (int i = 0; i < sampleW; i++)
        contentAvg += getY(lastHeight / 2, sampleStartX + i);
      borderAvg /= sampleW;
      contentAvg /= sampleW;
      uint32_t scanThreshold = (borderAvg + contentAvg) / 2;

      uint16_t sTop = 0, sBottom = 0, sLeft = 0, sRight = 0;
      for (int row = 0; row < lastHeight / 2; row++)
      {
        uint32_t sum = 0;
        for (int i = 0; i < sampleW; i++)
          sum += getY(row, sampleStartX + i);
        if (sum / sampleW > scanThreshold)
        {
          sTop = static_cast<uint16_t>(row);
          break;
        }
      }
      for (int row = lastHeight - 1; row >= lastHeight / 2; row--)
      {
        uint32_t sum = 0;
        for (int i = 0; i < sampleW; i++)
          sum += getY(row, sampleStartX + i);
        if (sum / sampleW > scanThreshold)
        {
          sBottom = static_cast<uint16_t>(lastHeight - 1 - row);
          break;
        }
      }
      // L/R only when frame is narrower than 16:9 (pillarbox possible).
      if (lastWidth * 1000 / lastHeight < 1778)
      {
        const int sampleH = std::min(64, lastHeight / 2);
        const int sampleStartY = lastHeight / 2 - sampleH / 2;
        uint32_t lrBorderAvg = 0, lrContentAvg = 0;
        for (int i = 0; i < sampleH; i++)
          lrBorderAvg += getY(sampleStartY + i, 0);
        for (int i = 0; i < sampleH; i++)
          lrContentAvg += getY(sampleStartY + i, lastWidth / 2);
        lrBorderAvg /= sampleH;
        lrContentAvg /= sampleH;
        uint32_t lrThreshold = (lrBorderAvg + lrContentAvg) / 2;
        for (int col = 0; col < lastWidth / 2; col++)
        {
          uint32_t sum = 0;
          for (int i = 0; i < sampleH; i++)
            sum += getY(sampleStartY + i, col);
          if (sum / sampleH > lrThreshold)
          {
            sLeft = static_cast<uint16_t>(col);
            break;
          }
        }
        for (int col = lastWidth - 1; col >= lastWidth / 2; col--)
        {
          uint32_t sum = 0;
          for (int i = 0; i < sampleH; i++)
            sum += getY(sampleStartY + i, col);
          if (sum / sampleH > lrThreshold)
          {
            sRight = static_cast<uint16_t>(lastWidth - 1 - col);
            break;
          }
        }
      }

      samples_top[validSamples] = sTop;
      samples_bottom[validSamples] = sBottom;
      samples_left[validSamples] = sLeft;
      samples_right[validSamples] = sRight;
      validSamples++;
      CLog::Log(LOGDEBUG, "DetectActiveArea: sample {}: T={} B={} L={} R={}", validSamples,
                sTop, sBottom, sLeft, sRight);
    }

    if (s_detectCacheStarved.load())
    {
      s_detectState.store(DV_DETECT_SKIPPED);
      goto cleanup;
    }
    if (validSamples == 0)
      goto cleanup;

    // Require enough coverage (at most 1 failed position).
    {
      const int minUsable = numSeeks - 1;
      if (validSamples < minUsable)
      {
        CLog::Log(LOGINFO, "DetectActiveArea: insufficient coverage ({}/{}) - skipping",
                  validSamples, numSeeks);
        s_detectState.store(DV_DETECT_SKIPPED);
        goto cleanup;
      }
    }

    detTop = pickBest(samples_top, validSamples);
    detBottom = pickBest(samples_bottom, validSamples);

    // T/B consensus (majority; symmetric fallback; corroboration; else IMAX bail).
    {
      int required = (validSamples + 1) / 2;
      if (required < 2)
        required = 2;
      int topSupport = countSupport(samples_top, validSamples, detTop);
      int botSupport = countSupport(samples_bottom, validSamples, detBottom);
      bool topOk = topSupport >= required;
      bool botOk = botSupport >= required;
      bool corroborated = !topOk && !botOk && topSupport >= 2 && botSupport >= 2 &&
                          std::abs((int)detTop - (int)detBottom) <= agreeTolerance;
      if (corroborated)
        detTop = detBottom = (topSupport >= botSupport) ? detTop : detBottom;
      else if (!topOk && !botOk && validSamples >= 3)
      {
        CLog::Log(LOGINFO, "DetectActiveArea: no T/B consensus - skipping");
        s_detectState.store(DV_DETECT_SKIPPED);
        goto cleanup;
      }
      else if (topOk && !botOk)
        detBottom = detTop;
      else if (botOk && !topOk)
        detTop = detBottom;
    }

    // Variable-AR guard: significant bars but any sample full-frame => IMAX, bail.
    if (detTop > 0 || detBottom > 0)
    {
      uint16_t minSignificant = static_cast<uint16_t>(lastHeight / 40);
      if (detTop >= minSignificant || detBottom >= minSignificant)
      {
        for (int i = 0; i < validSamples; i++)
        {
          if (samples_top[i] <= agreeTolerance || samples_bottom[i] <= agreeTolerance)
          {
            CLog::Log(LOGINFO, "DetectActiveArea: variable AR - skipping to avoid IMAX crop");
            s_detectState.store(DV_DETECT_SKIP_IMAX);
            goto cleanup;
          }
        }
      }
    }

    detLeft = pickBest(samples_left, validSamples);
    detRight = pickBest(samples_right, validSamples);

    // L/R: majority support AND symmetric, else drop.
    {
      int lrRequired = (validSamples + 1) / 2;
      if (lrRequired < 2)
        lrRequired = 2;
      int leftSupport = countSupport(samples_left, validSamples, detLeft);
      int rightSupport = countSupport(samples_right, validSamples, detRight);
      if (leftSupport < lrRequired || rightSupport < lrRequired ||
          (detLeft && detRight &&
           std::abs((int)detLeft - (int)detRight) > (int)std::max(detLeft, detRight) / 10))
        detLeft = detRight = 0;
    }

    // Validate and snap to a common aspect ratio.
    if (detTop || detBottom || detLeft || detRight)
    {
      const uint32_t activeW = lastWidth - detLeft - detRight;
      const uint32_t activeH = lastHeight - detTop - detBottom;
      if (activeW > 0 && activeH > 0)
      {
        const uint32_t arX1000 = (activeW * 1000) / activeH;
        const uint32_t frameAR = (lastWidth * 1000) / lastHeight;
        if (arX1000 >= 1200 && arX1000 <= 2900)
        {
          uint32_t bestAR = arX1000, bestDist = UINT32_MAX;
          for (auto ar : s_commonAR)
          {
            uint32_t dist = (arX1000 > ar) ? (arX1000 - ar) : (ar - arX1000);
            if (dist < bestDist)
            {
              bestDist = dist;
              bestAR = ar;
            }
          }
          if (bestDist * 100 > arX1000 * 5)
            bestAR = arX1000;
          if (bestAR >= frameAR)
          {
            uint32_t snapH = (lastWidth * 1000 + bestAR / 2) / bestAR;
            if (snapH > (uint32_t)lastHeight)
              snapH = lastHeight;
            uint16_t tb = static_cast<uint16_t>((lastHeight - snapH) / 2);
            detTop = tb;
            detBottom = tb;
            detLeft = 0;
            detRight = 0;
          }
          else
          {
            uint32_t snapW = (lastHeight * bestAR + 500) / 1000;
            if (snapW > (uint32_t)lastWidth)
              snapW = lastWidth;
            uint16_t lr = static_cast<uint16_t>((lastWidth - snapW) / 2);
            detLeft = lr;
            detRight = lr;
            detTop = 0;
            detBottom = 0;
          }
          CLog::Log(LOGINFO, "DetectActiveArea: {}x{} -> T={} B={} L={} R={} ({} samples)",
                    lastWidth, lastHeight, detTop, detBottom, detLeft, detRight, validSamples);
        }
        else
          detTop = detBottom = detLeft = detRight = 0;
      }
    }
  }

  s_detectedTop.store(detTop);
  s_detectedBottom.store(detBottom);
  s_detectedLeft.store(detLeft);
  s_detectedRight.store(detRight);
  s_detectState.store(DV_DETECT_OK);
  s_detectStable.store(true);
  if (detTop || detBottom || detLeft || detRight)
    CLog::Log(LOGINFO, "DetectActiveArea: L5 offsets ready T={} B={} L={} R={}", detTop,
              detBottom, detLeft, detRight);

cleanup:
  s_detectThrottleActive.store(false);
  if (s_detectCacheStarved.load() && s_detectState.load() == DV_DETECT_RUNNING)
    s_detectState.store(DV_DETECT_SKIPPED);
  if (s_detectState.load() == DV_DETECT_RUNNING)
    s_detectState.store(DV_DETECT_FAILED);
  if (frame)
    av_frame_free(&frame);
  if (pkt)
    av_packet_free(&pkt);
  if (codecCtx)
    avcodec_free_context(&codecCtx);
  if (fmtCtx)
    avformat_close_input(&fmtCtx);
  if (avioCtx)
    avio_context_free(&avioCtx);
}

void aml_dv_detect_set_file(const std::string& path)
{
  s_detectFilePath = path;
}

void aml_dv_detect_active_area_start()
{
  s_detectCancel.store(false);
  s_detectThrottleActive.store(false);
  s_detectCacheStarved.store(false);
  s_detectStable.store(false);
  s_detectState.store(DV_DETECT_FAILED);
  s_detectedTop.store(0);
  s_detectedBottom.store(0);
  s_detectedLeft.store(0);
  s_detectedRight.store(0);

  std::string filePath = s_detectFilePath;
  if (filePath.empty())
    filePath = g_application.CurrentFile(); // fallback
  if (filePath.empty())
  {
    CLog::Log(LOGWARNING, "DetectActiveArea: no file path available");
    return;
  }

  if (s_detectThread.joinable())
  {
    s_detectCancel.store(true);
    s_detectThread.join();
    s_detectCancel.store(false);
  }
  s_detectState.store(DV_DETECT_RUNNING);
  s_detectThread = std::thread([filePath]() { DetectActiveAreaFromFile(filePath); });
}

void aml_dv_detect_active_area_stop()
{
  s_detectCancel.store(true);
  s_detectStable.store(false);
  s_detectState.store(DV_DETECT_FAILED);
  if (s_detectThread.joinable())
    s_detectThread.join();
}

bool aml_video_started()
{
  CSysfsPath videostarted{"/sys/class/tsync/videostarted"};
  return (StringUtils::EqualsNoCase(videostarted.Get<std::string>().value(), "0x1"));
}

int aml_amdv_wait(StreamHdrType hdrType)
{
  if (hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    CSysfsPath amdv_wait_delay{"/sys/module/aml_media/parameters/amdv_wait_delay"};
    return amdv_wait_delay.Get<int>().value();
  }
  else
    return 0;
}

void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode)
{
  int fd;
  if ((fd = open("/dev/amvideo", O_RDWR)) >= 0)
  {
    if (ioctl(fd, AMSTREAM_IOC_SET_3D_TYPE, mode) != 0)
      CLog::Log(LOGERROR, "AMLUtils::{} - unable to set 3D video mode 0x%x", __FUNCTION__, mode);
    close(fd);

    CSysfsPath("/sys/module/aml_media/parameters/g_framepacking_support", framepacking_support ? 1 : 0);
    CSysfsPath("/sys/module/amvdec_h264mvc/parameters/view_mode", view_mode);
  }
}
