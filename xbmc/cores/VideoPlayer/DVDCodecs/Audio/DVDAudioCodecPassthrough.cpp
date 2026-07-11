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
#include "DVDStreamInfo.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
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

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  Dispose();
}

void CDVDAudioCodecPassthrough::SetLavStyleSyncEnabled(bool enabled)
{
  // The MAT packer runs its seamless-branch handling unconditionally; this flag
  // only gates the codec's internal-clock retiming (kept off for realtime/PVR).
  m_lavStyleSyncEnabled = enabled;
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
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

  if (m_lavStyleSyncEnabled)
    CLog::LogF(LOGDEBUG, "LAV Audio sync enabled, jitter threshold {:.0f}ms for {}",
               m_jitterThreshold / 1000.0, m_codecName);

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
      {
        if (m_nextPts != LOCAL_NOPTS)
        {
          m_currentPts = m_nextPts;
          m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
        }
        else if (ptsIsValid)
        {
          m_currentPts = incomingPts;
        }
      }
      else if (ptsIsValid)
      {
        m_nextPts = incomingPts;
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
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);

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

      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;

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

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = 8;
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

  // STEP 1: resync the internal clock when needed (codec creation, after seeks).
  // If a RESYNC arrives later via SyncToResyncPts(), it overrides this with the
  // authoritative coordinated A/V clock value.
  if (m_needsResync && haveDemuxerPts)
  {
    m_internalClock = demuxerPts;
    m_needsResync = false;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock synced to demuxer PTS {:.3f}s",
               demuxerPts / DVD_TIME_BASE);
  }

  // STEP 2: track jitter between our clock and the demuxer PTS; whenever it
  // exceeds the threshold, correct the internal clock to realign. This runs on
  // every frame and corrects any jitter/drift - ongoing small drift as well as
  // the large sudden jumps at seamless branch points.
  if (IsValidPts(m_internalClock) && haveDemuxerPts)
  {
    const double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
    m_jitterTracker.Sample(jitter);

    // Correct toward the most stable value in the window (smallest absolute jitter).
    const double absMinJitter = m_jitterTracker.AbsMinimum();
    if (std::abs(absMinJitter) > m_jitterThreshold)
    {
      m_internalClock -= absMinJitter;
      m_jitterTracker.OffsetValues(-absMinJitter);

      CLog::LogF(LOGDEBUG, "jitter correction {:.2f}ms (threshold {:.0f}ms)", absMinJitter / 1000.0,
                 m_jitterThreshold / 1000.0);
    }
  }

  // STEP 3: output PTS from the internal clock (once synced), advancing by the
  // frame duration. Fall back to the demuxer PTS only before the first sync.
  if (IsValidPts(m_internalClock))
  {
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

void CDVDAudioCodecPassthrough::Reset()
{
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

  m_truehdPtsCache = LOCAL_NOPTS;
  m_truehdPtsCacheValid = false;
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();

  CLog::LogF(LOGDEBUG, "internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  if (!m_lavStyleSyncEnabled)
    return;

  // VideoPlayer::Sync() sends RESYNC with a coordinated A/V clock value; trust it
  // directly for the internal clock.
  if (IsValidPts(pts))
  {
    m_internalClock = pts;
    m_needsResync = false;
    m_jitterTracker.Reset();
    CLog::LogF(LOGDEBUG, "internal clock set to RESYNC pts {:.3f}s", pts / DVD_TIME_BASE);
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}
