/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioFollower.h"

#include "filesystem/File.h"
#include "utils/PresentationTimeline.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include <pthread.h>
#if defined(HAS_ALSA)
#include <alsa/asoundlib.h>
#endif

using namespace PRESENTATION;
using namespace std::chrono_literals;

namespace
{
constexpr double TAU_S = 120.0;          // fine law: phase pulled in with this time constant
constexpr double TRIM_LIMIT_PPM = 50.0;
constexpr double FINE_WRITE_PPM = 0.05;  // fine law: smallest change worth a write
constexpr double GRID_STEP_PPM = 30.0;   // log-only: one real G12B MPLL step
constexpr double GRID_BAND_S = 100e-6;   // grid law: switch codes when the phase leaves ±100 µs
constexpr int GRID_HOLD_S = 5;           // the phase average must refill after a switch
constexpr double ABORT_PHASE_S = 1e-3;
constexpr double ABORT_DRIFT_PPM = 100.0;
constexpr double CLOCK_SPEED_PPM = 100.0; // off 1:1 by more: the content does not run on the vblanks
constexpr size_t FIT_SECONDS = 60;
constexpr size_t FIT_MIN = 30;
constexpr int REPORT_SECONDS = 10;

double Median(std::vector<double> v)
{
  if (v.empty())
    return 0;
  const auto mid = v.begin() + v.size() / 2;
  std::nth_element(v.begin(), mid, v.end());
  return *mid;
}

// Theil-Sen: the median of the pairwise slopes, robust to the htstamp outliers
double RobustSlope(const std::deque<std::pair<double, double>>& points)
{
  std::vector<double> slopes;
  slopes.reserve(points.size() * points.size() / 2);
  for (size_t i = 0; i < points.size(); i++)
    for (size_t j = i + 1; j < points.size(); j++)
      if (points[j].first > points[i].first)
        slopes.push_back((points[j].second - points[i].second) /
                         (points[j].first - points[i].first));
  return Median(std::move(slopes));
}

// The kernel's "HDMI Audio Clock Trim" (common_drivers 0021), in 0.01 ppm:
// [0] trim, [1] achieved against the PLL's nominal, [2] state (1 ready)
class CTrimControl
{
public:
  ~CTrimControl()
  {
#if defined(HAS_ALSA)
    if (m_ctl)
      snd_ctl_close(m_ctl);
#endif
  }

  bool Open()
  {
#if defined(HAS_ALSA)
    for (int card = 0; card < 8 && !m_ctl; card++)
    {
      snd_ctl_t* ctl = nullptr;
      if (snd_ctl_open(&ctl, ("hw:" + std::to_string(card)).c_str(), 0) < 0)
        continue;
      snd_ctl_elem_id_t* id;
      snd_ctl_elem_info_t* info;
      snd_ctl_elem_id_alloca(&id);
      snd_ctl_elem_info_alloca(&info);
      snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
      snd_ctl_elem_id_set_name(id, "HDMI Audio Clock Trim");
      snd_ctl_elem_info_set_id(info, id);
      if (snd_ctl_elem_info(ctl, info) == 0 && snd_ctl_elem_info_get_count(info) == 3)
      {
        m_ctl = ctl;
        m_numid = snd_ctl_elem_info_get_numid(info);
      }
      else
        snd_ctl_close(ctl);
    }
#endif
    return m_ctl != nullptr;
  }

  // the achieved trim in ppm, or false when there is no HDMI stream to trim
  bool Read(double& achievedPpm, int& state)
  {
#if defined(HAS_ALSA)
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_numid(value, m_numid);
    if (!m_ctl || snd_ctl_elem_read(m_ctl, value) < 0)
      return false;
    achievedPpm = snd_ctl_elem_value_get_integer(value, 1) / 100.0;
    state = static_cast<int>(snd_ctl_elem_value_get_integer(value, 2));
    return state == 1;
#else
    return false;
#endif
  }

  // 0 or a negative errno
  int Write(double trimPpm)
  {
#if defined(HAS_ALSA)
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_numid(value, m_numid);
    snd_ctl_elem_value_set_integer(value, 0, std::lround(trimPpm * 100.0));
    const int ret = m_ctl ? snd_ctl_elem_write(m_ctl, value) : -ENODEV;
    return ret < 0 ? ret : 0;
#else
    return -ENODEV;
#endif
  }

private:
#if defined(HAS_ALSA)
  snd_ctl_t* m_ctl = nullptr;
#else
  void* m_ctl = nullptr;
#endif
  unsigned int m_numid = 0;
};

enum class Mode
{
  LOG_ONLY, // no control, or not enabled: both laws run on paper
  WAIT,     // enabled, waiting for the drift fit
  FINE,     // a fine PLL (S6): t = -d0 - φ/τ
  GRID,     // a coarse PLL (G12B): switch between the two codes that bracket -d0
  ABORTED,  // trim 0 for the rest of this sink open
  OFF_SPEED, // the clock is not 1:1 with the vblanks (resampling to the display, speed adjust)
};

const char* ModeName(Mode mode)
{
  switch (mode)
  {
    case Mode::LOG_ONLY:
      return "log-only";
    case Mode::WAIT:
      return "acquiring";
    case Mode::FINE:
      return "fine";
    case Mode::GRID:
      return "grid";
    case Mode::OFF_SPEED:
      return "off-speed";
    default:
      return "aborted";
  }
}
} // namespace

CAudioFollower::~CAudioFollower()
{
  Stop();
}

void CAudioFollower::Start()
{
  m_stop = false;
  m_thread = std::thread(&CAudioFollower::Run, this);
}

void CAudioFollower::Stop()
{
  m_stop = true;
  m_cond.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void CAudioFollower::Run()
{
  pthread_setname_np(pthread_self(), "AudioFollower");

  CTrimControl control;
  const bool enabled = XFILE::CFile::Exists("special://profile/audio_follower");
  const bool haveControl = enabled && control.Open();
  CLog::Log(LOGINFO, "FOLLOWER {}", !enabled     ? "log-only (special://profile/audio_follower absent)"
                                    : haveControl ? "enabled"
                                                  : "log-only: no HDMI Audio Clock Trim control");

  // ψ: the sink's position against the vblank sequence since the anchor (open,
  // rate or epoch change), less the trim applied; φ: the phase since the last
  // landing
  bool anchored = false;
  int64_t openId = -1, rate = 0, epoch = 0;
  uint64_t landings = 0;
  double framesA = 0, seqA = 0, periodS = 0;
  double psiAtLanding = 0;
  bool landingPending = true;
  bool landingBin = false;           // the current second is the first since a landing
  bool paused = false;
  bool haveDrift = false;            // d0 fitted at least once since the sink opened
  double lastDrift = 0;
  int64_t lastMono = 0;

  std::vector<double> bin;           // ψ samples in the current second
  int64_t binStartNs = 0;
  double binX = 0;                   // seconds since the anchor at the bin's last sample
  std::deque<std::pair<double, double>> psi; // per second: (seconds, median trim-free ψ)
  std::deque<std::pair<int64_t, int64_t>> clockAt; // per second: (vblank seq, CDVDClock µs)

  Mode mode = haveControl ? Mode::WAIT : Mode::LOG_ONLY;
  double written = 0;        // the trim last written, ppm
  double applied = 0;        // the trim the kernel reports achieved, from the trim-0 baseline
  double baseline = 0;       // achieved at trim 0
  double appliedIntegral = 0;
  double gridStep = GRID_STEP_PPM;
  double driftAtStart = 0;
  std::deque<double> appliedRecent; // the applied trim over the fit window
  std::string abortReason;

  // log-only: each law's phase had it been applied
  double fineTrim = 0, fineIntegral = 0;
  double gridTrim = 0, gridIntegral = 0;
  int gridHold = 0, switches = 0;
  std::deque<double> gridRecent;     // last 5 s of the grid law's phase
  int seconds = 0, samples = 0;
  std::vector<double> noise;

  const auto write = [&](double trim) -> bool
  {
    const int ret = control.Write(trim);
    double achieved = 0;
    int state = 0;
    if (ret < 0 || !control.Read(achieved, state))
    {
      abortReason = "trim write " + std::to_string(trim) + " ppm failed (" + std::to_string(ret) +
                    ", state " + std::to_string(state) + ")";
      return false;
    }
    written = trim;
    applied = achieved - baseline;
    return true;
  };
  const auto abort = [&](const std::string& reason)
  {
    if (mode == Mode::FINE || mode == Mode::GRID || mode == Mode::WAIT)
    {
      CLog::Log(LOGWARNING, "FOLLOWER stopped for this stream: {}", reason);
      if (written != 0)
        control.Write(0);
      written = applied = 0;
      mode = Mode::ABORTED;
    }
  };
  const auto reset = [&]()
  {
    if (written != 0)
      control.Write(0);
    written = applied = appliedIntegral = 0;
    haveDrift = false;
    lastDrift = 0;
    mode = haveControl ? Mode::WAIT : Mode::LOG_ONLY;
    anchored = false;
    landingPending = true;
    bin.clear();
    psi.clear();
    appliedRecent.clear();
    gridRecent.clear();
    fineTrim = fineIntegral = gridTrim = gridIntegral = 0;
    gridHold = switches = 0;
    clockAt.clear();
  };

  while (!m_stop)
  {
    {
      std::unique_lock lock(m_mutex);
      m_cond.wait_for(lock, 10ms, [this] { return m_stop.load(); });
    }
    if (m_stop)
      break;

    int64_t tl[TL_COUNT], ap[AP_COUNT];
    if (!TimelineBoard().Read(tl) || !AudioBoard().Read(ap) || ap[AP_RATE] <= 0 ||
        !ap[AP_HTSTAMP_NS] || ap[AP_MONO_NS] == lastMono)
      continue;
    lastMono = ap[AP_MONO_NS];
    if (tl[TL_SYNTHETIC] || std::llabs(tl[TL_VBLANK_NS] - ap[AP_MONO_NS]) > 500000000)
      continue;

    if (ap[AP_OPEN_ID] != openId || ap[AP_RATE] != rate || tl[TL_EPOCH] != epoch)
    {
      openId = ap[AP_OPEN_ID];
      rate = ap[AP_RATE];
      epoch = tl[TL_EPOCH];
      reset();
    }

    // Paused, the sink plays pause bursts in whole milliseconds: its count is
    // no phase. Hold the trim, and on resume measure afresh from there.
    int64_t cs[CS_COUNT];
    const bool nowPaused = CoordinatorBoard().Read(cs) && !cs[CS_PLAYING] &&
                           std::llabs(tl[TL_KERNEL_SEQ] - cs[CS_SEQ]) < 30;
    if (nowPaused)
    {
      paused = true;
      continue;
    }
    if (paused)
    {
      paused = false;
      anchored = false;
      landingPending = true;
      psi.clear();
      appliedRecent.clear();
      appliedIntegral = 0;
      clockAt.clear();
    }
    const bool csFresh = CoordinatorBoard().Read(cs) && cs[CS_PLAYING] &&
                         std::llabs(tl[TL_KERNEL_SEQ] - cs[CS_SEQ]) < 30;

    const double periodNs = 1e9 * static_cast<double>(tl[TL_PERIOD_NUM]) / tl[TL_PERIOD_DEN];
    const int64_t htMono = ap[AP_HTSTAMP_NS] - (ap[AP_WALL_NS] - ap[AP_MONO_NS]);
    const double seqAtHt =
        static_cast<double>(tl[TL_KERNEL_SEQ]) + (htMono - tl[TL_VBLANK_NS]) / periodNs;
    const double frames = static_cast<double>(ap[AP_FRAMES_PLAYED]);
    if (!anchored)
    {
      anchored = true;
      framesA = frames;
      seqA = seqAtHt;
      periodS = periodNs / 1e9;
      binStartNs = htMono;
    }
    const double x = (seqAtHt - seqA) * periodS;
    const double psiNow = (frames - framesA) / rate - x;

    const uint64_t landed = AudioLandings().load(std::memory_order_acquire);
    if (landed != landings)
    {
      landings = landed;
      landingPending = true;
      clockAt.clear();
    }
    if (landingPending)
    {
      // φ is zero over the first second after it: one sample may be an
      // htstamp outlier of 100 µs and more
      landingPending = false;
      landingBin = true;
      fineIntegral = gridIntegral = 0;
      gridRecent.clear();
      bin.clear();
      binStartNs = htMono;
    }
    bin.push_back(psiNow);
    binX = x;
    samples++;

    if (htMono - binStartNs < 1000000000)
      continue;
    binStartNs = htMono;

    // one second: the median φ, and how far the samples scatter round it
    if (bin.empty())
      continue;
    const double psiMedian = Median(bin);
    if (landingBin)
    {
      landingBin = false;
      psiAtLanding = psiMedian;
    }
    const double phi = psiMedian - psiAtLanding;
    for (double& v : bin)
      v = std::fabs(v - psiMedian);
    noise.push_back(Median(bin));
    bin.clear();

    // the trim applied over the second just measured
    appliedIntegral += applied * 1e-6;
    appliedRecent.push_back(applied);
    if (appliedRecent.size() > FIT_SECONDS)
      appliedRecent.pop_front();
    psi.emplace_back(binX, psiAtLanding + phi - appliedIntegral);
    if (psi.size() > FIT_SECONDS)
      psi.pop_front();
    const bool fitted = psi.size() >= FIT_MIN;
    if (fitted)
    {
      lastDrift = RobustSlope(psi) * 1e6;
      haveDrift = true;
    }
    // d0 is the hardware's: after a pause the laws run on the last fit while
    // the next one builds
    const double driftPpm = lastDrift;

    // The PLL holds the sink to the vblanks, which is right only while the
    // content clock runs 1:1 with them. Counted in vblanks, not CLOCK_MONOTONIC
    // (slewed hundreds of ppm after a boot); the median of the per-second
    // rates ignores a clock jump.
    if (csFresh)
    {
      clockAt.emplace_back(cs[CS_SEQ], cs[CS_CLOCK_US]);
      if (clockAt.size() > 11)
        clockAt.pop_front();
    }
    std::vector<double> clockRates;
    for (size_t i = 1; i < clockAt.size(); i++)
      if (clockAt[i].first > clockAt[i - 1].first)
        clockRates.push_back(1e3 * (clockAt[i].second - clockAt[i - 1].second) /
                             ((clockAt[i].first - clockAt[i - 1].first) * periodNs));
    const double clockPpm = clockRates.size() >= 5 ? (Median(clockRates) - 1.0) * 1e6 : 0.0;
    const bool offSpeed = std::fabs(clockPpm) > CLOCK_SPEED_PPM;
    if (offSpeed && (mode == Mode::WAIT || mode == Mode::FINE || mode == Mode::GRID))
    {
      CLog::Log(LOGINFO, "FOLLOWER open {}: the clock runs {:+.0f} ppm off the vblanks, trim 0",
                openId, clockPpm);
      if (written != 0)
        control.Write(0);
      written = applied = 0;
      mode = Mode::OFF_SPEED;
    }
    else if (!offSpeed && mode == Mode::OFF_SPEED && clockRates.size() >= 5)
    {
      CLog::Log(LOGINFO, "FOLLOWER open {}: the clock runs 1:1 again", openId);
      mode = Mode::WAIT;
      // the phase built up while off speed is not the follower's to undo
      landingPending = true;
    }

    // the drift does not depend on the trim, so it is d0 whatever the loop does
    if (mode == Mode::WAIT && fitted)
    {
      double achieved = 0;
      int state = 0;
      if (std::fabs(driftPpm) > ABORT_DRIFT_PPM)
        abort("drift " + std::to_string(driftPpm) + " ppm");
      else if (!control.Read(achieved, state) || control.Write(3.0) < 0 ||
               !control.Read(achieved, state))
        abort("no HDMI stream to trim (state " + std::to_string(state) + ")");
      else
      {
        double zero = 0;
        control.Write(0);
        control.Read(zero, state);
        baseline = zero;
        const double fine = achieved - zero;
        if (std::fabs(fine - 3.0) < 0.5)
          mode = Mode::FINE;
        else if (control.Write(GRID_STEP_PPM) == 0 && control.Read(achieved, state) &&
                 achieved - zero > 15.0 && achieved - zero < 60.0)
        {
          gridStep = achieved - zero;
          control.Write(0);
          mode = Mode::GRID;
        }
        else
        {
          control.Write(0);
          abort("the PLL moved " + std::to_string(fine) + " ppm for 3");
        }
        driftAtStart = driftPpm;
        if (mode == Mode::FINE || mode == Mode::GRID)
          CLog::Log(LOGINFO,
                    "FOLLOWER open {}: {} PLL (step {:.2f} ppm), drift {:+.2f} ppm, achieved {:+.2f} "
                    "ppm at trim 0",
                    openId, ModeName(mode), mode == Mode::FINE ? fine : gridStep, driftPpm, baseline);
      }
    }

    const bool fineVirtual = mode != Mode::FINE;
    const bool gridVirtual = mode != Mode::GRID;

    // fine: t = -d0 - φ/τ; d0 comes from ψ, so the loop is one integrator on φ
    if (fineVirtual)
      fineIntegral += fineTrim * 1e-6;
    const double finePhi = phi + (fineVirtual ? fineIntegral : 0);
    if (haveDrift)
      fineTrim = std::clamp(-driftPpm - finePhi / TAU_S * 1e6, -TRIM_LIMIT_PPM, TRIM_LIMIT_PPM);

    // grid: the two codes that bracket -d0, one making the phase fall and the
    // other rise; switch between them when the phase leaves the band
    if (gridVirtual)
      gridIntegral += gridTrim * 1e-6;
    const double gridPhi = phi + (gridVirtual ? gridIntegral : 0);
    const double step = mode == Mode::GRID ? gridStep : GRID_STEP_PPM;
    gridRecent.push_back(gridPhi);
    if (gridRecent.size() > 5)
      gridRecent.pop_front();
    double gridMean = 0;
    for (double v : gridRecent)
      gridMean += v;
    gridMean /= gridRecent.size();
    if (gridHold > 0)
      gridHold--;
    if (haveDrift)
    {
      const double low = step * std::floor(-driftPpm / step);
      double want = gridTrim;
      if (std::fabs(gridTrim - low) > step / 2 && std::fabs(gridTrim - low - step) > step / 2)
        want = gridMean > 0 ? low : low + step;
      else if (gridHold == 0 && gridMean > GRID_BAND_S)
        want = low;
      else if (gridHold == 0 && gridMean < -GRID_BAND_S)
        want = low + step;
      if (std::fabs(want - gridTrim) > step / 2)
      {
        gridTrim = want;
        gridHold = GRID_HOLD_S;
        switches++;
      }
    }

    if (mode == Mode::FINE && std::fabs(fineTrim - written) >= FINE_WRITE_PPM && !write(fineTrim))
      abort(abortReason);
    else if (mode == Mode::GRID && std::fabs(gridTrim - written) > step / 2 && !write(gridTrim))
      abort(abortReason);

    // a trim that does not act shows as a drift that moves with it
    if (mode == Mode::FINE || mode == Mode::GRID)
    {
      double mean = 0;
      for (double v : appliedRecent)
        mean += v;
      mean /= appliedRecent.size();
      const double phiNow = mode == Mode::FINE ? finePhi : gridPhi;
      if (haveDrift && std::fabs(phiNow) > ABORT_PHASE_S)
        abort("phase " + std::to_string(phiNow * 1e6) + " us");
      else if (fitted && appliedRecent.size() >= FIT_SECONDS &&
               std::fabs(driftPpm - driftAtStart) > std::max(0.3, 0.5 * std::fabs(mean)))
        abort("the drift moved from " + std::to_string(driftAtStart) + " to " +
              std::to_string(driftPpm) + " ppm under a trim of " + std::to_string(mean) + " ppm");
    }

    if (++seconds < REPORT_SECONDS)
      continue;
    seconds = 0;
    CLog::Log(LOGINFO,
              "FOLLOWER {} open {} {} Hz | samples/s {} noise {:.0f} us | phase {:+.1f} us drift "
              "{:+.2f} ppm (n {}) | trim written {:+.2f} applied {:+.2f} ppm | fine law: trim "
              "{:+.2f} ppm phase {:+.1f} us | grid law: trim {:+.0f} ppm phase {:+.1f} us "
              "switches {}",
              ModeName(mode), openId, rate, samples / REPORT_SECONDS, Median(noise) * 1e6,
              phi * 1e6, driftPpm, psi.size(), written, applied, fineTrim, finePhi * 1e6, gridTrim,
              gridPhi * 1e6, switches);
    samples = 0;
    noise.clear();
  }

  if (written != 0)
    control.Write(0);
}
