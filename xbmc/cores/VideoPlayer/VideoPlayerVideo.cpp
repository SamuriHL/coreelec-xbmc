/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerVideo.h"

#include "cores/VideoPlayer/BDStageTrace.h"
#include "cores/VideoPlayer/StreamDiag.h"

#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include "platform/linux/SysfsPath.h"

#include <iomanip>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <chrono>

using namespace std::chrono_literals;

class CDVDMsgVideoCodecChange : public CDVDMsg
{
public:
  CDVDMsgVideoCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDVideoCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgVideoCodecChange() override = default;

  std::unique_ptr<CDVDVideoCodec> m_codec;
  CDVDStreamInfo  m_hints;
};

CVideoPlayerVideo::CVideoPlayerVideo(CDVDClock* pClock,
                                     CDVDOverlayContainer* pOverlayContainer,
                                     CDVDMessageQueue& parent,
                                     CRenderManager& renderManager,
                                     CProcessInfo& processInfo,
                                     double messageQueueTimeSize)
  : CThread("VideoPlayerVideo"),
    IDVDStreamPlayerVideo(processInfo),
    m_messageQueue("video"),
    m_messageParent(parent),
    m_renderManager(renderManager)
{
  m_pClock = pClock;
  m_pOverlayContainer = pOverlayContainer;
  m_speed = DVD_PLAYSPEED_NORMAL;

  m_bRenderSubs = false;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_iSubtitleDelay = 0;
  m_iLateFrames = 0;
  m_iDroppedRequest = 0;
  m_fForcedAspectRatio = 0;

  // queue data size is dynamical changed by stream
  m_messageQueue.SetMaxDataSize(LvLVideoMIN);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_iDroppedFrames = 0;
  m_fFrameRate = 25;
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount = 0;
  m_bAllowDrop = false;
  m_iFrameRateErr = 0;
  m_iFrameRateLength = 0;
  m_bFpsInvalid = false;
}

CVideoPlayerVideo::~CVideoPlayerVideo()
{
  m_bAbortOutput = true;
  StopThread();
}

double CVideoPlayerVideo::GetOutputDelay()
{
  double time = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET);
  if( m_fFrameRate )
    time = (time * DVD_TIME_BASE) / m_fFrameRate;
  else
    time = 0.0;

  if( m_speed != 0 )
    time = time * DVD_PLAYSPEED_NORMAL / abs(m_speed);

  return time;
}

bool CVideoPlayerVideo::OpenStream(CDVDStreamInfo hint)
{
  if (hint.flags & AV_DISPOSITION_ATTACHED_PIC)
    return false;
  if (!hint.extradata)
  {
    // codecs which require extradata
    // clang-format off
    if (hint.codec == AV_CODEC_ID_NONE ||
        hint.codec == AV_CODEC_ID_MPEG1VIDEO ||
        hint.codec == AV_CODEC_ID_MPEG2VIDEO ||
        (hint.codec == AV_CODEC_ID_H264 && (hint.codec_tag == 0 || hint.codec_tag == MKTAG('a','v','c','1') || hint.codec_tag == MKTAG('a','v','c','2'))) ||
        hint.codec == AV_CODEC_ID_HEVC ||
        hint.codec == AV_CODEC_ID_MPEG4 ||
        hint.codec == AV_CODEC_ID_WMV3 ||
        hint.codec == AV_CODEC_ID_VC1)
    {
      CLog::LogF(LOGERROR, "Codec id {} require extradata.", hint.codec);
      return false;
    }
    // clang-format on
  }

  CLog::Log(LOGINFO, "Creating video codec: {}", avcodec_get_name(hint.codec));
  hint.pClock = m_pClock;

  if (m_messageQueue.IsInited())
  {
    if (m_pVideoCodec && !m_processInfo.IsVideoHwDecoder())
    {
      hint.codecOptions |= CODEC_ALLOW_FALLBACK;
    }

    std::unique_ptr<CDVDVideoCodec> codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);
    if (!codec)
    {
      CLog::Log(LOGINFO, "CVideoPlayerVideo::OpenStream - could not open video codec");
    }

    SendMessage(std::make_shared<CDVDMsgVideoCodecChange>(hint, std::move(codec)), 0);
  }
  else
  {
    m_processInfo.ResetVideoCodecInfo();
    hint.codecOptions |= CODEC_ALLOW_FALLBACK;

    std::unique_ptr<CDVDVideoCodec> codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);
    if (!codec)
    {
      CLog::Log(LOGERROR, "CVideoPlayerVideo::OpenStream - could not open video codec");
      return false;
    }

    OpenStream(hint, std::move(codec));
    CLog::Log(LOGINFO, "Creating video thread");
    m_messageQueue.Init();
    m_flushesQueued = 0;
    m_processInfo.SetLevelVQ(0);
    Create();
  }
  return true;
}

void CVideoPlayerVideo::OpenStream(CDVDStreamInfo& hint, std::unique_ptr<CDVDVideoCodec> codec)
{
  CLog::Log(LOGDEBUG, "CVideoPlayerVideo::OpenStream - open stream with codec: {}",
            avcodec_get_name(hint.codec));

  m_drainStarted = false;
  m_drainAbort = false;
  m_drainFlushed = false;

  m_processInfo.GetVideoBufferManager().ReleasePools();

  //reported fps is usually not completely correct
  if (hint.fpsrate && hint.fpsscale)
  {
    m_fFrameRate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                       (double)DVD_TIME_BASE * hint.fpsscale / hint.fpsrate);

    m_bFpsInvalid = false;
    m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
  }
  else
  {
    m_fFrameRate = 25;
    m_bFpsInvalid = true;
    m_processInfo.SetVideoFps(0);
  }

  m_ptsTracker.ResetVFRDetection();
  ResetFrameRateCalc();
  m_cadence.count = 0;
  m_cadenceSnap = aml_presentation_validated();

  m_iDroppedRequest = 0;
  m_iLateFrames = 0;

  if( m_fFrameRate > 120 || m_fFrameRate < 5 )
  {
    CLog::Log(LOGERROR,
              "CVideoPlayerVideo::OpenStream - Invalid framerate {}, using forced 25fps and just "
              "trust timestamps",
              (int)m_fFrameRate);
    m_fFrameRate = 25;
  }

  // use aspect in stream if available
  if (hint.forced_aspect)
    m_fForcedAspectRatio = static_cast<float>(hint.aspect);
  else
    m_fForcedAspectRatio = 0.0f;

  if (m_pVideoCodec && m_pVideoCodec->Reconfigure(hint))
  {
    // reuse old decoder
    codec = std::move(m_pVideoCodec);
  }

  m_pVideoCodec.reset();

  if (!codec)
  {
    CLog::Log(LOGINFO, "Creating video codec: {}", avcodec_get_name(hint.codec));
    hint.pClock = m_pClock;
    hint.codecOptions |= CODEC_ALLOW_FALLBACK;
    codec = CDVDFactoryCodec::CreateVideoCodec(hint, m_processInfo);
    if (!codec)
    {
      CLog::Log(LOGERROR, "CVideoPlayerVideo::OpenStream - could not open video codec");
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_ABORT));
      StopThread();
    }
  }

  m_pVideoCodec = std::move(codec);
  // design 17: a decoder that can run without sleeping gets the stream worker
  // (a stream change on a running thread without its eventfd stays legacy)
  m_worker = m_pVideoCodec && aml_presentation_validated() &&
             !XFILE::CFile::Exists("special://profile/streamsplit_video_off") &&
             !(IsRunning() && m_wakeFd < 0) && m_pVideoCodec->SetNonBlocking(true);
  // a reused decoder left non-blocking by a previous stream must sleep again
  if (!m_worker && m_pVideoCodec)
    m_pVideoCodec->SetNonBlocking(false);
  m_hints = hint;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_rewindStalled = false;
  m_packets.clear();
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_renderManager.ShowVideo(false);
}

void CVideoPlayerVideo::CloseStream(bool bWaitForBuffers)
{
  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0)
  {
    BeginDrain();
    // a display mode change pauses the clock mid-drain; that is not a stall
    m_messageQueue.WaitUntilEmpty([this] { return m_pClock->IsPaused(); });

    // The queue empties when the drain message is taken, while the segment's
    // last pictures are still inside the decoder: wait for the drain itself.
    XbmcThreads::EndTime<> timer(3000ms);
    XbmcThreads::EndTime<> heldBudget(30000ms); // a display that never comes back
    while (!m_drained.Wait(20ms))
    {
      if (m_drainAbort || m_messageQueue.ReceivedAbortRequest() || !IsRunning())
        break;
      if (m_pClock->IsPaused() && !heldBudget.IsTimePast())
        timer.Set(3000ms);
      else if (timer.IsTimePast())
      {
        CLog::Log(LOGWARNING, "CVideoPlayerVideo::CloseStream - drain not finished after 3s");
        break;
      }
    }
  }
  m_drainStarted = false;

  m_messageQueue.Abort();

  // wait for decode_video thread to end
  CLog::Log(LOGINFO, "waiting for video thread to exit");

  m_bAbortOutput = true;
  StopThread();

  m_messageQueue.End();
  m_flushesQueued = 0;

  CLog::Log(LOGINFO, "deleting video codec");
  m_pVideoCodec.reset();

  if (m_picture.videoBuffer)
  {
    m_picture.videoBuffer->Release();
    m_picture.videoBuffer = nullptr;
  }
}

void CVideoPlayerVideo::BeginDrain()
{
  if (m_drainStarted.exchange(true))
    return;
  m_drained.Reset();
  SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::VIDEO_DRAIN), 0);
}

void CVideoPlayerVideo::AbortDrain()
{
  m_drainAbort = true;
  m_drained.Set();
}

bool CVideoPlayerVideo::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerVideo::HasData() const
{
  bool ret;

  if (!(ret = (m_messageQueue.GetDataSize() > 0)))
  {
    if (m_pVideoCodec && m_processInfo.IsVideoHwDecoder())
      ret = m_pVideoCodec->GetDataLevel() > 0;
  }

  return ret;
}

bool CVideoPlayerVideo::IsInited() const
{
  return m_messageQueue.IsInited();
}

inline void CVideoPlayerVideo::SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority)
{
  m_messageQueue.Put(pMsg, priority);
  m_processInfo.SetLevelVQ(m_messageQueue.GetLevel());
}

inline void CVideoPlayerVideo::SendMessageBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  m_messageQueue.PutBack(pMsg, priority);
  m_processInfo.SetLevelVQ(m_messageQueue.GetLevel());
}

inline void CVideoPlayerVideo::FlushMessages()
{
  m_messageQueue.Flush();
  m_processInfo.SetLevelVQ(m_messageQueue.GetLevel());
}

inline MsgQueueReturnCode CVideoPlayerVideo::GetMessage(std::shared_ptr<CDVDMsg>& pMsg,
                                                        std::chrono::milliseconds timeout,
                                                        int& priority)
{
  MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);
  m_processInfo.SetLevelVQ(m_messageQueue.GetLevel());
  return ret;
}

void CVideoPlayerVideo::Process()
{
  CLog::Log(LOGINFO, "running thread: video_thread");

  double pts = 0;
  double frametime = (double)DVD_TIME_BASE / m_fFrameRate;

  bool bRequestDrop = false;
  int iDropDirective;
  bool onlyPrioMsgs = false;

  m_vfmt.clear();
  int vfmtCheckCount = 0;

  m_videoStats.Start();
  m_droppingStats.Reset();
  m_iDroppedFrames = 0;
  m_rewindStalled = false;
  m_outputSate = OUTPUT_NORMAL;

  if (aml_presentation_validated())
  {
    m_wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd >= 0)
    {
      m_messageQueue.SetWakeFd(m_wakeFd);
      m_renderManager.SetWorkerWakeFd(m_wakeFd);
    }
  }
  if (m_wakeFd < 0 && m_worker)
  {
    m_worker = false;
    m_pVideoCodec->SetNonBlocking(false);
  }
  m_idleSince = std::chrono::steady_clock::now();

  while (!m_bStop)
  {
    if (m_worker)
      WorkerDuties();

    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::micro>(m_stalled ? frametime : frametime * 10));
    int iPriority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      iPriority = 1;

    if (m_paused)
      iPriority = 1;

    if (onlyPrioMsgs)
    {
      iPriority = 1;
      timeout = 1ms;
    }

    // the worker takes no packet while a barrier, a drain or the forced start
    // picture is pending, nor until the decoder's next poll after a refusal
    if (m_worker)
    {
      const auto now = std::chrono::steady_clock::now();
      if (m_pendingSync || m_draining || m_startPendingGen >= 0 || now < m_feedBlockedUntil ||
          now < m_collectHoldoffUntil || now < m_outputRetryAt ||
          (m_outputSate == OUTPUT_AGAIN && m_picture.videoBuffer))
        iPriority = 1;
    }

    std::shared_ptr<CDVDMsg> pMsg;
    STREAMDIAG::MaybeReport();
    MsgQueueReturnCode ret;
    {
      STREAMDIAG::Span sd(iPriority ? STREAMDIAG_SITE("video wait GetMessage prio")
                                    : STREAMDIAG_SITE("video wait GetMessage"));
      ret = m_worker ? WorkerGetMessage(pMsg, timeout, iPriority)
                     : GetMessage(pMsg, timeout, iPriority);
    }
    if (ret == MSGQ_OK && pMsg && STREAMDIAG::Enabled() &&
        pMsg->m_diagPutNs.load() &&
        !pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      static int sites[64][2] = {};
      const int t = static_cast<int>(pMsg->GetMessageType()) - CDVDMsg::NONE;
      const int o = STREAMDIAG::PutBeforeOpenEnd(pMsg->m_diagPutNs.load()) ? 1 : 0;
      if (t >= 0 && t < 64)
      {
        if (!sites[t][o])
          sites[t][o] = STREAMDIAG::Site(("msg video " + std::to_string(t) + (o ? " open" : "")).c_str()) + 1;
        STREAMDIAG::Record(sites[t][o] - 1, std::chrono::duration<double, std::micro>(
                                             std::chrono::steady_clock::now().time_since_epoch() -
                                             std::chrono::steady_clock::duration(pMsg->m_diagPutNs.load()))
                                             .count());
        // the priority control messages (RESYNC, FLUSH, PAUSE, SETSPEED,
        // REQUEST_STATE, DISPLAY_RESET)
        const int64_t putNs = pMsg->m_diagPutNs.load();
        if (!o && (t == 1 || t == 2 || t == 6 || t == 20 || t == 21 || t == 28) &&
            std::chrono::steady_clock::now().time_since_epoch().count() - putNs > 5000000)
          STREAMDIAG::SlowMessage("video", t, putNs);
      }
    }

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        CLog::Log(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (m_worker)
      {
        const auto now = std::chrono::steady_clock::now();
        // the forced start picture first, and the duties' hold-offs
        if (m_startPendingGen >= 0 || now < m_collectHoldoffUntil || now < m_outputRetryAt)
          continue;
        if (m_draining)
        {
          WorkerDrainStep(frametime, pts);
          continue;
        }
      }
      if (m_outputSate == OUTPUT_AGAIN &&
          m_picture.videoBuffer)
      {
        m_outputSate = OutputPicture(&m_picture);
        // the worker retries a held picture at every wake: once, when it goes out
        if (m_processInfo.IsVideoHwDecoder() && (!m_worker || m_outputSate != OUTPUT_AGAIN))
        {
          vfmtCheckCount = 16;
          CLog::Log(LOGDEBUG, "CVideoPlayerVideo - OUTPUT_AGAIN - vfmt, interlace should be checked.");
        }
        if (m_outputSate == OUTPUT_AGAIN)
        {
          onlyPrioMsgs = true;
          continue;
        }
        m_idleSince = std::chrono::steady_clock::now();
      }
      // don't ask for a new frame if we can't deliver it to renderer
      else if ((m_speed != DVD_PLAYSPEED_PAUSE ||
                m_processInfo.IsFrameAdvance() ||
                m_syncState != IDVDStreamPlayer::SYNC_INSYNC) && !m_paused)
      {
        if (ProcessDecoderOutput(frametime, pts))
        {
          onlyPrioMsgs = true;
          continue;
        }
      }

      // if we only wanted priority messages, this isn't a stall
      if (iPriority)
        continue;

      // a start held for the output mode, or in its scheduled lead, has its
      // clock paused on purpose (at 50/60 fps the timeout, 10 frames, is
      // shorter than the 350 ms lead)
      int64_t startNs = 0;
      double startClock = 0.0;
      if (m_startHeld || m_pClock->GetPendingStart(startNs, startClock))
        continue;

      // an output wake, not the stall timeout
      if (m_worker && !m_workerTimedOut)
        continue;

      //Okey, start rendering at stream fps now instead, we are likely in a stillframe
      if (!m_stalled)
      {
        // squeeze pictures out
        while (!m_bStop && m_pVideoCodec)
        {
          m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
          if (!ProcessDecoderOutput(frametime, pts) || m_processInfo.IsVideoHwDecoder())
            break;
        }

        CLog::Log(LOGDEBUG, "CVideoPlayerVideo - Stillframe detected, switching to forced {:f} fps",
                  m_fFrameRate);
        m_stalled = true;
        pts += frametime * 4;
      }

      // Waiting timed out, output last picture
      if (m_picture.videoBuffer)
      {
        m_picture.pts = pts;
        m_outputSate = OutputPicture(&m_picture);
        pts += frametime;
      }
      m_idleSince = std::chrono::steady_clock::now();

      continue;
    }

    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      auto sync = std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg);
      bool synced;
      {
        STREAMDIAG::Span sd(STREAMDIAG_SITE("video wait SYNCHRONIZE"));
        // the worker registers its arrival and checks the barrier as a duty
        synced = sync->Wait(m_worker ? 0ms : 100ms, SYNCSOURCE_VIDEO);
      }
      if (synced)
      {
        CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::GENERAL_SYNCHRONIZE");
      }
      else if (m_worker)
        m_pendingSync = sync;
      else
        SendMessage(pMsg, 1); /* push back as prio message, to process other prio messages */
      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    {
      pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;

      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_droppingStats.Reset();
      m_rewindStalled = false;
      m_renderManager.ShowVideo(true);

      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::GENERAL_RESYNC({:f})", pts);
      if (m_processInfo.IsVideoHwDecoder())
      {
        vfmtCheckCount = 16;
        CLog::Log(LOGDEBUG, "CVideoPlayerVideo - OUTPUT_AGAIN - vfmt, interlace should be checked.");
      }
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_SET_ASPECT))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::VIDEO_SET_ASPECT");
      m_fForcedAspectRatio = static_cast<float>(*std::static_pointer_cast<CDVDMsgDouble>(pMsg));
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      WorkerCancelStart();
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();

      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
      m_packets.clear();
      m_droppingStats.Reset();
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
      m_renderManager.ShowVideo(false);
      m_rewindStalled = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SEGMENT_GEN))
    {
      m_renderManager.SetIncomingSegmentGen(
          static_cast<unsigned int>(std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value));
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SEGMENT_RESET))
    {
      // Light counterpart of GENERAL_RESET for a seamless segment boundary:
      // drop the codec's per-segment state only. The current picture, the
      // queued packets, the sync state and the renderer are all left alone -
      // the stream runs continuously across this boundary, so releasing the
      // picture, restarting the sync handshake (SYNC_STARTING) or hiding video
      // (ShowVideo(false)) would themselves be the glitch we are avoiding.
      if (m_pVideoCodec)
        m_pVideoCodec->ResetSegmentState();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH)) // private message sent by (CVideoPlayerVideo::Flush())
    {
      STREAMDIAG::CtxScope sdctx(STREAMDIAG::Ctx::FLUSH);
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      WorkerCancelStart();
      m_draining = false;
      m_feedBlockedUntil = {};
      m_collectHoldoffUntil = {};
      m_outputRetryAt = {};
      if(m_pVideoCodec)
      {
        STREAMDIAG::Span sd(STREAMDIAG_SITE("video codec Reset"));
        m_pVideoCodec->Reset();
      }

      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
      m_packets.clear();
      pts = 0;
      m_rewindStalled = false;

      m_ptsTracker.Flush();
      m_cadence.count = 0;
      //we need to recalculate the framerate
      //! @todo this needs to be set on a streamchange instead
      ResetFrameRateCalc();
      m_droppingStats.Reset();

      m_stalled = true;
      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_renderManager.ShowVideo(false);
      }

      m_renderManager.DiscardBuffer();
      FlushMessages();
      // FlushMessages takes only packets; the flushed stream's end-of-stream
      // drain (queued when the demuxer hit its end) goes with them
      m_messageQueue.Flush(CDVDMsg::VIDEO_DRAIN);
      m_drainStarted = false;
      m_drainFlushed = false;
      m_drained.Set();
      if (m_flushesQueued > 0)
        --m_flushesQueued;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      m_speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      if (m_pVideoCodec)
        m_pVideoCodec->SetSpeed(m_speed);

      m_droppingStats.Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgVideoCodecChange>(pMsg);

      while (!m_bStop && m_pVideoCodec)
      {
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN);
        bool cont = ProcessDecoderOutput(frametime, pts);

        if (!cont)
          break;
        // a stream change drains in place (bounded by the decoder's drain);
        // the worker's decoder does not sleep there itself, and a picture
        // waiting for a buffer gets WaitForBuffer's 500 ms
        if (m_worker && m_lastDecoderState == CDVDVideoCodec::VC_NONE)
          CThread::Sleep(2ms);
        for (int i = 0; m_worker && m_outputSate == OUTPUT_AGAIN && m_picture.videoBuffer &&
                        i < 250 && !m_bStop;
             ++i)
        {
          CThread::Sleep(2ms);
          m_outputSate = OutputPicture(&m_picture);
        }
      }

      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = NULL;
      if (m_picture.videoBuffer)
      {
        m_picture.videoBuffer->Release();
        m_picture.videoBuffer = nullptr;
      }
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_DRAIN) && m_worker)
    {
      // a drain is a state: one step per wake (WorkerDrainStep)
      m_draining = true;
      m_workerOutputDue = true;
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_DRAIN))
    {
      while (!m_bStop && m_pVideoCodec && !m_drainAbort && !m_drainFlushed)
      {
        // a paused clock (a display mode change) holds the output; the tail
        // has not ended, and the decoder is not starved
        if (m_pClock->IsPaused())
        {
          CThread::Sleep(20ms);
          continue;
        }
        // only a closing stream ends: the end-of-file path also sends VIDEO_DRAIN
        // at a transient empty read (a seek), after which input resumes
        m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN |
                                       (m_drainStarted ? DVD_CODEC_CTRL_DRAIN_EOS : 0));
        if (ProcessDecoderOutput(frametime, pts))
          continue;
        // a paused clock (a display mode change) holds the tail; it has not ended
        if (m_pClock->IsPaused() && !m_drainAbort && !m_drainFlushed && !m_bStop)
        {
          CThread::Sleep(20ms);
          continue;
        }
        break;
      }
      m_drained.Set();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (m_stalled)
      {
        CLog::Log(LOGDEBUG, "CVideoPlayerVideo - Stillframe left, switching to normal playback");
        m_stalled = false;
      }

      bRequestDrop = false;
      iDropDirective = CalcDropRequirement(pts);

      // Framerate calibration exists to avoid dropping frames during normal playback before we're
      // confident what the real framerate is. That doesn't apply during trick-play and
      // DROP_VERYlATE must not be silently disabled.
      if ((iDropDirective & DROP_VERYLATE) && (m_bAllowDrop || m_speed > DVD_PLAYSPEED_NORMAL) &&
          !bPacketDrop)
      {
        bRequestDrop = true;
      }
      if (iDropDirective & DROP_DROPPED)
      {
        m_iDroppedFrames++;
        m_ptsTracker.Flush();
      }
      if (m_messageQueue.GetDataSize() == 0 ||  m_speed < 0)
      {
        bRequestDrop = false;
        m_iDroppedRequest = 0;
        m_iLateFrames = 0;
      }

      int codecControl = 0;
      if (iDropDirective & DROP_BUFFER_LEVEL)
        codecControl |= DVD_CODEC_CTRL_HURRY;
      if (m_speed > DVD_PLAYSPEED_NORMAL)
        codecControl |= DVD_CODEC_CTRL_NO_POSTPROC;
      if (bPacketDrop)
        codecControl |= DVD_CODEC_CTRL_DROP;
      if (bRequestDrop)
        codecControl |= DVD_CODEC_CTRL_DROP_ANY;
      if (!m_renderManager.Supports(RENDERFEATURE_ROTATION))
        codecControl |= DVD_CODEC_CTRL_ROTATE;
      m_pVideoCodec->SetCodecControl(codecControl);

      bool added;
      {
        STREAMDIAG::Span sd(STREAMDIAG_SITE("video AddData"));
        added = m_pVideoCodec->AddData(*pPacket);
      }
      if (added)
      {
        // buffer packets so we can recover should decoder flush for some reason
        if (m_pVideoCodec->GetConvergeCount() > 0)
        {
          m_packets.emplace_back(pMsg, 0);
          if (m_packets.size() > m_pVideoCodec->GetConvergeCount() ||
              m_packets.size() * frametime > DVD_SEC_TO_TIME(10))
            m_packets.pop_front();
        }

        m_videoStats.AddSampleBytes(pPacket->iSize);
        UpdatePlayerInfo();

        if (ProcessDecoderOutput(frametime, pts))
        {
          onlyPrioMsgs = true;
        }

        if (vfmtCheckCount > 0 && --vfmtCheckCount % 5 == 0)
        {
          CSysfsPath frame_format{"/sys/class/deinterlace/di0/frame_format"};
          if (frame_format.Exists())
            m_vfmt = frame_format.Get<std::string>().value_or("");
          // Only update interlace state from vfmt when it gives a definitive answer.
          // For MBAFF content, the DI module transiently reports "progressive"
          // (reflecting current macroblock type), then falls back to "null".
          // Don't let a transient "progressive" clear interlace when the demuxer
          // flagged the stream as interlaced — the demuxer is authoritative for
          // the overall stream type, vfmt only for truly misidentified content.
          if (m_vfmt.size() > 4)
          {
            bool vfmtIsInterlaced = m_vfmt.compare("progressive") != 0;
            if (vfmtIsInterlaced || !m_hints.interlaced)
            {
              m_processInfo.SetVideoInterlaced(vfmtIsInterlaced);
              vfmtCheckCount = 0;
            }
          }
          CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::DEMUXER_PACKET - checking interlace vfmt: {}", m_vfmt);
        }
      }
      else
      {
        // the worker puts a refused replay back at its own priority (ahead of
        // the replays behind it) and takes no packet until the decoder's next poll
        SendMessageBack(pMsg, m_worker ? iPriority : 0);
        onlyPrioMsgs = true;
        if (m_worker)
          m_feedBlockedUntil = std::chrono::steady_clock::now() +
                               m_pVideoCodec->GetNextPoll(m_speed == DVD_PLAYSPEED_PAUSE);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      CLog::Log(LOGINFO, "CVideoPlayerVideo: display reset occurred, clear skipped frames");
      m_renderManager.DisplayReset();
    }
  }

  if (m_wakeFd >= 0)
  {
    // the writers stop under their own locks before the fd goes
    m_messageQueue.SetWakeFd(-1);
    m_renderManager.SetWorkerWakeFd(-1);
    WorkerCancelStart();
    close(m_wakeFd);
    m_wakeFd = -1;
  }
}

MsgQueueReturnCode CVideoPlayerVideo::WorkerGetMessage(std::shared_ptr<CDVDMsg>& pMsg,
                                                       std::chrono::milliseconds timeout,
                                                       int& priority)
{
  using namespace std::chrono;
  m_workerTimedOut = false;
  for (;;)
  {
    // an event since the last poll is still pending, and wakes the next one
    MsgQueueReturnCode ret = GetMessage(pMsg, 0ms, priority);
    if (ret != MSGQ_TIMEOUT)
    {
      m_idleSince = steady_clock::now();
      return ret;
    }
    if (m_workerOutputDue)
    {
      m_workerOutputDue = false;
      return MSGQ_TIMEOUT;
    }

    // the stall timeout runs from the last message or picture (priority-only
    // waits are no stall)
    const auto now = steady_clock::now();
    auto due = now + duration_cast<steady_clock::duration>(
                         m_pVideoCodec ? m_pVideoCodec->GetNextPoll(m_speed == DVD_PLAYSPEED_PAUSE)
                                       : microseconds(40000));
    if (!priority)
    {
      const auto stall = m_idleSince + timeout;
      if (now >= stall)
      {
        // each stall wait is a full timeout, as a blocking Get is
        m_idleSince = now;
        m_workerTimedOut = true;
        return MSGQ_TIMEOUT;
      }
      due = std::min(due, stall);
    }
    // while the output is held the decoder is not polled (as the blocking
    // loop slept or waited there): the hold's own end is the deadline
    const auto holdEnd = std::max(m_collectHoldoffUntil, m_outputRetryAt);
    if (holdEnd > now)
      due = holdEnd;
    if (m_startPendingGen >= 0)
      due = m_startPendingSince + 200ms;
    if (m_pendingSync)
      due = std::min(due, now + 10ms);
    if (m_feedBlockedUntil > now)
      due = std::min(due, m_feedBlockedUntil);

    struct pollfd fds[2] = {};
    int nfds = 0;
    fds[nfds].fd = m_wakeFd;
    fds[nfds++].events = POLLIN;
    const int pictureFd = (m_armPicture && m_outputSate != OUTPUT_AGAIN && m_startPendingGen < 0 &&
                           m_pVideoCodec)
                              ? m_pVideoCodec->GetPictureWakeFd()
                              : -1;
    if (pictureFd >= 0)
    {
      fds[nfds].fd = pictureFd;
      fds[nfds++].events = POLLIN;
    }
    const auto wait = duration_cast<microseconds>(due - now);
    const int waitMs =
        wait.count() <= 0 ? 0 : static_cast<int>((wait.count() + 999) / 1000);
    {
      STREAMDIAG::Span sd(STREAMDIAG_SITE("video wait worker poll"));
      poll(fds, nfds, waitMs);
    }
    // consumed before the state is read again: an event after this read wakes
    // the next poll
    uint64_t events = 0;
    [[maybe_unused]] ssize_t n = read(m_wakeFd, &events, sizeof(events));
    if (m_bStop)
      return MSGQ_TIMEOUT;
    // any wake or deadline: a message is taken first, else the output is tried once
    m_workerOutputDue = true;
    // a picture fd that woke is not armed again until a picture is taken
    m_armPicture = false;
  }
}

void CVideoPlayerVideo::WorkerDuties()
{
  const auto now = std::chrono::steady_clock::now();

  // input the decoder took in part is written at every wake, packets or not
  if (m_pVideoCodec)
    m_pVideoCodec->ServiceInput();

  if (m_startPendingGen >= 0)
  {
    const bool taken = m_renderManager.ForcedPictureTaken(m_startPendingGen);
    if (taken || now - m_startPendingSince >= 200ms)
    {
      if (!taken)
      {
        m_renderManager.CancelForce(m_startPendingGen);
        CLog::Log(LOGWARNING, "CVideoPlayerVideo - timeout waiting for the start picture to render");
      }
      m_startPendingGen = -1;
      if (m_startPendingMsg)
        m_messageParent.Put(m_startPendingMsg);
      m_startPendingMsg.reset();
      m_workerOutputDue = true;
    }
  }

  if (m_pendingSync && m_pendingSync->Wait(0ms, SYNCSOURCE_VIDEO))
  {
    CLog::Log(LOGDEBUG, "CVideoPlayerVideo - CDVDMsg::GENERAL_SYNCHRONIZE");
    m_pendingSync.reset();
  }
}

void CVideoPlayerVideo::WorkerCancelStart()
{
  if (m_startPendingGen >= 0)
    m_renderManager.CancelForce(m_startPendingGen);
  m_startPendingGen = -1;
  m_startPendingMsg.reset();
}

void CVideoPlayerVideo::WorkerDrainStep(double& frametime, double& pts)
{
  const auto finish = [this] {
    m_draining = false;
    m_drained.Set();
  };
  if (m_bStop || !m_pVideoCodec || m_drainAbort || m_drainFlushed)
  {
    finish();
    return;
  }
  // a paused clock (a display mode change) holds the output; the tail has not
  // ended, and the decoder is not starved
  if (m_pClock->IsPaused())
  {
    m_collectHoldoffUntil = std::chrono::steady_clock::now() + 20ms;
    return;
  }
  // only a closing stream ends: the end-of-file path also sends VIDEO_DRAIN
  // at a transient empty read (a seek), after which input resumes
  // a picture waiting for a render buffer goes first (a freed buffer wakes us)
  if (m_outputSate == OUTPUT_AGAIN && m_picture.videoBuffer)
  {
    m_outputSate = OutputPicture(&m_picture);
    if (m_outputSate == OUTPUT_AGAIN)
      return;
  }
  m_pVideoCodec->SetCodecControl(DVD_CODEC_CTRL_DRAIN |
                                 (m_drainStarted ? DVD_CODEC_CTRL_DRAIN_EOS : 0));
  if (ProcessDecoderOutput(frametime, pts))
  {
    if (m_lastDecoderState != CDVDVideoCodec::VC_NONE && m_outputSate != OUTPUT_AGAIN)
      m_workerOutputDue = true;
    return;
  }
  if (m_pClock->IsPaused() && !m_drainAbort && !m_drainFlushed && !m_bStop)
  {
    m_collectHoldoffUntil = std::chrono::steady_clock::now() + 20ms;
    return;
  }
  finish();
}

void CVideoPlayerVideo::UpdatePlayerInfo()
{
  m_processInfo.SetVideoLiveBitRate(GetVideoBitrate());
  m_processInfo.SetVideoQueueLevel(std::min(99, m_messageQueue.GetLevel()));
  m_processInfo.SetVideoQueueDataLevel(std::min(99, m_messageQueue.GetLevel(true)));
}

double CVideoPlayerVideo::SnapToCadence(double pts)
{
  // whole milliseconds at a known, normal-speed frame rate; a pts the decoder
  // filled in is not whole, and follows a cadence already held
  if (!m_cadenceSnap || m_bFpsInvalid || m_speed != DVD_PLAYSPEED_NORMAL)
  {
    m_cadence.count = 0;
    return pts;
  }
  const double ms = pts / 1000.0;
  const bool wholeMs = std::abs(ms - std::round(ms)) <= 0.001;
  const double duration = DVD_TIME_BASE / m_fFrameRate;
  if (!wholeMs && (m_cadence.count < 2 || m_cadence.duration != duration))
  {
    m_cadence.count = 0;
    return pts;
  }
  if (m_cadence.count == 0 || m_cadence.duration != duration)
  {
    m_cadence = {pts, duration, 1};
    return pts;
  }
  const double k = std::round((pts - m_cadence.phase) / duration);
  const double residual = pts - (m_cadence.phase + k * duration);
  // half a ms of rounding plus as much in the phase: more is not rounding
  if (std::abs(residual) > DVD_MSEC_TO_TIME(1.1))
  {
    if (m_cadence.count > 1)
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - pts {:.3f} off the cadence by {:.3f} ms",
                pts / DVD_TIME_BASE, residual / 1000.0);
    m_cadence = {pts, duration, 1};
    return pts;
  }
  if (!wholeMs)
    return m_cadence.phase + k * duration;
  if (m_cadence.count == 1)
    CLog::Log(LOGDEBUG, "CVideoPlayerVideo - millisecond pts: on the {:.3f} ms cadence",
              duration / 1000.0);
  m_cadence.count = std::min(m_cadence.count + 1, 48);
  m_cadence.phase += residual / m_cadence.count;
  return m_cadence.phase + k * duration;
}

bool CVideoPlayerVideo::ProcessDecoderOutput(double &frametime, double &pts)
{
  CDVDVideoCodec::VCReturn decoderState;
  {
    STREAMDIAG::Span sd(STREAMDIAG_SITE("video GetPicture"));
    decoderState = m_pVideoCodec->GetPicture(&m_picture);
  }
  m_lastDecoderState = decoderState;
  m_armPicture = false;

  // phase 0 counter (docs/real_player_disc_session_design.md 4.1): a run of
  // VC_NONE starves the priority-0 queue
  if (decoderState == CDVDVideoCodec::VC_NONE)
  {
    ++m_vcNoneRun;
    if (m_vcNoneRun == 50 || m_vcNoneRun % 1000 == 0)
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - phase0: {} consecutive VC_NONE, {} msgs queued, speed {}",
                m_vcNoneRun, m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET), m_speed);
  }
  else
  {
    if (m_vcNoneRun >= 50)
      CLog::Log(LOGDEBUG, "CVideoPlayerVideo - phase0: VC_NONE run ended after {} (state {})",
                m_vcNoneRun, static_cast<int>(decoderState));
    m_vcNoneRun = 0;
  }

  if (decoderState == CDVDVideoCodec::VC_BUFFER)
  {
    return false;
  }

  // if decoder was flushed, we need to seek back again to resume rendering
  if (decoderState == CDVDVideoCodec::VC_FLUSHED)
  {
    CLog::Log(LOGDEBUG, "CVideoPlayerVideo - video decoder was flushed");
    STREAMDIAG::CtxScope sdctx(STREAMDIAG::Ctx::CODECFLUSH);
    if (STREAMDIAG::Enabled())
    {
      size_t bytes = 0;
      for (const auto& p : m_packets)
        bytes += std::static_pointer_cast<CDVDMsgDemuxerPacket>(p.message)->GetPacketSize();
      CLog::Log(LOGINFO, "STREAMDIAG replay VC_FLUSHED packets {} bytes {}", m_packets.size(), bytes);
    }
    while (!m_packets.empty())
    {
      auto msg = std::static_pointer_cast<CDVDMsgDemuxerPacket>(m_packets.front().message);
      m_packets.pop_front();

      SendMessage(msg, 10);
    }

    m_pVideoCodec->Reset();
    m_packets.clear();
    //picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    m_renderManager.DiscardBuffer();
    return false;
  }

  if (decoderState == CDVDVideoCodec::VC_REOPEN)
  {
    STREAMDIAG::CtxScope sdctx(STREAMDIAG::Ctx::CODECFLUSH);
    if (STREAMDIAG::Enabled())
    {
      size_t bytes = 0;
      for (const auto& p : m_packets)
        bytes += std::static_pointer_cast<CDVDMsgDemuxerPacket>(p.message)->GetPacketSize();
      CLog::Log(LOGINFO, "STREAMDIAG replay VC_REOPEN packets {} bytes {}", m_packets.size(), bytes);
    }
    while (!m_packets.empty())
    {
      auto msg = std::static_pointer_cast<CDVDMsgDemuxerPacket>(m_packets.front().message);
      m_packets.pop_front();
      SendMessage(msg, 10);
    }

    m_pVideoCodec->Reopen();
    m_packets.clear();
    m_renderManager.DiscardBuffer();
    return false;
  }

  // if decoder had an error, tell it to reset to avoid more problems
  if (decoderState == CDVDVideoCodec::VC_ERROR)
  {
    CLog::Log(LOGDEBUG, "CVideoPlayerVideo - video decoder returned error");
    return false;
  }

  if (decoderState == CDVDVideoCodec::VC_EOF)
  {
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
    {
      SStartMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.cachetime = DVD_MSEC_TO_TIME(50);
      msg.cachetotal = DVD_MSEC_TO_TIME(100);
      msg.timestamp = DVD_NOPTS_VALUE;
      msg.timestampPending = true;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));
    }
    return false;
  }

  // check for a new picture
  if (decoderState == CDVDVideoCodec::VC_PICTURE)
  {
    bool hasTimestamp = true;

    // Detect progressive content misidentified as interlaced: if picture
    // duration consistently equals double what the fps implies, halve fps.
    // Never override when the demuxer flagged interlaced (hint.interlaced) —
    // MBAFF streams have genuine progressive macroblocks that cause transient
    // "progressive" vfmt readings and 40ms frame durations, but the stream
    // is still interlaced overall. Only allow for runtime-detected interlace
    // (not demuxer-flagged) when hardware confirms progressive.
    if (m_processInfo.GetVideoInterlaced() &&
        !m_hints.interlaced &&
        m_vfmt == "progressive" &&
        MathUtils::FloatEquals(static_cast<float>(m_picture.iDuration), static_cast<float>(2 * DVD_TIME_BASE) / m_processInfo.GetVideoFps(), 700.0f))
    {
      if (++m_retryProgressive > 3)
      {
        float halvedFps = m_processInfo.GetVideoFps() / 2.0f;
        m_processInfo.SetVideoFps(halvedFps);
        m_processInfo.SetVideoInterlaced(false);
        m_renderManager.TriggerUpdateResolution(halvedFps, m_hints.width, m_hints.height, m_hints.stereo_mode);
      }
    }
    else
      m_retryProgressive = 0;

    m_picture.iDuration = frametime;

    // validate picture timing,
    // if both dts/pts invalid, use pts calculated from picture.iDuration
    // if pts invalid use dts, else use picture.pts as passed
    if (m_picture.dts == DVD_NOPTS_VALUE && m_picture.pts == DVD_NOPTS_VALUE)
    {
      m_picture.pts = pts;
      hasTimestamp = false;
    }
    else if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = m_picture.dts;

    // use forced aspect if any
    if (m_fForcedAspectRatio != 0.0f)
    {
      m_picture.iDisplayWidth = (int) (m_picture.iDisplayHeight * m_fForcedAspectRatio);
      if (m_picture.iDisplayWidth > m_picture.iWidth)
      {
        m_picture.iDisplayWidth =  m_picture.iWidth;
        m_picture.iDisplayHeight = (int) (m_picture.iDisplayWidth / m_fForcedAspectRatio);
      }
    }

    // set stereo mode if not set by decoder
    if (m_picture.stereoMode.empty())
    {
      std::string stereoMode;
      switch (static_cast<RenderStereoMode>(m_processInfo.GetVideoSettings().m_StereoMode))
      {
        case RenderStereoMode::SPLIT_VERTICAL:
          stereoMode = "left_right";
          if (m_processInfo.GetVideoSettings().m_StereoInvert)
            stereoMode = "right_left";
          break;
        case RenderStereoMode::SPLIT_HORIZONTAL:
          stereoMode = "top_bottom";
          if (m_processInfo.GetVideoSettings().m_StereoInvert)
            stereoMode = "bottom_top";
          break;
        case RenderStereoMode::HARDWAREBASED:
          stereoMode = "block_lr";
          if (m_processInfo.GetVideoSettings().m_StereoInvert)
            stereoMode = "block_rl";
          break;
        default:
          stereoMode = m_hints.stereo_mode;
          break;
      }
      if (!stereoMode.empty() && stereoMode != "mono")
      {
        m_picture.stereoMode = stereoMode;
      }
    }

    // if frame has a pts (usually originating from demux packet), use that
    if (m_picture.pts != DVD_NOPTS_VALUE)
    {
      pts = SnapToCadence(m_picture.pts);
    }

    double extraDelay = 0.0;
    if (m_picture.iRepeatPicture)
    {
      extraDelay = m_picture.iRepeatPicture * m_picture.iDuration;
      m_picture.iDuration += extraDelay;
    }

    m_picture.pts = pts + extraDelay;
    // guess next frame pts. iDuration is always valid
    if (m_speed != 0)
      pts += m_picture.iDuration * m_speed / abs(m_speed);

    m_outputSate = OutputPicture(&m_picture);

    if (m_outputSate == OUTPUT_AGAIN)
    {
      return true;
    }
    else if (m_outputSate == OUTPUT_ABORT)
    {
      return false;
    }
    else if ((m_outputSate == OUTPUT_DROPPED) && !(m_picture.iFlags & DVP_FLAG_DROPPED))
    {
      m_iDroppedFrames++;
      m_ptsTracker.Flush();
    }
    // a picture was taken: the worker wakes for the next one as soon as it is ready
    m_armPicture = m_worker;
    m_idleSince = std::chrono::steady_clock::now();

    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING &&
        m_outputSate != OUTPUT_DROPPED &&
        !(m_picture.iFlags & DVP_FLAG_DROPPED))
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      SStartMsg msg;
      msg.player = VideoPlayer_VIDEO;
      msg.cachetime = DVD_MSEC_TO_TIME(50); //! @todo implement
      msg.cachetotal = DVD_MSEC_TO_TIME(100); //! @todo implement

      // Amlogic hardware deinterlace pipeline latency compensation.
      // When interlaced content is decoded by AML hardware, the VFM pipeline
      // includes a deinterlace module (di0) that buffers multiple fields before
      // producing output (buffer_keep_count=3, start_frame_drop=2, plus post-
      // processing). Kodi captures PTS via V4L2 DQBUF *before* the DI stage,
      // so the frame appears on screen ~240ms later than Kodi's sync expects.
      // Shift the video start timestamp forward to delay audio accordingly.
      double diCompensation = 0;
      if (m_processInfo.GetVideoInterlaced() && m_processInfo.IsVideoHwDecoder() &&
          CSysfsPath{"/sys/class/deinterlace/di0/frame_format"}.Exists())
      {
        constexpr int DI_PIPELINE_FIELDS = 12;
        diCompensation = DI_PIPELINE_FIELDS * DVD_TIME_BASE / m_fFrameRate;
        CLog::Log(LOGDEBUG, "CVideoPlayerVideo - DI pipeline latency compensation: "
                  "{:.0f}ms ({} fields at {:.1f}Hz)",
                  diCompensation / (DVD_TIME_BASE / 1000), DI_PIPELINE_FIELDS, m_fFrameRate);
      }

      msg.timestamp = hasTimestamp ? (pts + m_renderManager.GetDelay() * 1000 + diCompensation) : DVD_NOPTS_VALUE;
      auto started = std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg);
      // the worker sends it when a tick has taken the forced picture, as a
      // waiting AddVideoPicture would have returned first
      if (m_worker && m_startPendingGen >= 0)
        m_startPendingMsg = started;
      else
        m_messageParent.Put(started);
    }

    frametime = (double)DVD_TIME_BASE / m_fFrameRate;
  }

  return true;
}

void CVideoPlayerVideo::OnExit()
{
  CLog::Log(LOGINFO, "thread end: video_thread");
}

void CVideoPlayerVideo::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    SendMessage(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;
}

void CVideoPlayerVideo::Flush(bool sync)
{
  /* flush using message as this gets called from VideoPlayer thread */
  /* and any demux packet that has been taken out of queue need to */
  /* be disposed of before we flush */
  // Before the message, which queues behind whatever the video thread is already
  // doing - a hardware decoder can be deep into retrying a write of a packet
  // this flush will discard.
  if (m_pVideoCodec)
    m_pVideoCodec->Abort();

  // The flush discards the stream a drain is playing out. A drain in progress
  // holds while the clock is paused, and a seek or the next file keeps the clock
  // paused until this player starts again: without this the two wait on each
  // other until the player's start timeout.
  m_drainFlushed = true;
  if (m_worker)
    ++m_flushesQueued;
  SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);
  m_bAbortOutput = true;
}

void CVideoPlayerVideo::ProcessOverlays(const VideoPicture* pSource, double pts)
{
  // match at mid-frame so a start rounded just past the frame pts binds to it
  const double matchPts = pts + pSource->iDuration / 2;

  // remove any overlays that are out of time
  if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
    m_pOverlayContainer->CleanUp(matchPts - m_iSubtitleDelay);

  VecOverlays overlays;

  {
    std::unique_lock lock(*m_pOverlayContainer);

    VecOverlays* pVecOverlays = m_pOverlayContainer->GetOverlays();
    auto it = pVecOverlays->begin();

    //Check all overlays and render those that should be rendered, based on time and forced
    //Both forced and subs should check timing
    while (it != pVecOverlays->end())
    {
      std::shared_ptr<CDVDOverlay>& pOverlay = *it++;
      if(!pOverlay->bForced && !m_bRenderSubs)
        continue;

      // picked up at presentation time by the overlay renderer instead
      if (pOverlay->m_presentLatest)
        continue;

      // Keep-alive expiry (BD-J ARGB compositions maintained by continuous
      // re-posts, see CDVDOverlay::m_keepAliveTick): once the re-posts stop the
      // composition was abandoned - the disc sends no clear event - so stop
      // rendering it shortly after the stamp goes stale. Wall-clock by design
      // (render pts free-runs during stills; the player clock is title-offset).
      // Suspended while paused: the Xlet's repaint loop may pause with playback
      // and a visible subtitle must not vanish under a paused frame. The group
      // stays in the container untouched - the next re-post replaces it, and
      // menu-domain/decoder state is unaffected.
      if (pOverlay->m_keepAliveTick != 0 && m_speed != DVD_PLAYSPEED_PAUSE && !m_paused)
      {
        constexpr int64_t KEEPALIVE_TTL_MS = 1000;
        const int64_t stale = CurrentHostCounter() - pOverlay->m_keepAliveTick;
        if (stale > CurrentHostFrequency() * KEEPALIVE_TTL_MS / 1000)
        {
          // Diagnostics: logged once per distinct composition, not per
          // rendered frame. Expiry is meant for an abandoned BD-J self-drawn
          // subtitle, but the qualifying test is repaint cadence rather than
          // content - a BD-J menu that repaints fast enough to earn the stamp
          // and then goes static would be dropped here and simply vanish. If a
          // vanished-menu report ever arrives, this line names the culprit.
          if (m_lastExpiredKeepAliveTick != pOverlay->m_keepAliveTick)
          {
            m_lastExpiredKeepAliveTick = pOverlay->m_keepAliveTick;
            CLog::Log(LOGDEBUG,
                      "CVideoPlayerVideo::ProcessOverlays - keep-alive overlay expired "
                      "after {:.0f}ms without a re-post: dropping composition",
                      static_cast<double>(stale) * 1000.0 /
                          static_cast<double>(CurrentHostFrequency()));
          }
          continue;
        }
      }

      double pts2 = pOverlay->bForced ? matchPts : matchPts - m_iSubtitleDelay;

      if((pOverlay->iPTSStartTime <= pts2 && (pOverlay->iPTSStopTime > pts2 || pOverlay->iPTSStopTime == 0LL)))
      {

        pOverlay->m_3dSubtitleDepth = pSource->m_3dSubtitleDepth;

        if(pOverlay->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
          overlays.insert(overlays.end(),
                          static_cast<CDVDOverlayGroup&>(*pOverlay).m_overlays.begin(),
                          static_cast<CDVDOverlayGroup&>(*pOverlay).m_overlays.end());
        else
          overlays.push_back(pOverlay);
      }
    }

    for(it = overlays.begin(); it != overlays.end(); ++it)
    {
      double pts2 = (*it)->bForced ? pts : pts - m_iSubtitleDelay;
      m_renderManager.AddOverlay(*it, pts2);
    }
  }
}

CVideoPlayerVideo::EOutputState CVideoPlayerVideo::OutputPicture(const VideoPicture* pPicture)
{
  m_bAbortOutput = false;

  if (m_processInfo.GetVideoStereoMode() != pPicture->stereoMode)
  {
    m_processInfo.SetVideoStereoMode(pPicture->stereoMode);
    // signal about changes in video parameters
    m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  }

  double config_framerate = m_bFpsInvalid ?
    static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS()) :
    m_fFrameRate;
  if (m_processInfo.GetVideoInterlaced())
  {
    if (MathUtils::FloatEquals(config_framerate, 25.0, 0.02))
      config_framerate = 50.0;
    else if (MathUtils::FloatEquals(config_framerate, 29.97, 0.02))
      config_framerate = 59.94;
  }

  int sorient = m_processInfo.GetVideoSettings().m_Orientation;
  int orientation = sorient != 0 ? (sorient + m_hints.orientation) % 360
                                 : m_hints.orientation;

  bool configured;
  {
    STREAMDIAG::Span sd(STREAMDIAG_SITE("video Configure"));
    configured = m_renderManager.Configure(*pPicture, static_cast<float>(config_framerate),
                                           orientation, m_pVideoCodec->GetAllowedReferences());
  }
  if (!configured)
  {
    CLog::Log(LOGERROR, "{} - failed to configure renderer", __FUNCTION__);
    return OUTPUT_ABORT;
  }

  //try to calculate the framerate
  m_ptsTracker.Add(pPicture->pts);
  if (!m_stalled)
    CalcFrameRate();

  // signal to clock what our framerate is, it may want to adjust it's
  // speed to better match with our video renderer's output speed
  m_pClock->UpdateFramerate(m_fFrameRate);

  // calculate the time we need to delay this picture before displaying
  double iPlayingClock, iCurrentClock;

  iPlayingClock = m_pClock->GetClock(iCurrentClock, false); // snapshot current clock

  if (m_speed < 0)
  {
    double renderPts;
    int queued, discard;
    int lateframes;
    double inputPts = m_droppingStats.m_lastPts;
    m_renderManager.GetStats(lateframes, renderPts, queued, discard);
    if (pPicture->pts > renderPts || queued > 0)
    {
      if (inputPts >= renderPts)
      {
        m_rewindStalled = true;
        // the worker holds its collection for the same 50 ms instead
        if (m_worker)
          m_collectHoldoffUntil = std::chrono::steady_clock::now() + 50ms;
        else
          CThread::Sleep(50ms);
      }
      return OUTPUT_DROPPED;
    }
    else if (pPicture->pts < iPlayingClock)
    {
      return OUTPUT_DROPPED;
    }
  }

  if ((pPicture->iFlags & DVP_FLAG_DROPPED))
  {
    m_droppingStats.AddOutputDropGain(pPicture->pts, 1);
    CLog::Log(LOGDEBUG, "{} - dropped in output", __FUNCTION__);
    return OUTPUT_DROPPED;
  }

  auto timeToDisplay = std::chrono::milliseconds(DVD_TIME_TO_MSEC(pPicture->pts - iPlayingClock));

  // make sure waiting time is not negative
  std::chrono::milliseconds maxWaitTime = std::min(std::max(timeToDisplay + 500ms, 50ms), 500ms);
  // don't wait when going ff
  if (m_speed > DVD_PLAYSPEED_NORMAL)
    maxWaitTime = std::max(timeToDisplay, 0ms);

  int buffer;
  {
    STREAMDIAG::Span sd(STREAMDIAG_SITE("video wait WaitForBuffer"));
    if (m_worker)
    {
      // no wait: a freed buffer wakes the worker, and with the GUI not
      // rendering the discard is retried at the clock
      std::chrono::milliseconds retryIn{0};
      buffer = m_renderManager.TryGetBuffer(retryIn);
      if (buffer < 0 && retryIn > 0ms)
        m_outputRetryAt = std::chrono::steady_clock::now() + retryIn;
    }
    else
      buffer = m_renderManager.WaitForBuffer(m_bAbortOutput, maxWaitTime);
  }
  CLog::Log(LOGDEBUG,"CVideoPlayerVideo::{} - ttd:{:d}ms pts:{:.3f} Clock:{:.3f} Level:{:d}",
        __FUNCTION__, timeToDisplay.count(), pPicture->pts / DVD_TIME_BASE, static_cast<double>(iPlayingClock) / DVD_TIME_BASE, buffer);
  // Only once we are actually due and have a buffer: that is the first frame the
  // viewer sees for this segment.
  if (timeToDisplay.count() <= 0 && buffer >= 0)
    BDSTAGE::PictureShown();

  if (buffer < 0)
  {
    // The render buffer pool can fill up in trick-play FF, with more new pictures decoded than
    // are taken out at the display rate.
    // Drop the picture to let the renderer naturally drain the pool and make room for next iteration.
    // It will receive the most current new picture instead of whatever was decoded first (likely stale)
    if (m_speed > DVD_PLAYSPEED_NORMAL)
    {
      m_droppingStats.AddOutputDropGain(pPicture->pts, 1);
      return OUTPUT_DROPPED;
    }

    // a worker holds the picture until a buffer frees: no timeout
    if (m_speed != DVD_PLAYSPEED_PAUSE && !m_worker)
      CLog::Log(LOGWARNING, "{} - timeout waiting for buffer", __FUNCTION__);

    return OUTPUT_AGAIN;
  }

  ProcessOverlays(pPicture, pPicture->pts);

  EINTERLACEMETHOD deintMethod = EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE;
  deintMethod = m_processInfo.GetVideoSettings().m_InterlaceMethod;
  if (!m_processInfo.Supports(deintMethod))
    deintMethod = m_processInfo.GetDeinterlacingMethodDefault();

  bool pictureAdded;
  {
    STREAMDIAG::Span sd(STREAMDIAG_SITE("video wait AddVideoPicture"));
    if (m_worker && m_syncState == ESyncState::SYNC_STARTING)
    {
      // the start picture is forced without waiting; a tick that takes it
      // releases PLAYER_STARTED (WorkerDuties)
      const int gen = ++m_forceGenCounter;
      pictureAdded = m_renderManager.AddVideoPicture(*pPicture, m_bAbortOutput, deintMethod, false, gen);
      if (pictureAdded)
      {
        m_startPendingGen = gen;
        m_startPendingSince = std::chrono::steady_clock::now();
      }
    }
    else
      pictureAdded = m_renderManager.AddVideoPicture(*pPicture, m_bAbortOutput, deintMethod, (m_syncState == ESyncState::SYNC_STARTING));
  }
  if (!pictureAdded)
  {
    m_droppingStats.AddOutputDropGain(pPicture->pts, 1);
    return OUTPUT_DROPPED;
  }

  return OUTPUT_NORMAL;
}

std::string CVideoPlayerVideo::GetPlayerInfo()
{
  std::ostringstream s;
  int width, height;
  m_processInfo.GetVideoDimensions(width, height);
  s << "vq:" << std::setw(2) << std::min(99, m_messageQueue.GetLevel()) << "% (" << std::setw(2) << std::min(99, m_messageQueue.GetLevel(true));
  if (m_pVideoCodec && m_processInfo.IsVideoHwDecoder())
    s << "%/" << std::setw(2) << std::min(99, m_pVideoCodec->GetDataLevel());
  s << "%, " << std::setw(3) << m_messageQueue.GetMaxDataSize() / SIZE_1M << "MB)";
  s << std::fixed << std::setprecision(3) << m_messageQueue.GetTimeSize();
  s << "s, Mb/s:" << std::fixed << std::setprecision(2)
    << static_cast<double>(GetVideoBitrate()) / (1024.0 * 1024.0);
  s << ", dc:"   << m_processInfo.GetVideoDecoderName().c_str();
  s << ", " << width << "x" << height << (m_processInfo.GetVideoInterlaced() ? "i" : "p")
    << " [" << std::setprecision(2) << m_processInfo.GetVideoDAR()
    << "]@" << std::fixed << std::setprecision(3) << m_processInfo.GetVideoFps()
    << ", deint:" << m_processInfo.GetVideoDeintMethod();
  s << ", drop:" << m_iDroppedFrames;
  s << ", skip:" << m_renderManager.GetSkippedFrames();

  int pc = m_ptsTracker.GetPatternLength();
  if (pc > 0)
    s << ", pc:" << pc;
  else
    s << ", pc:none";

  return s.str();
}

int CVideoPlayerVideo::GetVideoBitrate()
{
  return (int)m_videoStats.GetBitrate();
}

void CVideoPlayerVideo::ResetFrameRateCalc()
{
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount = 0;
  m_iFrameRateLength = 1;
  m_iFrameRateErr = 0;
  m_bAllowDrop = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoFpsDetect == 0;
}

double CVideoPlayerVideo::GetCurrentPts()
{
  double renderPts;
  int sleepTime;
  int queued, discard;

  // get render stats
  m_renderManager.GetStats(sleepTime, renderPts, queued, discard);

  if (renderPts == DVD_NOPTS_VALUE)
    return DVD_NOPTS_VALUE;
  else if (m_stalled)
    return DVD_NOPTS_VALUE;
  else if (m_speed == DVD_PLAYSPEED_NORMAL)
  {
    if (renderPts < 0)
      renderPts = 0;
  }
  return renderPts;
}

#define MAXFRAMERATEDIFF   0.01
#define MAXFRAMESERR    1000

void CVideoPlayerVideo::CalcFrameRate()
{
  if (m_iFrameRateLength >= 128 || CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoFpsDetect == 0)
    return; //don't calculate the fps

  if (!m_ptsTracker.HasFullBuffer())
    return; //we can only calculate the frameduration if m_pullupCorrection has a full buffer

  //see if m_pullupCorrection was able to detect a pattern in the timestamps
  //and is able to calculate the correct frame duration from it
  double frameduration = m_ptsTracker.GetFrameDuration();
  if (m_ptsTracker.VFRDetection())
    frameduration = m_ptsTracker.GetMinFrameDuration();

  if ((frameduration==DVD_NOPTS_VALUE) ||
      ((CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoFpsDetect == 1) && ((m_ptsTracker.GetPatternLength() > 1) && !m_ptsTracker.VFRDetection())))
  {
    //reset the stored framerates if no good framerate was detected
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
    m_iFrameRateErr++;

    if (m_iFrameRateErr == MAXFRAMESERR && m_iFrameRateLength == 1)
    {
      CLog::Log(LOGDEBUG,
                "{} counted {} frames without being able to calculate the framerate, giving up",
                __FUNCTION__, m_iFrameRateErr);
      m_bAllowDrop = true;
      m_iFrameRateLength = 128;
    }
    return;
  }

  double framerate = DVD_TIME_BASE / frameduration;

  //store the current calculated framerate if we don't have any yet
  if (m_iFrameRateCount == 0)
  {
    m_fStableFrameRate = framerate;
    m_iFrameRateCount++;
  }
  //check if the current detected framerate matches with the stored ones
  else if (fabs(m_fStableFrameRate / m_iFrameRateCount - framerate) <= MAXFRAMERATEDIFF)
  {
    m_fStableFrameRate += framerate; //store the calculated framerate
    m_iFrameRateCount++;

    //if we've measured m_iFrameRateLength seconds of framerates,
    if (m_iFrameRateCount >= MathUtils::round_int(framerate) * m_iFrameRateLength)
    {
      //store the calculated framerate if it differs too much from m_fFrameRate
      if (fabs(m_fFrameRate - (m_fStableFrameRate / m_iFrameRateCount)) > MAXFRAMERATEDIFF || m_bFpsInvalid)
      {
        double calculated = m_fStableFrameRate / m_iFrameRateCount;
        // For demuxer-flagged interlaced content (e.g. MBAFF), don't let the
        // calculated frame rate halve the field rate just because progressive
        // sections dominate the measurement window — the stream is still
        // interlaced overall, and halving m_fFrameRate to 25 makes the
        // renderer and DI output path behave as if it were 25fps progressive.
        bool skipHalving = m_processInfo.GetVideoInterlaced() &&
                           calculated > 0 &&
                           fabs(m_fFrameRate - 2.0 * calculated) < MAXFRAMERATEDIFF;
        if (skipHalving)
        {
          CLog::Log(LOGDEBUG, "{} skipping halve: interlaced stream, keeping fps {:f} (measured {:f})",
                    __FUNCTION__, m_fFrameRate, calculated);
        }
        else
        {
          CLog::Log(LOGDEBUG, "{} framerate was:{:f} calculated:{:f}", __FUNCTION__, m_fFrameRate,
                    calculated);
          m_fFrameRate = calculated;
          m_bFpsInvalid = false;
          m_processInfo.SetVideoFps(static_cast<float>(m_fFrameRate));
        }
      }

      //reset the stored framerates
      m_fStableFrameRate = 0.0;
      m_iFrameRateCount = 0;
      m_iFrameRateLength *= 2; //double the length we should measure framerates

      //we're allowed to drop frames because we calculated a good framerate
      m_bAllowDrop = true;
    }
  }
  else //the calculated framerate didn't match, reset the stored ones
  {
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
  }
}

int CVideoPlayerVideo::CalcDropRequirement(double pts)
{
  int result = 0;
  int lateframes;
  double iDecoderPts, iRenderPts;
  int iSkippedPicture = -1;
  int iDroppedFrames = -1;
  int iBufferLevel;
  int queued, discard;

  m_droppingStats.m_lastPts = pts;

  // get decoder stats
  if (!m_pVideoCodec->GetCodecStats(iDecoderPts, iDroppedFrames, iSkippedPicture))
    iDecoderPts = pts;
  if (iDecoderPts == DVD_NOPTS_VALUE)
    iDecoderPts = pts;

  // get render stats
  m_renderManager.GetStats(lateframes, iRenderPts, queued, discard);
  iBufferLevel = queued + discard;

  if (iBufferLevel < 0)
    result |= DROP_BUFFER_LEVEL;
  else if (iBufferLevel < 2)
  {
    result |= DROP_BUFFER_LEVEL;
    CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayerVideo::CalcDropRequirement - hurry: {}",
              iBufferLevel);
  }

  if (m_bAllowDrop)
  {
    if (iSkippedPicture > 0)
    {
      CDroppingStats::CGain gain;
      gain.frames = iSkippedPicture;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += gain.frames;
      result |= DROP_DROPPED;
      CLog::Log(LOGDEBUG, LOGVIDEO,
                "CVideoPlayerVideo::CalcDropRequirement - dropped pictures, lateframes: {}, "
                "Bufferlevel: {}, dropped: {}",
                lateframes, iBufferLevel, iSkippedPicture);
    }
    if (iDroppedFrames > 0)
    {
      CDroppingStats::CGain gain;
      gain.frames = iDroppedFrames;
      gain.pts = iDecoderPts;
      m_droppingStats.m_gain.push_back(gain);
      m_droppingStats.m_totalGain += iDroppedFrames;
      result |= DROP_DROPPED;
      CLog::Log(LOGDEBUG, LOGVIDEO,
                "CVideoPlayerVideo::CalcDropRequirement - dropped in decoder, lateframes: {}, "
                "Bufferlevel: {}, dropped: {}",
                lateframes, iBufferLevel, iDroppedFrames);
    }
  }

  // subtract gains
  while (!m_droppingStats.m_gain.empty() &&
         iRenderPts >= m_droppingStats.m_gain.front().pts)
  {
    m_droppingStats.m_totalGain -= m_droppingStats.m_gain.front().frames;
    m_droppingStats.m_gain.pop_front();
  }

  // calculate lateness
  int lateness = lateframes - m_droppingStats.m_totalGain;

  if (lateness > 0 && m_speed)
  {
    result |= DROP_VERYLATE;
  }
  return result;
}

void CDroppingStats::Reset()
{
  m_gain.clear();
  m_totalGain = 0;
}

void CDroppingStats::AddOutputDropGain(double pts, int frames)
{
  CDroppingStats::CGain gain;
  gain.frames = frames;
  gain.pts = pts;
  m_gain.push_back(gain);
  m_totalGain += frames;
}
