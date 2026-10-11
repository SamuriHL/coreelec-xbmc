/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DiscTimeOwner.h"

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <algorithm>
#include <time.h>

#if defined(HAVE_LIBBLURAY)
extern "C"
{
#include <libbluray/bluray.h>
}
#endif

using namespace std::chrono_literals;

namespace
{
int64_t MonoNs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// CheckHeldStart's own cap is 12 s
constexpr auto HELD_CAP = 13s;
// the display-reset cap (delay + 3 s) counts from the mode set, which the
// player cannot see: allow for the mode set itself from OnLostDisplay
constexpr auto MODE_SET_ALLOWANCE = 10s;
} // namespace

void CDiscTimeOwner::Begin(bool enabled)
{
  const int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      "videoscreen.delayrefreshchange");
  std::unique_lock lock(m_lock);
  m_lostCap = std::chrono::milliseconds(std::max(delay, 0) * 100) + 3s + 1s + MODE_SET_ALLOWANCE;
  m_startHeld = false;
  m_heldExpired = false;
  m_runAtNs = 0;
  // the playback open (bd_disc_clock_start) left the clock running
  m_frozen = false;
  m_writtenRunAtNs = 0;
  m_active = enabled;
  CLog::Log(LOGINFO, "DISCDIAG TEMP owner begin: {}", enabled ? "on" : "off");
  Evaluate("begin");
}

void CDiscTimeOwner::End()
{
  std::unique_lock lock(m_lock);
  if (m_active && m_frozen)
    Write(true, 0, "end");
  m_active = false;
  m_startHeld = false;
  m_heldExpired = false;
  m_runAtNs = 0;
  m_frozen = false;
}

void CDiscTimeOwner::SetDisplayLost(bool lost, int64_t runAtNs)
{
  std::unique_lock lock(m_lock);
  m_displayLost = lost;
  m_lostExpired = false;
  if (lost)
    m_lostSince = std::chrono::steady_clock::now();
  else if (runAtNs > 0)
    m_runAtNs = runAtNs;
  Evaluate(lost ? "display lost" : "display reset");
}

void CDiscTimeOwner::SetStartHeld(bool held, int64_t runAtNs)
{
  std::unique_lock lock(m_lock);
  // a no-hold scheduled start releases a hold it never took: no output change
  if (!held && !m_startHeld)
    return;
  m_startHeld = held;
  m_heldExpired = false;
  if (held)
    m_heldSince = std::chrono::steady_clock::now();
  else if (runAtNs > 0)
    m_runAtNs = runAtNs;
  Evaluate(held ? "start held" : "start released");
}

void CDiscTimeOwner::CheckBounds()
{
  std::unique_lock lock(m_lock);
  if (!m_active || !m_frozen)
    return;
  const auto now = std::chrono::steady_clock::now();
  bool expired = false;
  if (m_displayLost && !m_lostExpired && now - m_lostSince > m_lostCap)
  {
    CLog::Log(LOGERROR, "DiscTimeOwner: display lost for over {} ms, disc time runs",
              m_lostCap.count());
    m_lostExpired = true;
    expired = true;
  }
  // CheckHeldStart does not release while the display is lost
  if (m_startHeld && !m_heldExpired && (!m_displayLost || m_lostExpired) &&
      now - m_heldSince > HELD_CAP)
  {
    CLog::Log(LOGERROR, "DiscTimeOwner: start held for over {} s, disc time runs",
              std::chrono::duration_cast<std::chrono::seconds>(HELD_CAP).count());
    m_heldExpired = true;
    expired = true;
  }
  if (expired)
    Evaluate("cap");
}

void CDiscTimeOwner::Evaluate(const char* why)
{
  if (!m_active)
    return;
  const bool freeze = (m_displayLost && !m_lostExpired) || (m_startHeld && !m_heldExpired);
  if (freeze)
  {
    if (!m_frozen)
    {
      // a T0 from before this freeze is stale
      m_runAtNs = 0;
      Write(false, 0, why);
      m_frozen = true;
    }
    return;
  }

  const int64_t now = MonoNs();
  const int64_t at = std::max(now, m_runAtNs);
  if (m_frozen)
  {
    Write(true, at, why);
    m_frozen = false;
    m_writtenRunAtNs = at;
  }
  else if (m_runAtNs > 0 && m_writtenRunAtNs > now && at != m_writtenRunAtNs)
  {
    // a run instant still to come moved
    Write(true, at, why);
    m_writtenRunAtNs = at;
  }
  m_runAtNs = 0;
}

void CDiscTimeOwner::Write(bool running, int64_t atNs, const char* why)
{
  // DISCDIAG TEMP
  CLog::Log(LOGINFO, "DISCDIAG TEMP owner: {} at {} ({}; lost {}{}, held {}{})",
            running ? "RUN" : "FREEZE", atNs, why, m_displayLost, m_lostExpired ? " expired" : "",
            m_startHeld, m_heldExpired ? " expired" : "");
#if defined(HAVE_LIBBLURAY) && defined(BD_DISC_CLOCK)
  bd_disc_clock_set(running ? 1 : 0, atNs);
#endif
}
