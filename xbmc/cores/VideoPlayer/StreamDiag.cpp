/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for complete license information.
 */

#include "StreamDiag.h"

#include "utils/AMLUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

#include <time.h>

namespace STREAMDIAG
{
namespace
{
constexpr int MAX_SITES = 160;
constexpr int NUM_CTX = 4;
constexpr int NUM_BUCKETS = 64;
constexpr double BUCKET_BASE_US = 10.0;
constexpr double BUCKET_FACTOR = 1.25;
constexpr int RING = 512;
const char* const CTX_NAMES[NUM_CTX] = {"steady", "flush", "codecflush", "format"};

struct Hist
{
  std::atomic<uint64_t> n{0};
  std::atomic<uint64_t> buckets[NUM_BUCKETS] = {};
  std::atomic<uint64_t> maxUs{0};
};

struct SiteStats
{
  Hist wall[NUM_CTX];
  Hist cpu[NUM_CTX];
};

struct Outlier
{
  std::atomic<int> site{-1};
  std::atomic<int> ctx{0};
  std::atomic<uint32_t> wallUs{0};
  std::atomic<int32_t> cpuUs{-1};
  std::atomic<int64_t> whenMs{0};
};

std::mutex g_registry;
std::array<std::string, MAX_SITES> g_names;
std::array<int, MAX_SITES> g_kind{}; // 0 other, 1 hold, 2 message
std::atomic<int> g_numSites{0};
SiteStats g_stats[MAX_SITES];
Outlier g_ring[RING];
std::atomic<uint64_t> g_ringHead{0};
uint64_t g_ringReported = 0;
std::atomic<int> g_ctxCount[NUM_CTX] = {};
std::atomic<bool> g_formatPicture{false};
std::atomic<int64_t> g_lastReportMs{0};

int Bucket(double us)
{
  if (us <= BUCKET_BASE_US)
    return 0;
  const int b = static_cast<int>(std::ceil(std::log(us / BUCKET_BASE_US) / std::log(BUCKET_FACTOR)));
  return std::min(b, NUM_BUCKETS - 1);
}

double BucketUpper(int b)
{
  return BUCKET_BASE_US * std::pow(BUCKET_FACTOR, b);
}

void AtomicMax(std::atomic<uint64_t>& m, uint64_t v)
{
  uint64_t cur = m.load(std::memory_order_relaxed);
  while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed))
  {
  }
}

void Add(Hist& h, double us)
{
  h.n.fetch_add(1, std::memory_order_relaxed);
  h.buckets[Bucket(us)].fetch_add(1, std::memory_order_relaxed);
  AtomicMax(h.maxUs, static_cast<uint64_t>(us));
}

double Percentile(const Hist& h, double q)
{
  const uint64_t n = h.n.load(std::memory_order_relaxed);
  if (!n)
    return 0.0;
  const uint64_t rank = static_cast<uint64_t>(std::ceil(q * static_cast<double>(n)));
  uint64_t acc = 0;
  for (int b = 0; b < NUM_BUCKETS; ++b)
  {
    acc += h.buckets[b].load(std::memory_order_relaxed);
    if (acc >= rank)
      return BucketUpper(b);
  }
  return BucketUpper(NUM_BUCKETS - 1);
}

int CurrentCtx()
{
  if (g_ctxCount[static_cast<int>(Ctx::FORMAT)].load() > 0 || g_formatPicture.load())
    return static_cast<int>(Ctx::FORMAT);
  if (g_ctxCount[static_cast<int>(Ctx::CODECFLUSH)].load() > 0)
    return static_cast<int>(Ctx::CODECFLUSH);
  if (g_ctxCount[static_cast<int>(Ctx::FLUSH)].load() > 0)
    return static_cast<int>(Ctx::FLUSH);
  return static_cast<int>(Ctx::STEADY);
}

int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void PushOutlier(int site, int ctx, double wallUs, double cpuUs)
{
  Outlier& o = g_ring[g_ringHead.fetch_add(1) % RING];
  o.site = site;
  o.ctx = ctx;
  o.wallUs = static_cast<uint32_t>(std::min(wallUs, 4.0e9));
  o.cpuUs = static_cast<int32_t>(cpuUs);
  o.whenMs = NowMs();
}

struct ThreadHolds
{
  int depth[2] = {0, 0};
  std::chrono::steady_clock::time_point t0[2];
  double cpu0[2] = {0.0, 0.0};
  double subWallUs[2] = {0.0, 0.0};
  double subCpuUs[2] = {0.0, 0.0};
};
thread_local ThreadHolds t_holds;
thread_local int64_t t_openEndNs = 0;
} // namespace

bool Enabled()
{
  static const bool enabled = aml_presentation_validated();
  return enabled;
}

int Site(const char* name)
{
  std::lock_guard<std::mutex> lock(g_registry);
  const int n = g_numSites.load();
  for (int i = 0; i < n; ++i)
    if (g_names[i] == name)
      return i;
  if (n >= MAX_SITES)
    return -1;
  g_names[n] = name;
  const std::string s(name);
  g_kind[n] = s.rfind("hold ", 0) == 0 ? 1 : (s.rfind("msg ", 0) == 0 ? 2 : 0);
  g_numSites.store(n + 1);
  return n;
}

void Record(int site, double wallUs, double cpuUs)
{
  if (!Enabled() || site < 0)
    return;
  const int ctx = CurrentCtx();
  SiteStats& s = g_stats[site];
  Add(s.wall[ctx], std::max(0.0, wallUs));
  if (cpuUs >= 0.0)
    Add(s.cpu[ctx], cpuUs);
  const bool outlier = (g_kind[site] == 1 && (wallUs > 5000.0 || cpuUs > 2000.0)) ||
                       (g_kind[site] == 2 && wallUs > 5000.0);
  if (outlier)
    PushOutlier(site, ctx, wallUs, cpuUs);
}

void EnterCtx(Ctx ctx)
{
  g_ctxCount[static_cast<int>(ctx)].fetch_add(1);
}

void LeaveCtx(Ctx ctx)
{
  g_ctxCount[static_cast<int>(ctx)].fetch_sub(1);
}

void FormatPicturePending()
{
  g_formatPicture = true;
}

void PictureAdded()
{
  g_formatPicture = false;
}

double ThreadCpuUs()
{
  struct timespec ts = {};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) * 1e6 + ts.tv_nsec / 1e3;
}

Hold::Hold(Lock lock, int site) : m_lock(lock), m_site(site)
{
  if (!Enabled())
  {
    m_ended = true;
    return;
  }
  const int l = static_cast<int>(m_lock);
  if (t_holds.depth[l]++ == 0)
  {
    m_outer = true;
    t_holds.t0[l] = std::chrono::steady_clock::now();
    t_holds.cpu0[l] = ThreadCpuUs();
    t_holds.subWallUs[l] = 0.0;
    t_holds.subCpuUs[l] = 0.0;
  }
}

void Hold::End()
{
  if (m_ended)
    return;
  m_ended = true;
  const int l = static_cast<int>(m_lock);
  --t_holds.depth[l];
  if (!m_outer)
    return;
  const double wall =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_holds.t0[l])
          .count() -
      t_holds.subWallUs[l];
  const double cpu = ThreadCpuUs() - t_holds.cpu0[l] - t_holds.subCpuUs[l];
  Record(m_site, wall, std::max(0.0, cpu));
}

CvWait::CvWait() : m_t0(std::chrono::steady_clock::now()), m_cpu0(Enabled() ? ThreadCpuUs() : 0.0)
{
}

CvWait::~CvWait()
{
  if (!Enabled())
    return;
  const double wall =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_t0).count();
  const double cpu = ThreadCpuUs() - m_cpu0;
  for (int l = 0; l < 2; ++l)
  {
    if (t_holds.depth[l] > 0)
    {
      t_holds.subWallUs[l] += wall;
      t_holds.subCpuUs[l] += cpu;
    }
  }
}

Span::~Span()
{
  if (!Enabled())
    return;
  Record(m_site,
         std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_t0).count());
}

OpenSpan::~OpenSpan()
{
  t_openEndNs = std::chrono::steady_clock::now().time_since_epoch().count();
}

bool PutBeforeOpenEnd(int64_t putNs)
{
  return putNs < t_openEndNs;
}

void MaybeReport(bool force)
{
  if (!Enabled())
    return;
  const int64_t now = NowMs();
  int64_t last = g_lastReportMs.load();
  if (!force && now - last < 30000)
    return;
  if (!g_lastReportMs.compare_exchange_strong(last, now))
    return;

  static std::mutex reportLock;
  std::lock_guard<std::mutex> lock(reportLock);

  const uint64_t head = g_ringHead.load();
  uint64_t from = std::max(g_ringReported, head > RING ? head - RING : 0);
  for (; from < head; ++from)
  {
    const Outlier& o = g_ring[from % RING];
    const int site = o.site.load();
    if (site < 0)
      continue;
    CLog::Log(LOGINFO, "STREAMDIAG outlier {} [{}] wall {:.0f} us cpu {} us", g_names[site],
              CTX_NAMES[o.ctx.load()], static_cast<double>(o.wallUs.load()), o.cpuUs.load());
  }
  g_ringReported = head;

  const int n = g_numSites.load();
  for (int i = 0; i < n; ++i)
  {
    for (int c = 0; c < NUM_CTX; ++c)
    {
      const Hist& w = g_stats[i].wall[c];
      const uint64_t count = w.n.load();
      if (!count)
        continue;
      const Hist& cp = g_stats[i].cpu[c];
      if (cp.n.load())
        CLog::Log(LOGINFO,
                  "STREAMDIAG {} [{}] n {} wall p50 {:.0f} p99 {:.0f} p99.9 {:.0f} max {} us | "
                  "cpu p99.9 {:.0f} max {} us",
                  g_names[i], CTX_NAMES[c], count, Percentile(w, 0.5), Percentile(w, 0.99),
                  Percentile(w, 0.999), w.maxUs.load(), Percentile(cp, 0.999), cp.maxUs.load());
      else
        CLog::Log(LOGINFO,
                  "STREAMDIAG {} [{}] n {} wall p50 {:.0f} p99 {:.0f} p99.9 {:.0f} max {} us",
                  g_names[i], CTX_NAMES[c], count, Percentile(w, 0.5), Percentile(w, 0.99),
                  Percentile(w, 0.999), w.maxUs.load());
    }
  }
}

} // namespace STREAMDIAG
