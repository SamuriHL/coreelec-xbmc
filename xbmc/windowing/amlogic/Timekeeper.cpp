/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Timekeeper.h"

#include "utils/PresentationTimeline.h"
#include "utils/log.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <numeric>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

using namespace PRESENTATION;
using namespace std::chrono_literals;

namespace
{
constexpr int TIME_PRIORITY = 45; // below the kernel's IRQ threads (50)
constexpr int64_t EVENT_LOST_NS = 100000000;
constexpr int REPORT_SECONDS = 10;
constexpr size_t MAX_SAMPLES = 3600;

int64_t MonotonicNs()
{
  struct timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct SequenceEvent
{
  bool got = false;
  uint64_t seq = 0;
  uint64_t ns = 0;
};
thread_local SequenceEvent t_sequence;

void OnSequence(int fd, uint64_t sequence, uint64_t ns, uint64_t userData)
{
  t_sequence.got = true;
  t_sequence.seq = sequence;
  t_sequence.ns = ns;
}

struct Fit
{
  size_t n = 0;
  double slope = 0;
  double rms = 0;
  double max = 0;
};

// least squares y = a + b x over centred values
Fit FitLine(const std::deque<std::pair<double, double>>& points)
{
  Fit fit;
  fit.n = points.size();
  if (fit.n < 3)
    return fit;
  double mx = 0, my = 0;
  for (const auto& [x, y] : points)
  {
    mx += x;
    my += y;
  }
  mx /= fit.n;
  my /= fit.n;
  double sxx = 0, sxy = 0;
  for (const auto& [x, y] : points)
  {
    sxx += (x - mx) * (x - mx);
    sxy += (x - mx) * (y - my);
  }
  if (sxx <= 0)
    return fit;
  fit.slope = sxy / sxx;
  double sum = 0;
  for (const auto& [x, y] : points)
  {
    const double r = y - (my + fit.slope * (x - mx));
    sum += r * r;
    fit.max = std::max(fit.max, std::fabs(r));
  }
  fit.rms = std::sqrt(sum / fit.n);
  return fit;
}

void Keep(std::deque<std::pair<double, double>>& points, double x, double y)
{
  points.emplace_back(x, y);
  if (points.size() > MAX_SAMPLES)
    points.pop_front();
}
} // namespace

CTimekeeper::CTimekeeper(int masterFd, uint32_t crtcId) : m_masterFd(masterFd), m_crtcId(crtcId)
{
}

CTimekeeper::~CTimekeeper()
{
  Stop();
}

bool CTimekeeper::Start()
{
  if (m_masterFd < 0 || !m_crtcId)
    return false;
  // its own fd: its vblank events never mix with anyone else's
  char* path = drmGetDeviceNameFromFd2(m_masterFd);
  if (path)
  {
    m_fd = open(path, O_RDWR | O_CLOEXEC);
    free(path);
  }
  m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (m_fd < 0 || m_wakeFd < 0)
  {
    CLog::Log(LOGWARNING, "CTimekeeper - no fds (drm:{} wake:{})", m_fd, m_wakeFd);
    Stop();
    return false;
  }
  FindFracProperty();
  if (!m_fracPropId)
    CLog::Log(LOGWARNING, "CTimekeeper - no FRAC_RATE_POLICY on the connector: 1000/1001 rates "
                          "will read as integer rates");
  ShadowActive() = true;
  m_thread = std::thread(&CTimekeeper::Run, this);
  m_reporter = std::thread(&CTimekeeper::Report, this);
  return true;
}

void CTimekeeper::Stop()
{
  ShadowActive() = false;
  m_stop = true;
  if (m_wakeFd >= 0)
  {
    const uint64_t one = 1;
    if (write(m_wakeFd, &one, sizeof(one)) < 0)
    {
    }
  }
  m_reportCond.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  if (m_reporter.joinable())
    m_reporter.join();
  if (m_fd >= 0)
    close(m_fd);
  if (m_wakeFd >= 0)
    close(m_wakeFd);
  m_fd = m_wakeFd = -1;
}

void CTimekeeper::FindFracProperty()
{
  // the connector driving our CRTC, and its FRAC_RATE_POLICY property
  drmModeResPtr res = drmModeGetResources(m_fd);
  if (!res)
    return;
  for (int i = 0; i < res->count_connectors && !m_fracPropId; i++)
  {
    drmModeConnectorPtr connector = drmModeGetConnector(m_fd, res->connectors[i]);
    if (!connector)
      continue;
    drmModeEncoderPtr encoder =
        connector->encoder_id ? drmModeGetEncoder(m_fd, connector->encoder_id) : nullptr;
    if (encoder && encoder->crtc_id == m_crtcId)
    {
      drmModeObjectPropertiesPtr props =
          drmModeObjectGetProperties(m_fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
      for (uint32_t p = 0; props && p < props->count_props; p++)
      {
        drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[p]);
        if (prop && std::string(prop->name) == "FRAC_RATE_POLICY")
        {
          m_connectorId = connector->connector_id;
          m_fracPropId = prop->prop_id;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
  }
  drmModeFreeResources(res);
}

int CTimekeeper::FracPolicy()
{
  if (!m_fracPropId)
    return 0;
  int value = 0;
  drmModeObjectPropertiesPtr props =
      drmModeObjectGetProperties(m_fd, m_connectorId, DRM_MODE_OBJECT_CONNECTOR);
  for (uint32_t p = 0; props && p < props->count_props; p++)
    if (props->props[p] == m_fracPropId)
      value = static_cast<int>(props->prop_values[p]);
  drmModeFreeObjectProperties(props);
  return value;
}

bool CTimekeeper::ReadMode(uint64_t& num, uint64_t& den)
{
  const int64_t start = MonotonicNs();
  const bool ok = ReadModeNow(num, den);
  const int64_t us = (MonotonicNs() - start) / 1000;
  m_modeReads.fetch_add(1, std::memory_order_relaxed);
  int64_t max = m_modeReadMaxUs.load(std::memory_order_relaxed);
  while (us > max && !m_modeReadMaxUs.compare_exchange_weak(max, us))
  {
  }
  return ok;
}

bool CTimekeeper::ReadModeNow(uint64_t& num, uint64_t& den)
{
  drmModeCrtcPtr crtc = drmModeGetCrtc(m_fd, m_crtcId);
  if (!crtc)
    return false;
  bool ok = false;
  if (crtc->mode_valid && crtc->mode.clock && crtc->mode.htotal && crtc->mode.vtotal)
  {
    // period = htotal * vtotal / (clock kHz * 1000) seconds
    num = static_cast<uint64_t>(crtc->mode.htotal) * crtc->mode.vtotal;
    den = static_cast<uint64_t>(crtc->mode.clock) * 1000;
    // Amlogic makes the 1000/1001 rates through the connector's
    // FRAC_RATE_POLICY; the mode's clock stays the integer-rate one
    const uint32_t hz = crtc->mode.vrefresh;
    if ((hz == 24 || hz == 30 || hz == 48 || hz == 60 || hz == 120 || hz == 240) &&
        FracPolicy() == 1)
    {
      num *= 1001;
      den *= 1000;
    }
    const uint64_t g = std::gcd(num, den);
    num /= g;
    den /= g;
    ok = true;
  }
  drmModeFreeCrtc(crtc);
  return ok;
}

void CTimekeeper::Run()
{
  pthread_setname_np(pthread_self(), "Timekeeper");
  sched_param param = {};
  param.sched_priority = TIME_PRIORITY;
  m_rtResult = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

  uint64_t num, den;
  if (ReadMode(num, den))
  {
    m_num = num;
    m_den = den;
  }

  drmEventContext context = {};
  context.version = 4;
  context.sequence_handler = OnSequence;

  while (!m_stop)
  {
    int64_t now = MonotonicNs();
    if (m_pending && now - m_queuedNs > EVENT_LOST_NS)
      m_pending = false;
    if (!m_pending)
    {
      uint64_t target = 0;
      if (drmCrtcQueueSequence(m_fd, m_crtcId, DRM_CRTC_SEQUENCE_RELATIVE, 1, &target, 0) == 0)
      {
        m_pending = true;
        m_queuedNs = now;
        m_failedQueues = 0;
      }
      else
      {
        // the CRTC is mid-commit (EINVAL at a mode set): a mode change may follow
        m_queueErrors.fetch_add(1, std::memory_order_relaxed);
        m_failedQueues++;
        if (!m_suspect && m_lastRealNs)
        {
          m_suspect = true;
          m_lastOnGridNs = m_lastRealNs;
          m_onGridRun = 0;
        }
      }
    }

    const int periodMs = static_cast<int>(std::ceil(NominalNs() / 1e6));
    struct pollfd fds[2] = {{m_fd, POLLIN, 0}, {m_wakeFd, POLLIN, 0}};
    // retry a failed queue quickly through a commit, slowly while the CRTC is off
    const int timeout =
        m_pending ? std::max(5, periodMs * 3 / 2) : (m_failedQueues > 50 ? periodMs : 2);
    const int ready = poll(fds, 2, timeout);
    if (m_stop)
      break;
    now = MonotonicNs();

    if (ready > 0 && (fds[0].revents & POLLIN))
    {
      t_sequence.got = false;
      drmHandleEvent(m_fd, &context);
      if (t_sequence.got)
      {
        m_pending = false;
        OnVblank(t_sequence.seq, static_cast<int64_t>(t_sequence.ns), now);
        continue;
      }
    }
    OnTimeout(now);
  }
}

void CTimekeeper::OnVblank(uint64_t kernelSeq, int64_t ns, int64_t now)
{
  if (m_lastRealNs)
  {
    const int64_t dt = ns - m_lastRealNs;
    double nominal = NominalNs();
    int64_t steps = std::llround(dt / nominal);
    const bool onGrid = steps >= 1 && std::fabs(dt - steps * nominal) <= 0.02 * nominal;
    if (!onGrid && !m_suspect)
    {
      // the vblank before this one was the last on the old grid
      m_suspect = true;
      m_lastOnGridNs = m_lastRealNs;
      m_onGridRun = 0;
    }
    if (m_suspect)
    {
      // re-read the mode (off the steady path) until it changes, or the old
      // grid holds for three vblanks again
      uint64_t num, den;
      if (ReadMode(num, den) && num * m_den != den * m_num)
      {
        m_modeOldNum = m_num;
        m_modeOldDen = m_den;
        m_num = num;
        m_den = den;
        m_epoch++;
        m_epochStartNs = m_lastOnGridNs;
        m_epochStartTick = m_tickAtLastReal;
        m_valid = false;
        m_validRun = 0;
        m_suspect = false;
        nominal = NominalNs();
        steps = std::llround(dt / nominal);
      }
      else if (onGrid && ++m_onGridRun >= 3)
        m_suspect = false;
    }
    if (steps < 1)
      steps = 1;
    if (steps > 1)
      m_skips.fetch_add(steps - 1, std::memory_order_relaxed);
    // timer ticks may have covered this period already; never step back
    const uint64_t target = m_tickAtLastReal + steps;
    if (target >= m_tick)
      m_tick = target;
    else
      m_heldBack.fetch_add(1, std::memory_order_relaxed);

    if (!m_valid)
    {
      m_validRun = std::fabs(dt - nominal) < 0.005 * nominal ? m_validRun + 1 : 0;
      if (m_validRun >= 3)
      {
        m_valid = true;
        m_validTicks = static_cast<int64_t>(m_tick - m_epochStartTick);
        m_validNs = ns - m_epochStartNs;
        m_validEpoch = m_epoch;
      }
    }
  }
  else
    m_tick = 1;

  m_tickAtLastReal = m_tick;
  m_lastRealNs = ns;
  m_lastKernelSeq = kernelSeq;

  const int64_t wakeUs = std::max<int64_t>(0, (now - ns) / 1000);
  m_wake[std::min<int64_t>(wakeUs / 50, WAKE_BINS - 1)].fetch_add(1, std::memory_order_relaxed);
  int64_t max = m_wakeMaxUs.load(std::memory_order_relaxed);
  while (wakeUs > max && !m_wakeMaxUs.compare_exchange_weak(max, wakeUs))
  {
  }

  Publish(ns, false);
}

void CTimekeeper::OnTimeout(int64_t now)
{
  // no vblank for 1.5 periods: keep time with timer ticks at the nominal rate
  if (!m_lastRealNs)
    return;
  const double nominal = NominalNs();
  while (now - m_lastPublishedNs > 1.5 * nominal)
  {
    m_tick++;
    m_synthetic.fetch_add(1, std::memory_order_relaxed);
    Publish(m_lastPublishedNs + static_cast<int64_t>(nominal), true);
  }
}

void CTimekeeper::Publish(int64_t vblankNs, bool synthetic)
{
  m_lastPublishedNs = vblankNs;
  const int64_t values[TL_COUNT] = {static_cast<int64_t>(m_tick),
                                    static_cast<int64_t>(m_lastKernelSeq),
                                    vblankNs,
                                    static_cast<int64_t>(m_num),
                                    static_cast<int64_t>(m_den),
                                    static_cast<int64_t>(m_epoch),
                                    synthetic ? 1 : 0};
  TimelineBoard().Write(values);
}

void CTimekeeper::Report()
{
  pthread_setname_np(pthread_self(), "TimekeeperLog");
  bool startLogged = false;
  int64_t lastEpoch = 0;
  uint64_t validLogged = 0;
  int64_t lastTick = -1;
  int seconds = 0;

  // samples since the last change of epoch / clock segment / sink position
  std::deque<std::pair<double, double>> rate;     // tick → vblank ns
  std::deque<std::pair<double, double>> clock;    // kernel seq → CDVDClock µs
  std::deque<std::pair<double, double>> audio;    // timeline tick (at the read) → frames
  std::deque<std::pair<double, double>> audioHt;  // timeline tick (at htstamp) → frames
  int64_t audioOpen = -1;
  int64_t audioRate = 0;
  int64_t lastAudioMono = 0;
  int64_t maxLag = 0;
  int64_t steppedBack = 0;

  while (!m_stop)
  {
    {
      std::unique_lock lock(m_reportMutex);
      m_reportCond.wait_for(lock, 1s, [this] { return m_stop.load(); });
    }
    if (m_stop)
      break;

    int64_t tl[TL_COUNT];
    if (!TimelineBoard().Read(tl))
      continue;
    const double nominalNs = 1e9 * static_cast<double>(tl[TL_PERIOD_NUM]) / tl[TL_PERIOD_DEN];

    if (!startLogged)
    {
      const int rt = m_rtResult.load();
      CLog::Log(LOGINFO,
                "TIMEKEEPER started (shadow): SCHED_FIFO {} {}, mode period {}/{} s ({:.6f} Hz)",
                TIME_PRIORITY, rt == 0 ? std::string("granted") : "refused: " + std::string(strerror(rt)),
                tl[TL_PERIOD_NUM], tl[TL_PERIOD_DEN], 1e9 / nominalNs);
      startLogged = true;
    }

    if (tl[TL_EPOCH] != lastEpoch)
    {
      if (lastEpoch)
      {
        const double oldNs = 1e9 * static_cast<double>(m_modeOldNum.load()) / m_modeOldDen.load();
        CLog::Log(LOGINFO, "TIMEKEEPER epoch {}: mode period {}/{} s ({:.6f} Hz), was {:.6f} Hz",
                  tl[TL_EPOCH], tl[TL_PERIOD_NUM], tl[TL_PERIOD_DEN], 1e9 / nominalNs,
                  1e9 / oldNs);
      }
      lastEpoch = tl[TL_EPOCH];
      rate.clear();
      clock.clear();
      audio.clear();
      audioHt.clear();
    }
    const uint64_t validEpoch = m_validEpoch.load();
    if (validEpoch == static_cast<uint64_t>(tl[TL_EPOCH]) && validEpoch != validLogged &&
        validEpoch > 1)
    {
      CLog::Log(LOGINFO,
                "TIMEKEEPER epoch {}: period valid after {} ticks, {:.1f} ms after the last vblank "
                "on the previous mode's grid",
                validEpoch, m_validTicks.load(), m_validNs.load() / 1e6);
      validLogged = validEpoch;
    }

    if (tl[TL_TICK] < lastTick)
      steppedBack++;
    lastTick = tl[TL_TICK];
    if (!tl[TL_SYNTHETIC])
      Keep(rate, static_cast<double>(tl[TL_TICK]), static_cast<double>(tl[TL_VBLANK_NS]));

    // CDVDClock as the coordinator saw it at its last tick
    int64_t cs[CS_COUNT];
    if (CoordinatorBoard().Read(cs) && cs[CS_PLAYING])
    {
      maxLag = std::max(maxLag, tl[TL_KERNEL_SEQ] - cs[CS_SEQ]);
      const double x = static_cast<double>(cs[CS_SEQ]);
      const double y = static_cast<double>(cs[CS_CLOCK_US]);
      if (!clock.empty())
      {
        // a seek or discontinuity: start a new segment
        const auto& [px, py] = clock.back();
        const double predicted = py + (x - px) * nominalNs / 1000.0;
        if (x <= px || std::fabs(y - predicted) > 50000.0)
          clock.clear();
      }
      if (clock.empty() || x > clock.back().first)
        Keep(clock, x, y);
    }
    else
      clock.clear();

    // the sink's position against the timeline
    int64_t ap[AP_COUNT];
    if (AudioBoard().Read(ap) && ap[AP_RATE] > 0)
    {
      if (ap[AP_OPEN_ID] != audioOpen || ap[AP_RATE] != audioRate)
      {
        audio.clear();
        audioHt.clear();
        audioOpen = ap[AP_OPEN_ID];
        audioRate = ap[AP_RATE];
      }
      const int64_t age = tl[TL_VBLANK_NS] - ap[AP_MONO_NS];
      if (ap[AP_MONO_NS] != lastAudioMono && std::llabs(age) < 500000000)
      {
        lastAudioMono = ap[AP_MONO_NS];
        const double tickAtRead =
            tl[TL_TICK] + static_cast<double>(ap[AP_MONO_NS] - tl[TL_VBLANK_NS]) / nominalNs;
        Keep(audio, tickAtRead, static_cast<double>(ap[AP_FRAMES_PLAYED]));
        if (ap[AP_HTSTAMP_NS])
        {
          const int64_t htMono = ap[AP_HTSTAMP_NS] - (ap[AP_WALL_NS] - ap[AP_MONO_NS]);
          const double tickAtHt =
              tl[TL_TICK] + static_cast<double>(htMono - tl[TL_VBLANK_NS]) / nominalNs;
          Keep(audioHt, tickAtHt, static_cast<double>(ap[AP_FRAMES_PLAYED]));
        }
      }
    }

    if (++seconds < REPORT_SECONDS)
      continue;
    seconds = 0;

    // wake latency after vblank over the window
    uint32_t bins[WAKE_BINS];
    uint64_t total = 0;
    for (int i = 0; i < WAKE_BINS; i++)
    {
      bins[i] = m_wake[i].exchange(0, std::memory_order_relaxed);
      total += bins[i];
    }
    auto percentile = [&](double p)
    {
      const uint64_t want = static_cast<uint64_t>(std::ceil(p * total));
      uint64_t seen = 0;
      for (int i = 0; i < WAKE_BINS; i++)
      {
        seen += bins[i];
        if (seen >= want)
          return (i + 1) * 50;
      }
      return WAKE_BINS * 50;
    };
    const int64_t wakeMax = m_wakeMaxUs.exchange(0);

    const Fit r = FitLine(rate);
    const double ratePpm = r.n >= 3 ? (r.slope / nominalNs - 1.0) * 1e6 : 0.0;
    const Fit c = FitLine(clock);
    const double clockPpm = c.n >= 3 ? (c.slope / (nominalNs / 1000.0) - 1.0) * 1e6 : 0.0;
    const Fit a = FitLine(audio);
    const Fit h = FitLine(audioHt);
    const double framesPerTick = audioRate * nominalNs / 1e9;
    const double audioPpm = a.n >= 3 ? (a.slope / framesPerTick - 1.0) * 1e6 : 0.0;
    const double audioHtPpm = h.n >= 3 ? (h.slope / framesPerTick - 1.0) * 1e6 : 0.0;
    const double usPerFrame = audioRate ? 1e6 / audioRate : 0.0;

    CLog::Log(LOGINFO,
              "TIMEKEEPER epoch {} tick {} kseq {} | wake us p50 {} p99 {} p99.9 {} max {} (n {}) | "
              "skips {} synthetic {} heldback {} queue errors {} stepped back {} mode reads {} "
              "(max {} us) | vblank vs "
              "nominal {:+.2f} ppm (n {}, resid {:.0f} us) | CDVDClock vs timeline {:+.1f} ppm "
              "(n {}, resid rms {:.2f} max {:.2f} ms, coordinator lag {} vblanks) | audio {} Hz vs "
              "timeline {:+.2f} ppm (n {}, resid rms {:.0f} max {:.0f} us); at htstamp {:+.2f} ppm "
              "(n {}, resid rms {:.0f} max {:.0f} us)",
              tl[TL_EPOCH], tl[TL_TICK], tl[TL_KERNEL_SEQ], percentile(0.5), percentile(0.99),
              percentile(0.999), wakeMax, total, m_skips.load(), m_synthetic.load(),
              m_heldBack.load(), m_queueErrors.load(), steppedBack, m_modeReads.exchange(0),
              m_modeReadMaxUs.exchange(0), ratePpm, r.n, r.rms / 1000.0,
              clockPpm, c.n, c.rms / 1000.0, c.max / 1000.0, maxLag, audioRate, audioPpm, a.n,
              a.rms * usPerFrame, a.max * usPerFrame, audioHtPpm, h.n, h.rms * usPerFrame,
              h.max * usPerFrame);
    maxLag = 0;
  }
}
