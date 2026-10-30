/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DisplayState.h"

#include "AMLDisplay.h"
#include "OutputWorker.h"
#include "ServiceBroker.h"
#include "ThreadRole.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "settings/DisplaySettings.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <utility>

namespace DISPLAYSTATE
{
namespace
{
std::atomic<std::shared_ptr<const DisplayState>> s_state;
uint64_t s_epoch = 0; // publishers are never concurrent
uint64_t s_capsEpoch = 0;

std::atomic<uint64_t> s_modePublishes{0};
std::atomic<uint64_t> s_commits{0};
// ModeTiming packed (5.B2-3): fps bits 0-31, height 32-47, windowed 48, frac-held 49, a mode 61,
// connected 62, published 63
std::atomic<uint64_t> s_timing{0};
constexpr uint64_t TIMING_FRAC_HELD = 1ull << 49;
constexpr uint64_t TIMING_MODE = 1ull << 61;
constexpr uint64_t TIMING_CONNECTED = 1ull << 62;
constexpr uint64_t TIMING_PUBLISHED = 1ull << 63;
// caps writes begun, and the last one a caps publish read after: pending while they differ
std::atomic<uint64_t> s_capsGen{0};
std::atomic<uint64_t> s_capsDoneGen{0};
// the last caps publish failed: the state holds older caps until a good one
std::atomic<bool> s_capsFailed{false};
std::atomic<bool> s_inSet{false};
std::atomic<bool> s_restored{false};
uint64_t s_setBeginPublishes = 0; // main

// the publisher's own accessor reads are not compares
thread_local bool t_publishing = false;
// why a reader keeps today's read (R10)
enum Cause
{
  NOT_PUBLISHED,
  DISCONNECTED,
  CAPS_PENDING,
  CAPS_FAILED,
  CAUSES
};

// the state the last accessor shadow on this thread compared with; null on a skip, for t_cause
thread_local std::shared_ptr<const DisplayState> t_compared;
thread_local Cause t_cause = NOT_PUBLISHED;

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
  // 5.B3-3, only under hotplugworker (latched before W's seed): off takes no DRM lock here
  if (OUTPUT::HotplugWorkerOn())
    display.aml_get_display_ui_plane(s.primaryPlaneId, s.uiSrcW, s.uiSrcH, s.uiScreenW,
                                     s.uiScreenH, s.overlayActive);
}

void FillCaps(DisplayState& s, const CAMLDisplay& display, std::optional<bool> has2160)
{
  s.hdrCaps = display.GetHDRCaps();
  s.support3d = display.aml_display_support_3d();
  s.hdrPq = aml_display_support_hdr_pq();
  s.hdrHlg = aml_display_support_hdr_hlg();
  s.hdr10Plus = aml_display_support_hdr10plus();
  s.capsEpoch = ++s_capsEpoch;
  if (has2160)
  {
    s.has2160 = *has2160;
    return;
  }
  // 5.B3-7b (H4): W's hotplug job passes its own table's value
  if (OUTPUT::HotplugJobsOnW())
    MAINCOUPLING("DisplaySettings FillCaps table");
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
}

void Store(DisplayState& s, const char* kind)
{
  s.epoch = ++s_epoch;
  s_state.store(std::make_shared<const DisplayState>(s));
  uint32_t fpsBits = 0;
  std::memcpy(&fpsBits, &s.fps, sizeof(fpsBits));
  const uint64_t height = static_cast<uint64_t>(std::clamp(s.screenHeight, 0, 0xffff));
  // a seed with no committed mode has no mode fields
  const bool mode = s.refresh > 0 || !s.strId.empty();
  s_timing.store(TIMING_PUBLISHED | (s.connected ? TIMING_CONNECTED : 0) |
                     (mode ? TIMING_MODE : 0) | (s.fracHeld ? TIMING_FRAC_HELD : 0) |
                     (s.windowed ? 1ull << 48 : 0) | height << 32 | fpsBits,
                 std::memory_order_release);
  CLog::Log(LOGINFO, "DISPLAYSTATE publish {} {}{} connected {} fd {} crtc {}", kind, Describe(s),
            s.fracHeld ? " frac-held" : "", s.connected, s.drmFd, s.crtcId);
}

// a failed read (the hdr_cap sysfs read throws) skips this publish and stays on W; the caps
// readers keep today's read until a caps publish succeeds
void PublishWithCaps(DisplayState& s,
                     const CAMLDisplay& display,
                     const char* kind,
                     std::optional<bool> has2160 = std::nullopt)
{
  // a write begun after this load stays pending for its own publish
  const uint64_t gen = s_capsGen.load();
  struct SPublishing
  {
    SPublishing() { t_publishing = true; }
    ~SPublishing() { t_publishing = false; }
  } publishing;
  std::string error;
  try
  {
    FillLink(s, display);
    FillCaps(s, display, has2160);
    Store(s, kind);
    s_capsFailed = false;
    s_capsDoneGen = gen;
    return;
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "unknown error";
  }
  if (!s_capsFailed.exchange(true))
    CLog::Log(LOGWARNING, "DISPLAYSTATE caps read failed, not published: {}", error);
  s_capsDoneGen = gen;
}

template<typename F>
void ShadowCaps(const char* field, bool equal, F&& detail)
{
  if (equal)
    Count(field, MATCH);
  else
    CountDiff(field, DIFF, detail);
}

// the caps state to compare against, or null for a skip, with its cause
std::shared_ptr<const DisplayState> CapsState(const char* field, Cause* cause = nullptr)
{
  // doneGen before failed: a publish stores failed (and the state) before doneGen
  const bool pending = s_capsGen.load() != s_capsDoneGen.load();
  const bool failed = s_capsFailed.load();
  auto state = s_state.load();
  Cause skip;
  if (failed)
    skip = CAPS_FAILED;
  else if (pending)
    skip = CAPS_PENDING;
  else if (!state)
    skip = NOT_PUBLISHED;
  else if (!state->connected)
    skip = DISCONNECTED;
  else
    return state;
  Count(field, SKIPPED);
  if (cause)
    *cause = skip;
  return nullptr;
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

//! the state compared with, null on a skip
std::shared_ptr<const DisplayState> ShadowBool(const char* field,
                                               bool today,
                                               bool DisplayState::*member,
                                               Cause* cause = nullptr)
{
  if (!Active())
    return nullptr;
  auto state = CapsState(field, cause);
  if (state)
    ShadowCaps(field, (*state).*member == today,
               [&]
               {
                 return StringUtils::Format("displaystate {} today {} ({})", (*state).*member,
                                            today, Describe(*state));
               });
  return state;
}

// R10: a reader keeps today's read; logged once per cause and published state
void Fallback(const char* field, Cause cause)
{
  static constexpr const char* causes[CAUSES] = {"before W's first publish", "not connected",
                                                 "main writing the caps", "caps read failed"};
  static std::array<std::atomic<uint64_t>, CAUSES> logged{};
  const auto state = s_state.load();
  const uint64_t key = (state ? state->epoch : 0) + 1;
  if (logged[cause].exchange(key) != key)
    CLog::Log(LOGINFO, "DISPLAYSTATE {} reads today's value: {}", field, causes[cause]);
}

//! the state the accessor's shadow compared with, for the value just read
std::shared_ptr<const DisplayState> TakeCompared(const char* field)
{
  std::shared_ptr<const DisplayState> state;
  state.swap(t_compared);
  if (!state)
    Fallback(field, t_cause);
  return state;
}

bool PickBool(const char* field, bool today, bool DisplayState::*member)
{
  if (!Active())
    return today;
  Cause cause = NOT_PUBLISHED;
  const auto state = ShadowBool(field, today, member, &cause);
  if (!state)
  {
    Fallback(field, cause);
    return today;
  }
  return (*state).*member;
}
} // namespace

std::shared_ptr<const DisplayState> Get()
{
  return s_state.load();
}

std::shared_ptr<const DisplayState> LinkState()
{
  if (!OUTPUT::HotplugWorkerOn())
    return nullptr;
  auto state = s_state.load();
  // no plane fields: published before the latch (the boot set) or after a failed re-init; HEAD's
  // live path is the answer there
  if (state && !state->primaryPlaneId)
    return nullptr;
  return state;
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

void PublishCaps(const CAMLDisplay& display,
                 std::optional<bool> has2160,
                 const ModeFacts* desktopMode)
{
  if (!aml_displaystate_on())
    return;
  DisplayState s = Current();
  if (desktopMode)
    s.desktopMode = *desktopMode;
  PublishWithCaps(s, display, "caps", has2160);
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
    s_capsGen.fetch_add(1);
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

bool CommitTimingOn()
{
  static const bool on = []
  {
    const bool result = OUTPUT::OutputDriveOn() && !OUTPUT::OutputDriveSkip("B2-3");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-3: timing from the committed mode");
    return result;
  }();
  return on;
}

bool CommittedTiming(ModeTiming& timing, const char* field)
{
  const uint64_t word = s_timing.load(std::memory_order_acquire);
  if (!(word & TIMING_CONNECTED) || !(word & TIMING_MODE))
  {
    if (field)
      Fallback(field, word & TIMING_MODE ? DISCONNECTED : NOT_PUBLISHED);
    return false;
  }
  const uint32_t fpsBits = static_cast<uint32_t>(word);
  std::memcpy(&timing.fps, &fpsBits, sizeof(fpsBits));
  timing.screenHeight = static_cast<int>((word >> 32) & 0xffff);
  timing.windowed = (word >> 48) & 1;
  timing.todaysTweak = (word & TIMING_FRAC_HELD) || s_restored.load();
  return true;
}

bool Restored()
{
  return s_restored.load();
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

namespace
{
int s_gcSetDepth = 0; // main
} // namespace

CGcSetScope::CGcSetScope()
{
  ++s_gcSetDepth;
}

CGcSetScope::~CGcSetScope()
{
  --s_gcSetDepth;
}

int GcSetDepth()
{
  return s_gcSetDepth;
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
  if (!aml_displaystate_on())
    return;
  t_compared = nullptr;
  if (!Active())
    return;
  const auto state = CapsState("hdrcaps@caps", &t_cause);
  if (!state)
    return;
  t_compared = state;
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
  if (!aml_displaystate_on())
    return;
  t_compared = nullptr;
  if (!Active())
    return;
  const auto state = CapsState("dv@caps", &t_cause);
  if (!state)
    return;
  t_compared = state;
  const bool dv = state->hdrCaps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
  ShadowCaps("dv@caps", dv == today,
             [&] {
               return StringUtils::Format("displaystate {} today {} ({})", dv, today,
                                          Describe(*state));
             });
}

void Shadow3d(bool today)
{
  if (aml_displaystate_on())
    t_compared = ShadowBool("3d@caps", today, &DisplayState::support3d, &t_cause);
}

CHDRCapabilities HdrCaps(const CHDRCapabilities& today)
{
  if (!Active())
    return today;
  const auto state = TakeCompared("hdrcaps@caps");
  return state ? state->hdrCaps : today;
}

bool HdrDisplay(bool today)
{
  if (!Active())
    return today;
  const auto state = TakeCompared("hdrcaps@caps");
  if (!state)
    return today;
  const CHDRCapabilities& c = state->hdrCaps;
  return c.SupportsHDR10() || c.SupportsHDR10Plus() || c.SupportsHLG() ||
         c.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
}

bool Dv(bool today)
{
  if (!Active())
    return today;
  const auto state = TakeCompared("dv@caps");
  return state ? state->hdrCaps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE
               : today;
}

bool Support3d(bool today)
{
  if (!Active())
    return today;
  const auto state = TakeCompared("3d@caps");
  return state ? state->support3d : today;
}

bool HdrPq(bool today)
{
  return PickBool("hdr-pq@sysfs", today, &DisplayState::hdrPq);
}

bool HdrHlg(bool today)
{
  return PickBool("hdr-hlg@sysfs", today, &DisplayState::hdrHlg);
}

bool Hdr10Plus(bool today)
{
  return PickBool("hdr10plus@sysfs", today, &DisplayState::hdr10Plus);
}

bool Has2160(bool today)
{
  return PickBool("has2160@bd-uhd", today, &DisplayState::has2160);
}

void DrmIds(int& fd, uint32_t& crtc)
{
  if (!Active())
    return;
  Cause cause = NOT_PUBLISHED;
  const auto state = CapsState("drm@frame-latency", &cause);
  if (!state)
  {
    Fallback("drm@frame-latency", cause);
    return;
  }
  ShadowCaps("drm@frame-latency", state->drmFd == fd && state->crtcId == crtc,
             [&]
             {
               return StringUtils::Format("displaystate fd {} crtc {} today fd {} crtc {} ({})",
                                          state->drmFd, state->crtcId, fd, crtc,
                                          Describe(*state));
             });
  fd = state->drmFd;
  crtc = state->crtcId;
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

std::string FactsString(const ModeFacts& f)
{
  return StringUtils::Format("{} {}x{} @ {:.3f} flags {:#x}", f.strId, f.screenWidth,
                             f.screenHeight, f.refresh, f.dwFlags);
}

ModeFacts FactsOf(const RESOLUTION_INFO& info)
{
  return {info.strId, info.iScreenWidth, info.iScreenHeight, info.fRefreshRate, info.dwFlags};
}

namespace
{
// 5.B3-3's GuiInputs fields. Under it: CDisplaySettings' lock (PublishGuiResolution, so two
// threads' publishes store in order) and CLog; never a DisplayState or W lock
std::mutex s_guiInputsMutex;
GuiInputs s_guiInputs;
bool s_videoRowHeld = false; //!< D5's hold is on (its line logged once per hold)
bool s_guiRowHeld = false; //!< B3-8's hold of guiResolution is on (logged once per hold)

std::optional<ModeFacts> CurrentGuiResolution()
{
  const CDisplaySettings& displaySettings = CDisplaySettings::GetInstance();
  const RESOLUTION res = displaySettings.GetCurrentResolution();
  if (res <= RES_DESKTOP)
    return std::nullopt;
  return FactsOf(displaySettings.GetResolutionInfo(res));
}

ModeFacts CurrentVideoResolution()
{
  return FactsOf(CDisplaySettings::GetInstance().GetResolutionInfo(
      CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution()));
}

// 5.B3-7b (D5): GC's video row is no row of main's table at RES_INVALID (before GC's first set)
// and while a hotplug reply's table step is cut short by a throw (ClearCustomResolutions done,
// the probe's rows not added) until the retry's reply refills it; GetResolutionInfo then returns
// a default RESOLUTION_INFO, no mode
bool RowInTable(RESOLUTION res)
{
  return res > RES_INVALID &&
         static_cast<size_t>(res) < CDisplaySettings::GetInstance().ResolutionInfoSize();
}

bool VideoPlaying()
{
  const auto appPlayer = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  return appPlayer && appPlayer->IsPlayingVideo();
}

// s_guiInputsMutex held
void LogGuiInputs(const char* why)
{
  const GuiInputs& g = s_guiInputs;
  CLog::Log(LOGINFO,
            "GUIINPUTS guires {} nativeGui {} videoPlaying {} videoResolution {} ({})",
            g.guiResolution ? FactsString(*g.guiResolution) : "desktop", g.nativeGui,
            g.videoPlaying, FactsString(g.videoResolution), why);
}
} // namespace

void PublishGuiInputsAtStart(bool nativeGui)
{
  if (!OUTPUT::HotplugWorkerOn())
    return;
  // main only for these two; GC's and the player's locks are not taken under the mutex
  const ModeFacts videoResolution = CurrentVideoResolution();
  const bool playing = VideoPlaying();
  std::unique_lock lock(s_guiInputsMutex);
  s_guiInputs.guiResolution = CurrentGuiResolution();
  s_guiInputs.nativeGui = nativeGui;
  s_guiInputs.videoPlaying = playing;
  s_guiInputs.videoResolution = videoResolution;
  s_guiInputs.published = true;
  LogGuiInputs("start");
}

void PublishGuiResolution()
{
  if (!OUTPUT::HotplugWorkerOn())
    return;
  // read under the lock: a settings thread's publish and main's cannot store out of order
  std::unique_lock lock(s_guiInputsMutex);
  if (!s_guiInputs.published)
    return;
  // 5.B3-8 (D5's twin): W's no-player Restore reads these facts. A setting above RES_DESKTOP whose
  // row is not in main's table (a table step cut short by a throw until the retry refills it, or
  // a shorter table after a reconnect: main does not re-derive its index) names no mode, so the
  // published facts stand
  if (OUTPUT::RestoreRow2On())
  {
    const RESOLUTION res = CDisplaySettings::GetInstance().GetCurrentResolution();
    if (res > RES_DESKTOP && !RowInTable(res))
    {
      if (!std::exchange(s_guiRowHeld, true))
        CLog::Log(LOGINFO, "GUIINPUTS guires held at {}: the setting's row {} is not in main's table",
                  s_guiInputs.guiResolution ? FactsString(*s_guiInputs.guiResolution) : "desktop",
                  static_cast<int>(res));
      return;
    }
    s_guiRowHeld = false;
  }
  const auto guiResolution = CurrentGuiResolution();
  if (s_guiInputs.guiResolution == guiResolution)
    return;
  s_guiInputs.guiResolution = guiResolution;
  LogGuiInputs("resolution");
}

void PublishNativeGui(bool nativeGui)
{
  if (!OUTPUT::HotplugWorkerOn())
    return;
  std::unique_lock lock(s_guiInputsMutex);
  if (!s_guiInputs.published || s_guiInputs.nativeGui == nativeGui)
    return;
  s_guiInputs.nativeGui = nativeGui;
  LogGuiInputs("gui scaling");
}

void PublishGuiPlayback()
{
  if (!OUTPUT::HotplugWorkerOn())
    return;
  const bool playing = VideoPlaying();
  const RESOLUTION videoRow = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution();
  ModeFacts videoResolution = FactsOf(CDisplaySettings::GetInstance().GetResolutionInfo(videoRow));
  // under the move only (W's video mode reads it): GC is still on its row, the table under it is
  // short, so the row's published facts stand
  const bool rowInTable = !OUTPUT::HotplugOnW() || RowInTable(videoRow);
  std::unique_lock lock(s_guiInputsMutex);
  if (!s_guiInputs.published)
    return;
  if (rowInTable)
    s_videoRowHeld = false;
  else if (videoResolution != s_guiInputs.videoResolution)
  {
    if (!std::exchange(s_videoRowHeld, true))
      CLog::Log(LOGINFO, "GUIINPUTS videoResolution held at {}: GC's row {} is not in main's table",
                FactsString(s_guiInputs.videoResolution), static_cast<int>(videoRow));
    videoResolution = s_guiInputs.videoResolution;
  }
  if (s_guiInputs.videoPlaying == playing && s_guiInputs.videoResolution == videoResolution)
    return;
  s_guiInputs.videoPlaying = playing;
  s_guiInputs.videoResolution = videoResolution;
  LogGuiInputs("playback");
}

GuiInputs GetGuiInputs()
{
  GuiInputs inputs;
  {
    std::unique_lock lock(s_guiInputsMutex);
    inputs = s_guiInputs;
  }
  const int stereo = s_guiStereoMode.load(std::memory_order_acquire);
  if (stereo >= 0)
    inputs.stereoMode = static_cast<RenderStereoMode>(stereo);
  return inputs;
}

void ShadowUiIds(uint32_t planeId, uint32_t crtcId, int srcW, int srcH, int screenW, int screenH)
{
  if (!OUTPUT::HotplugWorkerOn() || !Active())
    return;
  static constexpr const char* field = "ui-ids";
  // the windows before the state, as BeginModeShadow; a re-init not yet published is a skip,
  // as the caps readers'
  const ModeShadow shadow = BeginModeShadow();
  if (!shadow.state || s_capsGen.load() != s_capsDoneGen.load())
  {
    Count(field, SKIPPED);
    return;
  }
  const DisplayState& s = *shadow.state;
  if (s.primaryPlaneId == planeId && s.crtcId == crtcId && s.uiSrcW == srcW &&
      s.uiSrcH == srcH && s.uiScreenW == screenW && s.uiScreenH == screenH)
    Count(field, MATCH);
  else if (shadow.inSet)
    Count(field, WINDOW_SET);
  else if (shadow.restored)
    Count(field, WINDOW_RESTORE);
  else
    CountDiff(field, DIFF,
              [&]
              {
                return StringUtils::Format(
                    "displaystate plane {} crtc {} src {}x{} screen {}x{} flip plane {} crtc {} "
                    "src {}x{} screen {}x{} ({})",
                    s.primaryPlaneId, s.crtcId, s.uiSrcW, s.uiSrcH, s.uiScreenW, s.uiScreenH,
                    planeId, crtcId, srcW, srcH, screenW, screenH, Describe(s));
              });
}

namespace
{
// W's posts (a leaf lock: nothing is taken under it)
std::mutex s_postMutex;
uint64_t s_postedEpoch = 0;
uint64_t s_postedPublishes = 0;
unsigned int s_postedWriters = 0;
std::atomic<uint64_t> s_dvPublishes{0};
// main only
uint64_t s_handledPublishes = 0;
uint64_t s_reencodeRequest = 0;

std::string WriterNames(unsigned int writers)
{
  static constexpr std::pair<unsigned int, const char*> names[] = {
      {DV_WRITER_VS10, "vs10"},
      {DV_WRITER_VS10_FOLLOW, "vs10_follow"},
      {DV_WRITER_DV_APPLY, "dv_apply"},
      {DV_WRITER_TEARDOWN, "teardown_pre"},
  };
  std::string result;
  for (const auto& [bit, name] : names)
    if (writers & bit)
      result += (result.empty() ? "" : ",") + std::string(name);
  return result.empty() ? "none" : result;
}
} // namespace

bool DvModePublishOn()
{
  if (!OUTPUT::ModeSetsOnW())
    return false;
  static const bool on = []
  {
    const bool result = !OUTPUT::OutputDriveSkip("B2-8");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-8: a DV-mode publish posts OutputChanged; main requests "
                         "RM's re-encode");
    return result;
  }();
  return on;
}

void PublishDvMode(unsigned int oldMode, unsigned int mode, DvWriter writer)
{
  if (!DvModePublishOn())
    return;
  // the publishers' epoch is W's alone
  if (!OUTPUT::COutputWorker::Get().OnWorker())
  {
    static std::atomic_bool logged{false};
    if (!logged.exchange(true))
      CLog::Log(LOGWARNING, "OUTPUTDRIVE B2-8 dvmode publish off W (writer={}): not posted",
                WriterNames(writer));
    return;
  }
  uint64_t epoch = 0;
  if (s_state.load())
  {
    DisplayState s = Current();
    Store(s, "dvmode");
    epoch = s.epoch;
  }
  const uint64_t publishes = s_dvPublishes.fetch_add(1) + 1;
  {
    std::unique_lock lock(s_postMutex);
    s_postedEpoch = epoch;
    s_postedPublishes = publishes;
    s_postedWriters |= writer;
  }
  CLog::Log(LOGINFO, "OUTPUTDRIVE B2-8 dvmode publish epoch={} writer={} mode={}->{} posted", epoch,
            WriterNames(writer), oldMode, mode);
}

void HandleOutputChanged()
{
  if (s_dvPublishes.load() == s_handledPublishes)
    return;
  uint64_t epoch;
  uint64_t publishes;
  unsigned int writers;
  {
    std::unique_lock lock(s_postMutex);
    epoch = s_postedEpoch;
    publishes = s_postedPublishes;
    writers = std::exchange(s_postedWriters, 0u);
  }
  if (publishes == s_handledPublishes)
    return;
  const uint64_t count = publishes - s_handledPublishes;
  s_handledPublishes = publishes;
  const char* outcome;
  if (!(writers & (DV_WRITER_VS10 | DV_WRITER_VS10_FOLLOW)))
    outcome = "the decoder's next picture configures";
  else if (s_reencodeRequest)
    outcome = "coalesced";
  else
    outcome = "reconfigure requested";
  if (writers & (DV_WRITER_VS10 | DV_WRITER_VS10_FOLLOW))
    s_reencodeRequest = std::max<uint64_t>(epoch, 1);
  CLog::Log(LOGINFO, "OUTPUTDRIVE B2-8 OutputChanged epoch={} publishes={} writers={}: {}", epoch,
            count, WriterNames(writers), outcome);
}

uint64_t TakeReencodeRequest()
{
  return std::exchange(s_reencodeRequest, 0);
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
