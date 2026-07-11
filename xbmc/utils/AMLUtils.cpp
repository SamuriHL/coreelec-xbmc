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
