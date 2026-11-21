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
#include "utils/log.h"

#include <algorithm>

#include <atomic>
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
  std::unique_lock lock(s_mutex);
  return s_on;
}

uint64_t RouteSession()
{
  std::unique_lock lock(s_mutex);
  return s_session;
}

void EndRoute(const char* why)
{
  if (!Requested())
    return;
  bool log = false;
  {
    std::unique_lock lock(s_mutex);
    if (!s_endedWhy)
      s_endedWhy = why;
    if (s_on)
    {
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
    if (!s_on)
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

void NoteProcessed(uint64_t serial)
{
  s_processed.store(serial, std::memory_order_relaxed);
}

uint64_t Processed()
{
  return s_processed.load(std::memory_order_relaxed);
}

} // namespace GRAPHICSTICK
