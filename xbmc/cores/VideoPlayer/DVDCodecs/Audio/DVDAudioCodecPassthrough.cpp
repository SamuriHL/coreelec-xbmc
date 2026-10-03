/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  The "LAV Audio" passthrough A/V sync path (m_lavStyleSyncEnabled) is derived
 *  from LAV Filters by Hendrik Leppkes (Nevcairiel):
 *  https://github.com/Nevcairiel/LAVFilters
 *  It is always on for normal playback; the only exclusion is realtime/PVR
 *  streams (see VideoPlayerAudio, which enables it via !IsRealtimeStream()).
 */

#include "DVDAudioCodecPassthrough.h"

#include "DVDCodecs/DVDCodecs.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
constexpr auto TRUEHD_BUF_SIZE = 61440;

// A valid PTS must be >= 0 and within a sane range. During seamless branching
// the demuxer can emit DVD_NOPTS_VALUE (which becomes ~1.8e19 as a double) or
// other garbage; this rejects those without rejecting real timestamps.
constexpr double MAX_REASONABLE_PTS = 86400000000.0; // 24 hours in DVD_TIME_BASE units
inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}
} // namespace

CDVDAudioCodecPassthrough::CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType) :
  CDVDAudioCodec(processInfo)
{
  m_format.m_streamInfo.m_type = streamType;
  m_deviceIsRAW = processInfo.WantsRawPassthrough();

  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->RegisterCallback(this, {CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_TRUEHD_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM});
  }

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->UnregisterCallback(this);
  }

  Dispose();
}

void CDVDAudioCodecPassthrough::SetLavStyleSyncEnabled(bool enabled)
{
  // The MAT packer runs its seamless-branch handling unconditionally; this flag
  // only gates the codec's internal-clock retiming (kept off for realtime/PVR).
  m_lavStyleSyncEnabled = enabled;
}

void CDVDAudioCodecPassthrough::UpdateDialNormSettings()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings)
    return;

  m_defeatAC3DialNorm = settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM);
  m_defeatEAC3AtmosDialNorm =
      settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_EAC3_ATMOS_DIALNORM);
  m_defeatTrueHDDialNorm = settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_TRUEHD_DIALNORM);
  m_defeatDTSDialNorm = settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_DTS_DIALNORM);
}

void CDVDAudioCodecPassthrough::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (setting)
    UpdateDialNormSettings();
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  m_boundaryEnabled =
      aml_presentation_validated() && !XFILE::CFile::Exists("special://profile/boundary_off");

  m_hints = hints;
  UpdateDialNormSettings();
  m_isEAC3JOC = hints.codec == AV_CODEC_ID_EAC3 && hints.profile == AV_PROFILE_EAC3_DDP_ATMOS;
  m_parser.SetCoreOnly(false);
  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_codecName = "pt-ac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_codecName = "pt-eac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      m_codecName = "pt-dtshd";
      // TrueHD/DTS bitstreaming can only skip/duplicate whole frames, so use a
      // looser jitter threshold (LAV Filters).
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd";
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      CLog::LogF(LOGDEBUG, "passthrough output device is {}", m_deviceIsRAW ? "RAW" : "IEC");
      break;

    default:
      return false;
  }

  // Report the threshold unconditionally: retiming is always on for non-realtime
  // passthrough in this build, and m_lavStyleSyncEnabled is not even settled yet here
  // (VideoPlayerAudio calls SetLavStyleSyncEnabled after Open), so gating the line on
  // it only ever suppressed it.
  CLog::LogF(LOGDEBUG, "{}: jitter threshold {:.0f}ms", m_codecName, m_jitterThreshold / 1000.0);

  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;

  if (m_lavStyleSyncEnabled)
  {
    // LAV Audio: use the LOCAL_NOPTS sentinel for robust PTS validation
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_jitterTracker.Reset();
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
  return true;
}

void CDVDAudioCodecPassthrough::Dispose()
{
  if (m_buffer)
  {
    delete[] m_buffer;
    m_buffer = NULL;
  }

  free(m_backlogBuffer);
  m_backlogBuffer = nullptr;
  m_backlogBufferSize = 0;

  m_bufferSize = 0;
}

bool CDVDAudioCodecPassthrough::AddData(const DemuxPacket &packet)
{
  // Rewriting E-AC-3 dialnorm breaks JOC (Atmos) rendering on receivers, so Atmos
  // needs its own opt-in.
  m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm && (!m_isEAC3JOC || m_defeatEAC3AtmosDialNorm));
  m_parser.SetDefeatTrueHDDialNorm(m_defeatTrueHDDialNorm);
  m_parser.SetDefeatDTSDialNorm(m_defeatDTSDialNorm);

  if (m_backlogSize)
  {
    m_dataSize = m_bufferSize;
    unsigned int consumed = m_parser.AddData(m_backlogBuffer, m_backlogSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    if (consumed != m_backlogSize)
    {
      memmove(m_backlogBuffer, m_backlogBuffer+consumed, m_backlogSize-consumed);
    }
    m_backlogSize -= consumed;
  }

  unsigned char *pData(const_cast<uint8_t*>(packet.pData));
  int iSize(packet.iSize);

  bool boundaryMark = false;
  if (pData)
  {
    // a glided seam's correction is not a step of this stream
    const double correction = packet.m_ptsOffsetCorrection - packet.m_seamOffsetCorrection;
    if (m_haveOffsetCorrection && correction != m_lastOffsetCorrection)
      m_boundaryMarkPending = true;
    m_lastOffsetCorrection = correction;
    m_haveOffsetCorrection = true;
    if (m_boundaryMarkPending && IsValidPts(packet.pts))
    {
      boundaryMark = true;
      m_boundaryMarkPending = false;
    }
  }

  if (m_lavStyleSyncEnabled)
  {
    // LAV Audio: validate PTS with the robust range check so seamless-branch
    // garbage does not poison the internal clock.
    const double incomingPts = packet.pts;
    const bool ptsIsValid = IsValidPts(incomingPts);

    if (pData)
    {
      // Sanitize the tracked PTS members if they contain garbage values
      if (!IsValidPts(m_currentPts))
        m_currentPts = LOCAL_NOPTS;
      if (!IsValidPts(m_nextPts))
        m_nextPts = LOCAL_NOPTS;
      if (m_currentPts == LOCAL_NOPTS)
        m_currentMark = false;
      if (m_nextPts == LOCAL_NOPTS)
        m_nextMark = false;

      if (m_currentPts == LOCAL_NOPTS)
      {
        if (m_nextPts != LOCAL_NOPTS)
        {
          m_currentPts = m_nextPts;
          m_currentMark = m_nextMark;
          m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
          m_nextMark = ptsIsValid && boundaryMark;
        }
        else if (ptsIsValid)
        {
          m_currentPts = incomingPts;
          m_currentMark = boundaryMark;
        }
      }
      else if (ptsIsValid)
      {
        // a replaced next label was never emitted: the boundary moves on with it
        m_nextPts = incomingPts;
        m_nextMark = m_nextMark || boundaryMark;
      }
    }
  }
  else
  {
    // Stock Kodi PTS handling
    if (pData)
    {
      if (m_currentPts == DVD_NOPTS_VALUE)
      {
        if (m_nextPts != DVD_NOPTS_VALUE)
        {
          m_currentPts = m_nextPts;
          m_nextPts = packet.pts;
        }
        else if (packet.pts != DVD_NOPTS_VALUE)
        {
          m_currentPts = packet.pts;
        }
      }
      else
      {
        m_nextPts = packet.pts;
      }
    }
  }

  if (pData && !m_backlogSize)
  {
    if (iSize <= 0)
      return true;

    m_dataSize = m_bufferSize;
    const bool wasLocked = m_parser.IsValid();
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    // A packet parsed from its start (no backlog ahead of it) emits its own first
    // unit here: while the stream waits for its seed, remember the latest such
    // packet's pts and the units before it (counted where they are packed)
    if (m_lavStyleSyncEnabled && m_needsResync && m_dataSize > 0 && IsValidPts(packet.pts) &&
        m_dataSize <= static_cast<unsigned int>(iSize) && memcmp(m_buffer, pData, 4) == 0)
    {
      m_seedRefPts = packet.pts;
      m_seedRefUnits = m_seedUnits;
    }

    // The packet the parser locked onto a TrueHD stream in: its pts belongs to
    // its first access unit, and the major sync the stream starts from can be
    // further in (MKV blocks carry several units). Each unit starts with its
    // length, so count the units before the major sync and move the pts on by
    // their duration (40 samples at the base rate, 1/1200 s or 1/1102.5 s).
    if (m_lavStyleSyncEnabled && m_needsResync && !wasLocked && m_parser.IsValid() &&
        m_parser.GetStreamInfo().m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD &&
        IsValidPts(m_currentPts) && IsValidPts(packet.pts) && m_currentPts == packet.pts)
    {
      int offset = 0;
      int units = 0;
      bool found = false;
      while (offset + 8 <= iSize)
      {
        if (pData[offset + 4] == 0xF8 && pData[offset + 5] == 0x72 && pData[offset + 6] == 0x6F &&
            pData[offset + 7] == 0xBA)
        {
          found = true;
          break;
        }
        const int length = ((pData[offset] << 8 | pData[offset + 1]) & 0x0FFF) * 2;
        if (length < 4)
          break;
        offset += length;
        units++;
      }
      if (found && units > 0)
      {
        const unsigned int rate = m_parser.GetSampleRate();
        const double unitTime = DVD_TIME_BASE / ((rate % 44100) == 0 ? 1102.5 : 1200.0);
        m_currentPts += units * unitTime;
        CLog::LogF(LOGDEBUG, "TrueHD: major sync {} access units into the packet, pts moved {:.3f} ms",
                   units, units * unitTime / 1000.0);
      }
    }

    if (used != iSize)
    {
      if (m_backlogBufferSize < static_cast<unsigned int>(iSize - used))
      {
        m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, iSize - used);
        m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
      }
      m_backlogSize = iSize - used;
      memcpy(m_backlogBuffer, pData + used, m_backlogSize);
    }
  }
  else if (pData)
  {
    if (m_backlogBufferSize < (m_backlogSize + iSize))
    {
      m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, static_cast<int>(m_backlogSize + iSize));
      m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
    }
    memcpy(m_backlogBuffer + m_backlogSize, pData, iSize);
    m_backlogSize += iSize;
  }

  // After a reset (a seek, a new stream) the parser locks onto the stream only
  // at a sync point - for TrueHD a major sync, up to 128 access units (107 ms)
  // in - and consumes what comes before without emitting anything. A packet it
  // consumed whole without locking carries the pts of audio that will never
  // play: drop it, so the first frame emitted takes the pts of a packet it came
  // from. Labelling it with the first packet's pts put every label after a seek
  // that much early (13 Hours: standing jitter -108 ms, corrected only past the
  // 100 ms threshold; anything under it stayed for the title).
  // TrueHD only: an AC-3/E-AC-3/DTS parser also holds its first frame back
  // while it reads into the next one, and dropping that packet's pts would
  // label everything a frame late.
  if (m_lavStyleSyncEnabled && m_needsResync && !m_dataSize && !m_parser.IsValid() &&
      m_hints.codec == AV_CODEC_ID_TRUEHD)
  {
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
  }

  if (!m_dataSize)
    return true;

  m_format.m_dataFormat = AE_FMT_RAW;
  m_format.m_streamInfo = m_parser.GetStreamInfo();
  m_format.m_sampleRate = m_parser.GetSampleRate();
  m_format.m_frameSize = 1;
  CAEChannelInfo layout;
  for (unsigned int i = 0; i < m_parser.GetChannels(); i++)
  {
    layout += AE_CH_RAW;
  }
  m_format.m_channelLayout = layout;

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (m_trueHDBuffer.empty())
    {
      m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

      if (!m_deviceIsRAW && !m_packerMAT)
        m_packerMAT = std::make_unique<CPackerMAT>();
    }

    if (m_deviceIsRAW) // RAW
    {
      m_dataSize = PackTrueHD();
    }
    else // IEC
    {
      if (m_lavStyleSyncEnabled)
      {
        // LAV Audio: a MAT frame contains 24 TrueHD frames; cache the timestamp
        // of the first one so the whole MAT burst carries that PTS.
        if (!m_truehdPtsCacheValid && IsValidPts(m_currentPts))
        {
          m_truehdPtsCache = m_currentPts;
          m_truehdPtsCacheValid = true;
        }
      }

      const bool packed = m_packerMAT->IsStarted() ||
                          (m_dataSize >= 8 && m_buffer[4] == 0xF8 && m_buffer[5] == 0x72 &&
                           m_buffer[6] == 0x6F && m_buffer[7] == 0xBA);
      if (packed && m_needsResync)
        m_seedUnits++;
      if (packed)
        m_matUnits++;
      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;
        m_matUnits = 0;

        if (m_lavStyleSyncEnabled)
        {
          // Consume the MAT packer's discontinuity flag. We don't need to react
          // to it here — the packer already handled padding carry-forward and
          // our internal clock continues smoothly regardless.
          (void)m_packerMAT->HadDiscontinuity();

          // Apply the cached timestamp for this MAT frame, then reset the cache.
          if (m_truehdPtsCacheValid)
          {
            m_currentPts = m_truehdPtsCache;
            m_truehdPtsCacheValid = false;
            m_truehdPtsCache = LOCAL_NOPTS;
          }

          // After a reset the first unit's pts can be stale: FFmpeg's TrueHD
          // parser discards a Matroska block up to its first major sync and
          // stamps the unit it outputs with the block's pts (13 Hours after a
          // seek: stamped 20.992 s, the next unit 21.056 s). Hold the MAT
          // frames until a packet with its own pts dates them (GetData's
          // seed), at most MAX_HELD_MAT; held frames keep their order.
          if (m_needsResync || !m_heldMat.empty())
          {
            if (m_heldMat.empty())
              m_heldFirstPts = m_currentPts;
            if (!m_holdDone &&
                ((IsValidPts(m_seedRefPts) && m_seedRefUnits > 0) ||
                 m_heldMat.size() + 1 >= MAX_HELD_MAT))
              m_holdDone = true;
            m_heldMat.push_back(m_trueHDBuffer);
            m_dataSize = 0;
          }
        }
      }
      else
        m_dataSize = 0;
    }
  }

  return true;
}

unsigned int CDVDAudioCodecPassthrough::PackTrueHD()
{
  unsigned int dataSize{0};

  if (m_trueHDoffset == 0)
    m_trueHDframes = 0;

  memcpy(m_trueHDBuffer.data() + m_trueHDoffset, m_buffer, m_dataSize);

  m_trueHDoffset += m_dataSize;
  m_trueHDframes++;

  if (m_trueHDframes == 24)
  {
    dataSize = m_trueHDoffset;
    m_trueHDoffset = 0;
    m_trueHDframes = 0;
    return dataSize;
  }

  return 0;
}

void CDVDAudioCodecPassthrough::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = GetData(frame.data);
  frame.framesOut = 0;
  frame.padBefore = 0;
  frame.labelLag = 0;

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = m_hints.bitspersample;
  frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());

  if (!m_lavStyleSyncEnabled)
  {
    // Stock Kodi PTS handling
    frame.pts = m_currentPts;
    m_currentPts = DVD_NOPTS_VALUE;
    return;
  }

  //============================================================================
  // LAV Audio internal-clock A/V sync
  // Based on LAV Filters by Hendrik Leppkes (Nevcairiel).
  //
  // We maintain our own internal clock, synced to the RESYNC PTS from
  // VideoPlayer (the coordinated A/V clock). We output PTS from that clock, not
  // the demuxer, and continuously correct any timing jitter/drift against the
  // demuxer PTS whenever it exceeds the threshold. This applies to ongoing drift
  // as well as the large sudden jumps at seamless branch points; it is not
  // limited to branch points.
  //============================================================================
  const CAEStreamInfo::DataType streamType = m_format.m_streamInfo.m_type;
  const bool isTrueHD = (streamType == CAEStreamInfo::STREAM_TYPE_TRUEHD);

  // TrueHD: compensate for sub-MAT-frame drift via the MAT packer samples offset.
  double samplesOffsetTime = 0.0;
  if (isTrueHD && m_packerMAT && m_format.m_sampleRate > 0)
  {
    const int samplesOffset = m_packerMAT->GetSamplesOffset();
    if (samplesOffset != 0)
      samplesOffsetTime =
          static_cast<double>(samplesOffset) / m_format.m_sampleRate * DVD_TIME_BASE;
  }

  const double demuxerPts = m_currentPts;
  const bool haveDemuxerPts = IsValidPts(demuxerPts);
  const bool boundaryFrame = m_currentMark;
  m_currentMark = false;
  bool seeded = false;

  // STEP 1: resync the internal clock when needed (codec creation, after seeks).
  // This seed is EXACT - it is the true PTS of the content being emitted - and
  // it is authoritative. SyncToResyncPts() may have parked a provisional
  // pts + delay estimate here first so there is always a usable clock, but it
  // deliberately leaves m_needsResync set so this overrides it; see the comment
  // there for what happens when the estimate is trusted instead.
  const bool seedDated = isTrueHD && IsValidPts(m_seedRefPts) && m_seedRefUnits > 0;
  if (m_needsResync && (haveDemuxerPts || seedDated || IsValidPts(m_heldFirstPts)))
  {
    // the first held frame's own pts, when frames were held
    double seed = IsValidPts(m_heldFirstPts) ? m_heldFirstPts : demuxerPts;
    // TrueHD in Matroska: only a block's first unit carries a pts, and a seek
    // into a laced block delivers a later unit still stamped with the block's
    // pts (13 Hours: a major sync stamped 20.992 s, the next unit 21.056 s). A
    // later packet with its own pts inside this first MAT frame dates the start
    // exactly: its pts less the units before it.
    if (seedDated)
    {
      const double unitTime =
          DVD_TIME_BASE / ((m_format.m_sampleRate % 44100) == 0 ? 1102.5 : 1200.0);
      const double dated = m_seedRefPts - m_seedRefUnits * unitTime;
      CLog::LogF(LOGDEBUG,
                 "TrueHD seed {:.3f}s from the packet {} units in (first packet said {:.3f}s)",
                 dated / DVD_TIME_BASE, m_seedRefUnits, seed / DVD_TIME_BASE);
      seed = dated;
    }
    m_seedRefPts = LOCAL_NOPTS;
    m_seedRefUnits = 0;
    m_seedUnits = 0;
    m_heldFirstPts = LOCAL_NOPTS;
    m_internalClock = seed;
    m_labelLag = 0.0;
    m_needsResync = false;
    seeded = true;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock synced to demuxer PTS {:.3f}s",
               demuxerPts / DVD_TIME_BASE);
  }

  // A timeline correction steps the labels against the content already
  // emitted (an M3GAN menu loop wrap: -6.167 ms of AC-3, every wrap). Present
  // by label as a player does, in whole frames: the frames the content would
  // lag its labels by are dropped and the residual, under half a frame either
  // way, carries to the next boundary. A pause is audible on a receiver, so a
  // gap is padded only when it would leave the sound early by more than half a
  // frame. Frame-sized formats only; TrueHD steps are left.
  if (m_boundaryDrops > 0)
  {
    m_boundaryDrops--;
    frame.nb_frames = 0;
    m_currentPts = LOCAL_NOPTS;
    return;
  }
  if (seeded)
  {
    // an exact seed: nothing before it to be continuous with
    ClearBoundary();
  }
  else if (boundaryFrame && haveDemuxerPts && IsValidPts(m_internalClock))
  {
    // R: the label less where the content emitted so far ends, against the
    // standing jitter before the boundary; negative is an overlap
    const double step = m_lastJitter - (m_internalClock - demuxerPts + samplesOffsetTime);
    const double frameTime = frame.duration;
    const bool frameSized = streamType == CAEStreamInfo::STREAM_TYPE_AC3 ||
                            streamType == CAEStreamInfo::STREAM_TYPE_EAC3 ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTSHD_CORE ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTS_512 ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTS_1024 ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTS_2048 ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTSHD ||
                            streamType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA;
    const bool realise = m_boundaryEnabled && frameSized && frameTime > 0.0 &&
                         std::abs(step) <= DVD_MSEC_TO_TIME(100);
    // the content's lag behind its labels once the step is taken
    const double lag = m_lastJitter - step;
    // the residual carried reaches half a frame: retiming the clock to it
    // would hide it from the pins
    if (realise)
      m_jitterThreshold = std::max(m_jitterThreshold, frameTime / 2 + DVD_MSEC_TO_TIME(4));
    unsigned int drops = 0;
    double pad = 0.0;
    if (realise && lag >= frameTime / 2)
      drops = static_cast<unsigned int>(std::lround(lag / frameTime));
    else if (realise && lag < -frameTime / 2 && step > 0.0)
      pad = step;
    CLog::Log(LOGINFO,
              "CDVDAudioCodecPassthrough: timeline step {:+.3f} ms at label {:.3f} ms{}", step / 1000.0,
              demuxerPts / 1000.0,
              realise ? fmt::format(": lag {:+.3f} ms, {} frames dropped, {:.3f} ms padded",
                                    lag / 1000.0, drops, pad / 1000.0)
                      : std::string(" left"));
    if (drops > 0)
    {
      m_boundaryDrops = drops - 1;
      frame.nb_frames = 0;
      m_currentPts = LOCAL_NOPTS;
      return;
    }
    if (pad > 0.0)
    {
      // the residual before the boundary stands
      frame.padBefore = pad;
      m_internalClock = demuxerPts + m_lastJitter;
    }
  }

  // STEP 2: track jitter between our clock and the demuxer PTS; whenever it
  // exceeds the threshold, correct the internal clock to realign. This runs on
  // every frame and corrects any jitter/drift - ongoing small drift as well as
  // the large sudden jumps at seamless branch points.
  if (IsValidPts(m_internalClock) && haveDemuxerPts)
  {
    const double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
    m_jitterTracker.Sample(jitter);
    m_lastJitter = jitter;

    // Correct toward the most stable value in the window (smallest absolute jitter).
    const double absMinJitter = m_jitterTracker.AbsMinimum();
    if (std::abs(absMinJitter) > m_jitterThreshold)
    {
      m_internalClock -= absMinJitter;
      m_jitterTracker.OffsetValues(-absMinJitter);
      m_lastJitter -= absMinJitter;

      CLog::LogF(LOGDEBUG, "jitter correction {:.2f}ms (threshold {:.0f}ms)", absMinJitter / 1000.0,
                 m_jitterThreshold / 1000.0);
    }

    // The STANDING jitter - how far the free-running internal clock currently
    // sits from the demuxer - is otherwise only ever written to the log at the
    // instant a correction fires. Everything BELOW m_jitterThreshold is silent,
    // and that threshold is 100ms for TrueHD and every DTS variant, so an
    // offset of up to 100ms can sit here for a whole title, uncorrected and
    // unlogged.
    //
    // It is also invisible to every A/V sync statistic Kodi has, structurally:
    // frame.pts below is emitted FROM this clock, and ActiveAE's sync error is
    // computed from that same pts, so a constant offset between the clock and
    // the real audio content cancels out of the measurement entirely. The only
    // way to see it is to print it here.
    //
    // The source found was SyncToResyncPts(pts + delay) in VideoPlayerAudio's
    // GENERAL_RESYNC handler: user logs on AC3 titles (10ms threshold, so the
    // correction fires and is logged) showed it seeding this clock -40.9,
    // +30.2, -37.0, -32.8, -72.4, -34.1, -33.7 and +61.2ms away from the
    // demuxer. That seed is now provisional only (see SyncToResyncPts); this
    // trace stays to catch any other source of a constant offset.
    if (++m_jitterTraceCount >= 100)
    {
      m_jitterTraceCount = 0;
      CLog::LogF(LOGDEBUG, "standing jitter {:+.2f}ms (absmin {:+.2f}ms, threshold {:.0f}ms)",
                 jitter / 1000.0, absMinJitter / 1000.0, m_jitterThreshold / 1000.0);
    }
  }

  // STEP 3: output PTS from the internal clock (once synced), advancing by the
  // frame duration. Fall back to the demuxer PTS only before the first sync.
  if (IsValidPts(m_internalClock))
  {
    if (haveDemuxerPts)
      m_labelLag = m_internalClock - demuxerPts + samplesOffsetTime;
    frame.labelLag = m_labelLag;
    frame.pts = m_internalClock;
    m_internalClock += frame.duration;
  }
  else if (haveDemuxerPts)
  {
    frame.pts = demuxerPts;
    m_internalClock = demuxerPts + frame.duration;
  }
  else
  {
    frame.pts = DVD_NOPTS_VALUE;
  }

  m_currentPts = LOCAL_NOPTS;
}

int CDVDAudioCodecPassthrough::GetData(uint8_t** dst)
{
  if (!m_heldMat.empty() && m_holdDone)
  {
    m_trueHDBuffer = std::move(m_heldMat.front());
    m_heldMat.pop_front();
    if (m_heldMat.empty())
      m_holdDone = false;
    *dst = m_trueHDBuffer.data();
    return TRUEHD_BUF_SIZE;
  }

  if (!m_dataSize)
    AddData(DemuxPacket());

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    *dst = m_trueHDBuffer.data();
  else
    *dst = m_buffer;

  int bytes = m_dataSize;
  m_dataSize = 0;
  return bytes;
}

void CDVDAudioCodecPassthrough::ClearBoundary()
{
  m_boundaryMarkPending = false;
  m_currentMark = false;
  m_nextMark = false;
  m_boundaryDrops = 0;
  m_lastJitter = 0.0;
}

void CDVDAudioCodecPassthrough::Reset()
{
  m_haveOffsetCorrection = false;
  ClearBoundary();
  m_trueHDoffset = 0;
  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;
  m_parser.Reset();

  if (m_lavStyleSyncEnabled)
  {
    // LAV Audio reset: use the LOCAL_NOPTS sentinel and reset the sync state.
    m_currentPts = LOCAL_NOPTS;
    m_nextPts = LOCAL_NOPTS;
    m_truehdPtsCache = LOCAL_NOPTS;
    m_truehdPtsCacheValid = false;
    m_internalClock = LOCAL_NOPTS;
    m_needsResync = true;
    m_seedRefPts = LOCAL_NOPTS;
    m_seedRefUnits = 0;
    m_seedUnits = 0;
    m_matUnits = 0;
    m_heldMat.clear();
    m_holdDone = false;
    m_heldFirstPts = LOCAL_NOPTS;
    m_jitterTracker.Reset();

    if (m_packerMAT)
      m_packerMAT->Reset();

    CLog::LogF(LOGDEBUG, "LAV internal clock reset, will resync");
  }
  else
  {
    m_currentPts = DVD_NOPTS_VALUE;
    m_nextPts = DVD_NOPTS_VALUE;
  }
}

void CDVDAudioCodecPassthrough::ResetLavSyncState()
{
  if (!m_lavStyleSyncEnabled)
    return;

  ClearBoundary();

  m_truehdPtsCache = LOCAL_NOPTS;
  m_truehdPtsCacheValid = false;
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();
  m_seedRefPts = LOCAL_NOPTS;
  m_seedRefUnits = 0;
  // the packer keeps the frame it is building: its first unit is the seed's
  m_seedUnits = -m_matUnits;
  m_heldMat.clear();
  m_holdDone = false;
  m_heldFirstPts = LOCAL_NOPTS;

  CLog::LogF(LOGDEBUG, "internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  if (!m_lavStyleSyncEnabled)
    return;

  // VideoPlayer::Sync() sends RESYNC with a coordinated A/V clock value, and the
  // caller passes pts + m_audioSink.GetDelay(). That is an ESTIMATE, not ground
  // truth: it is only as good as the sink's delay report at that instant. Stock
  // Kodi uses pts + delay purely as a fallback seed for m_audioClock and throws
  // it away on the next frame that carries a real timestamp - this fork used to
  // promote it to authoritative here by clearing m_needsResync, which pinned the
  // internal clock to whatever error the delay estimate happened to carry.
  //
  // That error is permanent on TrueHD/DTS, because every audio timestamp is
  // emitted FROM this clock (GetData STEP 3) and the jitter tracker only pulls
  // it back past m_jitterThreshold = 100ms. Nothing else can see it either: the
  // A/V sync error ActiveAE reports is computed from the same emitted pts, so a
  // constant clock-vs-content offset cancels out of the measurement entirely.
  //
  // Measured on am9pro 2026-08-10, DTS-HD MA, 12 seeks (jittertrace2):
  //   GENERAL_RESYNC(1559.792 ... cache:0.449)
  //   internal clock set to RESYNC pts 1560.241s     <- 1559.792 + 0.449
  //   standing jitter -77.21ms                       <- 1ms later, 77ms wrong
  // and it held at -77 +/- 1ms for the whole segment with ZERO corrections,
  // because 77 < 100. Parked offsets over 9 seeds: -15.5 -1.1 -0.4 -1.3 -77.5
  // +0.7 -0.2 +0.8 +0.5 ms. Most draws are harmless; the tail is audible
  // (ITU-R BT.1359-1 puts audio-early detectability near 45ms) and it is exactly
  // the "sometimes out of sync, a seek fixes it, stats show nothing" report.
  //
  // So keep the value as an immediate fallback - it is better than no clock at
  // all when the next frames carry no PTS - but LEAVE m_needsResync SET, so
  // GetData STEP 1 re-seeds from the true demuxer PTS as soon as one arrives.
  // That seed is exact by construction ("internal clock synced to demuxer PTS").
  if (IsValidPts(pts))
  {
    m_internalClock = pts;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock provisionally set to RESYNC pts {:.3f}s (awaiting demuxer)",
               pts / DVD_TIME_BASE);
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}
