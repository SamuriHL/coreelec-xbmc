/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderManager.h"

#include <unistd.h>

/* to use the same as player */
#include "../VideoPlayer/DVDClock.h"
#include "HdrGraphics.h"
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
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/ThreadRole.h"
#else
#define MAINCOUPLING(what) static_cast<void>(0)
#define MAINCOUPLING_ACCESSOR(what) static_cast<void>(0)
// non-AML builds: the AML display hooks used below compile to no-ops so this
// cross-platform file stays buildable/upstreamable (review finding F11)
static inline void aml_dv_set_subtitles_visible(bool) {}
static inline bool aml_video_started() { return true; }
static inline bool aml_disc_mode_hold() { return false; }
static inline bool aml_disc_mode_anchored() { return true; }
static inline void aml_set_disc_mode_anchored(bool) {}
static inline unsigned int aml_presenter_epoch() { return 0; }
static inline void aml_set_video_presenter_active(bool) {}
static inline bool aml_presentation_validated() { return false; }
static inline bool aml_codec_speed_fix() { return false; }
static inline void aml_set_frac_reclock_request(bool) {}
#endif
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/PresentationTimeline.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <typeinfo>
#include <mutex>
#include <utility>
#include <vector>

#include <time.h>

using namespace std::chrono_literals;

namespace
{
int64_t SteadyNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(HAS_LIBAMCODEC)
// 5.B2-2's switch, one value for both of its sites
bool HeldStartPendingWindow()
{
  static const bool on = OUTPUT::OutputDriveOn() && !OUTPUT::OutputDriveSkip("B2-2");
  return on;
}

// test: special://profile/displaystate_mainstall holds "<after-set|framemove> <ms>"; main sleeps
// there, after an RM-decided mode set or at the top of every FrameMove
void TestMainStall(const char* where)
{
  static const auto stall = []
  {
    std::pair<std::string, int> result{"", 0};
    XFILE::CFile file;
    std::vector<uint8_t> buf;
    if (XFILE::CFile::Exists("special://profile/displaystate_mainstall") &&
        file.LoadFile("special://profile/displaystate_mainstall", buf) > 0)
    {
      std::string line(buf.begin(), buf.end());
      line = line.substr(0, line.find('\n'));
      const auto space = line.rfind(' ');
      if (space != std::string::npos)
      {
        result = {line.substr(0, space), std::atoi(line.c_str() + space + 1)};
        CLog::Log(LOGWARNING, "TIMING: test stall of {} ms on main at \"{}\"", result.second,
                  result.first);
      }
    }
    return result;
  }();
  if (stall.second > 0 && stall.first == where)
    std::this_thread::sleep_for(std::chrono::milliseconds(stall.second));
}

// after a write of an input of PublishDisplayTiming (5.V1-4)
void TimingInputsWritten()
{
  aml_timing_inputs_bump();
}

// test: special://profile/vconfigure_mainstall holds "<form> <event> <ms>"; main stalls once per
// arming of the event: form sleep (no lock), overlay (the next overlay pass, inside its lock) or
// statelock (holding m_statelock)
struct SVStall
{
  std::string form;
  std::string event;
  int ms = 0;
};

const SVStall& VStall()
{
  static const SVStall stall = []
  {
    static constexpr const char* forms[] = {"sleep", "overlay", "statelock"};
    static constexpr const char* events[] = {"start", "firstpic", "flush",        "outputchanged",
                                             "stop",  "nextfile", "afterpublish", "after-set"};
    SVStall result;
    const auto line = OUTPUT::ReadTestSwitch("vconfigure_mainstall");
    if (!line)
      return result;
    const std::vector<std::string> fields = StringUtils::Split(*line, ' ');
    if (fields.size() == 3 &&
        std::find_if(std::begin(forms), std::end(forms),
                     [&](const char* f) { return fields[0] == f; }) != std::end(forms) &&
        std::find_if(std::begin(events), std::end(events),
                     [&](const char* e) { return fields[1] == e; }) != std::end(events))
      result = {fields[0], fields[1], std::atoi(fields[2].c_str())};
    if (result.ms > 0)
      CLog::Log(LOGWARNING, "VCONFIGURE test stall {} of {} ms on main at \"{}\"", result.form,
                result.ms, result.event);
    else
      CLog::Log(LOGWARNING, "VCONFIGURE test stall \"{}\" not known, ignored", *line);
    return result;
  }();
  return stall;
}

bool VStallAt(const char* event)
{
  const SVStall& stall = VStall();
  return stall.ms > 0 && stall.event == event;
}

// test: special://profile/vconfigure_forceswap: V's next picture after main entered the
// afterpublish stall reconfigures as if its params changed
bool TestForceSwap()
{
  static const bool on = []
  {
    const bool result = XFILE::CFile::Exists("special://profile/vconfigure_forceswap");
    if (result)
      CLog::Log(LOGWARNING, "VCONFIGURE test: a forced reconfigure in the afterpublish stall");
    return result;
  }();
  return on;
}

// RMHOLD (5.V2): main's hold of m_statelock or m_presentlock, timed from the take to the release;
// only a thread's outermost hold is timed. A long hold is an ERROR only under vconfigure at a site
// whose work has moved off main (moved false: a long site its sub-step has yet to move)
thread_local int t_holdDepth = 0;
thread_local bool t_holdInjected = false;

class CMainHold
{
public:
  explicit CMainHold(const char* site, bool moved = true) : m_site(site), m_moved(moved)
  {
    if (OUTPUT::ThreadRole() != OUTPUT::ROLE_MAIN)
      return;
    m_counted = true;
    if (t_holdDepth++ == 0)
      m_startNs = SteadyNs();
  }
  ~CMainHold() { End(); }
  CMainHold(const CMainHold&) = delete;
  CMainHold& operator=(const CMainHold&) = delete;

  //! at the release, where it comes before the end of the scope
  void End()
  {
    if (!m_counted)
      return;
    m_counted = false;
    if (--t_holdDepth > 0)
      return;
    const double ms = static_cast<double>(SteadyNs() - m_startNs) / 1e6;
    const bool injected = std::exchange(t_holdInjected, false);
    if (ms <= 5.0)
      return;
    const char* tag = "";
    if (injected)
      tag = " (test stall)";
    else if (!aml_vconfigure_on())
      tag = " (off path)";
    else if (!m_moved)
      tag = " (not moved)";
    CLog::Log(*tag ? LOGINFO : LOGERROR, "RMHOLD {} held {:.1f} ms on main{}", m_site, ms, tag);
  }

private:
  const char* m_site;
  bool m_moved;
  bool m_counted = false;
  int64_t m_startNs = 0;
};

const char* RendererKind(const CBaseRenderer* renderer)
{
  if (!renderer)
    return "none";
  // the mangled name: skip the length prefix
  const char* name = typeid(*renderer).name();
  while (*name >= '0' && *name <= '9')
    ++name;
  return name;
}
#else
void TestMainStall(const char*)
{
}

void TimingInputsWritten()
{
}

bool VStallAt(const char*)
{
  return false;
}

bool TestForceSwap()
{
  return false;
}

class CMainHold
{
public:
  explicit CMainHold(const char*, bool = true) {}
  void End() {}
};
#endif

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
#if defined(HAS_LIBAMCODEC)
  m_commitTiming = DISPLAYSTATE::CommitTimingOn();
#endif
  // the test hooks' files are read here, never under an RM lock
  VStallAt("");
  TestForceSwap();
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
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    m_pRenderer->GetVideoRect(source, dest, view);
}

float CRenderManager::GetAspectRatio() const
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetAspectRatio();
  else
    return 1.0f;
}

unsigned int CRenderManager::GetOrientation() const
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetOrientation();
  else
    return 0;
}

void CRenderManager::SetVideoSettings(const CVideoSettings& settings)
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
  {
    m_pRenderer->SetVideoSettings(settings);
  }
}

bool CRenderManager::Configure(const VideoPicture& picture, float fps, unsigned int orientation, int buffers)
{

  if (m_amdv_wait_delay.load() == -1)
  {
    int unarmed = -1;
    m_amdv_wait_delay.compare_exchange_strong(unarmed, aml_amdv_wait(picture.hdrType));
  }

  // check if something has changed
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);

    if (!m_bRenderGUI)
      return true;

    if (m_pRenderer != nullptr && m_picture.IsSameParams(picture) && m_orientation == orientation &&
        m_NumberBuffers == buffers && !m_pRenderer->ConfigChanged(picture) &&
        !(TestForceSwap() && m_testForceSwap.exchange(false)))
    {
      if (m_fps != fps)
      {
        CLog::Log(LOGDEBUG, "CRenderManager::Configure - framerate changed from {:4.2f} to {:4.2f}",
                  m_fps, fps);
        m_fps = fps;
        m_pictureDuration = 0.0;
        m_pRenderer->SetFps(fps);
        m_bTriggerUpdateResolution = true;
        // Clear stale vsync/late-frame state from the old framerate; CheckEnableClockSync() will recalibrate on the next FrameMove on the main thread.
        {
          // PrepareNextRender publishes under m_presentlock
          MAINCOUPLING_ACCESSOR("RM m_presentlock");
          std::unique_lock presentLock(m_presentlock);
          m_clockSync.Reset();
          m_dvdClock.ResetVsyncAdjust();
        }
        m_lateframes = -1;
      }
      PromoteReclockArm();
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
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
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
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    m_picture.SetParams(picture);
    m_pictureHdrType = picture.hdrType;
    m_fps = fps;
    m_pictureDuration = 0.0;
    m_orientation = orientation;
    m_NumberBuffers  = buffers;
    m_renderState = STATE_CONFIGURING;
    m_stateEvent.Reset();
    m_clockSync.Reset();
    m_dvdClock.ResetVsyncAdjust();
    m_pConfigPicture = std::make_unique<VideoPicture>();
    m_pConfigPicture->CopyRef(picture);
    PromoteReclockArm();

    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock2(m_presentlock);
    m_presentstep = PRESENT_READY;
    m_presentevent.notifyAll();
  }

  if (!m_stateEvent.Wait(std::chrono::seconds(m_render_timeout)))
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for configure");
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    return false;
  }

  MAINCOUPLING_ACCESSOR("RM m_statelock");
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
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock(m_presentlock);
    // with the coordinator: the source FrameMove pinned
    source = m_presenterMode && m_presentsource != -1 ? m_renderSource : m_presentsource;
  }
  return m_overlays.HasVisibleOverlay(source);
}

bool CRenderManager::Configure()
{
  // lock all interfaces
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock2(m_presentlock);
  std::unique_lock lock3(m_datalock);
  CMainHold hold("Configure", false);
#if defined(HAS_LIBAMCODEC)
  const int64_t startNs = SteadyNs();
  const char* oldKind = RendererKind(m_pRenderer);
  // 5.V2-1 (N2): whether V's path would take it
  const auto logConfigure = [&](bool result)
  {
    const char* path = "V's path would take it";
    if (!aml_vconfigure_on())
      path = "vconfigure off";
    else if (!m_commitTiming)
      path = "B2-3 timing off";
    else if (!result)
      path = "failed";
    else if (!m_presenterMode)
      path = "not a presenter-mode renderer";
    CLog::Log(
        LOGINFO, "VCONFIGURE main pass player={} file={} renderer {}->{} {} in {:.1f} ms ({})",
        m_playerGen, m_fileGen.load(), oldKind, RendererKind(m_pRenderer),
        result ? "configured" : "failed", static_cast<double>(SteadyNs() - startNs) / 1e6, path);
  };
#else
  const auto logConfigure = [](bool) {};
#endif

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
    {
      logConfigure(false);
      return false;
    }
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
    NotifyWorker();

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
  logConfigure(result);
  return result;
}

bool CRenderManager::IsConfigured() const
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
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
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);
  while(m_presentstep == PRESENT_IDLE && !timeout.IsTimePast())
    m_presentevent.wait(lock, timeout.GetTimeLeft());
}

bool CRenderManager::IsPresenting()
{
  if (!IsConfigured())
    return false;

  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);
  if (!m_presentTimer.IsTimePast())
    return true;
  else
    return false;
}

void CRenderManager::FrameMove()
{
  TestMainStall("framemove");
  TestStall("start");
  TestStall("stop");
  TestStall("nextfile");
  bool firstFrame = false;
#if defined(HAS_LIBAMCODEC)
  ReencodeOnOutputChange();
#endif

  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("FrameMove state");
    if (m_renderState == STATE_UNCONFIGURED)
    {
      hold.End();
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
  TestStall("afterpublish");

  // A GUI pass while graphics were withheld left the plane without them, and
  // an unchanged composition never dirties the GUI again (TNG's language page
  // drawn just before a held start, P5-11): repaint once they are released.
  const bool withheld = GraphicsWithheld();
  if (m_graphicsWithheld && !withheld)
    OVERLAY::MarkDirty();
  m_graphicsWithheld = withheld;

  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("FrameMove configuring");

    if (m_renderState == STATE_UNCONFIGURED)
      return;
    else if (m_renderState == STATE_CONFIGURING)
    {
      hold.End();
      lock.unlock();
      if (!Configure())
        return;
      UpdateLatencyTweak("configure");
      firstFrame = true;
      FrameWait(50ms);
    }

    if (!m_presenterMode)
      CheckEnableClockSync();
  }
  TestStall("firstpic");
  int source;
  if (m_presenterMode)
  {
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock2(m_presentlock);
    CMainHold hold("FrameMove pin");
    m_bRenderGUI = true;
    source = m_presentsource;
    m_renderSource = source;
  }
  else
  {
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock2(m_presentlock);
    CMainHold hold("FrameMove present");

    if (m_queued.empty())
    {
      m_presentstep = PRESENT_IDLE;
    }
    else
    {
      m_presentTimer.Set(1000ms);
    }

    const bool forced = m_forceNext;
    if (m_presentstep == PRESENT_READY)
      PrepareNextRender(-1);
    // without the presenter the render thread takes the worker's start picture
    if (forced && m_forceGen >= 0 && m_presentstep != PRESENT_READY)
    {
      m_forceNext = false;
      m_forceDoneGen = m_forceGen;
      m_forceGen = -1;
      NotifyWorker();
    }

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
        NotifyWorker();
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
  unsigned int segmentGen = 0;
  if (source >= 0 && source < NUM_BUFFERS)
  {
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock(m_presentlock);
    segmentGen = m_Queue[source].segmentGen;
  }
  m_overlays.PrepareOverlays(source, segmentGen);
}

void CRenderManager::BeginFile()
{
  // a new file: any phase still pending belongs to the last one, and so does
  // its output mode decision (a running player opening the next file skips
  // PreInit; left set, a start from the GUI skipped its hold for the mode)
  m_dvdClock.ClearVsyncAdjust(true);
  m_sessionModeDecided = false;
  m_startHeld = false;
  m_startGate = false;
  ++m_fileGen;
  if (VStallAt("firstpic"))
    m_testStallArmed = 1;
  else if (VStallAt("afterpublish"))
    m_testStallArmed = 2;
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("BeginFile");
    if (m_reclockPending || m_reclockArmed)
      CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm dropped (new file)");
    m_reclockPending = false;
    m_reclockArmed = false;
#if defined(HAS_LIBAMCODEC)
    // 5.B2-5b: a retry of the last file's set is not this file's
    if (m_rmRetry)
    {
      m_rmRetry = false;
      m_resolutionInFlight = false;
      CLog::Log(LOGINFO, "OUTPUTDRIVE rm retry abandoned (new file)");
    }
#endif
  }
  // the kernel's DV wait (amdv_wait_delay vsyncs before the mode set) is
  // armed by the file's first picture: left at its first value, a second DV
  // file in one player waited nothing, or never once an SDR file came first
  if (aml_codec_speed_fix())
    m_amdv_wait_delay = -1;
}

void CRenderManager::ArmTestStall(const char* event)
{
  if (VStallAt(event))
    m_testStallArmed = 2;
}

void CRenderManager::TestStall(const char* event, bool immediate)
{
  if (!VStallAt(event))
    return;
  int armed = 2;
  if (!immediate && !m_testStallArmed.compare_exchange_strong(armed, 0))
    return;
#if defined(HAS_LIBAMCODEC)
  const SVStall& stall = VStall();
  CLog::Log(LOGWARNING, "VCONFIGURE test stall {} {} ms on main at \"{}\" player={} file={}",
            stall.form, stall.ms, event, m_playerGen, m_fileGen.load());
  if (stall.form == "overlay")
  {
    m_overlays.ArmTestStall(stall.ms);
    return;
  }
  if (std::strcmp(event, "afterpublish") == 0 && TestForceSwap())
  {
    m_testForceSwap = true;
    CLog::Log(LOGWARNING, "VCONFIGURE test: V's next picture reconfigures");
  }
  if (stall.form == "statelock")
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("test stall");
    t_holdInjected = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(stall.ms));
    return;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(stall.ms));
#endif
}

void CRenderManager::PreInit()
{
  BeginFile();
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
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

  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  CMainHold hold("PreInit");

  if (!m_pRenderer)
  {
    CreateRenderer();
  }

  // The debug info renderer's set-up (subtitle/libass adapter) takes 18 ms per
  // open and 111 ms at the first (G12B, design §16.15), here under m_statelock,
  // which the presentation coordinator's tick needs. On validated hardware it is
  // built when the debug info is first shown (Render, outside the lock) instead;
  // most sessions never show it.
  if (!aml_presentation_validated())
    m_debugRenderer.Initialize();

  UpdateLatencyTweak("preinit");

  m_QueueSize   = 2;
  m_QueueSkip   = 0;
  m_presentstep = PRESENT_IDLE;
  m_bRenderGUI = true;
  ArmTestStall("start");

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

  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  CMainHold hold("UnInit", false);
  m_presenterMode = false;
#if defined(HAS_LIBAMCODEC)
  if (m_rmRetry)
  {
    m_rmRetry = false;
    m_resolutionInFlight = false;
    CLog::Log(LOGINFO, "OUTPUTDRIVE rm retry abandoned (uninit)");
  }
#endif

  m_overlays.UnInit();
  // UnInit empties the overlay render buffers but, unlike Flush(), does not
  // repaint - so a subtitle/menu on screen at stop lingers on the cached Amlogic
  // OSD front buffer until an unrelated redraw. Force one repaint (process thread
  // only, like Flush()) so the emptied overlay plane is re-composited at stop.
  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
    OVERLAY::MarkDirty();
  m_debugRenderer.Dispose();
  m_debugRendererTried = false;

  m_captureBlit.reset();
  if (m_pRenderer)
    m_pRenderer->EndRenderSession();
  DeleteRenderer();

  m_renderState = STATE_UNCONFIGURED;
  m_picture.Reset();
  m_pictureHdrType = m_picture.hdrType;
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
    TestStall("flush");

    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock2(m_presentlock);
    std::unique_lock lock3(m_datalock);
    CMainHold hold("Flush", false);

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
      // a worker's start picture is gone with the queue
      if (m_forceGen >= 0)
      {
        m_forceNext = false;
        m_forceGen = -1;
      }
      NotifyWorker();

      m_flushEvent.Set();
    }
  }
  else
  {
    m_flushEvent.Reset();
    ArmTestStall("flush");
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
        break;
    }
    if (!m_pRenderer)
      m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer("default", buffer);
    m_presentVblanks = m_pRenderer ? m_pRenderer->PresentVblanks() : -1;
    TimingInputsWritten();
  }
}

void CRenderManager::DeleteRenderer()
{
  if (m_pRenderer)
  {
    CLog::Log(LOGDEBUG, "{} - deleting renderer", __FUNCTION__);

    delete m_pRenderer;
    m_pRenderer = NULL;
    m_presentVblanks = -1;
    TimingInputsWritten();
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
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  CMainHold hold("SetViewMode");
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
    if (configured && winSystem->HdrGraphicsOnPlane())
    {
      PresentHdrGraphics(-1, view, view, view);
      winSystem->EndHdrOverlayRender(false);
    }
    // the HDR composite only exists once a renderer is configured
    else if (configured && !GraphicsWithheld() && m_overlays.HasHDROverlays(-1))
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

  if (GraphicsWithheld())
    return;
  m_overlays.SetVideoRect(view, view, view);
  m_overlays.Render(-1);
}

// The session's first output mode is still being decided: a real player sends
// nothing before the mode is set, graphics included (a BD-J resume prompt drawn
// ahead of the first picture showed in the GUI's mode, then again after the
// switch). Bounded like the held start, in case the decision never runs.
bool CRenderManager::GraphicsWithheld()
{
  if (!CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo())
    return false;
  // a held start shows its first picture still: the graphics come with the
  // moving picture, as the sound does (design 13.ab)
  if (m_startHeld)
    return true;
  if (!m_bTriggerUpdateResolution || m_sessionModeDecided)
    return false;
  // only a configured video start: a BD-J screen with no playlist behind it
  // has no mode to wait for (a fullscreen toggle also raises the trigger)
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  return m_renderState == STATE_CONFIGURED &&
         std::chrono::steady_clock::now() - m_videostarted < std::chrono::seconds(12);
}

void CRenderManager::PresentHdrGraphics(int idx,
                                        const CRect& source,
                                        const CRect& dest,
                                        const CRect& view)
{
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  SHdrGraphics graphics;
  const bool timed = !GraphicsWithheld() && m_overlays.CollectHDROverlays(idx, graphics.images);
  graphics.source = source;
  graphics.dest = dest;
  graphics.view = view;
  graphics.width = static_cast<float>(winSystem->GetGfxContext().GetWidth());
  graphics.height = static_cast<float>(winSystem->GetGfxContext().GetHeight());
  graphics.pts = idx >= 0 && timed ? m_Queue[idx].pts : DVD_NOPTS_VALUE;
  winSystem->PresentHdrGraphics(graphics);
}

void CRenderManager::Render(bool clear, DWORD flags, DWORD alpha, bool gui)
{
  CSingleExit exitLock(CServiceBroker::GetWinSystem()->GetGfxContext());

  int source;
  {
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock(m_presentlock);
    // with the coordinator: the source FrameMove pinned
    source = m_presenterMode && m_presentsource != -1 ? m_renderSource : m_presentsource;
  }

  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("Render state");
    if (source == -1 || (m_renderState != STATE_CONFIGURED))
    {
      const bool configured = m_renderState == STATE_CONFIGURED;
      hold.End();
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
    if (winSystem->HdrGraphicsOnPlane())
    {
      PresentHdrGraphics(source, src, dst, view);
      winSystem->EndHdrOverlayRender(false);
    }
    else if (!GraphicsWithheld() && m_overlays.HasHDROverlays(source))
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
    const bool withheld = GraphicsWithheld();
    if (!withheld)
      m_overlays.Render(source);

    // DV L5 "osdst": report whether a subtitle/overlay is actually painted this
    // frame - per-frame accurate (forced subs + in-window regular subs only, NOT
    // mere track enablement), so the DV L5 path un-masks the letterbox bars only
    // while text is really on screen. See CBitstreamConverter::processDoviRpu.
    aml_dv_set_subtitles_visible(!withheld && m_overlays.HasVisibleOverlay(source));

    if (m_renderDebug)
    {
      if (!m_debugRendererTried) // built on first show (see PreInit)
      {
        m_debugRendererTried = true;
        m_debugRenderer.Initialize();
      }
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
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock(m_presentlock);
    CMainHold hold("Render step");

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
#if defined(HAS_LIBAMCODEC)
  // 5.V1-7 (MC-25): the overlay renderer's lock is not taken inside m_statelock, which the
  // coordinator takes every tick
  if (aml_displaystate_on())
  {
    int index;
    {
      MAINCOUPLING_ACCESSOR("RM m_statelock");
      std::unique_lock lock(m_statelock);
      CMainHold hold("IsGuiLayer");

      if (!m_pRenderer)
        return false;

      index = (m_presentsource != -1) ? m_presentsource : 0;
      if ((m_pRenderer->IsGuiLayer() && IsPresenting()) || m_renderedDebugOverlay)
        return true;

      if (m_renderDebug && m_debugTimer.IsTimePast())
        return true;
    }
    return m_overlays.HasVisibleOverlay(index);
  }
#endif
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    CMainHold hold("IsGuiLayer");

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

void CRenderManager::UpdateLatencyTweak(const char* site, RESOLUTION target)
{
  const auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const RESOLUTION_INFO res = target != RES_INVALID ? gfx.GetResInfo(target) : gfx.GetResInfo();
  float fps = target != RES_INVALID ? res.fRefreshRate : gfx.GetFPS();
  bool isHDREnabled = CServiceBroker::GetWinSystem()->GetOSHDRStatus() == HDR_STATUS::HDR_ON;

  float refresh = fps;
  if ((target != RES_INVALID ? target : gfx.GetVideoResolution()) == RES_WINDOW)
    refresh = 0; // No idea about refresh rate when windowed, just get the default latency
  int height = res.iScreenHeight;
#if defined(HAS_LIBAMCODEC)
  if (target == RES_INVALID && aml_displaystate_on())
    DISPLAYSTATE::ShadowMainWindowed("windowed@latency-tweak", res,
                                     gfx.GetVideoResolution() == RES_WINDOW);
  // 5.B2-3: the committed mode's tweak; the pre-set latch keeps its target
  DISPLAYSTATE::ModeTiming committed;
  if (target == RES_INVALID && m_commitTiming &&
      DISPLAYSTATE::CommittedTiming(committed, "mode@latency-tweak") && !committed.todaysTweak)
  {
    refresh = committed.windowed ? 0.0f : committed.fps;
    height = committed.screenHeight;
    isHDREnabled = DISPLAYSTATE::HdrDisplay(isHDREnabled);
  }
#endif
  const bool isHDRUsed = isHDREnabled && (m_picture.hdrType != StreamHdrType::HDR_TYPE_NONE);
  m_latencyTweak = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          refresh, isHDRUsed, height));

  m_latencyTweakMs = m_latencyTweak;
  TimingInputsWritten();

  CLog::Log(LOGDEBUG, "CRenderManager::UpdateLatencyTweak - got latency tweak of {:.1f}ms with a refresh rate of {:.3f}Hz and resolution of {:d}, HDR used: {}",
    m_latencyTweak, refresh, height, isHDRUsed);
  SComposedTiming today;
  today.refresh = refresh;
  today.height = height;
  today.hdrUsed = isHDRUsed;
  today.tweakMs = m_latencyTweak;
  ShadowTiming(site, today, false, m_tweakToAudio);
  PublishDisplayTiming(site);
}

void CRenderManager::SetLatencyTweakToAudio(bool toAudio)
{
  m_tweakToAudio = toAudio;
  TimingInputsWritten();
}

void CRenderManager::PublishDisplayTiming(const char* site)
{
#if defined(HAS_LIBAMCODEC)
  if (aml_displaystate_on())
  {
    // the counters before any input: a write after them bumps them again
    const uint64_t commitCount = DISPLAYSTATE::CommitCount();
    const uint64_t inputsGen = aml_timing_inputs_gen();
    const unsigned int epoch = aml_presenter_epoch();
    const auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
    const float gcFps = gfx.GetFPS();
    DISPLAYSTATE::ModeTiming committed;
    const float fps = m_commitTiming && DISPLAYSTATE::CommittedTiming(committed, "fps@publish")
                          ? committed.fps
                          : gcFps;
    const int vblanks = m_presentVblanks;
    const double display = vblanks >= 0 && fps > 0.0f
                               ? vblanks * 1000.0 / static_cast<double>(fps)
                               : static_cast<double>(gfx.GetDisplayLatency());
    const bool toAudio = m_tweakToAudio;
    const double latencyMs = (toAudio ? 0.0 : m_latencyTweak) + display;
    m_timingFps = fps;
    m_timingLatencyMs = latencyMs;
    WriteTimingSnapshot(fps, latencyMs, inputsGen, commitCount);
    m_timingEpoch = epoch;
    DISPLAYSTATE::ShadowMainMode("fps@publish", "height@publish", gfx.GetResInfo(), gcFps);
    SComposedTiming today;
    today.height = -1;
    today.tweakMs = m_latencyTweak;
    today.fps = fps;
    today.latencyMs = latencyMs;
    ShadowTiming(site, today, true, toAudio);
    return;
  }
#endif
  // the epoch first: values read after a display transaction started carry
  // the old epoch, which the coordinator refuses
  const unsigned int epoch = aml_presenter_epoch();
  const auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  m_timingFps = gfx.GetFPS();
  // a renderer whose plane fixes the release-to-display vblanks knows its
  // latency; the window system's figure assumes GL buffering
  const int vblanks = m_presentVblanks;
  const double display = vblanks >= 0 && m_timingFps > 0.0f
                             ? vblanks * 1000.0 / static_cast<double>(m_timingFps)
                             : static_cast<double>(gfx.GetDisplayLatency());
  m_timingLatencyMs = (m_tweakToAudio ? 0.0 : m_latencyTweak) + display;
  m_timingEpoch = epoch;
}

void CRenderManager::WriteTimingSnapshot(float fps,
                                         double latencyMs,
                                         uint64_t inputsGen,
                                         uint64_t commitCount)
{
  std::unique_lock lock(m_timingSnapshot.writeLock);
  auto& s = m_timingSnapshot;
  s.seq.store(++s.writerSeq, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.fps.store(fps, std::memory_order_relaxed);
  s.latencyMs.store(latencyMs, std::memory_order_relaxed);
  s.inputsGen.store(inputsGen, std::memory_order_relaxed);
  s.commitCount.store(commitCount, std::memory_order_relaxed);
  s.seq.store(++s.writerSeq, std::memory_order_release);
}

bool CRenderManager::ReadTimingSnapshot(STickTiming& timing,
                                        uint64_t& inputsGen,
                                        uint64_t& commitCount) const
{
  const auto& s = m_timingSnapshot;
  const uint32_t seq0 = s.seq.load(std::memory_order_acquire);
  timing.fps = s.fps.load(std::memory_order_relaxed);
  timing.latencyMs = s.latencyMs.load(std::memory_order_relaxed);
  inputsGen = s.inputsGen.load(std::memory_order_relaxed);
  commitCount = s.commitCount.load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  return !(seq0 & 1) && seq0 == s.seq.load(std::memory_order_relaxed);
}

#if defined(HAS_LIBAMCODEC)
bool CRenderManager::ComposeCommitTiming(STickTiming& timing, uint64_t& inputsGen) const
{
  // as PublishDisplayTiming: the counter first, the latched tweak, the committed fps
  inputsGen = aml_timing_inputs_gen();
  DISPLAYSTATE::ModeTiming committed;
  const int vblanks = m_presentVblanks;
  // with the tweak on the video (realtime input) the latched one is the old mode's until main's
  // after-set latch: wait for main, as off does
  if (!m_tweakToAudio || vblanks < 0 || !DISPLAYSTATE::CommittedTiming(committed))
    return false;
  timing.fps = committed.fps;
  timing.latencyMs = vblanks * 1000.0 / static_cast<double>(committed.fps);
  return true;
}

CRenderManager::SComposedTiming CRenderManager::ComposeTiming(
    const DISPLAYSTATE::DisplayState* state, StreamHdrType hdrType, int vblanks, bool tweakToAudio)
{
  SComposedTiming t;
  bool hdrDisplay = false;
  if (state)
  {
    // a seed with no mode keeps 60 fps and height 0
    t.fps = state->fps;
    t.refresh = state->windowed ? 0.0f : (state->fracHeld ? state->requestedFps : state->fps);
    t.height = state->screenHeight;
    const CHDRCapabilities& c = state->hdrCaps;
    hdrDisplay = c.SupportsHDR10() || c.SupportsHDR10Plus() || c.SupportsHLG() ||
                 c.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
  }
  t.hdrUsed = hdrDisplay && hdrType != StreamHdrType::HDR_TYPE_NONE;
  t.tweakMs = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetLatencyTweak(
          t.refresh, t.hdrUsed, t.height));
  // before a configured renderer: the AML renderer's
  const int presentVblanks = vblanks >= 0 ? vblanks : AML_VD1_PRESENT_VBLANKS;
  t.latencyMs =
      (tweakToAudio ? 0.0 : t.tweakMs) + presentVblanks * 1000.0 / static_cast<double>(t.fps);
  return t;
}
#endif

void CRenderManager::ShadowTiming(const char* site,
                                  const SComposedTiming& today,
                                  bool latency,
                                  bool toAudio)
{
#if defined(HAS_LIBAMCODEC)
  if (!aml_displaystate_on())
    return;
  const auto state = DISPLAYSTATE::Get();
  SShadowKey key;
  key.site = site;
  key.today = today;
  key.stateEpoch = state ? state->epoch : 0;
  key.restored = DISPLAYSTATE::Restored();
  key.hdrType = m_pictureHdrType;
  key.vblanks = m_presentVblanks;
  key.toAudio = toAudio;
  key.video = ComposeTiming(state.get(), key.hdrType, key.vblanks, key.toAudio);
  const SComposedTiming& video = key.video;
  const auto same = [](double a, double b) { return std::abs(a - b) < 0.001; };
  bool equal = same(video.tweakMs, today.tweakMs);
  // main's latency without a renderer's vblanks is GC's display latency: not compared
  if (latency)
    equal = equal && video.fps == today.fps &&
            (key.vblanks < 0 || same(video.latencyMs, today.latencyMs));
  // pre-set: main composes the target mode, the published state is still the current one
  const bool target = std::strcmp(site, "pre-set") == 0;
  auto count = std::find_if(m_vtimingCounts.begin(), m_vtimingCounts.end(),
                            [&](const SShadowCount& c) { return std::strcmp(c.site, site) == 0; });
  if (count == m_vtimingCounts.end())
    count = m_vtimingCounts.insert(m_vtimingCounts.end(), SShadowCount{site});
  ++(target ? count->target : (equal ? count->match : count->diff));
  if (key == m_vtimingLast)
    return;
  m_vtimingLast = key;

  std::string values =
      StringUtils::Format("tweak {:.3f} main {:.3f}", video.tweakMs, today.tweakMs);
  if (latency)
  {
    values += StringUtils::Format(" fps {:.3f} main {:.3f} latency {:.3f} main ", video.fps,
                                  today.fps, video.latencyMs);
    values += key.vblanks < 0 ? "n/a" : StringUtils::Format("{:.3f}", today.latencyMs);
  }
  // an UpdateLatencyTweak site: main's table inputs
  if (today.height >= 0)
    values += StringUtils::Format(" refresh {:.3f} main {:.3f} height {} main {} hdr {} main {}",
                                  video.refresh, today.refresh, video.height, today.height,
                                  video.hdrUsed, today.hdrUsed);
  std::string inputs = "none published";
  if (state)
  {
    const CHDRCapabilities& c = state->hdrCaps;
    inputs = StringUtils::Format(
        "epoch {} mode {} fps {:.3f} requested {:.3f} height {} frac-held {} windowed {} "
        "connected {} hdr-caps {}",
        state->epoch, state->strId, state->fps, state->requestedFps, state->screenHeight,
        state->fracHeld, state->windowed, state->connected,
        c.SupportsHDR10() || c.SupportsHDR10Plus() || c.SupportsHLG() ||
            c.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE);
  }
  CLog::Log(LOGINFO,
            "VTIMING shadow {} {} {} | {} restored {} hdrType {} vblanks {} toAudio {} | {} match "
            "{} diff {} target {}",
            site, target ? "TARGET" : (equal ? "MATCH" : "DIFF"), values, inputs, key.restored,
            static_cast<int>(key.hdrType), key.vblanks, key.toAudio, site, count->match,
            count->diff, count->target);
#endif
}

bool CRenderManager::IsResolutionUpdatePending(std::optional<unsigned int>& decisions) const
{
  // 5.B2-2 (P5-18's twin): the consume clears the trigger before its set runs, so
  // the set is pending until its count lands; flag and count are read under the
  // lock that writes both, never in-flight with the new count
  std::unique_lock<CCriticalSection> lock(m_statelock, std::defer_lock);
#if defined(HAS_LIBAMCODEC)
  if (HeldStartPendingWindow())
  {
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    lock.lock();
    decisions = m_resolutionDecisions.load();
    if (m_resolutionInFlight)
      return true;
  }
#endif
  // A held start waits for this decision. A session's first start goes
  // fullscreen just after the hold begins, so it always waits. Later, with the
  // GUI over the video (a seek there raises the trigger again), none comes
  // until the user goes fullscreen: the start was held for its 12 s timeout,
  // the picture frozen (measured on G12B, design 15.40).
  if (!m_bTriggerUpdateResolution)
    return false;
  if (!m_sessionModeDecided)
    return true;
  const CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  return gfx.IsFullScreenVideo() && gfx.IsFullScreenRoot();
}

#if defined(HAS_LIBAMCODEC)
void CRenderManager::AbandonRmRetry(const char* why)
{
  if (!m_rmRetry)
    return;
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (!m_rmRetry)
    return;
  // a stuck in-flight flag would hold every later start until a decision or the 12 s cap
  m_rmRetry = false;
  m_resolutionInFlight = false;
  CLog::Log(LOGINFO, "OUTPUTDRIVE rm retry abandoned ({})", why);
}
#endif

void CRenderManager::UpdateResolution()
{
  if (m_bTriggerUpdateResolution)
  {
#if defined(HAS_LIBAMCODEC)
    if (m_rmRetry && !(CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo() &&
                       CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot()))
      AbandonRmRetry("left fullscreen");
#endif
    if (CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo() && CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot())
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_videostarted);

      int wait = m_amdv_wait_delay.load();
      if (wait == -1)
      {
#if defined(HAS_LIBAMCODEC)
        AbandonRmRetry("a new file's DV wait");
#endif
        return;
      }

      if (wait > 0)
      {
        // a new file's reset (BeginFile, another thread) wins over this count
        m_amdv_wait_delay.compare_exchange_strong(wait, wait - 1);
        return;
      }
      // a held start (E1) has decoded its first picture and paused the clock,
      // so the decoder may never report video started before the mode is set
      // (Superman UHD: the decision waited out the 5 s render timeout)
      else if (aml_video_started() || m_startHeld ||
               elapsed > std::chrono::seconds(m_render_timeout))
      {
        // Consume the trigger with the parameters it was raised for; a trigger
        // raised while the mode switch below runs is kept for the next pass.
        float fps;
        int width, height;
        std::string stereoMode;
        StreamHdrType hdrType;
        bool fracArmed;
#if defined(HAS_LIBAMCODEC)
        const bool outputDrive = HeldStartPendingWindow(); // read before the lock
        // 5.B2-5b: W's reply was failed or failed(park)
        bool setFailed = false;
        const char* setFailedReply = "";
#endif
        {
          MAINCOUPLING_ACCESSOR("RM m_statelock");
          std::unique_lock lock(m_statelock);
          CMainHold hold("UpdateResolution");
          fps = m_fps;
          width = m_picture.iWidth;
          height = m_picture.iHeight;
          stereoMode = m_picture.stereoMode;
          hdrType = m_picture.hdrType;
          m_bTriggerUpdateResolution = false;
#if defined(HAS_LIBAMCODEC)
          // pending until the count below (5.B2-2)
          m_resolutionInFlight = outputDrive;
#endif
          fracArmed = std::exchange(m_reclockArmed, false);
          if (fracArmed && std::chrono::steady_clock::now() - m_reclockArmedAt > 12s)
          {
            CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm expired");
            fracArmed = false;
          }
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
          const bool fracReclock = fracArmed && mayChooseMode && !held;
          if (fracArmed)
            CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm consumed at {:.3f} fps: {}", fps,
                      fracReclock ? "may re-clock"
                                  : (held ? "menu hold, no re-clock" : "refresh switching off"));
          CServiceBroker::GetWinSystem()->GetGfxContext().SetHDRType(hdrType);
          // the tweak before the mode set: with no refresh-change delay the
          // display reset lands the audio inside it, and lands the new tweak
          if (m_tweakToAudio)
            UpdateLatencyTweak("pre-set", res);
#if defined(HAS_LIBAMCODEC)
          const uint64_t commits = DISPLAYSTATE::CommitCount();
          uint64_t replySeq = 0;
          OUTPUT::LastDriveReply(replySeq);
#endif
          {
            // UpdateResolution runs on main, where SetVideoResolution runs inline
            struct FracReclockRequest
            {
              explicit FracReclockRequest(bool request) { aml_set_frac_reclock_request(request); }
              ~FracReclockRequest() { aml_set_frac_reclock_request(false); }
            } fracRequest(fracReclock);
#if defined(HAS_LIBAMCODEC)
            // 5.B2-5b: the request is origin=rm (r1 M4); 5.B2-6: a film start's when the player
            // holds a start for it (Q6's lock wait)
            OUTPUT::CDriveOriginRm origin(m_startHeld);
#endif
            CServiceBroker::GetWinSystem()->GetGfxContext().SetVideoResolution(res, false);
          }
#if defined(HAS_LIBAMCODEC)
          {
            uint64_t seq = 0;
            const OUTPUT::DriveReply reply = OUTPUT::LastDriveReply(seq);
            setFailed = seq != replySeq && (reply == OUTPUT::DriveReply::FAILED ||
                                            reply == OUTPUT::DriveReply::FAILED_PARK);
            if (setFailed)
              setFailedReply = OUTPUT::DriveReplyName(reply);
          }
#endif
          TestMainStall("after-set");
          TestStall("after-set", true);
          UpdateLatencyTweak("after-set");
#if defined(HAS_LIBAMCODEC)
          if (aml_displaystate_on())
          {
            const auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
            DISPLAYSTATE::ShadowMainMode("fps@after-set", "height@after-set", gfx.GetResInfo(),
                                         gfx.GetFPS(), DISPLAYSTATE::CommitCount() != commits);
            DISPLAYSTATE::LogCounters();
          }
#endif
          if (m_pRenderer)
            m_pRenderer->Update();
        }
        else if (fracArmed)
        {
          // the set waits for the user's stereo choice, which triggers again
          MAINCOUPLING_ACCESSOR("RM m_statelock");
          std::unique_lock lock(m_statelock);
          CMainHold hold("UpdateResolution");
          m_reclockArmed = true;
          m_reclockArmedAt = std::chrono::steady_clock::now();
          CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm kept for the stereo choice");
        }
        // after the mode set: a real change has already run OnLostDisplay
#if defined(HAS_LIBAMCODEC)
        if (outputDrive)
        {
          // one step for IsResolutionUpdatePending's locked read (5.B2-2)
          MAINCOUPLING_ACCESSOR("RM m_statelock");
          std::unique_lock lock(m_statelock);
          CMainHold hold("UpdateResolution");
          // 5.B2-5b (r2 N5): a failed set is not a decision. The trigger is raised again once,
          // with the in-flight flag kept, so the held start waits for the retry's reply (or
          // the 12 s cap); the retry's result counts, whatever it is
          if (setFailed && OUTPUT::ModeSetsOnW() && !m_rmRetry)
          {
            m_rmRetry = true;
            m_bTriggerUpdateResolution = true;
            // the same request: the re-clock it consumed goes with it
            if (fracArmed)
              m_reclockArmed = true;
            CLog::Log(LOGINFO, "OUTPUTDRIVE rm retry ({})", setFailedReply);
          }
          else
          {
            if (m_rmRetry)
              CLog::Log(LOGINFO, "OUTPUTDRIVE rm retry done ({})",
                        setFailed ? setFailedReply : "set");
            m_rmRetry = false;
            ++m_resolutionDecisions;
            m_sessionModeDecided = true;
            m_resolutionInFlight = false;
          }
        }
        else
#endif
        {
          ++m_resolutionDecisions;
          m_sessionModeDecided = true;
        }
        m_playerPort->VideoParamsChange();
      }
    }
  }
}

void CRenderManager::ArmContentReclock(bool now)
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  m_reclockPending = true;
  CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm created ({})",
            now ? "running stream" : "stream open");
  if (now)
    PromoteReclockArm();
}

// m_statelock held; the decision runs on the rate just configured
void CRenderManager::PromoteReclockArm()
{
  if (!m_reclockPending)
    return;
  m_reclockPending = false;
  m_reclockArmed = true;
  m_reclockArmedAt = std::chrono::steady_clock::now();
  m_videostarted = m_reclockArmedAt;
  m_bTriggerUpdateResolution = true;
  CLog::Log(LOGINFO, "CRenderManager - frac re-clock arm promoted at {:.3f} fps", m_fps);
}

void CRenderManager::TriggerUpdateResolution(float fps, int width, int height, std::string &stereomode)
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (width)
  {
    m_fps = fps;
    m_pictureDuration = 0.0;
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

bool CRenderManager::AddVideoPicture(const VideoPicture& picture,
                                     volatile std::atomic_bool& bStop,
                                     EINTERLACEMETHOD deintMethod,
                                     bool wait,
                                     int forceGen)
{
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);

  if (m_free.empty())
    return false;

  int index = m_free.front();

  {
    std::unique_lock lock(m_datalock);
    if (!m_pRenderer)
      return false;

    if (VStallAt("firstpic"))
    {
      int waiting = 1;
      m_testStallArmed.compare_exchange_strong(waiting, 2);
    }
    m_pRenderer->AddVideoPicture(picture, index);
    // the shortest since the rate was set: a repeated (telecined) picture
    // lasts longer than the spacing
    const double held = m_pictureDuration.load();
    if (picture.iDuration > 0.0 && (held <= 0.0 || picture.iDuration < held))
      m_pictureDuration = picture.iDuration;
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
  m.segmentGen = m_incomingSegmentGen;
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

  // a stream worker does not wait: the tick that takes the picture clears the
  // force and wakes it
  if (forceGen >= 0)
  {
    m_forceNext = true;
    m_forceGen = forceGen;
  }
  else if (wait)
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
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
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
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(feature);
  else
    return false;
}

bool CRenderManager::Supports(ESCALINGMETHOD method) const
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(method);
  else
    return false;
}

void CRenderManager::SetWorkerWakeFd(int fd)
{
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);
  m_workerWakeFd = fd;
}

// under m_presentlock
void CRenderManager::NotifyWorker()
{
  if (m_workerWakeFd >= 0)
  {
    const uint64_t one = 1;
    [[maybe_unused]] ssize_t n = write(m_workerWakeFd, &one, sizeof(one));
  }
}

bool CRenderManager::ForcedPictureTaken(int gen) const
{
  return m_forceDoneGen == gen;
}

void CRenderManager::CancelForce(int gen)
{
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);
  if (m_forceGen == gen)
  {
    m_forceNext = false;
    m_forceGen = -1;
  }
}

int CRenderManager::TryGetBuffer(std::chrono::milliseconds& retryIn)
{
  retryIn = 0ms;
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);

  // as WaitForBuffer: with the GUI not rendering, pictures are discarded at
  // the clock (its wait becomes the caller's retry)
  MAINCOUPLING("GetRenderGUI");
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
    if (sleeptime > 0ms)
    {
      retryIn = sleeptime;
      return -1;
    }
    DiscardBuffer();
    return 0;
  }

  if (m_free.empty())
    return -1;

  const int level = m_queued.size() + m_discard.size();
  const int index = m_free.front();
  lock.unlock();

  m_overlays.Release(index);
  return level;
}

int CRenderManager::WaitForBuffer(volatile std::atomic_bool& bStop,
                                  std::chrono::milliseconds timeout)
{
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);

  // check if gui is active and discard buffer if not
  // this keeps videoplayer going
  MAINCOUPLING("GetRenderGUI");
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

  // only this thread takes m_free's front, so the slot stays free unlocked
  const int level = m_queued.size() + m_discard.size();
  const int index = m_free.front();
  // the overlay lock is held across overlay drawing; don't make the
  // presentation tick wait behind it
  lock.unlock();

  // make sure overlay buffer is released, this won't happen on AddOverlay
  m_overlays.Release(index);

  // return buffer level
  return level;
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

void CRenderManager::PrepareNextRender(int64_t vblankNs, const STickTiming* timing)
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
  else
  {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const int64_t nowNs = static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
    const int64_t sinceNs = nowNs - vblankNs;
    // the vblank time frame selection assumed: the shadow reference anchors on it
    m_prepareAnchorNs = nowNs;
    if (vblankNs > 0 && sinceNs >= 0 && sinceNs <= 100000000)
    {
      m_prepareAnchorNs = vblankNs;
      frameLatencyAdjustment = static_cast<double>(sinceNs) / 1000000.0;
      m_sampleOffset.Add(static_cast<double>(sinceNs) / 1000.0, aml_last_present_rendered());
    }
  }
  const double frametime = 1.0 / static_cast<double>(TimingFps(timing)) * DVD_TIME_BASE;
  // A frame shorter than a vblank (48p at 24 Hz, 50p at 25 Hz): selection
  // centres on half a frame and the phase repeats with the frame. On half a
  // vblank the next frame sat on the threshold and the frame shown flipped
  // between neighbours (Turbo 3D at 47.95 on 23.976 Hz).
  // The pictures' own duration, not the stream rate: interlaced content is
  // configured at its field rate, its frames twice as long.
  double phaseFold = frametime;
  const double pictureDuration = m_pictureDuration.load();
  if (pictureDuration > 0.0 && aml_presentation_validated())
    phaseFold = std::min(frametime, pictureDuration);

  m_displayLatency =
      DVD_MSEC_TO_TIME(TimingLatencyMs(timing) - m_videoDelay - frameLatencyAdjustment);

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

  bool syncOffCentred = false;
  double syncOffMargin = 0.0;
  if (m_clockSync.m_enabled)
  {
    double err = fmod(renderPts - nextFramePts, phaseFold);
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
    // The clock publishes the phase to audio reduced to within about half a
    // frame, so audio may hold it a whole frame from the renderer's unwrapped
    // value: every value frame selection uses goes on audio's branch, or the
    // picture runs a frame early or late against the audio (measured am9pro:
    // -33.4 ms against audio's +8.3 ms after a resume, the picture 42 ms ahead
    // until the next pause).
    const auto onAudioBranch = [&](double value)
    {
      bool published = false;
      m_dvdClock.GetVsyncPhaseGeneration(published);
      if (!published)
        return value;
      return value -
             phaseFold * std::round((value + m_dvdClock.GetVsyncAdjust()) / phaseFold);
    };
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
    // a phase the clock carried (across a pause) seeds the reference
    if (m_clockSync.m_errCount == 0 && !m_clockSync.m_refValid && clockHasPhase)
    {
      m_clockSync.m_ref = -m_dvdClock.GetVsyncAdjust();
      m_clockSync.m_refValid = true;
      m_clockSync.m_syncOffset = m_clockSync.m_ref;
    }
    if (m_clockSync.m_errCount == 0 && !m_clockSync.m_refValid)
    {
      m_clockSync.m_ref = err;
      m_clockSync.m_refValid = true;
    }
    else
      err -= phaseFold * std::round((err - m_clockSync.m_ref) / phaseFold);
    m_sampleOffset.AddPhase(err);
    // Give the audio clock the phase before the first window completes (~31
    // frames): a passthrough start sync that lands without it keeps the
    // missing phase (up to half a frame) for the rest of playback. A paused
    // clock or a lost display gives no phase. Frame selection (m_syncOffset)
    // takes the seed too, so it selects on the phase audio lands against.
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
          m_clockSync.m_ref = onAudioBranch(m_clockSync.m_ref);
          m_clockSync.m_syncOffset = onAudioBranch(mean);
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
        double average = m_clockSync.m_error / m_clockSync.m_errCount;
        // after a grid landing (phase published, not measured): the windows
        // verify it, and a departure is logged once before it is published
        double landed = 0.0;
        if (m_dvdClock.GetGridLanding(phaseGeneration, landed))
        {
          const double departure = std::abs(-onAudioBranch(average) - landed);
          if (m_clockSync.m_landingGeneration != phaseGeneration)
          {
            m_clockSync.m_landingGeneration = phaseGeneration;
            m_clockSync.m_landingDeparted = false;
            CLog::Log(LOGINFO, "RenderManager: phase after a grid landing {:+.3f} ms (published {:+.3f})",
                      -onAudioBranch(average) / 1000.0, landed / 1000.0);
          }
          if (departure > DVD_MSEC_TO_TIME(1) && !m_clockSync.m_landingDeparted)
          {
            m_clockSync.m_landingDeparted = true;
            CLog::Log(LOGWARNING,
                      "RenderManager: phase moved {:+.3f} ms from its grid landing's {:+.3f}",
                      -onAudioBranch(average) / 1000.0, landed / 1000.0);
          }
        }
        m_dvdClock.SetVsyncAdjust(-average, phaseGeneration);
        average = onAudioBranch(average);
        m_clockSync.m_syncOffset = average;
        m_clockSync.m_error = 0;
        m_clockSync.m_errCount = 0;

        // Track the cluster with the reference, wrapped back onto the raw sample
        // range (-frametime, +frametime) so a slowly rotating phase cannot walk
        // it (and the unwrapped means with it) arbitrarily far from zero.
        double ref = average;
        if (!std::isfinite(ref))
          ref = 0; // never let a poisoned value stick in the persistent reference
        else if (ref <= -phaseFold)
          ref += phaseFold;
        else if (ref > phaseFold)
          ref -= phaseFold;
        m_clockSync.m_ref = ref;
        m_clockSync.m_adjustSeeded = true;
      }
    }
    if (!isPaused)
      renderPts += phaseFold / 2 - m_clockSync.m_syncOffset;
  }
  else
  {
    // frames shown without sync on a live display only
    if (m_displayLost)
      m_clockSync.m_disabledFrames = 0;
    else if (!isPaused)
      m_clockSync.m_disabledFrames++;
    m_dvdClock.ClearVsyncAdjust(false);
    // Selection centres here too, as with sync: the newest due frame, none
    // early or late, the frame shown the one nearest the clock (rates that
    // don't match: within half a vblank either side, not up to two behind).
    // A fixed pulldown (23.976 on 59.94: 2:3) puts the clock at `den` points
    // a vblank/den apart against the frames, and a start on the grid puts one
    // on half a vblank, where jitter picked the 2:3 alignment and flipped it
    // mid-play. With den even the margin moves off it by half a spacing, to
    // the alignment with the picture early rather than the sound. Pictures
    // shorter than a vblank centre on half a picture, as with sync.
    if (m_presenterMode && pictureDuration > 0.0 && aml_presentation_validated())
    {
      syncOffMargin = phaseFold / 2;
      const double ratio = pictureDuration / frametime;
      for (int den = 1; den <= 5; den++)
      {
        const double cycles = ratio * den;
        if (std::abs(cycles - std::round(cycles)) < 0.0005 * den)
        {
          if (std::lround(phaseFold * den / frametime) % 2 == 0)
            syncOffMargin += frametime / (2 * den);
          break;
        }
      }
      if (!isPaused)
      {
        renderPts += syncOffMargin;
        syncOffCentred = true;
      }
    }
  }

  // A start the clock has yet to make (held for it, or in a scheduled start's
  // lead): the latency kept while paused would select frames up to the display
  // latency ahead of a clock that has not started, and they would be on
  // screen, ahead of the audio, when it does (measured: at 23.976 Hz the first
  // two frames lost and the picture two frames ahead at the start). Select by
  // the clock at the instant the frame will be on screen.
  int64_t startNs = 0;
  double startClock = 0.0;
  if (m_dvdClock.GetPendingStart(startNs, startClock))
  {
    double clockOnScreen = startClock;
    if (startNs)
    {
      struct timespec now = {};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const int64_t nowNs = static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
      const int64_t onScreenNs =
          (vblankNs > 0 ? vblankNs : nowNs) +
          std::llround((TimingLatencyMs(timing) - m_videoDelay) * 1000000.0);
      clockOnScreen += static_cast<double>(std::max<int64_t>(0, onScreenNs - startNs)) / 1000.0;
    }
    renderPts = clockOnScreen + (syncOffMargin > 0.0 ? syncOffMargin : phaseFold / 2);
    m_startGate = true;
  }
  else if (m_startGate)
  {
    bool published = false;
    m_dvdClock.GetVsyncPhaseGeneration(published);
    if (published || !m_clockSync.m_enabled)
      m_startGate = false;
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

  // The presenter releases straight to the video plane: no queue absorbs a
  // frame released late or early, it is shown late or early. Once selection
  // is centred on audio's phase, a due frame has half a frame of margin either
  // side, so the newest due frame is shown at once and none before it is due
  // (measured am9pro: a one-vsync display hold left the picture a frame
  // behind the audio for 7 vsyncs; at 25p on 50 Hz the early release below
  // put every frame on screen a display period, 20 ms, ahead of the audio).
  bool centred = m_presenterMode && syncOffCentred;
  if (m_presenterMode && m_clockSync.m_enabled && !isPaused)
  {
    bool published = false;
    m_dvdClock.GetVsyncPhaseGeneration(published);
    centred = published &&
              std::abs(m_clockSync.m_syncOffset + m_dvdClock.GetVsyncAdjust()) < phaseFold / 4;
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
    // a frame shown late is shown late on the presenter's plane (see centred)
    const double x = centred ? 0 : (m_lateframes <= 6) ? lateWindow : 0;

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
  else if (!combined && !centred && !m_startGate && renderPts > (nextFramePts - frametime))
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
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
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
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock lock(m_presentlock);
  lateframes = m_lateframes / 10;
  pts = m_presentpts;
  queued = m_queued.size();
  discard  = m_discard.size();
  return true;
}

bool CRenderManager::GetResumePts(double& pts, double& maxStep)
{
  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  if (m_renderState != STATE_CONFIGURED || m_fps <= 0.0f)
    return false;
  const double frametime = DVD_TIME_BASE / static_cast<double>(m_fps);
  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock presentLock(m_presentlock);
  if (!m_queued.empty())
    pts = m_Queue[m_queued.front()].pts;
  else if (m_presentsource >= 0)
    pts = m_Queue[m_presentsource].pts + frametime;
  else
    return false;
  // paused, frames up to a vsync past the clock on screen are released
  const double vsync = DVD_TIME_BASE / static_cast<double>(m_timingFps.load());
  maxStep = m_displayLatency + vsync + frametime + DVD_MSEC_TO_TIME(5);
  return pts != DVD_NOPTS_VALUE;
}

void CRenderManager::CheckEnableClockSync(const STickTiming* timing)
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

    diff = static_cast<double>(TimingFps(timing)) / fps;
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
#if defined(HAS_LIBAMCODEC)
    const bool refClockOff =
        !OUTPUT::CSettingsMirror::Get().GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK);
#else
    const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    const bool refClockOff =
        settings && !settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_DISPLAY_AS_CLOCK);
#endif
    m_dvdClock.ClearVsyncAdjust(refClockOff ||
                                (!m_displayLost && (refClockRunning ||
                                                    m_clockSync.m_disabledFrames > 31)));
  }

  m_dvdClock.SetClockSyncEnabled(m_clockSync.m_enabled);
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

void CRenderManager::ShadowReference(SPresentResult& result, bool forced, const STickTiming* timing)
{
  // Phase 4 shadow (docs/presentation_planes_design.md, samurihl tree): what
  // audio would sync to if it followed the screen - the frame released last,
  // advanced from its vblank - against what it syncs to, clock + vsyncAdjust.
  // Measured only; nothing reads it back.
  // The anchor goes with the clock's phase: a pause, seek, display loss or
  // reset drops it, and the next released frame sets it again.
  // A frame forced out by a reconfigure is released without regard to the
  // clock: no anchor until the next one released by it. The presentation
  // reference advances at wall rate, so only normal speed is compared.
  bool hasPhase = false;
  const unsigned int generation = m_dvdClock.GetVsyncPhaseGeneration(hasPhase);
  const bool normalSpeed = std::abs(m_dvdClock.GetClockSpeed() - 1.0) <= 0.01;
  if (!result.playing || generation != m_shadowGeneration || !normalSpeed || forced)
  {
    m_shadowValid = false;
    m_shadowGeneration = generation;
  }
  if (!result.playing || !normalSpeed || forced)
    return;
  if (result.newFrame)
  {
    m_shadowPts = result.pts;
    m_shadowReleaseNs = m_prepareAnchorNs;
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
      DVD_MSEC_TO_TIME(TimingLatencyMs(timing) - m_videoDelay);
  result.shadow = true;
  result.shadowDiff = clock + adjust - presented;
  result.shadowAdjust = adjust;
  result.shadowSynced = m_clockSync.m_enabled;
  result.frametime = DVD_TIME_BASE / static_cast<double>(TimingFps(timing));
}

CRenderManager::SRefusedTicks CRenderManager::NoteRefusedTick(const SPresentTick& tick,
                                                              int64_t nowNs)
{
  SRefusedTicks ended;
  if (m_refused.ticks && m_refused.epoch != tick.epoch)
  {
    ended = m_refused;
    m_refused.ticks = 0;
  }
  if (!m_refused.ticks)
  {
    m_refused.epoch = tick.epoch;
    m_refused.sinceNs = nowNs;
    m_refused.holdSite = tick.holdSite;
    m_refused.holdTid = tick.holdTid;
    m_refused.holdReleaseNs = tick.holdReleaseNs;
  }
  m_refused.ticks++;
  return ended;
}

void CRenderManager::LogRefusedTicks(const SRefusedTicks& refused, int64_t endNs, bool published)
{
  const auto ms = [](int64_t ns) { return static_cast<double>(ns) / 1e6; };
  const char* end = published ? "published" : "superseded";
  if (refused.holdSite)
    CLog::Log(LOGINFO,
              "TIMING refused {} ticks / {:.1f} ms for epoch {} (hold {} tid {}), {} {:.1f} ms "
              "after the hold release",
              refused.ticks, ms(endNs - refused.sinceNs), refused.epoch, refused.holdSite,
              refused.holdTid, end, ms(endNs - refused.holdReleaseNs));
  else
    CLog::Log(LOGINFO, "TIMING refused {} ticks / {:.1f} ms for epoch {} (no hold release), {}",
              refused.ticks, ms(endNs - refused.sinceNs), refused.epoch, end);
}

void CRenderManager::LogAheadTicks(const SAheadTicks& ahead, int64_t endNs, const char* end)
{
  CLog::Log(LOGINFO,
            "TIMING {} {} ticks / {:.1f} ms for epoch {} (hold {} tid {}) at {:.6f} Hz "
            "{:.6f} ms, inputs {}{}: {}",
            ahead.commits ? "after-commit" : "ahead-of-main", ahead.ticks,
            static_cast<double>(endNs - ahead.sinceNs) / 1e6, ahead.epoch,
            ahead.holdSite ? ahead.holdSite : "none", ahead.holdTid, ahead.timing.fps,
            ahead.timing.latencyMs, ahead.inputsGen,
            ahead.commits ? StringUtils::Format(" commit {}", ahead.commits) : "", end);
}

#if defined(HAS_LIBAMCODEC)
void CRenderManager::ReencodeOnOutputChange()
{
  const uint64_t epoch = DISPLAYSTATE::TakeReencodeRequest();
  if (!epoch)
    return;
  TestStall("outputchanged", true);
  const char* outcome;
  bool reencoded = false;
  {
    // the configured picture's params, under the locks Configure() takes: a paused film or a
    // still has no next picture to run ConfigChanged
    MAINCOUPLING_ACCESSOR("RM m_statelock");
    std::unique_lock lock(m_statelock);
    MAINCOUPLING_ACCESSOR("RM m_presentlock");
    std::unique_lock lock2(m_presentlock);
    CMainHold hold("B2-8 re-encode", false);
    if (m_renderState != STATE_CONFIGURED || !m_pRenderer)
      outcome = "not configured, the next picture configures";
    else if (!m_pRenderer->ConfigChanged(m_picture))
      outcome = "encoding current";
    else if ((reencoded = m_pRenderer->Configure(m_picture, m_fps, m_orientation)))
      outcome = "CRendererAML::Configure, no new picture";
    else
      outcome = "CRendererAML::Configure failed";
  }
  CLog::Log(LOGINFO, "OUTPUTDRIVE B2-8 re-encode epoch={}: {}", epoch, outcome);
  if (!reencoded)
    return;
  // a static GUI is not redrawn on its own, so the plane would keep pixels encoded for the old
  // declaration; the overlay cache rebuilds for the new regime at that render
  OVERLAY::MarkDirty();
  CLog::Log(LOGINFO, "OUTPUTDRIVE B2-8 re-encode epoch={}: GUI marked dirty", epoch);
}

void CRenderManager::CheckCommittedFps(const SPresentTick& tick,
                                       const STickTiming* timing,
                                       SFpsMismatch& ended,
                                       SFpsMismatch& begun)
{
  DISPLAYSTATE::ModeTiming committed;
  const bool known = DISPLAYSTATE::CommittedTiming(committed);
  const float presented = TimingFps(timing);
  const bool mismatch = known && presented != committed.fps;
  if (m_fpsMismatch.ticks &&
      (!mismatch || m_fpsMismatch.epoch != tick.epoch || m_fpsMismatch.presented != presented ||
       m_fpsMismatch.committed != committed.fps))
  {
    ended = m_fpsMismatch;
    m_fpsMismatch.ticks = 0;
  }
  if (!mismatch)
    return;
  if (!m_fpsMismatch.ticks)
  {
    m_fpsMismatch = {tick.epoch, 0, SteadyNs(), presented, committed.fps};
    begun = m_fpsMismatch;
    begun.ticks = 1;
  }
  m_fpsMismatch.ticks++;
}
#endif

void CRenderManager::LogFpsMismatch(const SFpsMismatch& run, int64_t endNs, bool ended)
{
  if (!ended)
    CLog::Log(LOGERROR, "TIMING presented fps {:.6f} committed {:.6f}: begins in epoch {}",
              run.presented, run.committed, run.epoch);
  else
    CLog::Log(LOGERROR,
              "TIMING presented fps {:.6f} committed {:.6f}: {} ticks / {:.1f} ms for epoch {}",
              run.presented, run.committed, run.ticks,
              static_cast<double>(endNs - run.sinceNs) / 1e6, run.epoch);
}

void CRenderManager::PresentTick(const SPresentTick& tick, SPresentResult& result)
{
  // TEMP TICKDIAG (timebase audit Q6): where a slow coordinator tick spends its time
  using diagClock = std::chrono::steady_clock;
  const auto diag0 = diagClock::now();
  auto diagLock = diag0, diagPresent = diag0, diagPrepare = diag0, diagFrame = diag0;
  struct DiagLog
  {
    const diagClock::time_point& t0;
    const diagClock::time_point& a;
    const diagClock::time_point& b;
    const diagClock::time_point& c;
    const diagClock::time_point& d;
    ~DiagLog()
    {
      const auto us = [](auto from, auto to)
      { return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count(); };
      const auto end = diagClock::now();
      if (us(t0, end) < 5000)
        return;
      const auto last = [&](auto t) { return t < t0 ? t0 : t; };
      CLog::Log(LOGINFO,
                "TICKDIAG total {} us: statelock {} presentlock {} prepare {} release {} rest {}",
                us(t0, end), us(t0, last(a)), us(last(a), last(b)), us(last(b), last(c)),
                us(last(c), last(d)), us(last(d), end));
    }
  } diagLog{diag0, diagLock, diagPresent, diagPrepare, diagFrame};

  // a refusal this tick ended, logged once the locks are released
  struct RefusedLog
  {
    SRefusedTicks ended;
    int64_t endNs = 0;
    bool published = false;
    SAheadTicks aheadEnded;
    const char* aheadEnd = nullptr;
    STickTiming aheadPublished{}; //!< set on a DIFF
    SFpsMismatch fpsEnded;
    SFpsMismatch fpsBegun;
    ~RefusedLog()
    {
      if (aheadEnded.ticks)
        LogAheadTicks(aheadEnded, endNs, aheadEnd);
      if (aheadPublished.fps)
        CLog::Log(LOGERROR, "TIMING {} DIFF: published {:.6f} Hz {:.6f} ms",
                  aheadEnded.commits ? "after-commit" : "ahead-of-main", aheadPublished.fps,
                  aheadPublished.latencyMs);
      if (fpsEnded.ticks)
        LogFpsMismatch(fpsEnded, SteadyNs(), true);
      if (fpsBegun.ticks)
        LogFpsMismatch(fpsBegun, fpsBegun.sinceNs, false);
      if (ended.ticks)
        LogRefusedTicks(ended, endNs, published);
    }
  } refusedLog;

  MAINCOUPLING_ACCESSOR("RM m_statelock");
  std::unique_lock lock(m_statelock);
  diagLock = diagClock::now();
  if (m_renderState != STATE_CONFIGURED || !m_presenterMode || !m_pRenderer)
    return;
  result.configured = true;
  result.displayLost = m_displayLost;
  result.playing = !m_displayLost && !m_dvdClock.IsPaused();
  if (PRESENTATION::ShadowActive() && tick.vblankNs)
  {
    const int64_t sample[PRESENTATION::CS_COUNT] = {
        static_cast<int64_t>(tick.seq), tick.vblankNs,
        static_cast<int64_t>(m_dvdClock.GetClock()), result.playing ? 1 : 0,
        std::llround((m_dvdClock.GetClockSpeed() - 1.0) * 1e6)};
    PRESENTATION::CoordinatorBoard().Write(sample);
  }
  STickTiming aheadTiming;
  const STickTiming* timing = nullptr;
  // the display timing of this display epoch has not been published yet
  if (m_timingEpoch != tick.epoch)
  {
    refusedLog.endNs = SteadyNs();
#if defined(HAS_LIBAMCODEC)
    // no input of main's last publish written since (a hold outside a mode set): its timing is
    // this epoch's. A write since waits for main.
    uint64_t inputsGen = 0;
    uint64_t commits = 0;
    const bool snapshot =
        aml_displaystate_on() && ReadTimingSnapshot(aheadTiming, inputsGen, commits);
    // 5.B2-3: a W commit main has not published: the committed mode's timing (I5)
    const uint64_t commitCount = m_commitTiming ? DISPLAYSTATE::CommitCount() : 0;
    uint64_t commitGen = 0;
    if (snapshot && commitCount > commits && ComposeCommitTiming(aheadTiming, commitGen))
    {
      inputsGen = commitGen;
      commits = commitCount;
    }
    else
      commits = 0;
    if (snapshot && (commits || inputsGen == aml_timing_inputs_gen()))
    {
      if (m_ahead.ticks && (m_ahead.epoch != tick.epoch || m_ahead.inputsGen != inputsGen ||
                            m_ahead.commits != commits))
      {
        refusedLog.aheadEnded = m_ahead;
        refusedLog.aheadEnd = "superseded";
        m_ahead.ticks = 0;
      }
      if (!m_ahead.ticks)
        m_ahead = {tick.epoch, 0, refusedLog.endNs, tick.holdSite, tick.holdTid, aheadTiming,
                   inputsGen, commits};
      m_ahead.ticks++;
      timing = &aheadTiming;
    }
    else if (m_ahead.ticks)
    {
      refusedLog.aheadEnded = m_ahead;
      refusedLog.aheadEnd = m_ahead.epoch == tick.epoch ? "refused after a write" : "superseded";
      m_ahead.ticks = 0;
    }
#endif
    if (!timing)
    {
      refusedLog.ended = NoteRefusedTick(tick, refusedLog.endNs);
      return;
    }
  }
#if defined(HAS_LIBAMCODEC)
  else if (m_ahead.ticks)
  {
    // main's publish for this epoch: with no write in between, the values the ticks used
    STickTiming published;
    uint64_t inputsGen = 0;
    uint64_t commits = 0;
    refusedLog.endNs = SteadyNs();
    refusedLog.aheadEnded = m_ahead;
    const bool read = ReadTimingSnapshot(published, inputsGen, commits);
    const bool write = inputsGen != m_ahead.inputsGen;
    // after-commit ticks: main's after-set latch writes before its publish; compare anyway
    if (!read || (m_ahead.commits ? commits != m_ahead.commits : write))
      refusedLog.aheadEnd = "published after a write";
    else if (published.fps == m_ahead.timing.fps &&
             published.latencyMs == m_ahead.timing.latencyMs)
      refusedLog.aheadEnd = write ? "published after a write, same" : "published, same";
    else
    {
      refusedLog.aheadEnd = write ? "published after a write, DIFF" : "published, DIFF";
      refusedLog.aheadPublished = published;
    }
    m_ahead.ticks = 0;
  }
#endif
  if (m_refused.ticks)
  {
    refusedLog.ended = m_refused;
    refusedLog.endNs = SteadyNs();
    // a later epoch may already be published when its first tick comes
    refusedLog.published = !timing && m_refused.epoch == tick.epoch;
    m_refused.ticks = 0;
  }

#if defined(HAS_LIBAMCODEC)
  if (m_commitTiming)
    CheckCommittedFps(tick, timing, refusedLog.fpsEnded, refusedLog.fpsBegun);
#endif

  CheckEnableClockSync(timing);

  MAINCOUPLING_ACCESSOR("RM m_presentlock");
  std::unique_lock presentLock(m_presentlock);
  diagPresent = diagClock::now();
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
  const bool forced = m_forceNext;
  if (m_presentstep == PRESENT_READY)
    PrepareNextRender(tick.vblankNs, timing);
  diagPrepare = diagClock::now();
  result.skipped = std::max(0, m_QueueSkip - skipped);

  if (m_presentstep == PRESENT_FLIP)
  {
    m_presentstep = PRESENT_FRAME;
    result.newFrame = true;
    result.pts = m_Queue[m_presentsource].pts;
  }

  // a worker's start picture: released where a waiting AddVideoPicture would be
  if (forced && m_forceGen >= 0 && m_presentstep != PRESENT_READY)
  {
    m_forceNext = false;
    m_forceDoneGen = m_forceGen;
    m_forceGen = -1;
    NotifyWorker();
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
      NotifyWorker();
    }
    else
      ++it;
  }

  if (result.newFrame)
    m_pRenderer->PresentFrame(m_presentsource);
  diagFrame = diagClock::now();

  ShadowReference(result, forced && result.newFrame, timing);

  m_playerPort->UpdateRenderBuffers(m_queued.size(), m_discard.size(), m_free.size());
  m_presentevent.notifyAll();
}
