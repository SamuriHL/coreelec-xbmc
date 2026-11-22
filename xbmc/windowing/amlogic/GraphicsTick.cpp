/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsTick.h"

#include "cores/VideoPlayer/DVDOverlayContainer.h"
#include "filesystem/File.h"
#include "utils/StringUtils.h"
#include "windowing/amlogic/GraphicsRegime.h"
#include "utils/log.h"

#include <algorithm>

#include <atomic>
#include <deque>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace GRAPHICSTICK
{
namespace
{
// G drains this every pass; more means G is stalled, and the oldest can no longer be shown
constexpr size_t INBOX_CAP = 64;

std::mutex s_mutex;
bool s_on = false;
uint64_t s_session = 0;
std::string s_logged; //!< the last GTICK line's state and why
const char* s_endedWhy = nullptr;
std::vector<SQueued> s_queued;
size_t s_dropped = 0;
bool s_released = false;
SReleased s_last;
SFloor s_floor;
std::function<void()> s_wake;
std::atomic<uint64_t> s_processed{0};
SSide s_gSide;

// GT-5, the untimed channel. s_untimedMutex is a leaf (taken under the container's lock);
// s_containerMutex is taken before the container's lock (WithContainer), never under it
std::mutex s_untimedMutex;
std::atomic<bool> s_hook{false};
uint64_t s_u = 0;
bool s_hasPending = false;
SUntimedState s_pending;
std::deque<SUntimedState> s_queue; //!< gnewest_off
bool s_haveLast = false;
std::vector<const CDVDOverlay*> s_lastCurrent; //!< the last posted state, to know a repost
std::vector<const CDVDOverlay*> s_lastWaiting;
unsigned int s_lastWaitingGen = 0;
std::atomic<int64_t> s_keepAlive{0};
std::atomic<bool> s_untimedHeld{false};
std::atomic<bool> s_cAdvancing{true};
std::atomic<uint64_t> s_graphicsLost{0};
std::atomic<uint64_t> s_posts{0};
// gnewest_off: each queued state holds its post's image references; BD-J posts every ~11 ms
constexpr size_t NEWEST_QUEUE_CAP = 256;
std::atomic<uint64_t> s_untimedCommitted{0};
std::mutex s_containerMutex;
std::atomic<CDVDOverlayContainer*> s_container{nullptr};
std::atomic<int> s_counts[static_cast<int>(UntimedCount::COUNT)];

std::vector<const CDVDOverlay*> Ids(const std::vector<std::shared_ptr<CDVDOverlay>>& images)
{
  std::vector<const CDVDOverlay*> ids;
  ids.reserve(images.size());
  for (const auto& image : images)
    ids.push_back(image.get());
  return ids;
}

bool Exists(const char* file)
{
  return XFILE::CFile::Exists(std::string("special://profile/") + file);
}

void Wake()
{
  std::function<void()> wake;
  {
    std::unique_lock lock(s_mutex);
    wake = s_wake;
  }
  if (wake)
    wake();
}
} // namespace

bool Requested()
{
  static const bool on = Exists("graphicstick_on");
  return on;
}

bool TestDelay1()
{
  static const bool on = Requested() && Exists("gt_delay1");
  return on;
}

bool ReleasedLog()
{
  static const bool on = Requested() && Exists("gt_released_log");
  return on;
}

void SetRoute(bool on, const char* why)
{
  if (!Requested())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    if (on && s_endedWhy)
      on = false;
    if (on != s_on)
    {
      s_on = on;
      s_session++;
      if (!on)
        s_queued.clear();
      else
      {
        // B1: RM's overlay serial restarts with each player, so a floor or a processed serial
        // is its session's only (the gen moves on: G compares it, never the value)
        s_floor.floor = 0;
        s_floor.gen++;
        s_processed.store(0, std::memory_order_relaxed);
      }
    }
    const std::string state =
        StringUtils::Format("{} ({})", on ? "on" : "off", on ? why : (s_endedWhy ? s_endedWhy : why));
    if (state != s_logged)
    {
      s_logged = state;
      line = "GTICK " + state;
    }
  }
  if (!line.empty())
    CLog::Log(on ? LOGINFO : LOGWARNING, "{}", line);
  Wake();
}

bool RouteOn()
{
  if (!Requested())
    return false;
  std::unique_lock lock(s_mutex);
  return s_on;
}

uint64_t RouteSession()
{
  if (!Requested())
    return 0;
  std::unique_lock lock(s_mutex);
  return s_session;
}

void RouteState(bool& on, uint64_t& session)
{
  on = false;
  session = 0;
  if (!Requested())
    return;
  std::unique_lock lock(s_mutex);
  on = s_on;
  session = s_session;
}

void EndRoute(const char* why)
{
  if (!Requested())
    return;
  bool log = false;
  bool handBack = false;
  {
    std::unique_lock lock(s_mutex);
    if (!s_endedWhy)
      s_endedWhy = why;
    if (s_on)
    {
      handBack = std::strcmp(why, "stereo") == 0;
      s_on = false;
      s_session++;
      s_queued.clear();
    }
    const std::string state = StringUtils::Format("off ({})", s_endedWhy);
    if (state != s_logged)
    {
      s_logged = state;
      log = true;
    }
  }
  if (log)
    CLog::Log(LOGWARNING, "GTICK off ({})", why);
  // impl note §6 item 14: the class at the stereo hand-back, whichever thread ends the route
  if (handBack)
    DISPLAYSTATE::RequestRegimeRepublish("stereo hand-back");
  Wake();
}

void NewFile()
{
  std::unique_lock lock(s_mutex);
  s_endedWhy = nullptr;
}

const char* EndedWhy()
{
  std::unique_lock lock(s_mutex);
  return s_endedWhy;
}

void PostQueued(SQueued&& queued)
{
  size_t dropped = 0;
  {
    std::unique_lock lock(s_mutex);
    if (!s_on)
      return;
    if (s_queued.size() >= INBOX_CAP)
    {
      s_queued.erase(s_queued.begin());
      dropped = ++s_dropped;
    }
    s_queued.push_back(std::move(queued));
  }
  // GT-D15: only on a drop; 0 % 100 logged a WARNING at every post, a log write per picture on V
  if (dropped > 0 && (dropped == 1 || dropped % 100 == 0))
    CLog::Log(LOGWARNING, "GTICK inbox full: {} pictures dropped unread (G stalled)", dropped);
  Wake();
}

void PostFloor(uint64_t floor, const char* why)
{
  {
    std::unique_lock lock(s_mutex);
    // M-1: a floor that has not moved (main's late repeat of P's) clears nothing new
    if (!s_on || floor <= s_floor.floor)
      return;
    s_floor.floor = std::max(s_floor.floor, floor);
    s_floor.gen++;
  }
  CLog::Log(LOGINFO, "GTICK floor {} ({})", floor, why);
  Wake();
}

SFloor Floor()
{
  std::unique_lock lock(s_mutex);
  return s_floor;
}

void PostReleased(const SReleased& released)
{
  {
    std::unique_lock lock(s_mutex);
    if (!s_on)
      return;
    s_released = true;
    s_last = released;
  }
  Wake();
}

void SetWake(std::function<void()> wake)
{
  std::unique_lock lock(s_mutex);
  s_wake = std::move(wake);
}

void Take(SInbox& inbox)
{
  std::unique_lock lock(s_mutex);
  inbox.queued = std::exchange(s_queued, {});
  inbox.released = std::exchange(s_released, false);
  inbox.last = s_last;
  inbox.floor = s_floor;
  inbox.session = s_session;
  inbox.on = s_on;
}

void NoteGSide(uint64_t session, bool on)
{
  {
    std::unique_lock lock(s_mutex);
    s_gSide = {session, on};
  }
}

SSide GSide()
{
  std::unique_lock lock(s_mutex);
  return s_gSide;
}

void Kick()
{
  Wake();
}

void NoteProcessed(uint64_t serial)
{
  s_processed.store(serial, std::memory_order_relaxed);
}

uint64_t Processed()
{
  return s_processed.load(std::memory_order_relaxed);
}

bool UntimedOff()
{
  static const bool off = Requested() && Exists("guntimed_off");
  return off;
}

bool NewestOff()
{
  static const bool off = Requested() && Exists("gnewest_off");
  return off;
}

bool UntimedHook()
{
  return s_hook.load(std::memory_order_acquire);
}

void RegisterContainer(CDVDOverlayContainer* container)
{
  if (Requested())
    s_container.store(container, std::memory_order_release);
}

void ContainerGone(CDVDOverlayContainer* container)
{
  if (!Requested())
    return;
  // waits for a WithContainer call in progress on it
  std::unique_lock lock(s_containerMutex);
  CDVDOverlayContainer* expected = container;
  s_container.compare_exchange_strong(expected, nullptr);
}

bool WithContainer(const std::function<void(CDVDOverlayContainer&)>& fn)
{
  std::unique_lock lock(s_containerMutex);
  CDVDOverlayContainer* container = s_container.load(std::memory_order_acquire);
  if (!container)
    return false;
  fn(*container);
  return true;
}

void PostUntimed(SUntimedState&& state)
{
  if (!UntimedHook())
    return;
  s_posts.fetch_add(1, std::memory_order_relaxed);
  {
    std::unique_lock lock(s_untimedMutex);
    s_keepAlive.store(state.keepAliveTick, std::memory_order_relaxed);
    std::vector<const CDVDOverlay*> current = Ids(state.current);
    std::vector<const CDVDOverlay*> waiting = Ids(state.waiting);
    // BD-J re-posts the retained composition every few ms: the same images are no new state,
    // only its keep-alive (above). Nor is a page release whose snapshot G already has
    if (s_haveLast && current == s_lastCurrent && waiting == s_lastWaiting &&
        state.waitingGen == s_lastWaitingGen)
      return;
    s_haveLast = true;
    s_lastCurrent = std::move(current);
    s_lastWaiting = std::move(waiting);
    s_lastWaitingGen = state.waitingGen;
    state.u = ++s_u;
    if (NewestOff())
    {
      // review rA m1: bounded; the oldest goes, counted and logged, never silently
      if (s_queue.size() >= NEWEST_QUEUE_CAP)
      {
        s_queue.pop_front();
        if (s_counts[static_cast<int>(UntimedCount::NEWEST_OVERFLOW)]++ == 0)
          CLog::Log(LOGWARNING, "GUNTIMED gnewest_off queue full ({}): oldest states dropped",
                    NEWEST_QUEUE_CAP);
      }
      s_queue.push_back(std::move(state));
    }
    else
    {
      // item 5 (b) (i): the newest only; one G never took is skipped, judged on its successor
      if (s_hasPending)
      {
        state.skipped = std::move(s_pending.skipped);
        state.skipped.push_back({s_pending.u, s_pending.postNs, s_pending.current.empty()});
        s_counts[static_cast<int>(UntimedCount::SKIPPED)]++;
      }
      s_pending = std::move(state);
      s_hasPending = true;
    }
  }
  Wake();
}

void SetUntimedHook(bool on)
{
  {
    std::unique_lock lock(s_untimedMutex);
    s_haveLast = false;
    s_hasPending = false;
    s_pending = SUntimedState();
    s_queue.clear();
  }
  s_hook.store(on, std::memory_order_release);
}

bool TakeUntimed(SUntimedState& state, size_t& depth)
{
  std::unique_lock lock(s_untimedMutex);
  depth = s_queue.size();
  if (NewestOff())
  {
    if (s_queue.empty())
      return false;
    state = std::move(s_queue.front());
    s_queue.pop_front();
    return true;
  }
  if (!s_hasPending)
    return false;
  state = std::move(s_pending);
  s_pending = SUntimedState();
  s_hasPending = false;
  return true;
}

int64_t UntimedKeepAlive()
{
  return s_keepAlive.load(std::memory_order_relaxed);
}

uint64_t NextUntimedU()
{
  std::unique_lock lock(s_untimedMutex);
  return ++s_u;
}

void SetUntimedHeld(bool held)
{
  s_untimedHeld.store(held, std::memory_order_relaxed);
}

bool UntimedHeld()
{
  return s_untimedHeld.load(std::memory_order_relaxed);
}

void SetCAdvancing(bool advancing)
{
  if (!Requested())
    return;
  if (s_cAdvancing.exchange(advancing, std::memory_order_relaxed) != advancing)
    Wake();
}

bool CAdvancing()
{
  return s_cAdvancing.load(std::memory_order_relaxed);
}

void NoteUntimedCommitted(uint64_t u)
{
  uint64_t was = s_untimedCommitted.load(std::memory_order_relaxed);
  while (u > was && !s_untimedCommitted.compare_exchange_weak(was, u))
  {
  }
  Wake();
}

uint64_t UntimedCommitted()
{
  return s_untimedCommitted.load(std::memory_order_relaxed);
}

void Count(UntimedCount what, int n)
{
  s_counts[static_cast<int>(what)].fetch_add(n, std::memory_order_relaxed);
}

void CountMax(UntimedCount what, int value)
{
  std::atomic<int>& cell = s_counts[static_cast<int>(what)];
  int was = cell.load(std::memory_order_relaxed);
  while (value > was && !cell.compare_exchange_weak(was, value))
  {
  }
}

std::string UntimedCountsLine(bool reset, bool& any)
{
  int v[static_cast<int>(UntimedCount::COUNT)];
  any = false;
  for (int i = 0; i < static_cast<int>(UntimedCount::COUNT); i++)
  {
    v[i] = reset ? s_counts[i].exchange(0, std::memory_order_relaxed)
                 : s_counts[i].load(std::memory_order_relaxed);
    any |= v[i] != 0;
  }
  const auto at = [&v](UntimedCount what) { return v[static_cast<int>(what)]; };
  return StringUtils::Format(
      "states={} untimedSkipped={} gUnitDeferred={} gWaitBuffer={} gWaitBufferMax_ms={} "
      "pagesAhead={} pageLatePosted={} pageFallback={} keepAliveExpired={} gnewestDepth={} "
      "gnewestOverflow={}",
      at(UntimedCount::STATES), at(UntimedCount::SKIPPED), at(UntimedCount::UNIT_DEFERRED),
      at(UntimedCount::WAIT_BUFFER), at(UntimedCount::WAIT_BUFFER_MAX_MS),
      at(UntimedCount::FIRST_PAGES), at(UntimedCount::PAGE_LATE_POSTED),
      at(UntimedCount::PAGE_FALLBACK), at(UntimedCount::KEEPALIVE_EXPIRED),
      at(UntimedCount::NEWEST_DEPTH_MAX), at(UntimedCount::NEWEST_OVERFLOW));
}

void NoteGraphicsLost()
{
  if (!Requested())
    return;
  s_graphicsLost.fetch_add(1, std::memory_order_relaxed);
  Wake();
}

uint64_t GraphicsLost()
{
  return s_graphicsLost.load(std::memory_order_relaxed);
}

uint64_t UntimedPosts()
{
  return s_posts.load(std::memory_order_relaxed);
}

} // namespace GRAPHICSTICK
