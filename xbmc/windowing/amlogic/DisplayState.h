/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "rendering/RenderSystemTypes.h"
#include "utils/HDRCapabilities.h"
#include "windowing/Resolution.h"

#include <cstdint>
#include <memory>
#include <string>

class CAMLDisplay;

// Phase 5 step 5.V1-2: what the output is, published by W (design I3) after each commit and
// caps refresh. Shadow only: every reader still reads today's source, and the shadows count
// "DISPLAYSTATE <field> match n diff m skipped k ..." against it. displaystate_off: nothing is
// published, compared or logged.

namespace DISPLAYSTATE
{

struct DisplayState
{
  uint64_t epoch = 0;
  bool connected = false;
  std::string modeString;
  // the committed mode; refresh is the one the display runs
  float refresh = 0.0f;
  int screenWidth = 0;
  int screenHeight = 0;
  uint32_t dwFlags = 0;
  std::string strId;
  float fps = 60.0f; //!< CGraphicContext::GetFPS's rule
  //! the set kept the live mode string, so the display may run the other 1000/1001 rate of it
  bool fracHeld = false;
  float requestedFps = 60.0f; //!< the set's request, by GetFPS's rule
  bool windowed = false;
  int drmFd = -1;
  uint32_t crtcId = 0;
  CHDRCapabilities hdrCaps;
  bool support3d = false;
  bool hdrPq = false;
  bool hdrHlg = false;
  bool hdr10Plus = false;
  bool has2160 = false; //!< a table entry with iScreenHeight >= 2160
  uint64_t capsEpoch = 0;
};

//! null before the seed and under displaystate_off
std::shared_ptr<const DisplayState> Get();

// W's publishes (or inline on their caller while W is not running)
void PublishMode(const CAMLDisplay& display, const RESOLUTION_INFO& res, bool windowed);
void PublishCaps(const CAMLDisplay& display);
//! res null: no committed mode yet
void PublishSeed(const CAMLDisplay& display, const RESOLUTION_INFO* res, bool windowed);

//! main, before a write of the caps, the DRM ids or the table; the next caps publish ends it
void CapsWriteBegins();
//! main, after a mode set job returned success
void NoteCommit();
uint64_t CommitCount();
//! CGraphicContext::SetVideoResolutionInternal: before m_Resolution changes, and once switched
//! is known
void GcSetBegins();
void GcSetEnds(bool switched);

// shadows of main's reads (render manager): only when GC's committed mode (strId + refresh) is
// DisplayState's, else a skip; committed: a skip after a commit is a FAIL
void ShadowMainMode(const char* fpsField,
                    const char* heightField,
                    const RESOLUTION_INFO& gcRes,
                    float gcFps,
                    bool committed = false);
void ShadowMainWindowed(const char* field, const RESOLUTION_INFO& gcRes, bool windowed);

// shadows at readers off main: a difference from GC:455 to the publish, or after a failure
// after the commit until the next publish, counts in its window; one where GC's fps is the
// other 1000/1001 rate of a held mode counts as frac-held
struct ModeShadow
{
  std::shared_ptr<const DisplayState> state; //!< null: skipped
  bool inSet = false;
  bool restored = false;
};
ModeShadow BeginModeShadow();
//! GC's fps (today's read) differs from DisplayState's by 1000/1001 at a held mode
bool FracHeld(const ModeShadow& shadow, float todayFps);
void CountMode(const ModeShadow& shadow,
               const char* field,
               double today,
               double displayState,
               bool fracHeld = false);

// shadows of the caps fields at their accessors
void ShadowHdrCaps(const CHDRCapabilities& today);
void ShadowDv(bool today);
void Shadow3d(bool today);
void ShadowHdrPq(bool today);
void ShadowHdrHlg(bool today);
void ShadowHdr10Plus(bool today);
void ShadowHas2160(bool today);
void ShadowDrm(int fd, uint32_t crtc);

// 5.V1-8 (S1): what main's GUI state gives the video side, published by main
struct GuiInputs
{
  RenderStereoMode stereoMode = RenderStereoMode::OFF;
};
void PublishGuiInputs(const GuiInputs& inputs);
//! the published stereo mode, counted against today's GC read; today's read before main's
//! first publish and under displaystate_off
RenderStereoMode GuiStereoMode(RenderStereoMode today);

// main only: the shadows' readers only count
//! logs new DIFF/FAIL details, and the counters every 10 s
void Tick();
//! the counters changed since their last line
void LogCounters();

} // namespace DISPLAYSTATE
