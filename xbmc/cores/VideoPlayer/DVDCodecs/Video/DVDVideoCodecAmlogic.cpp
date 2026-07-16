/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <math.h>

#include "DVDCodecs/DVDFactoryCodec.h"
#include "utils/MemUtils.h"
#include "DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "DVDStreamInfo.h"
#include "AMLCodec.h"
#include "ServiceBroker.h"
#include "utils/AMLUtils.h"
#include "utils/HDRCapabilities.h"
#include "utils/log.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/Thread.h"
#include "windowing/WinSystem.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define __MODULE_NAME__ "DVDVideoCodecAmlogic"

namespace
{
// Decode the display's Dolby Vision VSVDB (from sysfs) target max luminance, in
// nits, for the Smart CMv4.0 bypass default. Supports VSVDB version 2 (the
// common HDMI DV case); returns 0 if unavailable/unsupported so the caller
// keeps the manual override behaviour.
int GetDisplayVsvdbMaxNits()
{
  // VSVDB v2 5-bit "Maximum Luminance (PQ)" index -> nits (Dolby table).
  static const int lut_v2[32] = {
      96,   113,  132,  155,  181,  211,  245,  285,  332,  385,  447,
      518,  601,  696,  807,  934,  1082, 1252, 1450, 1678, 1943, 2250,
      2607, 3020, 3501, 4060, 4710, 5467, 6351, 7382, 8588, 10000};

  std::ifstream f("/sys/class/amhdmitx/amhdmitx0/dv_cap");
  if (!f.is_open())
    return 0;
  const std::string cap((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const std::string tag = "VSVDB: ";
  const size_t p = cap.find(tag);
  if (p == std::string::npos)
    return 0;
  const size_t start = p + tag.size();
  const size_t end = cap.find_first_of(" \t\r\n", start);
  const std::string hex = cap.substr(start, (end == std::string::npos ? cap.size() : end) - start);
  std::vector<uint8_t> b;
  for (size_t i = 0; i + 1 < hex.size(); i += 2)
  {
    try
    {
      b.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    catch (...)
    {
      return 0;
    }
  }
  // DV payload starts at b[5] (after 0xEB, ext-tag 0x01, 3-byte IEEE OUI).
  if (b.size() < 8)
    return 0;
  const int version = (b[5] >> 5) & 0x07;
  if (version != 2)
    return 0;
  const int idx = (b[7] >> 3) & 0x1F; // Maximum Luminance (PQ) index
  return lut_v2[idx];
}
} // namespace

CAMLVideoBufferPool::~CAMLVideoBufferPool()
{
  CLog::Log(LOGDEBUG, "CAMLVideoBufferPool::~CAMLVideoBufferPool: Deleting {:d} buffers", static_cast<unsigned int>(m_videoBuffers.size()) );
  for (auto buffer : m_videoBuffers)
    delete buffer;
}

CVideoBuffer* CAMLVideoBufferPool::Get()
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);

  if (m_freeBuffers.empty())
  {
    m_freeBuffers.push_back(m_videoBuffers.size());
    m_videoBuffers.push_back(new CAMLVideoBuffer(static_cast<int>(m_videoBuffers.size())));
  }
  int bufferIdx(m_freeBuffers.back());
  m_freeBuffers.pop_back();

  m_videoBuffers[bufferIdx]->Acquire(shared_from_this());

  return m_videoBuffers[bufferIdx];
}

void CAMLVideoBufferPool::Return(int id)
{
  std::unique_lock<CCriticalSection> lock(m_criticalSection);
  if (m_videoBuffers[id]->m_amlCodec)
  {
    m_videoBuffers[id]->m_amlCodec->ReleaseFrame(m_videoBuffers[id]->m_bufferIndex, true);
    m_videoBuffers[id]->m_amlCodec = nullptr;
  }
  m_freeBuffers.push_back(id);
}

/***************************************************************************/

CDVDVideoCodecAmlogic::CDVDVideoCodecAmlogic(CProcessInfo &processInfo)
  : CDVDVideoCodec(processInfo)
  , m_pFormatName("amcodec")
  , m_opened(false)
  , m_codecControlFlags(0)
  , m_framerate(0.0)
  , m_video_rate(0)
  , m_mpeg2_sequence(NULL)
  , m_h264_sequence(NULL)
  , m_has_keyframe(false)
  , m_bitparser(NULL)
  , m_bitstream(NULL)
{
}

CDVDVideoCodecAmlogic::~CDVDVideoCodecAmlogic()
{
  Close();
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecAmlogic::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecAmlogic>(processInfo);
}

bool CDVDVideoCodecAmlogic::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("amlogic_dec", CDVDVideoCodecAmlogic::Create);
  return true;
}

bool CDVDVideoCodecAmlogic::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEAMCODEC))
    return false;
  if ((hints.stills && hints.fpsrate == 0) || hints.width == 0)
    return false;

  // close open decoder if necessary
  if (m_opened)
    Close();

  m_hints = hints;
  m_hints.pClock = hints.pClock;

  CLog::Log(LOGDEBUG, "{}::{} - codec {:d} profile:{:d} extra_size:{:d} fps:{:d}/{:d}",
    __MODULE_NAME__, __FUNCTION__, m_hints.codec, m_hints.profile, m_hints.extradata.GetSize(), m_hints.fpsrate, m_hints.fpsscale);

  switch(m_hints.codec)
  {
    case AV_CODEC_ID_MJPEG:
      m_pFormatName = "am-mjpeg";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG2))
        goto FAIL;

      switch(m_hints.profile)
      {
        case AV_PROFILE_MPEG2_422:
          CLog::Log(LOGDEBUG, "{}: MPEG2 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 720 || m_hints.width == 544 || m_hints.width == 480) && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      m_mpeg2_sequence_pts = 0;
      m_mpeg2_sequence = new mpeg2_sequence;
      m_mpeg2_sequence->width  = m_hints.width;
      m_mpeg2_sequence->height = m_hints.height;
      m_mpeg2_sequence->ratio  = m_hints.aspect;
      m_mpeg2_sequence->fps_rate  = m_hints.fpsrate;
      m_mpeg2_sequence->fps_scale  = m_hints.fpsscale;
      m_pFormatName = "am-mpeg2";
      break;
    case AV_CODEC_ID_H264:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264))
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::h264 size check failed {:d}",CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECH264));
        goto FAIL;
      }
      switch(hints.profile)
      {
        case AV_PROFILE_H264_HIGH_10:
        case AV_PROFILE_H264_HIGH_10_INTRA:
        case AV_PROFILE_H264_HIGH_422:
        case AV_PROFILE_H264_HIGH_422_INTRA:
        case AV_PROFILE_H264_HIGH_444_PREDICTIVE:
        case AV_PROFILE_H264_HIGH_444_INTRA:
        case AV_PROFILE_H264_CAVLC_444:
          CLog::Log(LOGDEBUG, "{}: H264 unsupported hints.profile({:d})", __MODULE_NAME__, m_hints.profile);
          goto FAIL;
      }
      if ((aml_support_h264_4k2k() == AML_NO_H264_4K2K) && ((m_hints.width > 1920) || (m_hints.height > 1088)))
      {
        CLog::Log(LOGDEBUG, "{}::{} - 4K H264 is supported only on Amlogic S802 and S812 chips or newer", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }

      if (m_hints.aspect == 0.0)
      {
        m_h264_sequence_pts = 0;
        m_h264_sequence = new h264_sequence;
        m_h264_sequence->width  = m_hints.width;
        m_h264_sequence->height = m_hints.height;
        m_h264_sequence->ratio  = m_hints.aspect;
      }

      if (m_hints.codec_tag == MKTAG('M', 'V', 'C', '1'))
        m_pFormatName = "am-h264mvc";
      else
        m_pFormatName = "am-h264";
      // convert h264-avcC to h264-annex-b as h264-avcC
      // under streamers can have issues when seeking.
      if (m_hints.extradata && m_hints.extradata.GetData()[0] == 1)
      {
        m_bitstream = new CBitstreamConverter;
        m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);
        m_bitstream->ResetStartDecode();
        // make sure we do not leak the existing m_hints.extradata
        m_hints.extradata = {};
        m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
        memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      }
      else
      {
        m_bitparser = new CBitstreamParser();
        m_bitparser->Open();
      }

      // if we have SD PAL content assume it is widescreen
      // correct aspect ratio will be detected later anyway
      if (m_hints.width == 720 && m_hints.height == 576 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;

      // assume widescreen for "HD Lite" channels
      // correct aspect ratio will be detected later anyway
      if ((m_hints.width == 1440 || m_hints.width ==1280) && m_hints.height == 1080 && m_hints.aspect == 0.0)
          m_hints.aspect = 16.0 / 9.0;;

      break;
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:
      if (m_hints.width <= CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_USEAMCODECMPEG4))
        goto FAIL;
      m_pFormatName = "am-mpeg4";
      break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
      // amcodec can't handle h263
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support H263", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
//    case AV_CODEC_ID_FLV1:
//      m_pFormatName = "am-flv1";
//      break;
    case AV_CODEC_ID_RV10:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
      // m_pFormatName = "am-rv";
      // rmvb is not handled well by amcodec
      CLog::Log(LOGDEBUG, "{}::{} - amcodec does not support RMVB", __MODULE_NAME__, __FUNCTION__);
      goto FAIL;
    case AV_CODEC_ID_VC1:
      m_pFormatName = "am-vc1";
      break;
    case AV_CODEC_ID_WMV3:
      m_pFormatName = "am-wmv3";
      break;
    case AV_CODEC_ID_AVS:
    case AV_CODEC_ID_CAVS:
      m_pFormatName = "am-avs";
      break;
    case AV_CODEC_ID_AVS2:
      if (!aml_support_avs2())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AVS2 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-avs2";
      break;
    case AV_CODEC_ID_AVS3:
      if (!aml_support_avs3())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AVS3 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-avs3";
      break;
    case AV_CODEC_ID_VP9:
      if (!aml_support_vp9())
      {
        CLog::Log(LOGDEBUG, "{}::{} - VP9 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-vp9";
      break;
    case AV_CODEC_ID_AV1:
      if (!aml_support_av1())
      {
        CLog::Log(LOGDEBUG, "{}::{} - AV1 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-av1";
      break;
    case AV_CODEC_ID_HEVC:
      if (aml_support_hevc()) {
        if (!aml_support_hevc_8k4k() && ((m_hints.width > 4096) || (m_hints.height > 2176)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 8K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        } else if (!aml_support_hevc_4k2k() && ((m_hints.width > 1920) || (m_hints.height > 1088)))
        {
          CLog::Log(LOGDEBUG, "{}::{} - 4K HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
          goto FAIL;
        }
      } else {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      if ((hints.profile == AV_PROFILE_HEVC_MAIN_10) && !aml_support_hevc_10bit())
      {
        CLog::Log(LOGDEBUG, "{}::{} - HEVC 10-bit hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-h265";
      m_bitstream = new CBitstreamConverter();
      m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);

      // check for hevc-hvcC and convert to h265-annex-b
      if (m_hints.extradata && !m_hints.cryptoSession)
      {
        if (aml_support_dolby_vision())
        {
          bool user_dv_disable = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
              CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);

          // Dolby Vision L5 active-area (letterbox) mode: Source / Zero / Auto-
          // detect. Applies in both LED modes (the DV core masks bars from the RPU
          // L5 regardless), only for a real DV RPU stream (profile 5/7/8).
          //  - Hard-cropped (non-16:9 coded frame, e.g. 3840x1600): the display
          //    scaler adds bars the source RPU L5 can't describe. Derive them
          //    geometrically and inject them - in Source AND Auto (not Zero).
          //  - 16:9 coded frame + Auto: background luma-scan for baked-in bars.
          if (!user_dv_disable)
          {
            const auto dvsettings = CServiceBroker::GetSettingsComponent()->GetSettings();
            const int l5mode = dvsettings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_MODE);
            m_bitstream->SetDoviL5Mode(l5mode);
            m_bitstream->SetDoviL5OsdUnmask(
                dvsettings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_L5_OSD_UNMASK));
            const bool realDV = (m_hints.dovi.dv_profile == 5 || m_hints.dovi.dv_profile == 7 ||
                                 m_hints.dovi.dv_profile == 8);
            const int cw = m_hints.width, ch = m_hints.height;
            const bool preCropped = realDV && cw > 0 && ch > 0 &&
                                    ((cw * 9 / 16 > ch + 40) || (ch * 16 / 9 > cw + 40));
            if (l5mode != 1 /*Zero*/ && preCropped)
            {
              aml_dv_detect_active_area_stop();          // no scan needed
              aml_dv_set_geometric_active_area(cw, ch);  // synchronous geometric offsets
              m_bitstream->SetDoviL5Geometric(true);
            }
            else
            {
              m_bitstream->SetDoviL5Geometric(false);
              if (l5mode == 2 /*Auto*/ && realDV)
                aml_dv_detect_active_area_start();
              else
                aml_dv_detect_active_area_stop();
            }
          }

          if ((m_hints.dovi.dv_profile == 4 || m_hints.dovi.dv_profile == 7) && !user_dv_disable &&
               aml_get_cpufamily_id() == AML_S5)
          {
            CLog::Log(LOGINFO, "{}::{} - HEVC bitstream profile {} will be converted to profile 8.1", __MODULE_NAME__, __FUNCTION__,
              m_hints.dovi.dv_profile);

            m_hints.dovi.dv_profile = 8;
            m_hints.dovi.el_present_flag = false;
            m_bitstream->SetConvertDovi(true);
          }
          else if (m_hints.dovi.dv_profile == 7 && !user_dv_disable && m_hints.stills)
          {
            // Arm profile-7 MEL -> 8.1 flattening (see SetConvertMel), SCOPED TO
            // disc-menu playback (hints.stills, set only while IsInMenu). BD menu
            // loops close/reopen the decoder every playitem, and a MEL dual-layer
            // decoder reopened mid-session judders (drops frames) where a fresh
            // open is smooth; flattening to single-layer 8.1 makes each reopen a
            // clean single-layer open. Deliberately NOT applied to file/normal
            // playback (e.g. MEL MKVs), which never mid-session reopen and play
            // fine as dual-layer - no need to alter a working path.
            // MEL vs FEL is confirmed from the first parsed RPU, at which point
            // the conversion self-activates and the hints are corrected below
            // (before OpenDecoder). FEL stays dual-layer.
            m_bitstream->SetConvertMel(true);
          }

          // Smart CMv4.0 append: push mode + smart-bypass inputs to the
          // bitstream (read once at stream open, like the DV settings above).
          if (!user_dv_disable)
          {
            const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
            const int cmv40 = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_APPEND);
            if (static_cast<DOVICMv40Mode>(cmv40) == CMV40_SMART)
            {
              // Display peak nits for the Smart bypass threshold. The same
              // display.maxnits value also drives the VSVDB force-inject (see
              // aml_dv_apply_vsvdb), so the Smart threshold and the peak the amdv
              // core tone-maps to stay consistent by construction. 0 = auto-read
              // the display's real VSVDB (EDID) max luminance and fill the field.
              int nits = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS);
              if (nits <= 0)
              {
                nits = GetDisplayVsvdbMaxNits();
                if (nits > 0)
                  settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISPLAY_MAXNITS, nits);
              }
              // set the bypass inputs BEFORE the mode (SetAppendCMv40 resets the sentinel)
              m_bitstream->SetSmartBypassDisplayNits(nits);
              m_bitstream->SetSmartBypassThresholdPct(
                  settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_CMV40_SMART_THRESHOLD));
            }
            m_bitstream->SetAppendCMv40(static_cast<DOVICMv40Mode>(cmv40));
          }
        }
      }

      // make sure we do not leak the existing m_hints.extradata
      m_hints.extradata = {};
      m_hints.extradata = FFmpegExtraData(m_bitstream->GetExtraSize());
      memcpy(m_hints.extradata.GetData(), m_bitstream->GetExtraData(), m_hints.extradata.GetSize());
      break;
    case AV_CODEC_ID_VVC:
      if (!aml_support_h266())
      {
        CLog::Log(LOGDEBUG, "{}::{} - H266 hardward decoder is not supported on current platform", __MODULE_NAME__, __FUNCTION__);
        goto FAIL;
      }
      m_pFormatName = "am-h266";
      m_bitstream = new CBitstreamConverter();
      m_bitstream->Open(m_hints.codec, m_hints.extradata.GetData(), m_hints.extradata.GetSize(), true);
      if (m_hints.extradata.GetSize() == 0)
        m_bitstream->ResetStartDecode();
      break;
    default:
      CLog::Log(LOGDEBUG, "{}: Unknown hints.codec({:d})", __MODULE_NAME__, m_hints.codec);
      goto FAIL;
  }

  m_aspect_ratio = m_hints.aspect;

  m_Codec = std::shared_ptr<CAMLCodec>(new CAMLCodec(m_processInfo));
  if (!m_Codec)
  {
    CLog::Log(LOGERROR, "{}: Failed to create Amlogic Codec", __MODULE_NAME__);
    goto FAIL;
  }

  // allocate a dummy VideoPicture buffer.
  m_videobuffer.Reset();

  m_videobuffer.iWidth  = m_hints.width;
  m_videobuffer.iHeight = m_hints.height;

  m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
  m_videobuffer.iDisplayHeight = m_videobuffer.iHeight;
  if (m_hints.aspect > 0.0 && !m_hints.forced_aspect)
  {
    m_videobuffer.iDisplayWidth  = ((int)lrint(m_videobuffer.iHeight * m_hints.aspect)) & ~3;
    if (m_videobuffer.iDisplayWidth > m_videobuffer.iWidth)
    {
      m_videobuffer.iDisplayWidth  = m_videobuffer.iWidth;
      m_videobuffer.iDisplayHeight = ((int)lrint(m_videobuffer.iWidth / m_hints.aspect)) & ~3;
    }
  }

  m_videobuffer.hdrType = m_hints.hdrType;
  m_videobuffer.color_space = m_hints.colorSpace;
  m_videobuffer.color_primaries = m_hints.colorPrimaries;
  m_videobuffer.color_transfer = m_hints.colorTransferCharacteristic;

  m_processInfo.SetVideoDecoderName(m_pFormatName, true);
  m_processInfo.SetVideoDimensions(m_hints.width, m_hints.height);
  m_processInfo.SetVideoDeintMethod("hardware");
  m_processInfo.SetVideoDAR(m_hints.aspect);

  m_has_keyframe = false;

  if (m_bitstream)
  {
    const CHDRCapabilities caps = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
    if (!caps.SupportsHDR10Plus())
      m_bitstream->SetRemoveHdr10Plus(true);
    // Strip the DoVi RPU only when the DV core will NOT process this stream.
    // When VS10 engages the core on a non-DV display (aml_dv_core_active()), the
    // RPU must survive so the core can reconstruct FEL and tone-map to HDR10/SDR.
    if (caps.SupportsDolbyVision() == DolbyVisionFormat::DOLBYVISION_TYPE_NONE &&
        m_hints.dovi.dv_profile != 5 && !aml_dv_core_active())
      m_bitstream->SetRemoveDovi(true);

    // Non-DV source routed through the VS10 engine (dv_profile == 0): strip
    // HDR10+ dynamic metadata (VS10 consumes only static HDR10) and any stray
    // DoVi RPU so they can't conflict with the forced VS10 conversion. Native DV
    // streams (dv_profile != 0) are untouched.
    if (m_hints.dovi.dv_profile == 0 &&
        aml_dv_get_vs10_pending() != DOLBY_VISION_OUTPUT_MODE_BYPASS)
    {
      // Note: this discards HDR10+ dynamic metadata. It only fires when a VS10
      // per-source mode is set to non-bypass; the shipped defaults are bypass so
      // HDR10+ passes through untouched unless the user opts in.
      CLog::Log(LOGINFO, "{}: VS10 engaged (pending mode {}) on non-DV source - "
                         "stripping HDR10+/DoVi dynamic metadata", __MODULE_NAME__,
                aml_dv_get_vs10_pending());
      m_bitstream->SetRemoveHdr10Plus(true);
      m_bitstream->SetRemoveDovi(true);
    }
  }

  CLog::Log(LOGINFO, "{}: Opened Amlogic Codec", __MODULE_NAME__);
  return true;
FAIL:
  Close();
  return false;
}

void CDVDVideoCodecAmlogic::Close(void)
{
  CLog::Log(LOGDEBUG, "{}::{}", __MODULE_NAME__, __FUNCTION__);

  // Stop any in-flight L5 active-area detection thread.
  aml_dv_detect_active_area_stop();

  m_videoBufferPool = nullptr;

  if (m_Codec)
    m_Codec->CloseDecoder(), m_Codec = nullptr;

  m_videobuffer.iFlags = 0;

  if (m_mpeg2_sequence)
    delete m_mpeg2_sequence, m_mpeg2_sequence = NULL;
  if (m_h264_sequence)
    delete m_h264_sequence, m_h264_sequence = NULL;

  if (m_bitstream)
    delete m_bitstream, m_bitstream = NULL;

  if (m_bitparser)
    delete m_bitparser, m_bitparser = NULL;

  m_opened = false;
}

bool CDVDVideoCodecAmlogic::AddData(const DemuxPacket &packet)
{
  // Handle Input, add demuxer packet to input queue, we must accept it or
  // it will be discarded as VideoPlayerVideo has no concept of "try again".

  uint8_t *pData(packet.pData);
  uint32_t iSize(packet.iSize);
  bool doviIsFEL = false;
  bool IsHdr10Plus = false;
  int data_added = false;
  bool dual_layer_converted = false;

  if (pData)
  {
    if (m_bitstream)
    {
      // Push the latest detected L5 active-area offsets to the bitstream; the
      // background detector may finish a few seconds into playback. Only DOVI_L5_
      // DETECT mode consumes them (cheap atomic reads otherwise).
      {
        uint16_t l5t, l5b, l5l, l5r;
        const bool l5valid = aml_dv_detect_active_area_get(l5t, l5b, l5l, l5r);
        m_bitstream->SetDoviL5DetectedOffsets(l5valid, l5t, l5b, l5l, l5r);
        // osdst: refresh overlay (OSD/subtitle) visibility for the L5 un-mask.
        m_bitstream->SetDoviL5OverlayVisible(aml_dv_l5_overlay_visible());
      }

      // Merge BL+EL whenever the DV core will actually run -- including the VS10
      // path on a non-DV display, so profile 7 FEL titles are reconstructed
      // (BL+EL+RPU) and tone-mapped by VS10 rather than played base-layer-only.
      if (packet.isDualStream && aml_dv_core_active())
      {
        CLog::Log(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic::{}: {} package with dts: {:.3f}, pts: {:.3f} and size {} arrived, list {} empty", __FUNCTION__,
          packet.isELPackage ? "EL" : "BL", packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, iSize, m_packages.empty() ? "is" : "is not");

        if (!m_packages.empty())
        {
          // convert bl and el package to single package
          DLDemuxPacket dual_layer_packet = m_packages.front();
          uint8_t *pDataBackup = std::get<0>(dual_layer_packet);
          uint32_t iSizeBackup = std::get<1>(dual_layer_packet);
          bool isELPackageBackup = std::get<2>(dual_layer_packet);

          if (isELPackageBackup != packet.isELPackage)
          {
            if (!packet.isELPackage)
            {
              CLog::Log(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic::{}: found EL package with dts: {:.3f}, pts: {:.3f} and size {} in list", __FUNCTION__,
                packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, iSizeBackup);
              dual_layer_converted = m_bitstream->Convert(pData, iSize, pDataBackup, iSizeBackup);
            }
            else
            {
              CLog::Log(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic::{}: found BL package with dts: {:.3f}, pts: {:.3f} and size {} in list", __FUNCTION__,
                packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, iSizeBackup);
              dual_layer_converted = m_bitstream->Convert(pDataBackup, iSizeBackup, pData, iSize);
            }
          }
        }

        if (!dual_layer_converted)
        {
          // backup package and don't send to decoder yet
          uint8_t *pDataBackup = static_cast<uint8_t*>(KODI::MEMORY::AlignedMalloc(packet.iSize + AV_INPUT_BUFFER_PADDING_SIZE, 16));
          memcpy(pDataBackup, packet.pData, packet.iSize);
          m_packages.push_back(std::make_tuple(pDataBackup, iSize, packet.isELPackage));
          CLog::Log(LOGDEBUG, LOGVIDEO, "CDVDVideoCodecAmlogic::{}: did add {} package with dts: {:.3f}, pts: {:.3f} and size {} in list", __FUNCTION__,
            packet.isELPackage ? "EL" : "BL", packet.dts/DVD_TIME_BASE, packet.pts/DVD_TIME_BASE, packet.iSize);

          return true;
        }
      }
      else
      {
        if (!m_bitstream->Convert(pData, iSize))
          return true;
      }

      if (!m_bitstream->CanStartDecode())
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: waiting for keyframe (bitstream)", __FUNCTION__);
        return true;
      }
      pData = m_bitstream->GetConvertBuffer();
      iSize = m_bitstream->GetConvertSize();
      doviIsFEL = m_bitstream->GetDoviIsFEL();
      IsHdr10Plus = m_bitstream->GetIsHdrPlus();
    }
    else if (!m_has_keyframe && m_bitparser)
    {
      if (!m_bitparser->CanStartDecode(pData, iSize))
      {
        CLog::Log(LOGDEBUG, "CDVDVideoCodecAmlogic::{}: waiting for keyframe (bitparser)", __FUNCTION__);
        return true;
      }
      else
        m_has_keyframe = true;
    }
    FrameRateTracking( pData, iSize, packet.dts, packet.pts);

    if (!m_opened)
    {
      if (packet.pts == DVD_NOPTS_VALUE)
        m_hints.ptsinvalid = true;

      // Profile-7 MEL flattening self-activated during the first RPU parse
      // (the EL is now dropped and the RPU rewritten to 8.1): correct the hints
      // so the decoder opens as single-layer 8.1 rather than dual-layer MEL.
      if (m_bitstream && m_bitstream->GetDoviMelConverted() && m_hints.dovi.dv_profile == 7)
      {
        CLog::Log(LOGINFO, "CDVDVideoCodecAmlogic::{}: profile 7 MEL flattened to 8.1", __FUNCTION__);
        m_hints.dovi.dv_profile = 8;
        m_hints.dovi.el_present_flag = false;
      }

      m_processInfo.SetDoviIsFEL(doviIsFEL);
      m_processInfo.SetIsHdr10Plus(IsHdr10Plus);

      CLog::Log(LOGINFO, "CDVDVideoCodecAmlogic::{}: Open decoder: fps:{:d}/{:d}", __FUNCTION__, m_hints.fpsrate, m_hints.fpsscale);
      if (m_Codec && !m_Codec->OpenDecoder(m_hints, doviIsFEL))
        CLog::Log(LOGERROR, "CDVDVideoCodecAmlogic::{}: Failed to open Amlogic Codec", __FUNCTION__);

      m_videoBufferPool = std::shared_ptr<CAMLVideoBufferPool>(new CAMLVideoBufferPool());

      m_opened = true;
    }
  }

  if (packet.pSideData && packet.iSideDataElems > 0)
  {
    const AVPacketSideData* sideData = av_packet_side_data_get(static_cast<AVPacketSideData*>(packet.pSideData),
                                                               packet.iSideDataElems,
                                                               AV_PKT_DATA_DYNAMIC_HDR10_PLUS_RAW);

    if (sideData && sideData->size)
    {
      if (m_Codec->AddHDR10PData(sideData->data, sideData->size) < 0)
        CLog::Log(LOGWARNING, "CDVDVideoCodecAmlogic::{}: failed to set hdr10p data with size {}", __FUNCTION__,
          sideData->size);
    }
  }

  data_added = m_Codec->AddData(pData, iSize, packet.dts, m_hints.ptsinvalid ? DVD_NOPTS_VALUE : packet.pts);

  // pop package only from list if hardware decoder did accept the data
  if (data_added && dual_layer_converted)
  {
    DLDemuxPacket dual_layer_packet= m_packages.front();
    uint8_t *pDataBackup = std::get<0>(dual_layer_packet);
    KODI::MEMORY::AlignedFree(pDataBackup);
    m_packages.pop_front();
  }

  return data_added;
}

void CDVDVideoCodecAmlogic::Reset(void)
{
  m_Codec->Reset();

  while (!m_packages.empty())
  {
    DLDemuxPacket dual_layer_packet= m_packages.front();
    uint8_t *pDataBackup = std::get<0>(dual_layer_packet);
    KODI::MEMORY::AlignedFree(pDataBackup);
    m_packages.pop_front();
  }

  m_mpeg2_sequence_pts = 0;
  m_has_keyframe = false;
  if (m_bitstream)
  {
    switch(m_hints.codec)
    {
      case AV_CODEC_ID_VVC:
        if (m_hints.extradata.GetSize() > 0)
          break;
        [[fallthrough]];
      case AV_CODEC_ID_H264:
        m_bitstream->ResetStartDecode();
        break;
      default:
        break;
    }
  }
}

CDVDVideoCodec::VCReturn CDVDVideoCodecAmlogic::GetPicture(VideoPicture* pVideoPicture)
{
  if (!m_Codec)
    return VC_ERROR;

  VCReturn retVal = m_Codec->GetPicture(&m_videobuffer);

  if (retVal == VC_PICTURE)
  {
    if (pVideoPicture->videoBuffer)
      pVideoPicture->videoBuffer->Release();
    pVideoPicture->videoBuffer = nullptr;
    pVideoPicture->SetParams(m_videobuffer);

    pVideoPicture->videoBuffer = m_videoBufferPool->Get();
    static_cast<CAMLVideoBuffer*>(pVideoPicture->videoBuffer)->Set(this, m_Codec,
     m_Codec->GetOMXPts(), m_Codec->GetAmlDuration(), m_Codec->GetBufferIndex());;
  }

  // check for mpeg2 aspect ratio changes
  if (m_mpeg2_sequence && pVideoPicture->pts >= m_mpeg2_sequence_pts)
    m_aspect_ratio = m_mpeg2_sequence->ratio;

  // check for h264 aspect ratio changes
  if (m_h264_sequence && pVideoPicture->pts >= m_h264_sequence_pts)
    m_aspect_ratio = m_h264_sequence->ratio;

  pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
  pVideoPicture->iDisplayHeight = pVideoPicture->iHeight;
  if (m_aspect_ratio > 1.0f && !m_hints.forced_aspect)
  {
    pVideoPicture->iDisplayWidth  = ((int)lrint(pVideoPicture->iHeight * m_aspect_ratio)) & ~3;
    if (pVideoPicture->iDisplayWidth > pVideoPicture->iWidth)
    {
      pVideoPicture->iDisplayWidth  = pVideoPicture->iWidth;
      pVideoPicture->iDisplayHeight = ((int)lrint(pVideoPicture->iWidth / m_aspect_ratio)) & ~3;
    }
  }

  return retVal;
}

void CDVDVideoCodecAmlogic::SetCodecControl(int flags)
{
  if (m_codecControlFlags != flags)
  {
    CLog::Log(LOGDEBUG, LOGVIDEO, "{} {:x}->{:x}",  __func__, m_codecControlFlags, flags);
    m_codecControlFlags = flags;

    if (flags & DVD_CODEC_CTRL_DROP)
      m_videobuffer.iFlags |= DVP_FLAG_DROPPED;
    else
      m_videobuffer.iFlags &= ~DVP_FLAG_DROPPED;

    if (m_Codec)
      m_Codec->SetDrain((flags & DVD_CODEC_CTRL_DRAIN) != 0);
  }
}

int CDVDVideoCodecAmlogic::GetDataLevel() const
{
  if (m_Codec)
  {
    int data_len, free_len, size;
    return static_cast<int>(m_Codec->GetBufferLevel(0, data_len, free_len, size));
  }

  return 0;
}

void CDVDVideoCodecAmlogic::SetSpeed(int iSpeed)
{
  if (m_Codec)
    m_Codec->SetSpeed(iSpeed);
}

void CDVDVideoCodecAmlogic::FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts)
{
  // mpeg2 handling
  if (m_mpeg2_sequence)
  {
    // probe demux for sequence_header_code NAL and
    // decode aspect ratio and frame rate.
    if (CBitstreamConverter::mpeg2_sequence_header(pData, iSize, m_mpeg2_sequence) &&
       (m_mpeg2_sequence->fps_rate > 0) && (m_mpeg2_sequence->fps_scale > 0))
    {
      if (!m_mpeg2_sequence->fps_scale || !m_mpeg2_sequence->fps_scale)
        return;

      m_mpeg2_sequence_pts = pts;
      if (m_mpeg2_sequence_pts == DVD_NOPTS_VALUE)
        m_mpeg2_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}::{} fps:{:d}/{:d} mpeg2_fps:{:d}/{:d} options:0x{:2x}", __MODULE_NAME__, __FUNCTION__,
              m_hints.fpsrate, m_hints.fpsscale, m_mpeg2_sequence->fps_rate, m_mpeg2_sequence->fps_scale, m_hints.codecOptions);
      if  (!(m_hints.codecOptions & CODEC_INTERLACED))
      {
        m_hints.fpsrate = m_mpeg2_sequence->fps_rate;
        m_hints.fpsscale = m_mpeg2_sequence->fps_scale;
      }
      if (m_hints.fpsrate && m_hints.fpsscale)
      {
        m_framerate = static_cast<float>(m_hints.fpsrate) / m_hints.fpsscale;
        if (m_hints.codecOptions & CODEC_UNKNOWN_I_P)
          if (std::abs(m_framerate - 25.0) < 0.02 || std::abs(m_framerate - 29.97) < 0.02)
          {
            m_framerate += m_framerate;
            m_hints.fpsrate += m_hints.fpsrate;
          }
        m_video_rate = (int)(0.5 + (96000.0 / m_framerate));
      }
      m_hints.width    = m_mpeg2_sequence->width;
      m_hints.height   = m_mpeg2_sequence->height;
      m_hints.aspect   = m_mpeg2_sequence->ratio;

      m_processInfo.SetVideoFps(m_framerate);
      m_processInfo.SetVideoDAR(m_hints.aspect);
    }
    return;
  }

  // h264 aspect ratio handling
  if (m_h264_sequence)
  {
    // probe demux for SPS NAL and decode aspect ratio
    if (CBitstreamConverter::h264_sequence_header(pData, iSize, m_h264_sequence))
    {
      m_h264_sequence_pts = pts;
      if (m_h264_sequence_pts == DVD_NOPTS_VALUE)
          m_h264_sequence_pts = dts;

      CLog::Log(LOGDEBUG, "{}: detected h264 aspect ratio({:f})",
        __MODULE_NAME__, m_h264_sequence->ratio);
      m_hints.width    = m_h264_sequence->width;
      m_hints.height   = m_h264_sequence->height;
      m_hints.aspect   = m_h264_sequence->ratio;
    }
  }
}
