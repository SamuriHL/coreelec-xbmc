/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AudioFollower.h"

#include "utils/PresentationTimeline.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include <pthread.h>

using namespace PRESENTATION;
using namespace std::chrono_literals;

namespace
{
constexpr double TAU_S = 120.0;          // S6 law: phase pulled in with this time constant
constexpr double TRIM_LIMIT_PPM = 50.0;
constexpr double G12B_STEP_PPM = 30.0;   // one real MPLL step (two SDM codes)
constexpr double G12B_BAND_S = 100e-6;   // switch codes when the phase leaves ±100 µs
constexpr int G12B_HOLD_S = 5;           // the phase average must refill after a switch
constexpr double ABORT_PHASE_S = 1e-3;
constexpr double ABORT_DRIFT_PPM = 100.0;
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

  // ψ: the sink's position against the vblank sequence since the anchor (open,
  // rate or epoch change); φ: the same since the last landing
  bool anchored = false;
  int64_t openId = -1, rate = 0, epoch = 0;
  uint64_t landings = 0;
  double framesA = 0, seqA = 0, periodS = 0;
  double psiAtLanding = 0;
  bool landingPending = true;
  int64_t lastMono = 0;

  std::vector<double> bin;           // φ samples in the current second
  int64_t binStartNs = 0;
  double binX = 0;                   // seconds since the anchor at the bin's last sample
  std::deque<std::pair<double, double>> psi; // per second: (seconds, median ψ)

  // the virtual loops: the phase each law would have produced
  double s6Trim = 0, s6Integral = 0;
  double gTrim = 0, gIntegral = 0;
  int gHold = 0, gSteps = 0;
  bool s6Abort = false, gAbort = false;
  std::deque<double> gRecent;        // last 5 s of the G12B virtual phase
  int seconds = 0, samples = 0;
  std::vector<double> noise;

  const auto reset = [&]()
  {
    anchored = false;
    landingPending = true;
    bin.clear();
    psi.clear();
    gRecent.clear();
    s6Trim = s6Integral = gTrim = gIntegral = 0;
    gHold = gSteps = 0;
    s6Abort = gAbort = false;
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
    }
    if (landingPending)
    {
      landingPending = false;
      psiAtLanding = psiNow;
      s6Integral = gIntegral = 0;
      gRecent.clear();
      bin.clear();
    }
    bin.push_back(psiNow - psiAtLanding);
    binX = x;
    samples++;

    if (htMono - binStartNs < 1000000000)
      continue;
    binStartNs = htMono;

    // one second: the median φ, and how far the samples scatter round it
    if (bin.empty())
      continue;
    const double phi = Median(bin);
    for (double& v : bin)
      v = std::fabs(v - phi);
    noise.push_back(Median(bin));
    bin.clear();
    psi.emplace_back(binX, psiAtLanding + phi);
    if (psi.size() > FIT_SECONDS)
      psi.pop_front();
    const bool fitted = psi.size() >= FIT_MIN;
    const double driftPpm = fitted ? RobustSlope(psi) * 1e6 : 0.0;

    // S6: a fine trim, t = -d0 - φ/τ; d0 comes from ψ, so it does not depend
    // on the trim and the loop is one integrator on φ
    s6Integral += s6Trim * 1e-6;
    const double s6Phi = phi + s6Integral;
    if (fitted && !s6Abort)
      s6Trim = std::clamp(-driftPpm - s6Phi / TAU_S * 1e6, -TRIM_LIMIT_PPM, TRIM_LIMIT_PPM);
    if (fitted && (std::fabs(s6Phi) > ABORT_PHASE_S || std::fabs(driftPpm) > ABORT_DRIFT_PPM))
      s6Abort = true;

    // G12B: the two grid codes that bracket -d0, one making the phase fall and
    // the other rise; switch between them when the phase leaves the band
    gIntegral += gTrim * 1e-6;
    const double gPhi = phi + gIntegral;
    gRecent.push_back(gPhi);
    if (gRecent.size() > 5)
      gRecent.pop_front();
    double gMean = 0;
    for (double v : gRecent)
      gMean += v;
    gMean /= gRecent.size();
    if (gHold > 0)
      gHold--;
    if (fitted && !gAbort)
    {
      const double low = G12B_STEP_PPM * std::floor(-driftPpm / G12B_STEP_PPM);
      double want = gTrim;
      if (gTrim != low && gTrim != low + G12B_STEP_PPM)
        want = gMean > 0 ? low : low + G12B_STEP_PPM;
      else if (gHold == 0 && gMean > G12B_BAND_S)
        want = low;
      else if (gHold == 0 && gMean < -G12B_BAND_S)
        want = low + G12B_STEP_PPM;
      if (want != gTrim)
      {
        gTrim = want;
        gHold = G12B_HOLD_S;
        gSteps++;
      }
    }
    if (fitted && (std::fabs(gPhi) > ABORT_PHASE_S || std::fabs(driftPpm) > ABORT_DRIFT_PPM))
      gAbort = true;

    if (++seconds < REPORT_SECONDS)
      continue;
    seconds = 0;
    CLog::Log(LOGINFO,
              "FOLLOWER (log-only) open {} {} Hz | samples/s {} noise {:.0f} us | phase {:+.1f} us "
              "drift {:+.2f} ppm (n {}) | S6 law: trim {:+.2f} ppm phase {:+.1f} us{} | G12B "
              "bracket: trim {:+.0f} ppm phase {:+.1f} us switches {}{}",
              openId, rate, samples / REPORT_SECONDS, Median(noise) * 1e6, phi * 1e6, driftPpm,
              psi.size(), s6Trim, s6Phi * 1e6, s6Abort ? " ABORT" : "", gTrim, gPhi * 1e6, gSteps,
              gAbort ? " ABORT" : "");
    samples = 0;
    noise.clear();
  }
}
