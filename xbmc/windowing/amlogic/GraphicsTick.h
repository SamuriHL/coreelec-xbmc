/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/OverlayRenderer.h"
#include "utils/Geometry.h"

#include <cstdint>
#include <functional>
#include <vector>

// Phase 5 step 5.G2 GT-4 (design draft42 §10.2 items 2-4, 7, 8, 10-12; docs/phase5_g2_plan.md
// "GT-4", §7 M1-M3, M5): the timed graphics (PGS, DVB, libass text/SSA) leave main. V posts each
// queued picture's overlays to G, which draws them ahead on the graphics plane; C picks, at each
// tick, the drawn buffer of the frame on screen next and commits it with the GUI plane. Main keeps
// only the untimed graphics (menus) until GT-5.
//
// This file is the route's shared state: the decision (RM, at every configure), V's posts, the
// flush floors (M2) and C's Released posts to G. Nothing here waits.
//
// Opt-in: special://profile/graphicstick_on. Without it nothing in this file is ever on, posted
// or logged, and every caller keeps GT-3's route.
namespace GRAPHICSTICK
{
//! special://profile/graphicstick_on, read once
bool Requested();
//! test file special://profile/gt_delay1 (never a setting): C picks the buffer of the frame
//! released one tick earlier than the rule says, so the scan-out probe must read +1 (its
//! self-test with a real delay in Kodi's commit)
bool TestDelay1();
//! test file special://profile/gt_released_log: one GRELEASED line per released frame (the
//! probe's Released-tick check); off by default, it is a line per frame
bool ReleasedLog();

//! C's render area (CRendererAML::VideoGeometryFromC), in the GUI's space
struct SRects
{
  CRect source;
  CRect dest;
  CRect view;
  unsigned int rgen = 0;
  bool valid = false;
};

//! V's post at AddVideoPicture: the picture's own overlays (5.V2-7), never waited on
struct SQueued
{
  uint64_t serial = 0; //!< RM's overlay serial of the picture
  double pts = 0.0;
  unsigned int segmentGen = 0;
  OVERLAY::FrameOverlays overlays;
  SRects rects; //!< C's area when V queued it: G draws ahead with it
};

//! C's post after each tick that released a frame: F, the frame G's next buffers go with
struct SReleased
{
  uint64_t serial = 0;
  unsigned int segmentGen = 0;
  int64_t qbuf = -1; //!< amlvideo's running v4l2 index of F
  uint64_t tick = 0; //!< the vblank seq of the tick that released F
  SRects rects;
};

//! a flush's serial floor (M2): every composition of a picture at or below floor is gone
struct SFloor
{
  uint64_t floor = 0;
  uint64_t gen = 0; //!< one per post
};

// the route (RM decides; G and C follow)
//! RM, at each configure and at a session's end: logs "GTICK on (<why>)" / "GTICK off (<why>)"
//! on a change only. A no-op without graphicstick_on
void SetRoute(bool on, const char* why);
bool RouteOn();
//! both under one lock
void RouteState(bool& on, uint64_t& session);
//! moves at each on and each off: G starts and ends its route on a change
uint64_t RouteSession();
//! G (programs, plane refused) or V (a DVD's SPU): the route cannot go on; RM's next configure
//! reads it, and RouteOn() is false from now on
void EndRoute(const char* why);
//! RM, at a next file: an EndRoute's latch is the file's only
void NewFile();
//! the reason an EndRoute gave this file; nullptr: none
const char* EndedWhy();

// posts (any thread, no wait)
void PostQueued(SQueued&& queued);
void PostFloor(uint64_t floor, const char* why);
SFloor Floor();
void PostReleased(const SReleased& released);

// G's side
struct SInbox
{
  std::vector<SQueued> queued; //!< in serial order
  bool released = false; //!< a Released since the last take
  SReleased last;
  SFloor floor;
  uint64_t session = 0;
  bool on = false;
};
//! G: wake is called (from any thread) after every post
void SetWake(std::function<void()> wake);
void Take(SInbox& inbox);
//! G's side of a hand-back: main draws the timed overlays again once G has left the route
struct SSide
{
  uint64_t session = 0;
  bool on = false;
};
//! G: the route session it now draws for, or has left
void NoteGSide(uint64_t session, bool on);
SSide GSide();
//! wakes G (C: buffers back or picked)
void Kick();

//! G: the newest serial it has drawn or found unchanged; C counts a frame past it as missing
void NoteProcessed(uint64_t serial);
uint64_t Processed();

} // namespace GRAPHICSTICK
