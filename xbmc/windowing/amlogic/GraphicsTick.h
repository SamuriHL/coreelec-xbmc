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
#include <source_location>
#include <utility>

#include <fmt/format.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class CDVDOverlay;
class CDVDOverlayContainer;

// Phase 5 step 5.G2 GT-4 (design draft42 §10.2 items 2-4, 7, 8, 10-12; docs/phase5_g2_plan.md
// "GT-4", §7 M1-M3, M5): the timed graphics (PGS, DVB, libass text/SSA) leave main. V posts each
// queued picture's overlays to G, which draws them ahead on the graphics plane; C picks, at each
// tick, the drawn buffer of the frame on screen next and commits it with the GUI plane. Main keeps
// only the untimed graphics (menus) until GT-5.
//
// This file is the route's shared state: the decision (RM, at every configure), V's posts, the
// flush floors (M2) and C's Released posts to G. Nothing here waits.
//
// On by default (GT-7) on the validated SoCs with the plane on; special://profile/graphicstick_off
// or graphicsplane 0 is the exit. Without the route nothing in this file is ever on, posted or
// logged, and every caller keeps today's route.
namespace GRAPHICSTICK
{
// 5.G2 GT-5 (GT-D15): the log writer for C's and G's per-frame lines. RC6 coreelec-br: C's loop
// body took 20-27 ms while the whole log was silent and V stalled with it, a log write blocking
// (C logged under m_uiMutex too). With the route requested, C and G hand lines to a bounded queue
// (a short lock, never held across I/O) that a normal-priority GTLOG thread writes, in push
// order; a full queue drops and counts. Without it a line is logged at once, as
// before.
//! queue one line (formatted already)
void GTLog(int level, std::string line);
//! the log keeps this level (a debug line is not even formatted when debug logging is off)
bool GTLogLevelOn(int level);
template<typename... Args>
void GTLogF(int level, fmt::format_string<Args...> format, Args&&... args)
{
  if (GTLogLevelOn(level))
    GTLog(level, fmt::format(format, std::forward<Args>(args)...));
}
//! a component's line: CLog's component gating and its tag in the log, kept
void GTLog(int level, uint32_t component, std::string line);
bool GTLogComponentOn(int level, uint32_t component);
template<typename... Args>
void GTLogFC(int level, uint32_t component, fmt::format_string<Args...> format, Args&&... args)
{
  if (GTLogComponentOn(level, component))
    GTLog(level, component, fmt::format(format, std::forward<Args>(args)...));
}
//! CLog::LogFC through the queue, with the same "<function>: " prefix (log.h's non-Windows form)
#define GTLOG_FC(level, component, format, ...) \
  GRAPHICSTICK::GTLogFC((level), (component), ("{}: " format), \
                        std::source_location::current().function_name(), ##__VA_ARGS__)
//! waits (up to timeoutMs) until the lines queued before the call are written. Never on main,
//! never on a thread that a wait would delay mid-playback
void GTLogFlush(int timeoutMs);
//! C's start: the writer starts; until then (and with no coordinator) lines are written at once
void GTLogStart();
//! C's stop: new lines are written at once, the writer drains the queue and is joined
void GTLogStop();
//! lines, drops, writes over 5 ms and the slowest write since the last call with reset
std::string GTLogStatsLine(bool reset);

//! the route can run in this Kodi run (validated SoC, coordinator, plane 1, no graphicstick_off);
//! read once, logged "GTICK default on|off (<why>)"
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
//! on a change only. A no-op without Requested()
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

//! C: the newest picture serial it picked in this route session (its stale filter drops entries
//! below it); G makes the frame on screen's entry at or above it
void NotePickedSerial(uint64_t session, uint64_t serial);
//! 0 when C's last pick was of another session
uint64_t PickedSerial(uint64_t session);
//! G: the newest serial it has drawn or found unchanged; C counts a frame past it as missing
void NoteProcessed(uint64_t serial);
uint64_t Processed();

// 5.G2 GT-5 (design draft42 §10.2 items 5 (a)-(k), 10, 12; docs/phase5_g2_plan.md "GT-5"): the
// untimed graphics (BD-J ARGB, present-latest menu pages, reposts, clears) leave main too. The
// overlay container posts each change of its present-latest set here (its wake hook, under the
// container lock); G takes the newest posted state (skip-to-newest), draws it over the frames'
// timed overlays, and calls the container's NotePresentedSegment at C's Released. Only while
// G has the hook on: without Requested(), or with guntimed_off, nothing is posted.

//! special://profile/guntimed_off (with the route): untimed content keeps GT-4's bridge
//! (main selects it and hands it over), not today's route. Retires at GT-7
bool UntimedOff();
//! special://profile/gnewest_off (with the route): item 5 (j), no superseded state is
//! dropped; G draws every posted state in post order
bool NewestOff();

//! a state replaced in the slot before G took it (item 5 (b) (i)), judged on its successor
struct SUntimedPosted
{
  uint64_t u = 0;
  int64_t postNs = 0;
  bool cleared = false;
};

//! one untimed state (item 5 (a)): the container's present-latest set at a change
struct SUntimedState
{
  uint64_t u = 0; //!< numbered here, in post order (G's keep-alive expiry takes one too)
  uint64_t change = 0; //!< the container's change counter
  int64_t postNs = 0; //!< CLOCK_MONOTONIC, at the change
  //! the current composition's images, bottom first (empty: cleared)
  std::vector<std::shared_ptr<CDVDOverlay>> current;
  int64_t keepAliveTick = 0; //!< CDVDOverlay::m_keepAliveTick of the current group
  //! the newest page waiting for its segment's first picture, and that segment
  std::vector<std::shared_ptr<CDVDOverlay>> waiting;
  unsigned int waitingGen = 0;
  unsigned int releasedSegment = 0;
  unsigned int currentGen = 0; //!< the current group's segment (0: none or not gated)
  std::vector<SUntimedPosted> skipped; //!< posted since G's last take, never taken
};

// the container's side (under its lock: only this file's locks are taken)
//! true while G takes the container's states
bool UntimedHook();
//! the container that posts (never from its destructor: it stops posting first); cleared at its
//! destruction, after its last change (never called under its lock)
void RegisterContainer(CDVDOverlayContainer* container);
void ContainerGone(CDVDOverlayContainer* container);
//! a change of the present-latest set (a repost of the same images only refreshes the keep-alive)
void PostUntimed(SUntimedState&& state);

// G's side
//! the container's states go to G from now on (on: the next post is never taken as a repeat)
void SetUntimedHook(bool on);
//! runs fn on the registered container, which cannot go away meanwhile; false: none. Never call
//! with another lock held: fn takes the container's lock, which posts here
bool WithContainer(const std::function<void(CDVDOverlayContainer&)>& fn);
//! the newest posted state (gnewest_off: the oldest not taken); false: none since the last take
bool TakeUntimed(SUntimedState& state, size_t& depth);
//! the latest keep-alive stamp of the current composition (BD-J's re-posts refresh it)
int64_t UntimedKeepAlive();
//! G's own state change (the keep-alive expiry), numbered with the posts
uint64_t NextUntimedU();
//! RM: the start's graphics hold (E1's point for a no-video start, errata E1 row 3b); C
//! commits no untimed-only buffer while it is set and no video ticks
void SetUntimedHeld(bool held);
bool UntimedHeld();
//! C: it picks frame entries (the route on, RM configured, the clock running); false while
//! paused, between titles with RM unconfigured, or with video detached. A change wakes G
void SetCAdvancing(bool advancing);
bool CAdvancing();
//! C, gnewest_off only (item 5 (j)): the newest untimed state committed, or dropped whole
void NoteUntimedCommitted(uint64_t u);
uint64_t UntimedCommitted();

//! counters for C's report line (GUNTIMED report)
enum class UntimedCount
{
  STATES, //!< states G took
  SKIPPED, //!< replaced in the slot before G took them
  UNIT_DEFERRED, //!< item 5 (d): a re-draw unit with no buffers for it
  WAIT_BUFFER, //!< item 12: G waited on C for a free buffer
  WAIT_BUFFER_MAX_MS,
  FIRST_PAGES, //!< pages G drew for a segment's frames before its Released
  KEEPALIVE_EXPIRED,
  NEWEST_DEPTH_MAX, //!< gnewest_off: the deepest queue seen
  NEWEST_OVERFLOW, //!< gnewest_off: states dropped at the queue's bound (review rA m1)
  PAGE_LATE_POSTED, //!< review rB m8: a segment's page posted after its first frame's release
  PAGE_FALLBACK, //!< a waiting page released by the P-side 3 s fallback, not by its picture
  WAIT_SUPPRESSED, //!< GUNTIMED wait lines over the rate cap, not logged
  SCREEN_BRIDGE, //!< a menu change put on an untimed-only entry: no picture at the frame on screen
  SCREEN_BRIDGE_NO_TIMED, //!< the same outside a floor or start: it drops what is on screen
  COUNT
};
void Count(UntimedCount what, int n = 1);
void CountMax(UntimedCount what, int value);
//! the counters since the last call with reset; any: one of them is not 0
std::string UntimedCountsLine(bool reset, bool& any);
//! C (review rB m9): a commit carrying the graphics plane failed, lost its flip or was dropped
//! on a refused plane; G draws what is up again
void NoteGraphicsLost();
uint64_t GraphicsLost();
//! C (GT-6): its cleared buffer of a regime change or the OSD hold took the graphics plane; G
//! draws what is up again (nothing else would post a static subtitle or menu once more)
void NotePlaneCleared();
uint64_t PlaneCleared();
//! moves at every post, a repost of the same images included (review rB m4 (a))
uint64_t UntimedPosts();

} // namespace GRAPHICSTICK
