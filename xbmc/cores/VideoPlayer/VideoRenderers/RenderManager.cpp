/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderManager.h"

/* to use the same as player */
#include "../VideoPlayer/DVDClock.h"
#include "RenderFactory.h"
#include "RenderFlags.h"
#include "ServiceBroker.h"
#include "application/Application.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "guilib/GUIComponent.h"
#include "guilib/StereoscopicsManager.h"
#include "messaging/ApplicationMessenger.h"
#include "rendering/capture/CaptureBlit.h"
#include "rendering/capture/CaptureMetadata.h"
#include "rendering/capture/CaptureService.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#if defined(HAS_LIBAMCODEC)
#include "HwDecRender/PresentationCoordinator.h"
#include "utils/AMLUtils.h"
#else
// non-AML builds: the AML display hooks used below compile to no-ops so this
// cross-platform file stays buildable/upstreamable (review finding F11)
static inline void aml_dv_set_subtitles_visible(bool) {}
static inline bool aml_video_started() { return true; }
static inline bool aml_disc_mode_hold() { return false; }
static inline bool aml_disc_mode_anchored() { return true; }
static inline void aml_set_disc_mode_anchored(bool) {}
static inline unsigned int aml_presenter_epoch() { return 0; }
static inline void aml_set_video_presenter_active(bool) {}
#endif
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <typeinfo>
#include <mutex>

#include <time.h>

using namespace std::chrono_literals;

namespace
{
// Disc-session mode hold: keep the incumbent display RESOLUTION (no HDMI
// re-clock for a menu's resolution - strict sinks re-train on every re-clock),
// but adopt the CONTENT'S refresh rate. Holding the GUI's 60Hz over a disc's
// 23.976 menu-domain bumpers plays 24p at 60Hz = judder; choosing the content
// refresh at the incumbent resolution (e.g. 1080p24hz) gives native cadence
// with only the session's single, small refresh switch - no per-segment and no
// per-resolution re-clock. Falls back to the incumbent mode UNCHANGED when the
// content fps is unknown, or when no same-resolution mode matches the content
// rate (refresh whitelist) - so the hold can never itself cause a resolution
// re-clock, the strict-sink protection it exists for.
RESOLUTION ChooseHeldResolution(float fps, RESOLUTION incumbent, bool is3D, int width, int height)
{
  if (fps <= 0.0f)
    return incumbent;

  // ANCHOR THE SESSION ON THE DISC, NOT ON THE GUI.
  //
  // At the first video segment the "incumbent" is still the GUI's own mode, which
  // carries no information about the disc. Holding against it pins every
  // menu-domain segment to a resolution the disc may not use at all, and defers the
  // disc's real resolution switch to the first segment the hold is off for - the
  // feature - where it re-clocks HDMI on top of playback that has already started.
  // Measured on Superman UHD: all four segments are 3840x2160, yet the session was
  // held at 1920x1080 for 31.8s and then took a 1080p24 -> 2160p24 modeset 1.5s
  // into the movie, stalling video for 1.4s (the decoder stops being handed buffers
  // across a modeset) while audio ran on - the user sees a late picture over
  // running sound. And because the held mode still adopts the content REFRESH, the
  // hold had already spent a 1080p60 -> 1080p24 re-clock at the menu: it ADDED a
  // re-lock rather than saving one, which is the opposite of its purpose.
  //
  // So let the first segment choose from its own content and anchor there; every
  // later held segment holds against that. On a disc whose segments share a
  // resolution - the normal case - the session then costs exactly one re-clock,
  // taken on the first bumper where nothing is playing yet, and the feature's mode
  // string is unchanged so it takes no modeset at all.
  //
  // Deliberately NOT re-evaluated per segment: that would restore the per-segment
  // resolution thrash the hold exists to prevent. Anchor once, then hold.
  if (!aml_disc_mode_anchored())
  {
    if (width <= 0 || height <= 0)
      return incumbent; // dimensions not known yet - do not anchor on a guess
    aml_set_disc_mode_anchored(true);
    const RESOLUTION anchored = CResolutionUtils::ChooseBestResolution(fps, width, height, is3D);
    CLog::Log(LOGINFO, "ChooseHeldResolution - disc session mode hold: anchoring the session on "
                       "the first segment's own resolution ({}x{})", width, height);
    return anchored;
  }

  CLog::Log(LOGDEBUG, "ChooseHeldResolution - disc session mode hold: incumbent resolution at "
                      "content refresh");
  auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const RESOLUTION_INFO cur = gfx.GetResInfo(incumbent);
  const RESOLUTION best =
      CResolutionUtils::ChooseBestResolution(fps, cur.iScreenWidth, cur.iScreenHeight, is3D);
  const RESOLUTION_INFO bestInfo = gfx.GetResInfo(best);
  if (bestInfo.iScreenWidth == cur.iScreenWidth && bestInfo.iScreenHeight == cur.iScreenHeight)
    return best;
  return incumbent;
}
} // namespace

void CRenderManager::CClockSync::Reset()
{
  m_error = 0;
  m_ref = 0;
  m_refValid = false;
  m_adjustSeeded = false;
  m_disabledFrames = 0;
  m_idleMoves = 0;
  m_seedSum = 0;
  m_seedCount = 0;
  m_seedPrevValid = false;
  m_errCount = 0;
  m_syncOffset = 0;
  m_enabled = false;
}

CRenderManager::CRenderManager(CDVDClock &clock, IRenderMsg *player) :
  m_dvdClock(clock),
  m_playerPort(player)
{
  m_render_timeout = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoDecoderTimeout;
}

CRenderManager::~CRenderManager()
{
  StopCoordinator();
  // UnInit normally ended the session already; this covers a manager torn down
  // without it, as the renderer's own destructor used to.
  if (m_pRenderer)
    m_pRenderer->EndRenderSession();
  delete m_pRenderer;
}

void CRenderManager::GetVideoRect(CRect& source, CRect& dest, CRect& view) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    m_pRenderer->GetVideoRect(source, dest, view);
}

float CRenderManager::GetAspectRatio() const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetAspectRatio();
  else
    return 1.0f;
}

unsigned int CRenderManager::GetOrientation() const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetOrientation();
  else
    return 0;
}

void CRenderManager::SetVideoSettings(const CVideoSettings& settings)
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
  {
    m_pRenderer->SetVideoSettings(settings);
  }
}

bool CRenderManager::Configure(const VideoPicture& picture, float fps, unsigned int orientation, int buffers)
{

  if (m_amdv_wait_delay == -1)
    m_amdv_wait_delay = aml_amdv_wait(picture.hdrType);

  // check if something has changed
  {
    std::unique_lock lock(m_statelock);

    if (!m_bRenderGUI)
      return true;

    if (m_pRenderer != nullptr && m_picture.IsSameParams(picture) && m_orientation == orientation &&
        m_NumberBuffers == buffers && !m_pRenderer->ConfigChanged(picture))
    {
      if (m_fps != fps)
      {
        CLog::Log(LOGDEBUG, "CRenderManager::Configure - framerate changed from {:4.2f} to {:4.2f}",
                  m_fps, fps);
        m_fps = fps;
        m_pRenderer->SetFps(fps);
        m_bTriggerUpdateResolution = true;
        // Clear stale vsync/late-frame state from the old framerate; CheckEnableClockSync() will recalibrate on the next FrameMove on the main thread.
        {
          // PrepareNextRender publishes under m_presentlock
          std::unique_lock presentLock(m_presentlock);
          m_clockSync.Reset();
          m_dvdClock.ResetVsyncAdjust();
        }
        m_lateframes = -1;
      }
      return true;
    }
  }

  const std::string hdrStr = CStreamDetails::HdrTypeToString(picture.hdrType);
  CLog::Log(LOGDEBUG,
            "CRenderManager::Configure - change configuration. {}x{}. display: {}x{}. framerate: "
            "{:4.2f}. hdrType: {}.",
            picture.iWidth, picture.iHeight, picture.iDisplayWidth, picture.iDisplayHeight, fps,
            hdrStr.empty() ? "none" : hdrStr);

  // make sure any queued frame was fully presented
  {
    std::unique_lock lock(m_presentlock);
    XbmcThreads::EndTime<> endtime(5000ms);
    m_forceNext = true;
    while (m_presentstep != PRESENT_IDLE)
    {
      if(endtime.IsTimePast())
      {
        CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for state");
        m_forceNext = false;
        return false;
      }
      m_presentevent.wait(lock, endtime.GetTimeLeft());
    }
    m_forceNext = false;
  }

  {
    std::unique_lock lock(m_statelock);
    m_picture.SetParams(picture);
    m_fps = fps;
    m_orientation = orientation;
    m_NumberBuffers  = buffers;
    m_renderState = STATE_CONFIGURING;
    m_stateEvent.Reset();
    m_clockSync.Reset();
    m_dvdClock.ResetVsyncAdjust();
    m_pConfigPicture = std::make_unique<VideoPicture>();
    m_pConfigPicture->CopyRef(picture);

    std::unique_lock lock2(m_presentlock);
    m_presentstep = PRESENT_READY;
    m_presentevent.notifyAll();
  }

  if (!m_stateEvent.Wait(std::chrono::seconds(m_render_timeout)))
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for configure");
    std::unique_lock lock(m_statelock);
    return false;
  }

  std::unique_lock lock(m_statelock);
  if (m_renderState != STATE_CONFIGURED)
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - failed to configure");
    return false;
  }

  return true;
}

bool CRenderManager::HasVisibleOverlay() const
{
  // m_presentsource is written under m_presentlock by the render thread and
  // this is called from the GUI and process threads. Copy it under the lock,
  // then ask the overlay renderer without holding it (the overlay renderer
  // takes its own lock; never nest the two).
  int source;
  {
    std::unique_lock lock(m_presentlock);
    // with the coordinator: the source FrameMove pinned
    source = m_presenterMode && m_presentsource != -1 ? m_renderSource : m_presentsource;
  }
  return m_overlays.HasVisibleOverlay(source);
}

bool CRenderManager::Configure()
{
  // lock all interfaces
  std::unique_lock lock(m_statelock);
  std::unique_lock lock2(m_presentlock);
  std::unique_lock lock3(m_datalock);

  if (m_pRenderer)
  {
    // A new segment gets a new renderer. When it is the same kind, the process-
    // wide display state the old one set up (the Amlogic GUI HDR regime) is
    // carried over and the new renderer's Configure adjusts it; only a change
    // of kind - e.g. a software-decoded segment falling back to the GLES
    // renderer - must release it first, or the new renderer would paint into
    // a regime it knows nothing about.
    CBaseRenderer* old = m_pRenderer;
    m_pRenderer = nullptr;
    CreateRenderer();
    if (!m_pRenderer || typeid(*m_pRenderer) != typeid(*old))
      old->EndRenderSession();
    CLog::Log(LOGDEBUG, "{} - deleting renderer", __FUNCTION__);
    delete old;
  }

  if (!m_pRenderer)
  {
    CreateRenderer();
    if (!m_pRenderer)
      return false;
  }

  m_pRenderer->SetVideoSettings(m_playerPort->GetVideoSettings());
  bool result = m_pRenderer->Configure(*m_pConfigPicture, m_fps, m_orientation);
  if (result)
  {
    CRenderInfo info = m_pRenderer->GetRenderInfo();
    int renderbuffers = info.max_buffer_size;
    m_QueueSize = renderbuffers;
    if (m_NumberBuffers > 0)
      m_QueueSize = std::min(m_NumberBuffers, renderbuffers);

    if(m_QueueSize < 2)
    {
      m_QueueSize = 2;
      CLog::Log(LOGWARNING, "CRenderManager::Configure - queue size too small ({}, {}, {})",
                m_QueueSize, renderbuffers, m_NumberBuffers);
    }

    m_pRenderer->SetBufferSize(m_QueueSize);
    m_pRenderer->Update();

    const bool presenterMode =
        m_coordinator && m_coordinatorDelivers && m_pRenderer->SupportsOffThreadPresent();
    if (presenterMode != m_presenterMode)
      CLog::Log(LOGINFO, "CRenderManager::Configure - video frames released by the {}",
                presenterMode ? "presentation coordinator" : "render thread");
    m_presenterMode = presenterMode;
    m_pRenderer->SetOffThreadPresent(m_presenterMode);
    aml_set_video_presenter_active(m_presenterMode);

    m_playerPort->UpdateRenderInfo(info);
    m_playerPort->UpdateGuiRender(true);
    m_playerPort->UpdateVideoRender(m_pRenderer->VideoBypassesFramebuffer());

    m_queued.clear();
    m_discard.clear();
    m_free.clear();
    m_presentsource = -1;
    m_renderSource = -1;
    m_presentsourcePast = -1;
    for (int i = 0; i < m_QueueSize; i++)
      m_free.push_back(i);

    m_bRenderGUI = true;
    m_videostarted = std::chrono::steady_clock::now();
    m_bTriggerUpdateResolution = true;
    m_presentstep = PRESENT_IDLE;
    m_presentpts = DVD_NOPTS_VALUE;
    m_lateframes = -1;
    m_presentevent.notifyAll();
    m_renderedDebugOverlay = false;
    m_renderDebug = false;
    m_clockSync.Reset();
    m_dvdClock.ResetVsyncAdjust();
    m_overlays.Reset();
    m_overlays.SetStereoMode(m_picture.stereoMode);

    m_renderState = STATE_CONFIGURED;

    CLog::Log(LOGDEBUG, "CRenderManager::Configure - {}", m_QueueSize);
  }
  else
    m_renderState = STATE_UNCONFIGURED;

  m_pConfigPicture.reset();

  m_stateEvent.Set();
  m_playerPort->VideoParamsChange();
  return result;
}

bool CRenderManager::IsConfigured() const
{
  std::unique_lock lock(m_statelock);
  if (m_renderState == STATE_CONFIGURED)
    return true;
  else
    return false;
}

void CRenderManager::ShowVideo(bool enable)
{
  m_showVideo = enable;
  if (!enable)
    DiscardBuffer();
}

void CRenderManager::FrameWait(std::chrono::milliseconds duration)
{
  XbmcThreads::EndTime<> timeout{duration};
  std::unique_lock lock(m_presentlock);
  while(m_presentstep == PRESENT_IDLE && !timeout.IsTimePast())
    m_presentevent.wait(lock, timeout.GetTimeLeft());
}

bool CRenderManager::IsPresenting()
{
  if (!IsConfigured())
    return false;

  std::unique_lock lock(m_presentlock);
  if (!m_presentTimer.IsTimePast())
    return true;
  else
    return false;
}

void CRenderManager::FrameMove()
{
  bool firstFrame = false;

  {
    std::unique_lock lock(m_statelock);
    if (m_renderState == STATE_UNCONFIGURED)
    {
      lock.unlock();
      // No video configured. A disc can still show a screen with no playlist
      // (BD-J); keep its presentation-time composition current for Render().
      // No display-mode decision without a picture: the first Configure()
      // triggers the resolution update again.
      m_overlays.PrepareOverlays(-1);
      return;
    }
  }

  UpdateResolution();
  PublishDisplayTiming();

  {
    std::unique_lock lock(m_statelock);

    if (m_renderState == STATE_UNCONFIGURED)
      return;
    else if (m_renderState == STATE_CONFIGURING)
    {
      lock.unlock();
      if (!Configure())
        return;
      UpdateLatencyTweak();
      firstFrame = true;
      FrameWait(50ms);
    }

    if (!m_presenterMode)
      CheckEnableClockSync();
  }
  int source;
  if (m_presenterMode)
  {
    std::unique_lock lock2(m_presentlock);
    m_bRenderGUI = true;
    source = m_presentsource;
    m_renderSource = source;
  }
  else
  {
    std::unique_lock lock2(m_presentlock);

    if (m_queued.empty())
    {
      m_presentstep = PRESENT_IDLE;
    }
    else
    {
      m_presentTimer.Set(1000ms);
    }

    if (m_presentstep == PRESENT_READY)
      PrepareNextRender(-1);

    if (m_presentstep == PRESENT_FLIP)
    {
      m_presentstep = PRESENT_FRAME;
      m_presentevent.notifyAll();
    }

    // release all previous
    for (std::deque<int>::iterator it = m_discard.begin(); it != m_discard.end(); )
    {
      // renderer may want to keep the frame for postprocessing
      if (!m_pRenderer->NeedBuffer(*it) || !m_bRenderGUI)
      {
        m_pRenderer->ReleaseBuffer(*it);
        m_overlays.Release(*it);
        m_free.push_back(*it);
        it = m_discard.erase(it);
      }
      else
        ++it;
    }

    m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());
    m_bRenderGUI = true;
    source = m_presentsource;
  }

  m_playerPort->UpdateGuiRender(IsGuiLayer() || !m_pRenderer->VideoBypassesFramebuffer() ||
                                firstFrame);

  // Run libass for the current PTS and cache the output for ConvertLibass
  // to use during the render pass. PrepareOverlays MarkDirty's on libass
  // changes and on PGS/DVB/SPU arrival/disappearance.
  m_overlays.PrepareOverlays(source);
}

void CRenderManager::PreInit()
{
  // a new file: any phase still pending belongs to the last one
  m_dvdClock.ClearVsyncAdjust(true);
  {
    std::unique_lock lock(m_statelock);
    if (m_renderState != STATE_UNCONFIGURED)
      return;
  }

  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_PREINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to preinit", __FUNCTION__);
    }
  }

  StartCoordinator();

  std::unique_lock lock(m_statelock);

  if (!m_pRenderer)
  {
    CreateRenderer();
  }

  m_debugRenderer.Initialize();

  UpdateLatencyTweak();

  m_QueueSize   = 2;
  m_QueueSkip   = 0;
  m_presentstep = PRESENT_IDLE;
  m_bRenderGUI = true;

  m_initEvent.Set();
}

void CRenderManager::UnInit()
{
  // no renderer, no phase: audio must not wait for one
  m_dvdClock.ClearVsyncAdjust(true);
  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_UNINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to uninit", __FUNCTION__);
    }
  }

  // before the renderer goes: the coordinator releases frames through it
  StopCoordinator();

  std::unique_lock lock(m_statelock);
  m_presenterMode = false;

  m_overlays.UnInit();
  // UnInit empties the overlay render buffers but, unlike Flush(), does not
  // repaint - so a subtitle/menu on screen at stop lingers on the cached Amlogic
  // OSD front buffer until an unrelated redraw. Force one repaint (process thread
  // only, like Flush()) so the emptied overlay plane is re-composited at stop.
  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
    OVERLAY::MarkDirty();
  m_debugRenderer.Dispose();

  m_captureBlit.reset();
  if (m_pRenderer)
    m_pRenderer->EndRenderSession();
  DeleteRenderer();

  m_renderState = STATE_UNCONFIGURED;
  m_picture.Reset();
  m_bRenderGUI = false;
  CServiceBroker::GetWinSystem()->GetGfxContext().SetHDRType(m_picture.hdrType);

  m_initEvent.Set();
}

bool CRenderManager::Flush(bool wait, bool saveBuffers)
{
  if (!m_pRenderer)
    return true;

  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    CLog::Log(LOGDEBUG, "{} - flushing renderer", __FUNCTION__);

// fix deadlock on Windows only when is enabled 'Sync playback to display'
#ifndef TARGET_WINDOWS
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext());
#endif

    std::unique_lock lock(m_statelock);
    std::unique_lock lock2(m_presentlock);
    std::unique_lock lock3(m_datalock);

    if (m_pRenderer)
    {
      m_overlays.Flush();
      m_debugRenderer.Flush();
      // Flushing the overlay buffers removes the composited subtitle/menu
      // bitmaps, but the Amlogic GUI keeps scanning out the cached OSD front
      // buffer until something dirties it - so a subtitle or menu overlay that
      // was on screen at a stop/flush lingers over the (now black or changed)
      // video until an unrelated redraw. Force one repaint so the emptied
      // overlay plane is re-composited and swapped. Safe: this branch runs on
      // the render/process thread (IsProcessThread guard above), the only
      // thread allowed to touch the dirty-region tracker.
      OVERLAY::MarkDirty();

      if (!m_pRenderer->Flush(saveBuffers))
      {
        m_queued.clear();
        m_discard.clear();
        m_free.clear();
        m_presentsource = -1;
        m_renderSource = -1;
        m_presentsourcePast = -1;
        m_presentstep = PRESENT_IDLE;
        for (int i = 0; i < m_QueueSize; i++)
          m_free.push_back(i);
      }

      m_flushEvent.Set();
    }
  }
  else
  {
    m_flushEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_FLUSH);
    if (wait)
    {
      if (!m_flushEvent.Wait(1000ms))
      {
        CLog::Log(LOGERROR, "{} - timed out waiting for renderer to flush", __FUNCTION__);
        return false;
      }
      else
        return true;
    }
  }
  return true;
}

void CRenderManager::CreateRenderer()
{
  if (!m_pRenderer)
  {
    CVideoBuffer *buffer = nullptr;
    if (m_pConfigPicture)
      buffer = m_pConfigPicture->videoBuffer;

    auto renderers = VIDEOPLAYER::CRendererFactory::GetRenderers();
    for (auto &id : renderers)
    {
      if (id == "default")
        continue;

      m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer(id, buffer);
      if (m_pRenderer)
      {
        return;
      }
    }
    m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer("default", buffer);
  }
}

void CRenderManager::DeleteRenderer()
{
  if (m_pRenderer)
  {
    CLog::Log(LOGDEBUG, "{} - deleting renderer", __FUNCTION__);

    delete m_pRenderer;
    m_pRenderer = NULL;
  }
}

void CRenderManager::ServiceVideoCaptures()
{
  using namespace KODI::RENDERING::CAPTURE;

  const auto captureService = CServiceBroker::GetCaptureService();
  if (!captureService)
    return;

  const auto requests = captureService->TakeActive(CaptureContent::VIDEO);
  if (requests.empty())
    return;

  // the renderer does not draw video into the framebuffer, so the copy-back
  // below would capture the GUI (if any) instead of the video; fail the request
  if (m_pRenderer->VideoBypassesFramebuffer())
  {
    for (const auto& request : requests)
    {
      CaptureResult result;
      if (m_pRenderer->CaptureVideoFrame(request->spec, result))
        captureService->Complete(request, std::move(result));
      else
        captureService->Fail(request);
    }
    return;
  }

  CRect src;
  CRect dst;
  CRect view;
  m_pRenderer->GetVideoRect(src, dst, view);

  // zoom modes push destRect past the output; the copy is clamped to the
  // visible region, so native-size targets must be sized from it too
  const CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  CRect visible{dst};
  visible.Intersect(
      CRect(0.0f, 0.0f, static_cast<float>(gfx.GetWidth()), static_cast<float>(gfx.GetHeight())));

  if (!m_captureBlit)
    m_captureBlit = std::make_unique<CCaptureBlit>();

  for (const auto& request : requests)
  {
    const unsigned int width =
        request->spec.width ? request->spec.width : static_cast<unsigned int>(visible.Width());
    const unsigned int height =
        request->spec.height ? request->spec.height : static_cast<unsigned int>(visible.Height());

    CaptureResult result;
    if (m_captureBlit->Blit(visible, width, height, request->spec.format) &&
        m_captureBlit->Read(result))
    {
      result.color = GetOutputColorMetadata(*CServiceBroker::GetWinSystem());
      // the presented PQ is the content's during passthrough, so carry its
      // mastering metadata verbatim: the source peak an SDR tonemap needs.
      // NOTE: m_picture, not m_pConfigPicture, which Configure() has already
      // reset by the time this frame reaches CONFIGURED.
      result.hasDisplayMetadata = m_picture.hasDisplayMetadata;
      result.displayMetadata = m_picture.displayMetadata;
      result.hasLightMetadata = m_picture.hasLightMetadata;
      result.lightMetadata = m_picture.lightMetadata;
      result.content = CaptureContent::VIDEO; // this tap delivers video-only
      captureService->Complete(request, std::move(result));
    }
    else
      captureService->Fail(request);
  }
}

void CRenderManager::SetViewMode(int iViewMode)
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    m_pRenderer->SetViewMode(iViewMode);
  m_playerPort->VideoParamsChange();
}

// No picture presented: a disc screen with no playlist behind it, or the gap
// between a new video's Configure() and its first frame. A real player shows
// the disc's graphics over black here, so draw the presentation-time
// composition over the full screen (the fullscreen window has already cleared
// to black). Nothing else in the video path applies without a picture.
void CRenderManager::RenderWithoutPicture(bool gui, bool configured)
{
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  auto& gfx = winSystem->GetGfxContext();

  // the fullscreen video window owns this; a GUI video control (a skin's
  // preview) must not get the disc's full-screen graphics
  if (!gfx.IsFullScreenVideo())
    return;

  CRect view(0, 0, static_cast<float>(gfx.GetWidth()), static_cast<float>(gfx.GetHeight()));

  if (!gui)
  {
    // the HDR composite only exists once a renderer is configured
    if (configured && m_overlays.HasHDROverlays(-1))
    {
      m_overlays.SetVideoRect(view, view, view);
      const bool offscreen = winSystem->BeginHdrOverlayRender();
      m_overlays.RenderHDROverlays(-1);
      if (offscreen)
        winSystem->EndHdrOverlayRender(true);
    }
    else if (configured)
    {
      // nothing HDR this frame: the composite must not keep sampling the
      // last one (as the picture path does)
      winSystem->EndHdrOverlayRender(false);
    }
    return;
  }

  m_overlays.SetVideoRect(view, view, view);
  m_overlays.Render(-1);
}

void CRenderManager::Render(bool clear, DWORD flags, DWORD alpha, bool gui)
{
  CSingleExit exitLock(CServiceBroker::GetWinSystem()->GetGfxContext());

  int source;
  {
    std::unique_lock lock(m_presentlock);
    // with the coordinator: the source FrameMove pinned
    source = m_presenterMode && m_presentsource != -1 ? m_renderSource : m_presentsource;
  }

  {
    std::unique_lock lock(m_statelock);
    if (source == -1 || (m_renderState != STATE_CONFIGURED))
    {
      const bool configured = m_renderState == STATE_CONFIGURED;
      lock.unlock();
      RenderWithoutPicture(gui, configured);
      return;
    }
  }

  if (!gui && m_pRenderer->IsGuiLayer())
    return;

  bool presented = false;
  if (!gui || m_pRenderer->IsGuiLayer())
  {
    SPresent& m = m_Queue[source];

    if( m.presentmethod == PRESENT_METHOD_BOB )
      PresentFields(clear, flags, alpha);
    else if( m.presentmethod == PRESENT_METHOD_BLEND )
      PresentBlend(clear, flags, alpha);
    else
      PresentSingle(clear, flags, alpha);

    presented = true;
  }

  // the just-presented region is video-only: OSD, GUI and subtitles come later
  if (presented)
    ServiceVideoCaptures();

  if (presented && !gui)
  {
    CRect src, dst, view;
    m_pRenderer->GetVideoRect(src, dst, view);
    m_overlays.SetVideoRect(src, dst, view);

    // Off-screen when the platform composites them (see BeginHdrOverlayRender),
    // straight onto the back buffer otherwise.
    CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
    if (m_overlays.HasHDROverlays(source))
    {
      const bool offscreen = winSystem->BeginHdrOverlayRender();
      m_overlays.RenderHDROverlays(source);
      if (offscreen)
        winSystem->EndHdrOverlayRender(true);
    }
    else
      winSystem->EndHdrOverlayRender(false);
  }

  // Under the HDR GUI composite the video pass draws onto the PQ back buffer that
  // the GUI is composited over, so only HDR overlays belong there; the GUI pass
  // still renders the debug overlay into the FBO.
  if (gui || (m_renderDebug && !CServiceBroker::GetWinSystem()->IsHdrComposite()))
  {
    if (!m_pRenderer->IsGuiLayer())
      m_pRenderer->Update();

    CRect src, dst, view;
    m_pRenderer->GetVideoRect(src, dst, view);
    m_overlays.SetVideoRect(src, dst, view);
    m_overlays.Render(source);

    // DV L5 "osdst": report whether a subtitle/overlay is actually painted this
    // frame - per-frame accurate (forced subs + in-window regular subs only, NOT
    // mere track enablement), so the DV L5 path un-masks the letterbox bars only
    // while text is really on screen. See CBitstreamConverter::processDoviRpu.
    aml_dv_set_subtitles_visible(m_overlays.HasVisibleOverlay(source));

    if (m_renderDebug)
    {
      if (m_renderDebugVideo)
      {
        DEBUG_INFO_VIDEO video = m_pRenderer->GetDebugInfo(source);
        DEBUG_INFO_RENDER render = CServiceBroker::GetWinSystem()->GetDebugInfo();

        m_debugRenderer.SetInfo(video, render);
      }
      else
      {
        DEBUG_INFO_PLAYER info;

        m_playerPort->GetDebugInfo(info.audio, info.video, info.player);

        double refreshrate, clockspeed;
        int missedvblanks;
        info.vsync = StringUtils::Format("VSyncOff: {:.1f} latency: {:.3f}  ",
                                         m_clockSync.m_syncOffset / 1000,
                                         DVD_TIME_TO_MSEC(m_displayLatency) / 1000.0f);
        if (m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate))
        {
          info.vsync += StringUtils::Format("VSync: refresh:{:.3f} missed:{} speed:{:.3f}%",
                                            refreshrate, missedvblanks, clockspeed * 100);
        }

        m_debugRenderer.SetInfo(info);
      }

      m_debugRenderer.Render(src, dst, view);

      m_debugTimer.Set(1000ms);
      m_renderedDebugOverlay = true;
    }
  }

  // the coordinator advances the step when it releases the frames
  if (m_presenterMode)
    return;

  const SPresent& m = m_Queue[source];

  {
    std::unique_lock lock(m_presentlock);

    if (m_presentstep == PRESENT_FRAME)
    {
      if (m.presentmethod == PRESENT_METHOD_BOB)
        m_presentstep = PRESENT_FRAME2;
      else
        m_presentstep = PRESENT_IDLE;
    }
    else if (m_presentstep == PRESENT_FRAME2)
      m_presentstep = PRESENT_IDLE;

    if (m_presentstep == PRESENT_IDLE)
    {
      if (!m_queued.empty())
        m_presentstep = PRESENT_READY;
    }

    m_presentevent.notifyAll();
  }
}

bool CRenderManager::IsGuiLayer()
{
  {
    std::unique_lock lock(m_statelock);

    if (!m_pRenderer)
      return false;

    int index = (m_presentsource != -1) ? m_presentsource : 0;
    if ((m_pRenderer->IsGuiLayer() && IsPresenting()) || m_renderedDebugOverlay ||
        m_overlays.HasVisibleOverlay(index))
      return true;

    if (m_renderDebug && m_debugTimer.IsTimePast())
      return true;
  }
  return false;
}

/* simple present method */
void CRenderManager::PresentSingle(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& m = m_Queue[m_presentsource];

  if (m.presentfield == FS_BOT)
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_BOT, alpha);
  else if (m.presentfield == FS_TOP)
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_TOP, alpha);
  else
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags, alpha);
}

/* new simpler method of handling interlaced material, *
 * we just render the two fields right after each other */
void CRenderManager::PresentFields(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& m = m_Queue[m_presentsource];

  if(m_presentstep == PRESENT_FRAME)
  {
    if( m.presentfield == FS_BOT)
      m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD0, alpha);
    else
      m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD0, alpha);
  }
  else
  {
    if( m.presentfield == FS_TOP)
      m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD1, alpha);
    else
      m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD1, alpha);
  }
}

void CRenderManager::PresentBlend(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& m = m_Queue[m_presentsource];

  if( m.presentfield == FS_BOT )
  {
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_NOOSD, alpha);
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, false, flags | RENDER_FLAG_TOP, alpha / 2);
  }
  else
  {
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_NOOSD, alpha);
    m_pRenderer->RenderUpdate(m_presentsource, m_presentsourcePast, false, flags | RENDER_FLAG_BOT, alpha / 2);
  }
}

void CRenderManager::UpdateLatencyTweak()
{
  float fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  const RESOLUTION_INFO res = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  const bool isHDREnabled = CServiceBroker::GetWinSystem()->GetOSHDRStatus() == HDR_STATUS::HDR_ON;
  const bool isHDRUsed = isHDREnabled && (m_picture.hdrType != StreamHdrType::HDR_TYPE_NONE);

  float refresh = fps;
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution() == RES_WINDOW)
    refresh = 0; // No idea about refresh rate when windowed, just get the default latency
  m_latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          refresh, isHDRUsed, res.iScreenHeight));

  CLog::Log(LOGDEBUG, "CRenderManager::UpdateLatencyTweak - got latency tweak of {:.1f}ms with a refresh rate of {:.3f}Hz and resolution of {:d}, HDR used: {}",
    m_latencyTweak, refresh, res.iScreenHeight, isHDRUsed);
  PublishDisplayTiming();
}

void CRenderManager::PublishDisplayTiming()
{
  // the epoch first: values read after a display transaction started carry
  // the old epoch, which the coordinator refuses
  const unsigned int epoch = aml_presenter_epoch();
  const auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  m_timingFps = gfx.GetFPS();
  m_timingLatencyMs = m_latencyTweak + static_cast<double>(gfx.GetDisplayLatency());
  m_timingEpoch = epoch;
}

void CRenderManager::UpdateResolution()
{
  if (m_bTriggerUpdateResolution)
  {
    if (CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo() && CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot())
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_videostarted);

      if (m_amdv_wait_delay == -1)
        return;

      if (m_amdv_wait_delay > 0)
      {
        m_amdv_wait_delay--;
        return;
      }
      else if (aml_video_started() || elapsed > std::chrono::seconds(m_render_timeout))
      {
        // Consume the trigger with the parameters it was raised for; a trigger
        // raised while the mode switch below runs is kept for the next pass.
        float fps;
        int width, height;
        std::string stereoMode;
        StreamHdrType hdrType;
        {
          std::unique_lock lock(m_statelock);
          fps = m_fps;
          width = m_picture.iWidth;
          height = m_picture.iHeight;
          stereoMode = m_picture.stereoMode;
          hdrType = m_picture.hdrType;
          m_bTriggerUpdateResolution = false;
        }

        const RenderStereoMode user_stereo_mode =
          CServiceBroker::GetGUI()->GetStereoscopicsManager().GetStereoModeByUser();
        STEREOSCOPIC_PLAYBACK_MODE playbackMode =
          static_cast<STEREOSCOPIC_PLAYBACK_MODE>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE));
        if (stereoMode.empty() || playbackMode != STEREOSCOPIC_PLAYBACK_MODE_ASK ||
            user_stereo_mode != RenderStereoMode::UNDEFINED)
        {
          // Whether we may CHOOSE a different display mode. Separate from whether
          // the window update runs at all: on Amlogic the HDMI colour attributes
          // for a Dolby Vision engage or release are decided inside
          // CreateNewWindow (aml_output_wire_stale -> apply_dv_wire_format /
          // the RESERVED6 hand-back), and skipping the update entirely because
          // the user does not want refresh-rate switching left the link on the
          // GUI's own format with the DV core already engaged. Measured on an
          // AM9 Pro 2026-09-19 with videoplayer.adjustrefreshrate = Off: a live
          // VS10 switch to DV put the core in IPT_TUNNEL while the wire stayed
          // 12-bit YUV422 and the sink's EOTF never left SDR - no DV signal at
          // all - and the leaving path was equally inert. Reported from the
          // field as "DV-Std with 10-bit RGB instead of the 8-bit RGB tunnel",
          // which is the same fault on a sink that negotiates further.
          const bool mayChooseMode =
              CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
                  CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF &&
              fps > 0.0f;

          // Keeping the incumbent resolution makes this a no-op for everyone
          // else: CreateNewWindow still returns "no need to create a new window"
          // unless something genuinely changed - the HDR type, the stereo mode,
          // or a stale DV wire - so refresh-rate switching stays off for the
          // people who asked for it off.
          // Disc-session mode hold: a menu-domain segment holds the INCUMBENT
          // RESOLUTION instead of re-clocking HDMI for the menu's resolution
          // (strict sinks drop signal and re-train on every re-clock; menus
          // tolerate a one-time refresh switch, a resolution re-lock per
          // segment they don't). The held mode still adopts the content's
          // REFRESH so 23.976 bumpers play at native cadence. See
          // ChooseHeldResolution for the session anchor.
          const bool held = mayChooseMode && aml_disc_mode_hold();
          RESOLUTION res =
              !mayChooseMode
                  ? CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution()
                  : (aml_disc_mode_hold()
                         ? ChooseHeldResolution(
                               fps,
                               CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution(),
                               !stereoMode.empty(), width, height)
                         : CResolutionUtils::ChooseBestResolution(
                               fps, width, height, !stereoMode.empty()));
          // A segment that chose its own resolution - the feature, when the
          // disc started there (a resume, or a title started directly) - is
          // the session's anchor too: otherwise the first menu opened later
          // anchors on the menu's own resolution (1080p on a UHD disc) and the
          // session pays a re-clock down and another back up. Cleared at player
          // teardown, so plain file playback is unaffected.
          if (mayChooseMode && !held && width > 0 && height > 0)
            aml_set_disc_mode_anchored(true);
          CServiceBroker::GetWinSystem()->GetGfxContext().SetHDRType(hdrType);
          CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, false);
          UpdateLatencyTweak();
          if (m_pRenderer)
            m_pRenderer->Update();
        }
        m_playerPort->VideoParamsChange();
      }
    }
  }
}

void CRenderManager::TriggerUpdateResolution(float fps, int width, int height, std::string &stereomode)
{
  std::unique_lock lock(m_statelock);
  if (width)
  {
    m_fps = fps;
    m_picture.iWidth = width;
    m_picture.iHeight = height;
    m_picture.stereoMode = stereomode;
  }
  m_videostarted = std::chrono::steady_clock::now();
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::ToggleDebug()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = false;
}

void CRenderManager::ToggleDebugVideo()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = true;
}

void CRenderManager::SetSubtitleVerticalPosition(int value, bool save)
{
  m_overlays.SetSubtitleVerticalPosition(value, save);
}

bool CRenderManager::AddVideoPicture(const VideoPicture& picture, volatile std::atomic_bool& bStop, EINTERLACEMETHOD deintMethod, bool wait)
{
  std::unique_lock lock(m_presentlock);

  if (m_free.empty())
    return false;

  int index = m_free.front();

  {
    std::unique_lock lock(m_datalock);
    if (!m_pRenderer)
      return false;

    m_pRenderer->AddVideoPicture(picture, index);
  }


  // set fieldsync if picture is interlaced
  EFIELDSYNC displayField = FS_NONE;
  if (picture.iFlags & DVP_FLAG_INTERLACED)
  {
    if (deintMethod != EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE)
    {
      if (picture.iFlags & DVP_FLAG_TOP_FIELD_FIRST)
        displayField = FS_TOP;
      else
        displayField = FS_BOT;
    }
  }

  EPRESENTMETHOD presentmethod = PRESENT_METHOD_SINGLE;
  if (deintMethod == VS_INTERLACEMETHOD_NONE)
  {
    presentmethod = PRESENT_METHOD_SINGLE;
    displayField = FS_NONE;
  }
  else
  {
    if (displayField == FS_NONE)
      presentmethod = PRESENT_METHOD_SINGLE;
    else
    {
      if (deintMethod == VS_INTERLACEMETHOD_RENDER_BLEND)
        presentmethod = PRESENT_METHOD_BLEND;
      else if (deintMethod == VS_INTERLACEMETHOD_RENDER_BOB)
        presentmethod = PRESENT_METHOD_BOB;
      else
      {
        if (!m_pRenderer->WantsDoublePass())
          presentmethod = PRESENT_METHOD_SINGLE;
        else
          presentmethod = PRESENT_METHOD_BOB;
      }
    }
  }


  SPresent& m = m_Queue[index];
  m.presentfield = displayField;
  m.presentmethod = presentmethod;
  m.pts = picture.pts;
  m_queued.push_back(m_free.front());
  m_free.pop_front();
  m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());

  // signal to any waiters to check state
  if (m_presentstep == PRESENT_IDLE)
  {
    m_presentstep = PRESENT_READY;
    m_presentevent.notifyAll();
  }

  if (wait)
  {
    m_forceNext = true;
    XbmcThreads::EndTime<> endtime(200ms);
    while (m_presentstep == PRESENT_READY)
    {
      m_presentevent.wait(lock, 20ms);
      if(endtime.IsTimePast() || bStop)
      {
        if (!bStop)
        {
          CLog::Log(LOGWARNING, "CRenderManager::AddVideoPicture - timeout waiting for render");
        }
        break;
      }
    }
    m_forceNext = false;
  }

  return true;
}

void CRenderManager::AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts)
{
  int idx;
  {
    std::unique_lock lock(m_presentlock);
    if (m_free.empty())
      return;
    idx = m_free.front();
  }
  std::unique_lock lock(m_datalock);
  m_overlays.AddOverlay(std::move(o), pts, idx);
}

bool CRenderManager::Supports(ERENDERFEATURE feature) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(feature);
  else
    return false;
}

bool CRenderManager::Supports(ESCALINGMETHOD method) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(method);
  else
    return false;
}

int CRenderManager::WaitForBuffer(volatile std::atomic_bool& bStop,
                                  std::chrono::milliseconds timeout)
{
  std::unique_lock lock(m_presentlock);

  // check if gui is active and discard buffer if not
  // this keeps videoplayer going
  if (!m_bRenderGUI || !g_application.GetRenderGUI())
  {
    m_bRenderGUI = false;
    double presenttime = 0;
    double clock = m_dvdClock.GetClock();
    if (!m_queued.empty())
    {
      int idx = m_queued.front();
      presenttime = m_Queue[idx].pts;
    }
    else
      presenttime = clock + 0.02;

    auto sleeptime = std::chrono::milliseconds(static_cast<int>((presenttime - clock) * 1000));
    if (sleeptime < 0ms)
      sleeptime = 0ms;
    sleeptime = std::min(sleeptime, 20ms);
    m_presentevent.wait(lock, sleeptime);
    DiscardBuffer();
    return 0;
  }

  XbmcThreads::EndTime<> endtime{timeout};
  while(m_free.empty())
  {
    m_presentevent.wait(lock, std::min(50ms, timeout));
    if (endtime.IsTimePast() || bStop)
    {
      return -1;
    }
  }

  // make sure overlay buffer is released, this won't happen on AddOverlay
  m_overlays.Release(m_free.front());

  // return buffer level
  return m_queued.size() + m_discard.size();
}

void CRenderManager::CSampleOffsetStats::Add(double us, bool guiRendered)
{
  m_last = guiRendered ? 1 : 0;
  Bucket& b = m_bucket[m_last];
  if (!b.n || us < b.min)
    b.min = us;
  if (!b.n || us > b.max)
    b.max = us;
  b.n++;
  b.sum += us;
  b.sumSq += us * us;
  if (++m_total < 1200)
    return;
  auto fmt = [](const Bucket& k) {
    if (!k.n)
      return std::string("n=0");
    const double mean = k.sum / k.n;
    const double sd = std::sqrt(std::max(0.0, k.sumSq / k.n - mean * mean));
    return StringUtils::Format("n={} mean={:.0f}us sd={:.0f}us min={:.0f}us max={:.0f}us phase={:.2f}ms",
                               k.n, mean, sd, k.min, k.max,
                               k.phaseN ? k.phaseSum / k.phaseN / 1000.0 : 0.0);
  };
  CLog::Log(LOGINFO, "real_player X (poll return -> clock sample): gui idle [{}] | gui rendered [{}]",
            fmt(m_bucket[0]), fmt(m_bucket[1]));
  *this = CSampleOffsetStats();
}

void CRenderManager::CSampleOffsetStats::AddPhase(double err)
{
  m_bucket[m_last].phaseN++;
  m_bucket[m_last].phaseSum += err;
}

void CRenderManager::PrepareNextRender(int64_t vblankNs)
{
  if (m_queued.empty())
  {
    CLog::Log(LOGERROR, "CRenderManager::PrepareNextRender - asked to prepare with nothing available");
    m_presentstep = PRESENT_IDLE;
    m_presentevent.notifyAll();
    return;
  }

  if (!m_showVideo && !m_forceNext)
    return;

  const double frameOnScreen = m_dvdClock.GetClock();
  double frameLatencyAdjustment = 0.0;
  if (vblankNs < 0)
  {
    if (const int64_t pollUs = aml_poll_return_us())
    {
      const double x = static_cast<double>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count() -
          pollUs);
      if (x >= 0.0 && x < 60000.0)
        m_sampleOffset.Add(x, aml_last_present_rendered());
    }
    frameLatencyAdjustment =
        static_cast<double>(CServiceBroker::GetWinSystem()->GetFrameLatencyAdjustment());
  }
  else if (vblankNs > 0)
  {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t sinceNs =
        static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec - vblankNs;
    if (sinceNs >= 0 && sinceNs <= 100000000)
    {
      frameLatencyAdjustment = static_cast<double>(sinceNs) / 1000000.0;
      m_sampleOffset.Add(static_cast<double>(sinceNs) / 1000.0, aml_last_present_rendered());
    }
  }
  const double frametime = 1.0 / static_cast<double>(m_timingFps.load()) * DVD_TIME_BASE;

  m_displayLatency =
      DVD_MSEC_TO_TIME(m_timingLatencyMs.load() - m_videoDelay - frameLatencyAdjustment);

  const bool isPaused = m_dvdClock.IsPaused();
  // A hardware-plane renderer cannot take back a released frame, so a pause keeps
  // the display latency (xbmc/xbmc #29406) - except during a display reset, where
  // nothing may reach the plane while the TV relocks.
  // Twin: CDVDVideoCodecAmlogic::DrainMetadataToClock.
  const bool keepLatency =
      !m_displayLost && m_pRenderer && m_pRenderer->VideoBypassesFramebuffer();
  double renderPts = frameOnScreen;
  if (!isPaused || keepLatency)
    renderPts += m_displayLatency;

  const double nextFramePts =
      m_dvdClock.GetClockSpeed() < 0 ? renderPts : m_Queue[m_queued.front()].pts;

  if (m_clockSync.m_enabled)
  {
    double err = fmod(renderPts - nextFramePts, frametime);
    // ★ err is a CIRCULAR quantity sampled on (-frametime, +frametime). When the
    // phase sits near a fmod discontinuity the samples split between two branches
    // one frametime apart, and their LINEAR mean is then a mixing fraction that
    // sweeps the range as the split ratio shifts - measured on am9pro sweeping
    // ~64x faster than the true phase moves. CAudioSinkAE::GetClock() adds the
    // negated mean to the clock audio syncs against, so audio chases the sweep
    // and CDVDClock::ErrorAdjust "corrects" by moving video a whole frame: the
    // S6 drift/jolt cycle. Unwrap each sample onto the branch nearest a
    // persistent reference so the mean is the cluster centre, not a mixture.
    // The reference is seeded from the first sample of the session (so the
    // no-straddle case is bit-identical to the plain mean) and then follows the
    // window means; it must persist across windows - re-seeding per window would
    // make the branch choice a per-window coin flip in the straddle case and
    // flap frame selection by a whole frame every window.
    // See docs/s6_truehd_av_drift.md (samurihl tree, not Kodi's docs/).
    // The clock drops its phase when sync goes off (display lost), on a seek,
    // pause or clock reset, and its phase generation changes: seed it again,
    // and start a fresh window, reference and seed, since the samples taken
    // before belong to the old phase. A value measured across a drop is
    // refused by the clock (the generation it carries is stale).
    m_clockSync.m_idleMoves = 0;
    bool clockHasPhase = false;
    const unsigned int phaseGeneration = m_dvdClock.GetVsyncPhaseGeneration(clockHasPhase);
    if (phaseGeneration != m_clockSync.m_phaseGeneration)
    {
      m_clockSync.m_phaseGeneration = phaseGeneration;
      m_clockSync.m_error = 0;
      m_clockSync.m_errCount = 0;
      m_clockSync.m_refValid = false;
      // a phase carried across a pause needs no seed (and a seed a moment
      // after the resume would step it under the audio's landing): the first
      // window replaces it
      m_clockSync.m_adjustSeeded = clockHasPhase;
      m_clockSync.m_seedSum = 0;
      m_clockSync.m_seedCount = 0;
      m_clockSync.m_seedPrevValid = false;
    }
    const bool seed = !m_clockSync.m_adjustSeeded && !isPaused && !m_displayLost;
    if (!seed)
    {
      m_clockSync.m_seedSum = 0;
      m_clockSync.m_seedCount = 0;
      m_clockSync.m_seedPrevValid = false;
    }
    if (m_clockSync.m_errCount == 0 && !m_clockSync.m_refValid)
    {
      m_clockSync.m_ref = err;
      m_clockSync.m_refValid = true;
    }
    else
      err -= frametime * std::round((err - m_clockSync.m_ref) / frametime);
    m_sampleOffset.AddPhase(err);
    // Give the audio clock the phase before the first window completes (~31
    // frames): a passthrough start sync that lands without it keeps the
    // missing phase (up to half a frame) for the rest of playback. A paused
    // clock or a lost display gives no phase. Frame selection (m_syncOffset)
    // still waits for the window.
    // The phase moves over the first frames after a start (measured am9pro:
    // ramps of 3-6 ms over 5-10 frames, then +/-0.5 ms per frame), so seed
    // once it has settled, by the rule the audio start sync uses: means over
    // blocks as long as its 100 ms measurement window, two consecutive blocks
    // agreeing within its 1 ms landing band. A phase that never settles gets
    // the first window's mean below.
    bool seeded = false;
    if (seed)
    {
      const int blockFrames =
          std::max(2, static_cast<int>(std::ceil(DVD_MSEC_TO_TIME(100) / frametime)));
      m_clockSync.m_seedSum += err;
      if (++m_clockSync.m_seedCount >= blockFrames)
      {
        const double mean = m_clockSync.m_seedSum / m_clockSync.m_seedCount;
        m_clockSync.m_seedSum = 0;
        m_clockSync.m_seedCount = 0;
        if (m_clockSync.m_seedPrevValid &&
            std::abs(mean - m_clockSync.m_seedPrev) <= DVD_MSEC_TO_TIME(1))
        {
          m_clockSync.m_adjustSeeded = true;
          m_clockSync.m_seedPrevValid = false;
          m_dvdClock.SetVsyncAdjust(-mean, phaseGeneration);
          seeded = true;
        }
        else
        {
          m_clockSync.m_seedPrev = mean;
          m_clockSync.m_seedPrevValid = true;
        }
      }
    }
    // A paused clock stands still against a running display: its samples
    // measure how long the pause has lasted, not the display phase. Publishing
    // them moved the phase while paused (measured am9pro: 2.6 -> -6.8 ms) and
    // left the resume's side hint on that value.
    if (!isPaused)
    {
      m_clockSync.m_error += err;
      m_clockSync.m_errCount ++;
      if (seeded)
      {
        // the first window then averages settled samples only, and does not
        // step the phase the seed gave
        m_clockSync.m_error = 0;
        m_clockSync.m_errCount = 0;
      }
      if (m_clockSync.m_errCount > 30)
      {
        const double average = m_clockSync.m_error / m_clockSync.m_errCount;
        m_clockSync.m_syncOffset = average;
        m_clockSync.m_error = 0;
        m_clockSync.m_errCount = 0;

        // Track the cluster with the reference, wrapped back onto the raw sample
        // range (-frametime, +frametime) so a slowly rotating phase cannot walk
        // it (and the unwrapped means with it) arbitrarily far from zero.
        double ref = average;
        if (!std::isfinite(ref))
          ref = 0; // never let a poisoned value stick in the persistent reference
        else if (ref <= -frametime)
          ref += frametime;
        else if (ref > frametime)
          ref -= frametime;
        m_clockSync.m_ref = ref;

        m_dvdClock.SetVsyncAdjust(-average, phaseGeneration);
        m_clockSync.m_adjustSeeded = true;
      }
    }
    if (!isPaused)
      renderPts += frametime / 2 - m_clockSync.m_syncOffset;
  }
  else
  {
    // frames shown without sync on a live display only
    if (m_displayLost)
      m_clockSync.m_disabledFrames = 0;
    else if (!isPaused)
      m_clockSync.m_disabledFrames++;
    m_dvdClock.ClearVsyncAdjust(false);
  }

  CLog::LogFC(LOGDEBUG, LOGAVTIMING,
              "frameOnScreen: {:.3f} renderPts: {:.3f} nextFramePts: {:.3f} -> diff: {:.3f}  render: {:d} "
              "forceNext: {:d}",
              frameOnScreen / DVD_TIME_BASE, renderPts / DVD_TIME_BASE, nextFramePts / DVD_TIME_BASE,
              (renderPts - nextFramePts) / DVD_TIME_BASE, renderPts >= nextFramePts, m_forceNext);

  bool combined = false;
  if (m_presentsourcePast >= 0)
  {
    m_discard.push_back(m_presentsourcePast);
    m_presentsourcePast = -1;
    combined = true;
  }

  if (renderPts >= nextFramePts || m_forceNext)
  {
    // see if any future queued frames are already due
    auto iter = m_queued.begin();
    int idx = *iter;
    int lateframes = 0;

    // the slot for rendering in time is [pts .. (pts + frametime)]
    // renderer/drivers have internal queues, being slightly late here does not mean that
    // we are really late. The likelihood that we recover decreases the greater m_lateframes
    // get. Skipping a frame is easier than having decoder dropping one (lateframes > 10)
    // m_lateframes is not modified in the loop below, so the relaxation is hoisted.
    constexpr double lateWindow = 0.98;
    const double x = (m_lateframes <= 6) ? lateWindow : 0;

    while (iter != m_queued.end())
    {
      if (renderPts < m_Queue[*iter].pts + x * frametime)
        break;
      // Count lateness against the FIXED window, never the relaxed one. Once
      // m_lateframes passes 6, x becomes 0 and the test above is the exact negation
      // of the enclosing `renderPts >= nextFramePts`, so counting with it makes the
      // counter self-sustaining: lateframes is then >= 1 on every pass, the reset at
      // `else m_lateframes = 0` below becomes unreachable, and the renderer's late
      // tolerance stays permanently halved (skip at 1.0 instead of 1.98 frametime),
      // throwing away the driver-queue slack this window exists to provide.
      // A single +m_frameTime CDVDClock::ErrorAdjust is enough to trigger it; only a
      // later negative correction, a display reset or a Configure would clear it.
      if (renderPts >= m_Queue[*iter].pts + lateWindow * frametime)
        lateframes++;
      idx = *iter;
      ++iter;
    }

    // skip late frames
    while (m_queued.front() != idx)
    {
      m_presentsourcePast = m_queued.front();
      m_queued.pop_front();

      if (m_presentsourcePast >= 0)
      {
        m_discard.push_back(m_presentsourcePast);
        m_QueueSkip++;
        m_presentsourcePast = -1;
      }
    }

    if (m_displayReset.exchange(false))
    {
      m_QueueSkip = 0;
      m_lateframes = 0;
    }

    if (lateframes)
      m_lateframes += lateframes;
    else
      m_lateframes = 0;

    m_presentstep = PRESENT_FLIP;
    if (m_presentsource != -1)
      m_discard.push_back(m_presentsource);
    m_presentsource = idx;
    m_queued.pop_front();
    m_presentpts = m_Queue[m_presentsource].pts - m_displayLatency;
    m_presentevent.notifyAll();

    m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());
  }
  else if (!combined && renderPts > (nextFramePts - frametime))
  {
    m_lateframes = 0;
    m_presentstep = PRESENT_FLIP;
    m_presentsourcePast = m_presentsource;
    m_presentsource = m_queued.front();
    m_queued.pop_front();
    m_presentpts = m_Queue[m_presentsource].pts - m_displayLatency - frametime / 2;
    m_presentevent.notifyAll();
  }
}

void CRenderManager::DiscardBuffer()
{
  std::unique_lock lock2(m_presentlock);

  while(!m_queued.empty())
  {
    m_discard.push_back(m_queued.front());
    m_queued.pop_front();
  }

  if(m_presentstep == PRESENT_READY)
    m_presentstep = PRESENT_IDLE;
  m_presentevent.notifyAll();
}

bool CRenderManager::GetStats(int &lateframes, double &pts, int &queued, int &discard)
{
  std::unique_lock lock(m_presentlock);
  lateframes = m_lateframes / 10;
  pts = m_presentpts;
  queued = m_queued.size();
  discard  = m_discard.size();
  return true;
}

void CRenderManager::CheckEnableClockSync()
{
  // refresh rate can be a multiple of video fps
  double diff = 1.0;
  bool refClockRunning = false;

  if (m_fps != 0)
  {
    double fps = static_cast<double>(m_fps);
    double refreshrate, clockspeed;
    int missedvblanks;
    refClockRunning = m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate);
    if (refClockRunning)
    {
      fps *= clockspeed;
    }

    diff = static_cast<double>(m_timingFps.load()) / fps;
    if (diff < 1.0)
      diff = 1.0 / diff;

    // Calculate distance from nearest integer proportion
    diff = std::abs(std::round(diff) - diff);
  }

  // a lost display takes its phase with it; the next one is seeded on return
  if (m_displayLost)
    m_dvdClock.LoseVsyncAdjust();

  // Only phase-center flips / quantize audio corrections to vsync when the AML
  // hardware-vsync reference clock is actually driving CDVDClock. With the
  // clock on system time the fmod-phase average in PrepareNextRender is noise
  // and SetVsyncAdjust would quantize against a cadence nothing is locked to.
  if (refClockRunning && diff < 0.0005)
  {
    m_clockSync.m_enabled = true;
    // A phase is pending but no frame is being prepared (a still, or no video
    // queued): nothing will publish it soon, so a passthrough start must not
    // wait for it. Counted only while pending, from the drop on.
    if (!m_dvdClock.IsVsyncAdjustPending() || m_displayLost || m_dvdClock.IsPaused())
      m_clockSync.m_idleMoves = 0;
    else if (++m_clockSync.m_idleMoves > 31)
      m_dvdClock.SettleVsyncAdjust();
  }
  else
  {
    m_clockSync.m_enabled = false;
    // No phase is coming once the reference clock runs and the rates do not
    // match, or after a window's worth of frames without sync (reference
    // clock off); a relocking display may still enable it.
    const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    const bool refClockOff =
        settings && !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK);
    m_dvdClock.ClearVsyncAdjust(refClockOff ||
                                (!m_displayLost && (refClockRunning ||
                                                    m_clockSync.m_disabledFrames > 31)));
  }

  m_playerPort->UpdateClockSync(m_clockSync.m_enabled);
}

void CRenderManager::StartCoordinator()
{
#if defined(HAS_LIBAMCODEC)
  if (m_coordinator)
    return;
  m_coordinator = CPresentationCoordinator::Get();
  if (m_coordinator)
    m_coordinatorDelivers = m_coordinator->AttachVideo(this);
  if (m_coordinator && !m_coordinatorDelivers)
  {
    m_coordinator->DetachVideo(this);
    m_coordinator = nullptr;
  }
#endif
}

void CRenderManager::StopCoordinator()
{
#if defined(HAS_LIBAMCODEC)
  if (m_coordinator)
    m_coordinator->DetachVideo(this);
  m_coordinator = nullptr;
  m_coordinatorDelivers = false;
  if (m_presenterMode)
    aml_set_video_presenter_active(false);
#endif
}

void CRenderManager::ShadowReference(const SPresentTick& tick, SPresentResult& result)
{
  // Phase 4 shadow (docs/presentation_planes_design.md, samurihl tree): what
  // audio would sync to if it followed the screen - the frame released last,
  // advanced from its vblank - against what it syncs to, clock + vsyncAdjust.
  // Measured only; nothing reads it back.
  // The anchor goes with the clock's phase: a pause, seek, display loss or
  // reset drops it, and the next released frame sets it again.
  bool hasPhase = false;
  const unsigned int generation = m_dvdClock.GetVsyncPhaseGeneration(hasPhase);
  const double speed = m_dvdClock.GetClockSpeed();
  if (!result.playing || generation != m_shadowGeneration || speed < 0.9 || speed > 1.1)
  {
    m_shadowValid = false;
    m_shadowGeneration = generation;
  }
  if (!result.playing || speed < 0.9 || speed > 1.1)
    return;
  if (result.newFrame)
  {
    m_shadowPts = result.pts;
    m_shadowReleaseNs = tick.vblankNs ? tick.vblankNs : tick.wokeNs;
    m_shadowValid = true;
  }
  if (!m_shadowValid)
    return;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const int64_t nowNs = static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
  const double adjust = m_dvdClock.GetVsyncAdjust();
  const double clock = m_dvdClock.GetClock();
  const double presented =
      m_shadowPts + static_cast<double>(nowNs - m_shadowReleaseNs) / 1000000000.0 * DVD_TIME_BASE -
      DVD_MSEC_TO_TIME(m_timingLatencyMs.load() - m_videoDelay);
  result.shadow = true;
  result.shadowDiff = clock + adjust - presented;
  result.shadowAdjust = adjust;
  result.frametime = DVD_TIME_BASE / static_cast<double>(m_timingFps.load());
}

void CRenderManager::PresentTick(const SPresentTick& tick, SPresentResult& result)
{
  std::unique_lock lock(m_statelock);
  if (m_renderState != STATE_CONFIGURED || !m_presenterMode || !m_pRenderer)
    return;
  result.configured = true;
  result.displayLost = m_displayLost;
  result.playing = !m_displayLost && !m_dvdClock.IsPaused();
  // the display timing of this display epoch has not been published yet
  if (m_timingEpoch != tick.epoch)
    return;

  CheckEnableClockSync();

  std::unique_lock presentLock(m_presentlock);
  lock.unlock();

  // the frame released at the previous vsync is on screen now
  if (m_presentstep == PRESENT_FRAME && m_presentsource >= 0 &&
      m_Queue[m_presentsource].presentmethod == PRESENT_METHOD_BOB)
    m_presentstep = PRESENT_FRAME2;
  else if (m_presentstep == PRESENT_FRAME || m_presentstep == PRESENT_FRAME2)
    m_presentstep = PRESENT_IDLE;

  if (m_queued.empty())
    m_presentstep = PRESENT_IDLE;
  else
  {
    m_presentTimer.Set(1000ms);
    if (m_presentstep == PRESENT_IDLE)
      m_presentstep = PRESENT_READY;
  }

  const int skipped = m_QueueSkip;
  if (m_presentstep == PRESENT_READY)
    PrepareNextRender(tick.vblankNs);
  result.skipped = std::max(0, m_QueueSkip - skipped);

  if (m_presentstep == PRESENT_FLIP)
  {
    m_presentstep = PRESENT_FRAME;
    result.newFrame = true;
    result.pts = m_Queue[m_presentsource].pts;
  }

  // Drops go to the decoder before the new frame shows. Their overlays are
  // released by WaitForBuffer before the buffer is reused: the overlay lock
  // is held across the render thread's overlay pass. The source the render
  // thread is drawing overlays for is freed after it moves on.
  for (auto it = m_discard.begin(); it != m_discard.end();)
  {
    if (*it != m_renderSource && (!m_pRenderer->NeedBuffer(*it) || !m_bRenderGUI))
    {
      m_pRenderer->ReleaseBuffer(*it);
      m_free.push_back(*it);
      it = m_discard.erase(it);
    }
    else
      ++it;
  }

  if (result.newFrame)
    m_pRenderer->PresentFrame(m_presentsource);

  ShadowReference(tick, result);

  m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());
  m_presentevent.notifyAll();
}
