/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsTick.h"

#include "filesystem/File.h"
#include "utils/StringUtils.h"
#include "windowing/amlogic/GraphicsRegime.h"
#include "utils/log.h"

#include <algorithm>

#include <atomic>
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
  if (dropped == 1 || dropped % 100 == 0)
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

} // namespace GRAPHICSTICK
