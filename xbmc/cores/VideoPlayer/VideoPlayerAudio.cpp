/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudio.h"

#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/Audio/DVDAudioCodecPassthrough.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/log.h"

#include <mutex>

#ifdef TARGET_RASPBERRY_PI
#include "platform/linux/RBP.h"
#endif

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <math.h>
#include <sstream>

using namespace std::chrono_literals;

namespace
{
// A valid PTS is >= 0 and within a sane range; rejects DVD_NOPTS_VALUE
// (~1.8e19 as a double) and other garbage without rejecting real timestamps.
constexpr double LOCAL_NOPTS = -1.0;
constexpr double MAX_REASONABLE_PTS = 86400.0 * DVD_TIME_BASE; // 24 hours
inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}

// (Removed: DISCON_VSYNC_ADJUST_TIME_MS, the 20ms passthrough vsync-clock gate.
// It was ported from the CE21 LAV build where it was OPT-IN behind
// m_lavFullSyncEnabled, and made unconditional here because LAV is always-on -
// which is precisely why only CE22 builds showed the DTS sawtooth. A gate
// narrower than ErrorAdjust's whole-frame correction cannot settle; see the
// long note at the SYNC_DISCON gate in OutputPacket.)
} // namespace

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgAudioCodecChange() override = default;

  std::unique_ptr<CDVDAudioCodec> m_codec;
  CDVDStreamInfo  m_hints;
};

CVideoPlayerAudio::CVideoPlayerAudio(CDVDClock* pClock,
                                     CDVDMessageQueue& parent,
                                     CProcessInfo& processInfo,
                                     double messageQueueTimeSize)
  : CThread("VideoPlayerAudio"),
    IDVDStreamPlayerAudio(processInfo),
    m_messageQueue("audio"),
    m_messageParent(parent),
    m_audioSink(pClock)
{
  m_pClock = pClock;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  // queue data size is dynamical changed by stream
  m_messageQueue.SetMaxDataSize(LvLAudioMIN);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_disconAdjustTimeMs = processInfo.GetMaxPassthroughOffSyncDuration();
}

CVideoPlayerAudio::~CVideoPlayerAudio()
{
  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  CLog::Log(LOGINFO, "Finding audio codec for: {}", hints.codec);
  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);

  CAEStreamInfo::DataType streamType =
      m_audioSink.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgAudioCodecChange>(hints, std::move(codec)), 0);
  else
  {
    OpenStream(hints, std::move(codec));
    m_messageQueue.Init();
    CLog::Log(LOGINFO, "Creating audio thread");
    Create();
  }
  return true;
}

void CVideoPlayerAudio::OpenStream(CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
{
  m_pAudioCodec = std::move(codec);


  /* store our stream hints */
  m_streaminfo = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec   = m_pAudioCodec->GetFormat().m_channelLayout.Count();
  int samplerateFromCodec = m_pAudioCodec->GetFormat().m_sampleRate;

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  // LAV Audio: configure the passthrough codec's internal-clock retiming
  ConfigureLavAudioSync();

  // LAV PCM sync: for non-passthrough (decoded) audio, enable the internal
  // jitter tracker. Always on except realtime/PVR (passthrough is handled by
  // ConfigureLavAudioSync above).
  m_lavStylePcmSyncEnabled =
      m_pAudioCodec && !m_pAudioCodec->NeedPassthrough() && !m_processInfo.IsRealtimeStream();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;

  m_prevsynctype = -1;
  m_synctype = SYNC_DISCON;
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK))
    m_synctype = SYNC_RESAMPLE;

  if (m_synctype == SYNC_DISCON)
    CLog::LogF(LOGINFO, "Allowing max Out-Of-Sync Value of {} ms", m_disconAdjustTimeMs);

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;

  // LAV PCM: reset jitter tracking on stream open
  if (m_lavStylePcmSyncEnabled)
  {
    m_pcmJitterTracker.Reset();
    m_pcmOutputClock = LOCAL_NOPTS;
    m_pcmResyncTimestamp = true;
  }
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  bool bWait = bWaitForBuffers && m_speed > 0 && !CServiceBroker::GetActiveAE()->IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGINFO, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // destroy audio device
  CLog::Log(LOGINFO, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_audioSink.Drain();
    m_bStop = true;
  }
  else
  {
    m_audioSink.Flush();
  }

  m_audioSink.Destroy(true);

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGINFO, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    m_pAudioCodec.reset();
  }

  std::ostringstream s;
  SInfo info;
  info.info        = s.str();
  info.pts         = DVD_NOPTS_VALUE;
  info.passthrough = false;

  { std::unique_lock<CCriticalSection> lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::OnStartup()
{
}

void CVideoPlayerAudio::UpdatePlayerInfo()
{
  std::ostringstream s;
  s << "aq:" << std::setw(2) << std::min(99, m_messageQueue.GetLevel()) << "% ("
    << std::setw(2) << std::min(99,m_messageQueue.GetLevel(true)) << "%, "
    << std::fixed << std::setprecision(1) << static_cast<double>(m_messageQueue.GetMaxDataSize()) / SIZE_1M << "MB)";
  s << std::fixed << std::setprecision(3) << m_messageQueue.GetTimeSize();
  s << "s, Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;
  s << ", ac:"   << m_processInfo.GetAudioDecoderName().c_str();
  if (!m_info.passthrough)
    s << ", chan:" << m_processInfo.GetAudioChannels().c_str();
  s << ", " << m_streaminfo.samplerate/1000 << " kHz";

  // print a/v discontinuity adjustments counter when audio is not resampled (passthrough mode)
  if (m_synctype == SYNC_DISCON)
    s << ", a/v corrections (" << m_disconAdjustTimeMs << "ms): " << m_disconAdjustCounter;

  //print the inverse of the resample ratio, since that makes more sense
  //if the resample ratio is 0.5, then we're playing twice as fast
  else if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_audioSink.GetResampleRatio();

  SInfo info;
  info.info        = s.str();
  info.pts         = m_audioSink.GetPlayingPts();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough();

  {
    std::unique_lock lock(m_info_section);
    m_info = info;
  }

  m_processInfo.SetAudioLiveBitRate(m_audioStats.GetBitrate());
  m_processInfo.SetAudioQueueLevel(std::min(99, m_messageQueue.GetLevel()));
  m_processInfo.SetAudioQueueDataLevel(std::min(99, m_messageQueue.GetLevel(true)));
}

void CVideoPlayerAudio::Process()
{
  CLog::Log(LOGINFO, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  audioframe.nb_frames = 0;
  audioframe.framesOut = 0;
  m_audioStats.Start();
  m_disconAdjustCounter = 0;

  bool onlyPrioMsgs = false;

  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> pMsg;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::ratio<1>>(m_audioSink.GetCacheTime()));

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) ||
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    if (onlyPrioMsgs)
    {
      priority = 1;
      timeout = 0ms;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        CLog::Log(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
        continue;
      }

      // if we only wanted priority messages, this isn't a stall
      if (priority)
        continue;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) &&
          !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          CLog::Log(LOGINFO, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
        }
      }
      if (timeout == 0ms)
        CThread::Sleep(10ms);

      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_AUDIO))
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg, 1); // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC({:.3f} level: {:d} cache:{:.3f}",
                pts / DVD_TIME_BASE, m_messageQueue.GetLevel(), m_audioSink.GetDelay() / DVD_TIME_BASE);

      double delay = m_audioSink.GetDelay();
      if (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE)
      {
        m_audioSink.Flush();
      }
      m_audioClock = pts + delay;

      // LAV PCM: reset jitter tracking on resync
      if (m_lavStylePcmSyncEnabled)
      {
        m_pcmJitterTracker.Reset();
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
      }

      // LAV Audio: rebase the passthrough codec's internal clock to the
      // coordinated A/V clock (pts + delay). ResetLavSyncState() must run first
      // to clear the jitter tracker before adopting the new baseline.
      if (m_pAudioCodec && m_pAudioCodec->NeedPassthrough())
      {
        auto* passthroughCodec = dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
        if (passthroughCodec && passthroughCodec->IsLavStyleSyncEnabled())
        {
          passthroughCodec->ResetLavSyncState();
          passthroughCodec->SyncToResyncPts(pts + delay);
        }
      }

      if (m_speed != DVD_PLAYSPEED_PAUSE)
        m_audioSink.Resume();
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_syncTimer.Set(3000ms);
      m_disconSettleTimer.Set(6000ms);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;

      // LAV PCM: reset jitter tracking on reset
      if (m_lavStylePcmSyncEnabled)
      {
        m_pcmJitterTracker.Reset();
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;

      // LAV PCM: reset jitter tracking on flush
      if (m_lavStylePcmSyncEnabled)
      {
        m_pcmJitterTracker.Reset();
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
      }

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_audioSink.Pause();
      }

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_EOF");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::PLAYER_SETSPEED: {:f} last: {:d}", speed, m_speed);

      if (m_processInfo.IsTempoAllowed(static_cast<float>(speed)/DVD_PLAYSPEED_NORMAL))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_audioSink.Resume();
            m_stalled = false;
          }
        }
      }
      else
      {
        m_audioSink.Pause();
      }
      m_speed = (int)speed;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgAudioCodecChange>(pMsg);
      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = NULL;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (bPacketDrop)
      {
        if (m_syncState != IDVDStreamPlayer::SYNC_STARTING)
        {
          m_audioSink.Drain();
          m_audioSink.Flush();
          audioframe.nb_frames = 0;
        }
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        continue;
      }

      if (!m_processInfo.IsTempoAllowed(static_cast<float>(m_speed) / DVD_PLAYSPEED_NORMAL) &&
          m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        continue;
      }

      if (!m_pAudioCodec->AddData(*pPacket))
      {
        m_messageQueue.PutBack(pMsg);
        onlyPrioMsgs = true;
        continue;
      }

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      m_displayReset = true;
    }
  }
}

bool CVideoPlayerAudio::ProcessDecoderOutput(DVDAudioFrame &audioframe)
{
  if (audioframe.nb_frames <= audioframe.framesOut)
  {
    audioframe.hasDownmix = false;

    m_pAudioCodec->GetData(audioframe);

    if (audioframe.nb_frames == 0)
    {
      return false;
    }

    audioframe.hasTimestamp = true;
    if (audioframe.pts == DVD_NOPTS_VALUE)
    {
      audioframe.pts = m_audioClock;
      audioframe.hasTimestamp = false;
    }
    else
    {
      m_audioClock = audioframe.pts;
    }

    // LAV PCM jitter tracking (non-passthrough only). Runs after baseline PTS
    // handling and may adjust audioframe.pts. Derived from LAV Filters.
    if (m_lavStylePcmSyncEnabled && !audioframe.passthrough && audioframe.hasTimestamp)
    {
      double inputPts = audioframe.pts;
      bool inputPtsValid = IsValidPts(inputPts);

      if (m_pcmResyncTimestamp && inputPtsValid)
      {
        m_pcmOutputClock = inputPts;
        m_pcmResyncTimestamp = false;
        m_pcmJitterTracker.Reset();
        m_pcmStepRun = 0;
      }
      else if (IsValidPts(m_pcmOutputClock) && inputPtsValid)
      {
        // jitter: our running output clock vs the demuxer input PTS
        double jitter = m_pcmOutputClock - inputPts;
        m_pcmJitterTracker.Sample(jitter);
        double absMinJitter = m_pcmJitterTracker.AbsMinimum();
        double thresholdDvdTime = PCM_JITTER_THRESHOLD * DVD_TIME_BASE / 1000000.0;

        // genuine mid-size pts step (a 100-900ms discontinuity: concatenated
        // TS, HLS boundary, edit list): every recent sample far beyond the
        // jitter threshold with ONE sign is a step, not jitter - resync now
        // instead of chasing it through the 64-frame window and landing it
        // on the sink as one large late jump (review finding)
        const bool stepCandidate = std::abs(jitter) > 8.0 * thresholdDvdTime &&
                                   std::abs(jitter) <= DVD_TIME_BASE;
        if (stepCandidate && (m_pcmStepRun == 0 || (jitter > 0) == m_pcmStepPositive))
        {
          m_pcmStepPositive = jitter > 0;
          ++m_pcmStepRun;
        }
        else
          m_pcmStepRun = 0;

        if (std::abs(jitter) > DVD_TIME_BASE)
        {
          // large jump - resync instead of correcting
          m_pcmOutputClock = inputPts;
          m_pcmJitterTracker.Reset();
          m_pcmStepRun = 0;
          CLog::Log(LOGDEBUG,
                    "CVideoPlayerAudio::ProcessDecoderOutput: LAV PCM resync, large jump ({:.2f}s)",
                    jitter / DVD_TIME_BASE);
        }
        else if (m_pcmStepRun >= 8)
        {
          m_pcmOutputClock = inputPts;
          m_pcmJitterTracker.Reset();
          m_pcmStepRun = 0;
          CLog::Log(LOGDEBUG,
                    "CVideoPlayerAudio::ProcessDecoderOutput: LAV PCM resync, sustained pts "
                    "step ({:.1f}ms)", jitter / DVD_TIME_BASE * 1000.0);
        }
        else if (std::abs(absMinJitter) > thresholdDvdTime)
        {
          m_pcmOutputClock -= absMinJitter;
          m_pcmJitterTracker.OffsetValues(-absMinJitter);
          CLog::Log(LOGDEBUG,
                    "CVideoPlayerAudio::ProcessDecoderOutput: LAV PCM jitter correction {:.2f}ms",
                    absMinJitter / DVD_TIME_BASE * 1000.0);
        }

        audioframe.pts = m_pcmOutputClock;
      }
    }

    if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
    {
      // The sample rate has changed or we just got it for the first time
      // for this stream. See if we should enable/disable passthrough due
      // to it.
      m_streaminfo.samplerate = audioframe.format.m_sampleRate;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // Display reset event has occurred
    // See if we should enable passthrough
    if (m_displayReset)
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink.IsValidFormat(audioframe))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_audioSink.Create(audioframe, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "{} - failed to create audio renderer", __FUNCTION__);

      m_prevsynctype = -1;

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink.Resume();

      // a sink rebuild reproduces the start-sync transient (skip/insert +
      // swinging delay estimate) the settle window exists for - re-arm it,
      // or a mid-stream format change takes an ErrorAdjust on garbage
      // immediately (review finding)
      m_disconSettleTimer.Set(6000ms);
      CLog::Log(LOGDEBUG, LOGAUDIO,
                "CVideoPlayerAudio::ProcessDecoderOutput - sink rebuilt, DISCON settle re-armed");
    }

    m_audioSink.SetDynamicRangeCompression(
        static_cast<long>(m_processInfo.GetVideoSettings().m_VolumeAmplification * 100));

    SetSyncType(audioframe.passthrough);

    // downmix
    double clev = audioframe.hasDownmix ? audioframe.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe.centerMixLevel = pow(10, (curDB + m_processInfo.GetVideoSettings().m_CenterMixLevel) / 20);
    audioframe.hasDownmix = true;
  }

  // Hold clock corrections until the post-resync settle window
  // (m_disconSettleTimer, armed at SYNC_INSYNC) has passed: right after a
  // stream (re)open the AE start-sync is still skipping/inserting frames and
  // the sink delay estimate swings tens of ms per second, so an ErrorAdjust
  // taken now acts on garbage and parks the shared A/V clock a whole video
  // frame off for the rest of the stream (observed as a per-BD-playitem random
  // 1-4 frame lipsync offset; with per-playitem display resets the transient
  // still measured 40-80ms at the 3s mark, hence the longer window).
  if (m_synctype == SYNC_DISCON && m_disconSettleTimer.IsTimePast())
  {
    double syncerror = m_audioSink.GetSyncError();

    // The outer gate MUST stay wider than ErrorAdjust's correction step.
    // On the vsync-clock path ErrorAdjust answers a tripped gate with a WHOLE
    // video frame (m_frameTime; 41.7ms at 23.976) - see CDVDClock::ErrorAdjust.
    // So a gate narrower than a frame leaves the residual sitting next to the
    // OPPOSITE edge and the loop can never settle.
    //
    // b10c7e9eb7 tightened this to +20/-27ms and produced exactly that: a
    // permanent sawtooth on DTS passthrough - error walks to -27ms, takes
    // -41.7ms to +14.7ms, walks to +20ms, takes +41.7ms back to -21.7ms, for
    // ~12 corrections/hour, each one a visible frame of A/V desync. Measured
    // on-box 2026-08-03 over a 50-minute title.
    //
    // Upstream and CE21 (avdvplus, pannal) use this plain 50ms gate and do NOT
    // show the defect: 50 - 41.7 = 8.3ms residual, i.e. it lands near zero and
    // the next correction is a whole drift-cycle away. DTS is the codec that
    // exposes it because TrueHD gets sub-frame accuracy from the MAT packer's
    // sample offset and AC3/E-AC3 errors stay small.
    //
    // DO NOT narrow this below m_frameTime again without re-testing a DTS title
    // end to end. The audio-behind drift that b10c7e9eb7 set out to shorten is
    // the lesser evil: it is static, whereas the sawtooth is visible motion.
    if (std::abs(syncerror) > DVD_MSEC_TO_TIME(m_disconAdjustTimeMs))
    {
      double correction = m_pClock->ErrorAdjust(syncerror, "CVideoPlayerAudio::OutputPacket");
      if (correction != 0)
      {
        m_audioSink.SetSyncErrorCorrection(-correction);
        m_disconAdjustCounter++;
        CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio:: sync error correctiom:{:.3f}", correction / DVD_TIME_BASE);
      }
    }
  }
  CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::OutputPacket: pts:{:.3f} curr_pts:{:.3f} clock:{:.3f} level:{:d}",
    audioframe.pts / DVD_TIME_BASE, m_info.pts / DVD_TIME_BASE, m_pClock->GetClock() / DVD_TIME_BASE, GetLevel());

  int framesOutput = m_audioSink.AddPackets(audioframe);

  // guess next pts
  m_audioClock += audioframe.duration * ((double)framesOutput / audioframe.nb_frames);

  // LAV PCM: advance the output clock by the actual duration output
  if (m_lavStylePcmSyncEnabled && !audioframe.passthrough && IsValidPts(m_pcmOutputClock))
  {
    double durationOutput =
        audioframe.duration * (static_cast<double>(framesOutput) / audioframe.nb_frames);
    m_pcmOutputClock += durationOutput;
  }

  audioframe.framesOut += framesOutput;

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
  {
    double cachetotal = m_audioSink.GetCacheTotal();
    double cachetime = m_audioSink.GetCacheTime();
    if (cachetime >= cachetotal * 0.75)
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      m_stalled = false;
      SStartMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.cachetotal = m_audioSink.GetMaxDelay() * DVD_TIME_BASE;
      msg.cachetime = m_audioSink.GetDelay();
      msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));

      m_streaminfo.channels = audioframe.format.m_channelLayout.Count();
      m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
      m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);
      m_processInfo.SetAudioBitsPerSample(audioframe.bits_per_sample);
      m_processInfo.SetAudioDecoderName(m_pAudioCodec->GetName());
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
    }
  }

  return true;
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_DISCON;

  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  m_pClock->SetMaxSpeedAdjust(maxspeedadjust);

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 1) ? m_synctype : 2;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio:: synctype set to {}: {}", m_synctype,
              synctypes[synctype]);
    m_prevsynctype = m_synctype;
    if (m_synctype == SYNC_RESAMPLE)
      m_audioSink.SetResampleMode(1);
    else
      m_audioSink.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGINFO, "thread end: CVideoPlayerAudio::OnExit()");
}

void CVideoPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;
}

void CVideoPlayerAudio::Flush(bool sync)
{
  m_messageQueue.Flush();
  m_messageQueue.Put(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);

  m_audioSink.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  if (m_displayReset)
    CLog::Log(LOGINFO, "CVideoPlayerAudio: display reset occurred, checking for passthrough");
  else
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: stream props changed, checking for passthrough");

  m_displayReset = false;

  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_synctype == SYNC_RESAMPLE)
    allowpassthrough = false;

  CAEStreamInfo::DataType streamType = m_audioSink.GetPassthroughStreamType(
      m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      m_streaminfo, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);

  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough())
  {
    // passthrough state has not changed
    return false;
  }

  m_pAudioCodec = std::move(codec);

  // LAV Audio: configure sync on the freshly created codec (passthrough only)
  ConfigureLavAudioSync();

  return true;
}

void CVideoPlayerAudio::ConfigureLavAudioSync()
{
  if (!m_pAudioCodec || !m_pAudioCodec->NeedPassthrough())
    return;

  auto* passthroughCodec = dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
  if (!passthroughCodec)
    return;

  // LAV Audio is always on in this build, but its internal-clock model is not
  // appropriate for realtime/live streams (e.g. live PVR): those keep stock
  // passthrough behavior. Kodi already steers realtime audio to the resample
  // path and disables passthrough in SwitchCodecIfNeeded(), so a realtime
  // passthrough codec should not normally reach here - this check is a guard to
  // make sure the LAV path is never enabled for a realtime stream regardless.
  const bool enable = !m_processInfo.IsRealtimeStream();
  passthroughCodec->SetLavStyleSyncEnabled(enable);
  if (!enable)
    return;

  CLog::LogF(LOGDEBUG, "LAV Audio passthrough internal-clock retiming active");

  // If the codec is (re)created while playback is already in sync (e.g. a
  // display reset that flips passthrough), rebase its internal clock to the
  // master clock now so it does not latch onto a stale demuxer PTS.
  if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC && m_pClock)
    passthroughCodec->SyncToResyncPts(m_pClock->GetClock() + m_audioSink.GetDelay());
}

std::string CVideoPlayerAudio::GetPlayerInfo()
{
  std::unique_lock lock(m_info_section);
  return m_info.info;
}

int CVideoPlayerAudio::GetAudioChannels()
{
  return m_streaminfo.channels;
}

bool CVideoPlayerAudio::IsPassthrough() const
{
  std::unique_lock lock(m_info_section);
  return m_info.passthrough;
}
