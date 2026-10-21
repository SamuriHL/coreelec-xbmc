/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DisplayState.h"

#include "AMLDisplay.h"
#include "settings/DisplaySettings.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

namespace DISPLAYSTATE
{
namespace
{
std::atomic<std::shared_ptr<const DisplayState>> s_state;
uint64_t s_epoch = 0; // publishers are never concurrent
uint64_t s_capsEpoch = 0;

std::atomic<uint64_t> s_modePublishes{0};
std::atomic<uint64_t> s_commits{0};
std::atomic<bool> s_capsPending{false};
std::atomic<bool> s_inSet{false};
std::atomic<bool> s_restored{false};
uint64_t s_setBeginPublishes = 0; // main

// the publisher's own accessor reads are not compares
thread_local bool t_publishing = false;

enum Column
{
  MATCH,
  DIFF,
  SKIPPED,
  WINDOW_SET,
  WINDOW_RESTORE,
  FRAC_HELD,
  FAIL,
  COLUMNS
};

// the first DIFF/FAIL details of a field, each slot written once by the reader that claimed it
constexpr int MAX_DETAILS = 10;

struct SCounter
{
  const char* field = nullptr;
  std::array<std::atomic<uint64_t>, COLUMNS> n{};
  std::array<uint64_t, COLUMNS> logged{}; // main
  std::atomic<int> claimed{0};
  std::array<std::string, MAX_DETAILS> details;
  std::atomic<uint32_t> ready{0};
  uint32_t drained = 0; // main
};

constexpr int MAX_FIELDS = 32;
std::array<SCounter, MAX_FIELDS> s_counters;
std::atomic<int> s_fieldCount{0};
std::mutex s_registerMutex;
std::mutex s_logMutex;
int64_t s_lastLogMs = 0; // main

int64_t NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

SCounter* CounterFor(const char* field)
{
  const int count = s_fieldCount.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i)
    if (std::strcmp(s_counters[i].field, field) == 0)
      return &s_counters[i];
  std::unique_lock lock(s_registerMutex);
  const int now = s_fieldCount.load(std::memory_order_relaxed);
  for (int i = 0; i < now; ++i)
    if (std::strcmp(s_counters[i].field, field) == 0)
      return &s_counters[i];
  if (now == MAX_FIELDS)
    return nullptr;
  s_counters[now].field = field;
  s_fieldCount.store(now + 1, std::memory_order_release);
  return &s_counters[now];
}

// readers only count; main logs (Tick, LogCounters)
void Count(const char* field, Column column)
{
  if (SCounter* counter = CounterFor(field))
    counter->n[column].fetch_add(1, std::memory_order_relaxed);
}

//! detail() runs only for the first MAX_DETAILS differences of a field
template<typename F>
void CountDiff(const char* field, Column column, F&& detail)
{
  SCounter* counter = CounterFor(field);
  if (!counter)
    return;
  counter->n[column].fetch_add(1, std::memory_order_relaxed);
  const int slot = counter->claimed.fetch_add(1, std::memory_order_relaxed);
  if (slot >= MAX_DETAILS)
    return;
  counter->details[slot] =
      StringUtils::Format("{}: {}", column == DIFF ? "DIFF" : "FAIL", detail());
  counter->ready.fetch_or(1u << slot, std::memory_order_release);
}

std::string Describe(const DisplayState& s)
{
  return StringUtils::Format("epoch {} mode {} @ {:.3f} caps epoch {}", s.epoch, s.strId, s.refresh,
                             s.capsEpoch);
}

std::shared_ptr<const DisplayState> Connected()
{
  auto state = s_state.load();
  return state && state->connected ? state : nullptr;
}

DisplayState Current()
{
  auto state = s_state.load();
  return state ? *state : DisplayState{};
}

float FpsRule(float refresh)
{
  return refresh > 0 ? refresh : 60.0f;
}

// a and b are the integer and 1000/1001 rate of a 24/30/60 mode, as CAMLDisplay's
// FractionalRate
bool FracPair(float a, float b)
{
  const float hi = std::max(a, b);
  const float lo = std::min(a, b);
  return (hi == 24.0f || hi == 30.0f || hi == 60.0f) &&
         lo == static_cast<float>(static_cast<double>(hi) / 1.001);
}

// GC holds the held set's request at the other rate of the mode the display runs
bool FracHeldAtMain(const DisplayState& s, const RESOLUTION_INFO& gcRes)
{
  return s.fracHeld && s.strId == gcRes.strId && FpsRule(gcRes.fRefreshRate) == s.requestedFps &&
         FracPair(s.refresh, gcRes.fRefreshRate);
}

void FillMode(DisplayState& s, const RESOLUTION_INFO& res, float refresh, bool held, bool windowed)
{
  s.refresh = refresh;
  s.fracHeld = held;
  s.requestedFps = FpsRule(res.fRefreshRate);
  s.screenWidth = res.iScreenWidth;
  s.screenHeight = res.iScreenHeight;
  s.dwFlags = res.dwFlags;
  s.strId = res.strId;
  // the DRM mode name, as CAMLDisplay::set_display_resolution takes it from strId
  s.modeString = res.strId.substr(0, res.strId.find(' '));
  s.fps = FpsRule(refresh);
  s.windowed = windowed;
}

void FillLink(DisplayState& s, const CAMLDisplay& display)
{
  s.connected = display.aml_get_display_connected();
  s.drmFd = display.aml_get_Device_handle();
  s.crtcId = display.aml_get_Device_crtc_id();
}

void FillCaps(DisplayState& s, const CAMLDisplay& display)
{
  s.hdrCaps = display.GetHDRCaps();
  s.support3d = display.aml_display_support_3d();
  s.hdrPq = aml_display_support_hdr_pq();
  s.hdrHlg = aml_display_support_hdr_hlg();
  s.hdr10Plus = aml_display_support_hdr10plus();
  s.has2160 = false;
  const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  for (size_t i = RES_DESKTOP; i < displaySettings.ResolutionInfoSize(); ++i)
  {
    if (displaySettings.GetResolutionInfo(i).iScreenHeight >= 2160)
    {
      s.has2160 = true;
      break;
    }
  }
  s.capsEpoch = ++s_capsEpoch;
}

void Store(DisplayState& s, const char* kind)
{
  s.epoch = ++s_epoch;
  s_state.store(std::make_shared<const DisplayState>(s));
  CLog::Log(LOGINFO, "DISPLAYSTATE publish {} {} connected {} fd {} crtc {}", kind, Describe(s),
            s.connected, s.drmFd, s.crtcId);
}

// a failed read (the hdr_cap sysfs read throws) skips this publish and stays on W
void PublishWithCaps(DisplayState& s, const CAMLDisplay& display, const char* kind)
{
  struct SPublishing
  {
    SPublishing() { t_publishing = true; }
    ~SPublishing()
    {
      t_publishing = false;
      s_capsPending = false;
    }
  } publishing;
  try
  {
    FillLink(s, display);
    FillCaps(s, display);
    Store(s, kind);
    return;
  }
  catch (const std::exception& e)
  {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      CLog::Log(LOGWARNING, "DISPLAYSTATE caps read failed, not published: {}", e.what());
  }
  catch (...)
  {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      CLog::Log(LOGWARNING, "DISPLAYSTATE caps read failed, not published");
  }
}

template<typename F>
void ShadowCaps(const char* field, bool equal, F&& detail)
{
  if (equal)
    Count(field, MATCH);
  else
    CountDiff(field, DIFF, detail);
}

// the caps state to compare against, or null for a skip
std::shared_ptr<const DisplayState> CapsState(const char* field)
{
  const bool pending = s_capsPending.load();
  auto state = Connected();
  if (pending || !state)
  {
    Count(field, SKIPPED);
    return nullptr;
  }
  return state;
}

bool Active()
{
  return aml_displaystate_on() && !t_publishing;
}

std::string CapsString(const CHDRCapabilities& c)
{
  return StringUtils::Format("hdr10 {} hlg {} hdr10+ {} dv {} tvled {} playerled {}",
                             c.SupportsHDR10(), c.SupportsHLG(), c.SupportsHDR10Plus(),
                             static_cast<int>(c.SupportsDolbyVision()), c.SupportsDVTVLED(),
                             c.SupportsDVPlayerLED());
}

void ShadowBool(const char* field, bool today, bool DisplayState::*member)
{
  if (!Active())
    return;
  const auto state = CapsState(field);
  if (state)
    ShadowCaps(field, (*state).*member == today,
               [&]
               {
                 return StringUtils::Format("displaystate {} today {} ({})", (*state).*member,
                                            today, Describe(*state));
               });
}
} // namespace

std::shared_ptr<const DisplayState> Get()
{
  return s_state.load();
}

void PublishMode(const CAMLDisplay& display, const RESOLUTION_INFO& res, bool windowed)
{
  if (!aml_displaystate_on())
    return;
  DisplayState s = Current();
  bool held = false;
  const float refresh = display.aml_committed_refresh(res, held);
  FillMode(s, res, refresh, held, windowed);
  FillLink(s, display);
  Store(s, "mode");
  s_modePublishes.fetch_add(1);
  s_restored = false;
  s_inSet = false;
}

void PublishCaps(const CAMLDisplay& display)
{
  if (!aml_displaystate_on())
    return;
  DisplayState s = Current();
  PublishWithCaps(s, display, "caps");
}

void PublishSeed(const CAMLDisplay& display, const RESOLUTION_INFO* res, bool windowed)
{
  if (!aml_displaystate_on())
    return;
  DisplayState s;
  if (res)
  {
    // the boot set's held mode
    bool held = false;
    const float refresh = display.aml_committed_refresh(*res, held);
    FillMode(s, *res, refresh, held, windowed);
  }
  PublishWithCaps(s, display, "seed");
}

void CapsWriteBegins()
{
  if (aml_displaystate_on())
    s_capsPending = true;
}

void NoteCommit()
{
  if (aml_displaystate_on())
    s_commits.fetch_add(1);
}

uint64_t CommitCount()
{
  return s_commits.load();
}

void GcSetBegins()
{
  if (!aml_displaystate_on())
    return;
  s_setBeginPublishes = s_modePublishes.load();
  s_inSet = true;
}

void GcSetEnds(bool switched)
{
  if (!aml_displaystate_on())
    return;
  // the commit published, and GC goes back to the old mode without a new set
  if (!switched && s_modePublishes.load() != s_setBeginPublishes)
    s_restored = true;
  s_inSet = false;
}

void ShadowMainMode(const char* fpsField,
                    const char* heightField,
                    const RESOLUTION_INFO& gcRes,
                    float gcFps,
                    bool committed)
{
  if (!aml_displaystate_on())
    return;
  const auto state = Connected();
  const bool frac = state && FracHeldAtMain(*state, gcRes);
  if (!state || state->strId != gcRes.strId || (state->refresh != gcRes.fRefreshRate && !frac))
  {
    if (!committed || s_restored.load())
    {
      Count(fpsField, SKIPPED);
      Count(heightField, SKIPPED);
      return;
    }
    const auto detail = [&]
    {
      return StringUtils::Format("GC {} @ {:.3f}, displaystate {}", gcRes.strId, gcRes.fRefreshRate,
                                 state ? Describe(*state) : "none or disconnected");
    };
    CountDiff(fpsField, FAIL, detail);
    CountDiff(heightField, FAIL, detail);
    return;
  }
  if (state->fps != gcFps && frac)
    Count(fpsField, FRAC_HELD);
  else if (state->fps != gcFps)
    CountDiff(fpsField, DIFF,
              [&]
              {
                return StringUtils::Format("displaystate {:.6f} GC {:.6f} ({})", state->fps, gcFps,
                                           Describe(*state));
              });
  else
    Count(fpsField, MATCH);
  if (state->screenHeight != gcRes.iScreenHeight)
    CountDiff(heightField, DIFF,
              [&]
              {
                return StringUtils::Format("displaystate {} GC {} ({})", state->screenHeight,
                                           gcRes.iScreenHeight, Describe(*state));
              });
  else
    Count(heightField, MATCH);
}

void ShadowMainWindowed(const char* field, const RESOLUTION_INFO& gcRes, bool windowed)
{
  if (!aml_displaystate_on())
    return;
  const auto state = Connected();
  if (!state || state->strId != gcRes.strId ||
      (state->refresh != gcRes.fRefreshRate && !FracHeldAtMain(*state, gcRes)))
    Count(field, SKIPPED);
  else if (state->windowed != windowed)
    CountDiff(field, DIFF,
              [&]
              {
                return StringUtils::Format("displaystate {} GC {} ({})", state->windowed, windowed,
                                           Describe(*state));
              });
  else
    Count(field, MATCH);
}

ModeShadow BeginModeShadow()
{
  ModeShadow shadow;
  // the windows before the state: a publish stores the state before it ends them
  shadow.inSet = s_inSet.load();
  shadow.restored = s_restored.load();
  shadow.state = Connected();
  return shadow;
}

bool FracHeld(const ModeShadow& shadow, float todayFps)
{
  return shadow.state && shadow.state->fracHeld && todayFps == shadow.state->requestedFps &&
         FracPair(shadow.state->fps, todayFps);
}

void CountMode(
    const ModeShadow& shadow, const char* field, double today, double displayState, bool fracHeld)
{
  if (!shadow.state)
    Count(field, SKIPPED);
  else if (today == displayState)
    Count(field, MATCH);
  else if (shadow.inSet)
    Count(field, WINDOW_SET);
  else if (shadow.restored)
    Count(field, WINDOW_RESTORE);
  else if (fracHeld)
    Count(field, FRAC_HELD);
  else
    CountDiff(field, DIFF,
              [&]
              {
                return StringUtils::Format("displaystate {:.6f} today {:.6f} ({})", displayState,
                                           today, Describe(*shadow.state));
              });
}

void ShadowHdrCaps(const CHDRCapabilities& today)
{
  if (!Active())
    return;
  const auto state = CapsState("hdrcaps@caps");
  if (!state)
    return;
  const CHDRCapabilities& c = state->hdrCaps;
  const bool equal = c.SupportsHDR10() == today.SupportsHDR10() &&
                     c.SupportsHLG() == today.SupportsHLG() &&
                     c.SupportsHDR10Plus() == today.SupportsHDR10Plus() &&
                     c.SupportsDolbyVision() == today.SupportsDolbyVision() &&
                     c.SupportsDVTVLED() == today.SupportsDVTVLED() &&
                     c.SupportsDVPlayerLED() == today.SupportsDVPlayerLED();
  ShadowCaps("hdrcaps@caps", equal,
             [&]
             {
               return StringUtils::Format("displaystate {} today {} ({})", CapsString(c),
                                          CapsString(today), Describe(*state));
             });
}

void ShadowDv(bool today)
{
  if (!Active())
    return;
  const auto state = CapsState("dv@caps");
  if (!state)
    return;
  const bool dv = state->hdrCaps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
  ShadowCaps("dv@caps", dv == today,
             [&] {
               return StringUtils::Format("displaystate {} today {} ({})", dv, today,
                                          Describe(*state));
             });
}

void Shadow3d(bool today)
{
  ShadowBool("3d@caps", today, &DisplayState::support3d);
}

void ShadowHdrPq(bool today)
{
  ShadowBool("hdr-pq@sysfs", today, &DisplayState::hdrPq);
}

void ShadowHdrHlg(bool today)
{
  ShadowBool("hdr-hlg@sysfs", today, &DisplayState::hdrHlg);
}

void ShadowHdr10Plus(bool today)
{
  ShadowBool("hdr10plus@sysfs", today, &DisplayState::hdr10Plus);
}

void ShadowHas2160(bool today)
{
  ShadowBool("has2160@bd-uhd", today, &DisplayState::has2160);
}

void ShadowDrm(int fd, uint32_t crtc)
{
  if (!Active())
    return;
  const auto state = CapsState("drm@frame-latency");
  if (state)
    ShadowCaps("drm@frame-latency", state->drmFd == fd && state->crtcId == crtc,
               [&]
               {
                 return StringUtils::Format("displaystate fd {} crtc {} today fd {} crtc {} ({})",
                                            state->drmFd, state->crtcId, fd, crtc,
                                            Describe(*state));
               });
}

namespace
{
std::atomic<int> s_guiStereoMode{-1}; // -1: not published
} // namespace

void PublishGuiInputs(const GuiInputs& inputs)
{
  if (aml_displaystate_on())
    s_guiStereoMode.store(static_cast<int>(inputs.stereoMode), std::memory_order_release);
}

RenderStereoMode GuiStereoMode(RenderStereoMode today)
{
  if (!aml_displaystate_on())
    return today;
  const int published = s_guiStereoMode.load(std::memory_order_acquire);
  if (published < 0)
  {
    Count("stereo@ovc-guiinputs", SKIPPED);
    return today;
  }
  if (published == static_cast<int>(today))
    Count("stereo@ovc-guiinputs", MATCH);
  else
    CountDiff("stereo@ovc-guiinputs", DIFF,
              [&] {
                return StringUtils::Format("guiinputs {} today {}", published,
                                           static_cast<int>(today));
              });
  return static_cast<RenderStereoMode>(published);
}

void Tick()
{
  if (!aml_displaystate_on())
    return;
  std::unique_lock lock(s_logMutex);
  const int count = s_fieldCount.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i)
  {
    SCounter& c = s_counters[i];
    const uint32_t fresh = c.ready.load(std::memory_order_acquire) & ~c.drained;
    for (int slot = 0; fresh && slot < MAX_DETAILS; ++slot)
    {
      if (fresh & (1u << slot))
        CLog::Log(LOGWARNING, "DISPLAYSTATE {} {}{}", c.field, c.details[slot],
                  slot == MAX_DETAILS - 1 ? " (further ones counted only)" : "");
    }
    c.drained |= fresh;
  }
  lock.unlock();
  const int64_t now = NowMs();
  if (now - s_lastLogMs >= 10000)
    LogCounters();
}

void LogCounters()
{
  if (!aml_displaystate_on())
    return;
  std::unique_lock lock(s_logMutex);
  s_lastLogMs = NowMs();
  const int count = s_fieldCount.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i)
  {
    SCounter& c = s_counters[i];
    std::array<uint64_t, COLUMNS> n;
    for (int k = 0; k < COLUMNS; ++k)
      n[k] = c.n[k].load(std::memory_order_relaxed);
    if (n == c.logged)
      continue;
    c.logged = n;
    CLog::Log(LOGINFO,
              "DISPLAYSTATE {} match {} diff {} skipped {} window-set {} window-restore {} "
              "frac-held {} fail {}",
              c.field, n[MATCH], n[DIFF], n[SKIPPED], n[WINDOW_SET], n[WINDOW_RESTORE],
              n[FRAC_HELD], n[FAIL]);
  }
}

} // namespace DISPLAYSTATE
