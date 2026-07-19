/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "threads/Thread.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

namespace
{
// The reference clock destroys and recreates the videosync object every time
// the clock restarts, so the measured panel rate must survive outside the
// instance. Keyed to the nominal rate it was measured for; cleared on real
// display/mode changes. Touched from three threads (vblank Run(), the
// reference-clock thread's GetFps(), the render thread's OnResetDisplay()) —
// every access goes through g_measureLock.
std::mutex g_measureLock;
double g_measuredFps = 0.0;
float g_measuredNominal = 0.0f;
double g_lastLoggedFps = 0.0;
int g_discardStreak = 0;

// The integer-vs-fractional rate gap is 1000ppm and measurement noise over the
// 5s window is sub-ppm, so a real panel mismatch sits comfortably between the
// two bounds. A deviation above MAX cannot be a fractional-rate gap — it means
// vblanks stalled inside the measuring window (mode set, HDMI renegotiation,
// blanking) — so such a measurement is discarded instead of becoming the
// master clock rate. MIN and MAX gate both the restart decision in Run() and
// the apply decision in GetFps(); keeping them shared is what prevents a
// restart loop if the thresholds ever drifted apart.
constexpr uint64_t MEASURE_WINDOW_NS = UINT64_C(5000000000);
constexpr double RATE_MISMATCH_MIN = 0.0003;
constexpr double RATE_MISMATCH_MAX = 0.005;
// Two consecutive in-band windows must agree this closely before their rate
// becomes the master clock rate. Settled-panel measurement noise is sub-ppm,
// so 100ppm is generous for a real mismatch; a transient wobble (HDMI
// retrain at disc open moving CRTC timing inside one window - observed
// 23.9988 on a true 23.976 panel, which then armed a 50ms/min A/V drift)
// cannot agree with the settled window that follows it.
constexpr double RATE_CONFIRM_AGREE = 0.0001;
} // namespace

CVideoSyncAML::CVideoSyncAML(CVideoReferenceClock *clock)
: CVideoSync(clock)
, m_abort(false)
{
}

CVideoSyncAML::~CVideoSyncAML()
{
}

bool CVideoSyncAML::Setup()
{
  m_abort = false;
  m_measureAnchorSeq = 0;
  m_measureAnchorNs = 0;
  m_pendingMeasuredFps = 0.0;

  CServiceBroker::GetWinSystem()->Register(this);
  CLog::Log(LOGDEBUG, "CVideoSyncAML: setting up (DRM vblank)");

  // CE22 runs the display on DRM/KMS (mesondrmfb). The reference clock is the
  // DRM CRTC vblank sequence, read non-blocking via drmCrtcGetSequence — the
  // same mechanism CVideoSyncGbm uses. The legacy OSD-fb FBIO_WAITFORVSYNC
  // ioctl is not implemented on the DRM fbdev, so it can't be used here.
  auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem)
  {
    CLog::Log(LOGWARNING, "CVideoSyncAML: no Amlogic win system, falling back to system clock");
    return false;
  }

  m_fd = winSystem->GetDRMDeviceFd();
  m_crtcId = winSystem->GetDRMCrtcId();
  if (m_fd < 0 || m_crtcId == 0)
  {
    CLog::Log(LOGWARNING,
              "CVideoSyncAML: no DRM device/crtc (fd:{} crtc:{}), falling back to system clock",
              m_fd, m_crtcId);
    return false;
  }

  uint64_t ns = 0;
  int s = drmCrtcGetSequence(m_fd, m_crtcId, &m_sequence, &ns);
  if (s != 0)
  {
    CLog::Log(LOGWARNING,
              "CVideoSyncAML: drmCrtcGetSequence failed ({}), falling back to system clock", s);
    m_fd = -1;
    return false;
  }
  // ns is CLOCK_MONOTONIC; CurrentHostCounter() is CLOCK_MONOTONIC_RAW on
  // Linux — nearly the same domain, but NTP slew (tens of ppm) accumulates
  // between them. The offset is re-based at every Setup, which bounds the
  // divergence to one clock-run's worth.
  m_offset = CurrentHostCounter() - static_cast<int64_t>(ns);

  CLog::Log(LOGINFO, "CVideoSyncAML: using DRM vblank (fd:{} crtc:{} seq:{})",
            m_fd, m_crtcId, m_sequence);
  return true;
}

void CVideoSyncAML::Run(CEvent& stopEvent)
{
  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  while (!stopEvent.Signaled() && !m_abort)
  {
    uint64_t sequence = 0, ns = 0;
    usleep(1000);
    int s = drmCrtcGetSequence(m_fd, m_crtcId, &sequence, &ns);
    if (s != 0)
    {
      CLog::Log(LOGWARNING,
                "CVideoSyncAML: drmCrtcGetSequence failed ({}), stopping vblank clock", s);
      break;
    }

    if (sequence == m_sequence)
      continue;

    // The Setup-time offset between ns (CLOCK_MONOTONIC) and
    // CurrentHostCounter() (MONOTONIC_RAW) goes stale: NTP frequency
    // training right after boot slews MONOTONIC by up to 500ppm, walking a
    // fixed offset past TimeOfNextVblank's 1.3-period lateness threshold
    // within minutes. GetTime then misreads every hardware timestamp as
    // "vblank late", extrapolates the clock forward each frame, and the
    // missed-vblank bookkeeping discards the negative remainder - a
    // feedback loop that locks the master clock ~2x fast (observed: audio
    // 1s+ behind and skipping ~2min after boot). Track the true offset
    // continuously instead, sliding at most 1ms per vblank so poll-latency
    // spikes cannot yank it while any real slew (20us/frame) is followed
    // with huge margin.
    {
      const int64_t drift = (CurrentHostCounter() - static_cast<int64_t>(ns)) - m_offset;
      m_offset += std::clamp(drift, static_cast<int64_t>(-1000000), static_cast<int64_t>(1000000));
    }

    m_refClock->UpdateClock(static_cast<int>(sequence - m_sequence), m_offset + ns);
    m_sequence = sequence;

    // Rate-honesty check: the vblank timestamps are hardware time, so
    // (sequence delta / time delta) is the panel's true refresh rate. If it
    // disagrees with the rate the clock is running on by more than the
    // mismatch band's lower bound, restart the clock so GetFps() can hand it
    // the measured rate.
    if (m_measureAnchorNs == 0)
    {
      m_measureAnchorSeq = sequence;
      m_measureAnchorNs = ns;
    }
    else if (ns - m_measureAnchorNs >= MEASURE_WINDOW_NS)
    {
      const double measuredFps = static_cast<double>(sequence - m_measureAnchorSeq) * 1e9 /
                                 static_cast<double>(ns - m_measureAnchorNs);
      m_measureAnchorSeq = sequence;
      m_measureAnchorNs = ns;

      if (m_reportedFps > 0.0)
      {
        const double deviation = std::abs(measuredFps / m_reportedFps - 1.0);
        if (deviation > RATE_MISMATCH_MAX)
        {
          // Usually a one-off (vblank stall in one window). A persistent
          // streak means the panel truly runs far off the mode rate (silent
          // mode-set failure) and the clock is staying on nominal - surface
          // that once above LOGDEBUG.
          int streak;
          {
            std::lock_guard<std::mutex> lock(g_measureLock);
            streak = ++g_discardStreak;
          }
          CLog::Log(streak == 3 ? LOGWARNING : LOGDEBUG,
                    "CVideoSyncAML: discarding implausible measured vblank rate {:.4f} Hz "
                    "(clock rate {:.4f} Hz){}",
                    measuredFps, m_reportedFps,
                    streak == 3 ? " - persistent: panel rate far off the mode rate, "
                                  "clock stays on nominal"
                                : " - vblank stall inside the measuring window");
          m_pendingMeasuredFps = 0.0; // confirmation must be consecutive
        }
        else if (deviation > RATE_MISMATCH_MIN)
        {
          if (m_pendingMeasuredFps > 0.0 &&
              std::abs(measuredFps / m_pendingMeasuredFps - 1.0) < RATE_CONFIRM_AGREE)
          {
            CLog::Log(LOGWARNING,
                      "CVideoSyncAML: measured vblank rate {:.4f} Hz != clock rate {:.4f} Hz "
                      "(panel fractional-rate mismatch, confirmed by consecutive windows), "
                      "restarting clock with measured rate",
                      measuredFps, m_reportedFps);
            {
              std::lock_guard<std::mutex> lock(g_measureLock);
              g_measuredFps = measuredFps;
              g_measuredNominal = m_fps;
              g_discardStreak = 0;
            }
            m_abort = true;
          }
          else
          {
            // First in-band window (or disagreeing with the previous one):
            // candidate only. A transient CRTC wobble lands here once and is
            // dropped when the settled window that follows fails to agree.
            CLog::Log(LOGDEBUG,
                      "CVideoSyncAML: measured vblank rate {:.4f} Hz != clock rate {:.4f} Hz - "
                      "awaiting confirmation window",
                      measuredFps, m_reportedFps);
            m_pendingMeasuredFps = measuredFps;
          }
        }
        else
        {
          // in-band measurement: any discard before it was a one-off stall
          m_pendingMeasuredFps = 0.0;
          std::lock_guard<std::mutex> lock(g_measureLock);
          g_discardStreak = 0;
        }
      }
    }
  }
}

void CVideoSyncAML::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoSyncAML: cleaning up");
  // m_fd is borrowed from the win system's DRM device — do not close it.
  m_fd = -1;
  CServiceBroker::GetWinSystem()->Unregister(this);
}

float CVideoSyncAML::GetFps()
{
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  m_reportedFps = m_fps;

  // Hand the clock the measured panel rate when it materially disagrees with
  // the nominal mode rate (fractional rate not actually applied by the
  // kernel). The clock then advances at true wall-time rate and passthrough
  // audio no longer drifts against it; the renderer sees the panel's real
  // cadence either way.
  double measured = 0.0;
  bool logApply = false;
  {
    std::lock_guard<std::mutex> lock(g_measureLock);
    if (g_measuredFps > 0.0 && g_measuredNominal == m_fps)
    {
      const double deviation = std::abs(g_measuredFps / static_cast<double>(m_fps) - 1.0);
      if (deviation > RATE_MISMATCH_MIN && deviation <= RATE_MISMATCH_MAX)
      {
        measured = g_measuredFps;
        // The videosync is recreated on every clock restart (constantly while
        // BD menus cycle playitems) — warn once per measured value, not per
        // recreation.
        logApply = g_lastLoggedFps != g_measuredFps;
        g_lastLoggedFps = g_measuredFps;
      }
    }
  }
  if (measured > 0.0)
  {
    CLog::Log(logApply ? LOGWARNING : LOGDEBUG,
              "CVideoSyncAML: using measured vblank rate {:.4f} Hz instead of nominal {:.3f} Hz",
              measured, m_fps);
    m_reportedFps = measured;
  }

  CLog::Log(LOGDEBUG, "CVideoSyncAML: fps: {:.3f}", m_reportedFps);
  return static_cast<float>(m_reportedFps);
}

void CVideoSyncAML::OnResetDisplay()
{
  // A display/mode change moves the CRTC timing; abort so the reference clock
  // re-runs Setup and re-anchors the vblank sequence. The measured rate
  // belongs to the old mode — discard it.
  {
    std::lock_guard<std::mutex> lock(g_measureLock);
    g_measuredFps = 0.0;
    g_measuredNominal = 0.0f;
    g_discardStreak = 0;
  }
  m_abort = true;
}
