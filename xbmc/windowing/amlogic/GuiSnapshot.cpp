/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GuiSnapshot.h"

#include "DisplayState.h"
#include "Seqlock.h"
#include "ServiceBroker.h"
#include "settings/DisplaySettings.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "windowing/WinSystem.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <type_traits>

namespace GUISNAPSHOT
{
namespace
{
static_assert(std::is_trivially_copyable_v<SSnapshot>);
constexpr std::size_t WORDS = (sizeof(SSnapshot) + 7) / 8;
using Words = std::array<uint64_t, WORDS>;
static_assert(WORDS <= 24, "testbench/v3seqlock/stress.cpp tests 24 words");

OUTPUT::CSeqlock<WORDS> s_lock;

// main only
uint64_t s_noticeSeqDone = 0;
uint64_t s_modeSeqSeen = 0;
SSnapshot s_atRenderEx;
bool s_haveRenderEx = false;
Words s_lastWords{};
bool s_published = false;

// counters (any reader)
std::atomic<uint64_t> s_publishes{0};
std::atomic<uint64_t> s_unchanged{0};
std::atomic<uint64_t> s_reads{0};
std::atomic<uint64_t> s_torn{0};
std::atomic<uint64_t> s_readsOver20us{0};
std::atomic<int64_t> s_readMaxNs{0};
int64_t s_lastLogMs = 0;
uint64_t s_lastLogged[4] = {};

int64_t NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool On()
{
  return aml_geometrytick_inputs_on();
}

// main: GC's state now (the stereo mode read once) and the main-only seqs
SSnapshot Capture()
{
  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  SSnapshot s;
  const RenderStereoMode mode = gfx.GetStereoMode();
  const RESOLUTION res = gfx.GetVideoResolution();
  const RESOLUTION_INFO& raw = CDisplaySettings::GetInstance().GetResolutionInfo(res);
  s.rawRow = RowOf(raw);
  s.row = StereoRow(s.rawRow, mode);
  // GetResInfo's FPS override: GetResInfo(m_Resolution) takes it
  s.row.refreshRate = gfx.GetResInfo().fRefreshRate;
  s.stereoMode = static_cast<int>(mode);
  s.videoRect = gfx.GetVideoRect();
  s.fullScreenVideo = gfx.IsFullScreenVideo();
  s.calibrating = gfx.IsCalibrating();
  s.noticeSeqDone = s_noticeSeqDone;
  s.modeSeqSeen = s_modeSeqSeen;
  const auto committed = DISPLAYSTATE::Get();
  s.rowMoved = committed && committed->strId == raw.strId &&
               committed->wRow.refreshRate == raw.fRefreshRate &&
               committed->dwFlags == raw.dwFlags;
  s.valid = true;
  return s;
}

void Publish(const SSnapshot& s)
{
  Words words{};
  std::memcpy(words.data(), &s, sizeof(s));
  s_publishes.fetch_add(1, std::memory_order_relaxed);
  if (s_published && words == s_lastWords)
  {
    s_unchanged.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s_lock.Write(words);
  s_lastWords = words;
  s_published = true;
  aml_presenter_wake();
}
} // namespace

SRow RowOf(const RESOLUTION_INFO& info)
{
  SRow r;
  r.overscanLeft = info.Overscan.left;
  r.overscanTop = info.Overscan.top;
  r.overscanRight = info.Overscan.right;
  r.overscanBottom = info.Overscan.bottom;
  r.width = info.iWidth;
  r.height = info.iHeight;
  r.blanking = info.iBlanking;
  r.screenWidth = info.iScreenWidth;
  r.screenHeight = info.iScreenHeight;
  r.subtitles = info.iSubtitles;
  r.pixelRatio = info.fPixelRatio;
  r.flags = info.dwFlags;
  r.refreshRate = info.fRefreshRate;
  return r;
}

SRow WRowOf(const DISPLAYSTATE::DisplayState& state)
{
  const auto& w = state.wRow;
  SRow r;
  r.overscanLeft = w.overscanLeft;
  r.overscanTop = w.overscanTop;
  r.overscanRight = w.overscanRight;
  r.overscanBottom = w.overscanBottom;
  r.width = w.width;
  r.height = w.height;
  r.blanking = w.blanking;
  r.screenWidth = state.screenWidth;
  r.screenHeight = state.screenHeight;
  r.subtitles = w.subtitles;
  r.pixelRatio = w.pixelRatio;
  r.flags = state.dwFlags;
  r.refreshRate = w.refreshRate;
  return r;
}

SRow StereoRow(SRow info, RenderStereoMode mode)
{
  if (mode == RenderStereoMode::SPLIT_HORIZONTAL)
  {
    if ((info.flags & D3DPRESENTFLAG_MODE3DTB) == D3DPRESENTFLAG_MODE3DTB)
    {
      info.pixelRatio /= 2;
      info.blanking = 0;
      info.height = (info.height - info.blanking) / 2;
      info.overscanTop /= 2;
      info.overscanBottom = (info.overscanBottom - info.blanking) / 2;
      info.subtitles = (info.subtitles - info.blanking) / 2;
    }
  }

  if (mode == RenderStereoMode::HARDWAREBASED)
  {
    if ((info.flags & D3DPRESENTFLAG_MODE3DFP) == D3DPRESENTFLAG_MODE3DFP)
    {
      info.blanking = info.screenHeight == 1080 ? 45 : 30;
      info.overscanBottom = info.screenHeight;
    }
  }

  if (mode == RenderStereoMode::SPLIT_VERTICAL)
  {
    if ((info.flags & D3DPRESENTFLAG_MODE3DSBS) == D3DPRESENTFLAG_MODE3DSBS)
    {
      info.pixelRatio *= 2;
      info.blanking = 0;
      info.width = (info.width - info.blanking) / 2;
      info.overscanLeft /= 2;
      info.overscanRight = (info.overscanRight - info.blanking) / 2;
    }
  }
  return info;
}

void PublishFrameMove()
{
  if (!On())
    return;
  Publish(Capture());
  Tick();
}

void CaptureAtRenderEx()
{
  if (!On())
    return;
  s_atRenderEx = Capture();
  s_haveRenderEx = true;
}

void PublishAfterFlip()
{
  if (!On() || !s_haveRenderEx)
    return;
  s_haveRenderEx = false;
  SSnapshot s = s_atRenderEx;
  s.videoRect = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoRect();
  // the seqs main took since RenderEx are its own, not the GUI render's
  s.noticeSeqDone = s_noticeSeqDone;
  s.modeSeqSeen = s_modeSeqSeen;
  Publish(s);
}

void NoteNoticeDone(uint64_t seq)
{
  if (!On() || seq == 0)
    return;
  s_noticeSeqDone = std::max(s_noticeSeqDone, seq);
  CLog::Log(LOGINFO, "GEOMDIAG fullscreen notice seq {} done (noticeSeqDone {})", seq,
            s_noticeSeqDone);
}

void NoteModeSeq(uint64_t seq)
{
  if (On())
    s_modeSeqSeen = std::max(s_modeSeqSeen, seq);
}

bool Read(SSnapshot& snapshot)
{
  const int64_t start = NowNs();
  bool ok = false;
  Words words;
  uint64_t seq = 0;
  for (int attempt = 0; attempt < 3 && !ok; attempt++)
    ok = s_lock.TryRead(words, seq);
  const int64_t took = NowNs() - start;
  s_reads.fetch_add(1, std::memory_order_relaxed);
  if (took > 20000)
    s_readsOver20us.fetch_add(1, std::memory_order_relaxed);
  int64_t max = s_readMaxNs.load(std::memory_order_relaxed);
  while (took > max && !s_readMaxNs.compare_exchange_weak(max, took, std::memory_order_relaxed))
  {
  }
  if (!ok)
  {
    s_torn.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (seq == 0)
    return false;
  std::memcpy(&snapshot, words.data(), sizeof(snapshot));
  return true;
}

void Tick()
{
  const int64_t nowMs = NowNs() / 1000000;
  if (nowMs - s_lastLogMs < 10000)
    return;
  const uint64_t now[4] = {s_publishes.load(), s_unchanged.load(), s_reads.load(),
                           s_torn.load()};
  s_lastLogMs = nowMs;
  // every pass publishes; only a changed one, a read or a torn read is news
  if (now[0] - now[1] == s_lastLogged[0] - s_lastLogged[1] && now[2] == s_lastLogged[2] &&
      now[3] == s_lastLogged[3])
    return;
  std::memcpy(s_lastLogged, now, sizeof(now));
  CLog::Log(LOGINFO,
            "GEOMDIAG snapshot publishes {} (unchanged {}), reads {}, torn {}, reads over 20 us "
            "{}, read max {} ns",
            now[0], now[1], now[2], now[3], s_readsOver20us.load(), s_readMaxNs.load());
}

} // namespace GUISNAPSHOT
