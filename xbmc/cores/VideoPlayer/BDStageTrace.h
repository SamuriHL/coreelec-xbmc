/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ServiceBroker.h"
#include "utils/log.h"

#include <atomic>
#include <chrono>

// Where the time goes between a Blu-ray playlist change and the first picture
// actually reaching the screen, broken into stages:
//
//   wait    previous picture -> user's button press (HUMAN time, not ours)
//   vm      button press     -> playlist change (the disc's VM/BD-J deciding)
//   open    playlist change  -> decoder open
//   decode  decoder open     -> first picture out of the decoder
//   hold    picture emitted  -> picture shown
//
// wait exists because a menu sits on a still waiting for the viewer, and folding
// that into vm reports the viewer's thinking time as the disc's. When no button
// press falls inside the span - an automatic transition - wait is -1 and vm
// covers the whole previous-picture -> playlist-change gap as before.
//
// Costs nothing unless debug logging is on with the video component enabled.
// The marks are written from the player, input-stream and codec threads, so they
// are atomics; the "first one wins" reads are deliberately not a CAS - a torn
// decision only mis-attributes a diagnostic millisecond, never playback.
namespace BDSTAGE
{
using Clock = std::chrono::steady_clock;
using Rep = Clock::rep;

inline std::atomic<bool> g_on{false};
inline std::atomic<int> g_n{0};
inline std::atomic<int> g_playlist{-1};
inline std::atomic<bool> g_inMenu{false};
inline std::atomic<Rep> g_play{0};
inline std::atomic<Rep> g_prevShown{0};
inline std::atomic<Rep> g_input{0};
inline std::atomic<Rep> g_plChange{0};
inline std::atomic<Rep> g_open{0};
inline std::atomic<Rep> g_emit{0};

inline Rep Now()
{
  return Clock::now().time_since_epoch().count();
}

inline int Ms(Rep a, Rep b)
{
  if (a == 0 || b == 0 || b < a)
    return -1;
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration(b - a)).count());
}

inline void Play()
{
  g_on = CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG) &&
         CServiceBroker::GetLogging().CanLogComponent(LOGVIDEO);
  if (!g_on)
    return;

  const Rep now = Now();
  g_n = 0;
  g_playlist = -1;
  g_inMenu = false;
  g_play = now;
  g_prevShown = now;
  g_input = 0;
  g_plChange = 0;
  g_open = 0;
  g_emit = 0;
}

// A key handed to the disc. Last one wins: it is the press that actually caused
// the transition we are about to time, not the first of a navigation burst.
inline void UserInput()
{
  if (!g_on)
    return;

  g_input = Now();
}

inline void Playlist(int id, bool inMenu)
{
  if (!g_on)
    return;

  g_playlist = id;
  g_inMenu = inMenu;
  if (g_plChange == 0)
    g_plChange = Now();
}

inline void DecoderOpen()
{
  if (!g_on)
    return;

  if (g_open == 0)
    g_open = Now();
}

inline void PictureEmitted()
{
  if (!g_on)
    return;

  if (g_emit == 0)
    g_emit = Now();
}

inline void PictureShown()
{
  if (!g_on)
    return;

  // After the first picture, only report again once a playlist change has armed
  // a new segment - otherwise this would log on every frame.
  const Rep plChange = g_plChange;
  if (g_n != 0 && plChange == 0)
    return;

  const Rep now = Now();
  const Rep prevShown = g_prevShown;
  const Rep input = g_input;
  const Rep open = g_open;
  const Rep emit = g_emit;

  // Only credit a press that actually sits between the previous picture and the
  // playlist change; anything outside that window belongs to another segment.
  const bool waited =
      input != 0 && prevShown != 0 && input >= prevShown && plChange != 0 && input <= plChange;

  CLog::Log(LOGDEBUG, LOGVIDEO,
            "bdstage: n={} playlist={} inMenu={} sincePlay={} sincePrev={} wait={} vm={} open={} "
            "decode={} hold={}",
            g_n.load() + 1, g_playlist.load(), g_inMenu.load(), Ms(g_play, now),
            Ms(prevShown, now), waited ? Ms(prevShown, input) : -1,
            waited ? Ms(input, plChange) : Ms(prevShown, plChange), Ms(plChange, open),
            Ms(open, emit), Ms(emit, now));

  ++g_n;
  g_prevShown = now;
  g_input = 0;
  g_plChange = 0;
  g_open = 0;
  g_emit = 0;
}
} // namespace BDSTAGE
