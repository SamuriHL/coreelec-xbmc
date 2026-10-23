/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "rendering/RenderSystemTypes.h"
#include "utils/AMLUtils.h"
#include "utils/HDRCapabilities.h"
#include "windowing/Resolution.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class CAMLDisplay;

// Phase 5 step 5.V1-2: what the output is, published by W (design I3) after each commit and
// caps refresh. The caps readers take its values (5.V1-3); the others still read today's
// source. The shadows count "DISPLAYSTATE <field> match n diff m skipped k ..." against today's
// read. displaystate_off: nothing is published, compared or logged.

namespace DISPLAYSTATE
{

//! 5.B3-3: a mode by its facts, never by an index into the resolution table
struct ModeFacts
{
  std::string strId; //!< DisplayState's desktopMode: the DRM mode string
  int screenWidth = 0;
  int screenHeight = 0;
  float refresh = 0.0f;
  uint32_t dwFlags = 0;
  bool operator==(const ModeFacts& other) const
  {
    return strId == other.strId && screenWidth == other.screenWidth &&
           screenHeight == other.screenHeight && refresh == other.refresh &&
           dwFlags == other.dwFlags;
  }
  bool operator!=(const ModeFacts& other) const { return !(*this == other); }
};

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
  // 5.B3-3 (design §3): the GUI plane as BuildFlipRequest programs it, read at each publish
  // under the DRM lock, only under hotplugworker (0 otherwise); log-only ("DISPLAYSTATE ui-ids")
  // until B3-4's readers take them
  uint32_t primaryPlaneId = 0;
  int uiSrcW = 0; //!< the mode set's m_width/m_height
  int uiSrcH = 0;
  int uiScreenW = 0; //!< m_ScreenWidth/m_ScreenHeight (the stereo rule of the mode set)
  int uiScreenH = 0;
  //! G's graphics plane state at the publish, for the logs only. G sets and clears
  //! m_overlayActive with its own flips, between publishes, so this copy goes stale: B3-4 and
  //! later must not read it (a G-side publisher first)
  bool overlayActive = false;
  //! W's no-player Restore target (B3-8); empty until then
  ModeFacts desktopMode;
  //! set only by 5.E2's deferred Restore; always false in 5.B3
  bool linkUncommitted = false;
  //! W's last HPD read (from B3-6); not read before
  HpdRead hpd;
};

//! null before the seed and under displaystate_off
std::shared_ptr<const DisplayState> Get();
//! 5.B3-4: the DRM-object readers' source under hotplugworker: the last published state
//! (immutable, never freed while held), whatever a caps reader's skip cause. Null without
//! hotplugworker (latched at W's start) or before the first publish: the reader keeps HEAD's
//! live read
std::shared_ptr<const DisplayState> LinkState();

// W's publishes (or inline on their caller while W is not running)
void PublishMode(const CAMLDisplay& display, const RESOLUTION_INFO& res, bool windowed);
//! has2160: the caller's value (5.B3-7b: W's own table, or main's read of its table); without it
//! the publish reads CDisplaySettings
void PublishCaps(const CAMLDisplay& display, std::optional<bool> has2160 = std::nullopt);
//! res null: no committed mode yet
void PublishSeed(const CAMLDisplay& display, const RESOLUTION_INFO* res, bool windowed);

//! main, before a write of the caps, the DRM ids or the table; the next caps publish ends it
void CapsWriteBegins();
//! a mode set's commit: W's job after PublishMode under 5.B2-3, else main after the job
void NoteCommit();
uint64_t CommitCount();

// 5.B2-3 (I5): the timing readers take the committed mode
//! 5.B2-3's switch: OUTPUT::OutputDriveOn() and no outputdrive_skip "B2-3", read once
bool CommitTimingOn();
struct ModeTiming
{
  float fps = 60.0f; //!< CGraphicContext::GetFPS's rule, of the committed refresh
  int screenHeight = 0;
  bool windowed = false;
  //! a frac-held set, or a failure after the commit: the latency tweak keeps today's bucket (the
  //! set's target, GC's), so the tweak that landed the audio stays
  bool todaysTweak = false;
};
//! the last publish's mode, one lock-free word (the coordinator reads it); false before the
//! first publish of a mode and while disconnected, with R10's log line when a field is named
bool CommittedTiming(ModeTiming& timing, const char* field = nullptr);
//! CGraphicContext::SetVideoResolutionInternal: before m_Resolution changes, and once switched
//! is known
void GcSetBegins();
void GcSetEnds(bool switched);
//! 5.B3-7b (O1): GC's set (SetVideoResolutionInternal) on main's stack, unwound by a throw too; a
//! hotplug reply is not applied inside one
class CGcSetScope
{
public:
  CGcSetScope();
  ~CGcSetScope();
  CGcSetScope(const CGcSetScope&) = delete;
  CGcSetScope& operator=(const CGcSetScope&) = delete;
};
//! main: the GC sets on its stack
int GcSetDepth();

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

//! 5.B3-3, at each GUI-plane flip build (BuildFlipRequest), with its own reads: counted as
//! "DISPLAYSTATE ui-ids" against the published fields, only under hotplugworker. Called under
//! the DRM lock, except from the legacy FlipPage (not used while the coordinator runs); it
//! takes only DisplayState's leaf locks
void ShadowUiIds(uint32_t planeId, uint32_t crtcId, int srcW, int srcH, int screenW, int screenH);

// shadows of the caps fields at their accessors
void ShadowHdrCaps(const CHDRCapabilities& today);
void ShadowDv(bool today);
void Shadow3d(bool today);

// 5.V1-3: the caps readers' values. Under on, the published value: the one the shadow compared
// with today's read (the accessor's shadow, run on this thread for the value passed in, or
// the shadow here); today's read before W's first publish, while disconnected, while main
// writes the caps or after a failed caps read, logged once per cause and publish. Under off,
// today's read.
CHDRCapabilities HdrCaps(const CHDRCapabilities& today);
//! CWinSystemAmlogic::IsHDRDisplay's rule
bool HdrDisplay(bool today);
bool Dv(bool today);
bool Support3d(bool today);
bool HdrPq(bool today);
bool HdrHlg(bool today);
bool Hdr10Plus(bool today);
bool Has2160(bool today);
//! 5.V1-6 (MC-46): the DRM fd and CRTC id, by the same rules
void DrmIds(int& fd, uint32_t& crtc);

// 5.V1-8 (S1): what main's GUI state gives the video side, published by main
struct GuiInputs
{
  GuiInputs() = default;
  //! 5.V1-8's publish of the stereo mode alone ({mode})
  GuiInputs(RenderStereoMode mode) : stereoMode(mode) {}

  RenderStereoMode stereoMode = RenderStereoMode::OFF;
  // 5.B3-3: W's hotplug inputs (B3-7b's field table), published only under hotplugworker
  //! GetCurrentResolution()'s facts; nullopt: desktop (not above RES_DESKTOP)
  std::optional<ModeFacts> guiResolution;
  bool nativeGui = false; //!< DISABLEGUISCALING
  bool videoPlaying = false; //!< IsPlayingVideo()
  ModeFacts videoResolution; //!< the facts of GC's GetVideoResolution() entry
  bool published = false; //!< false before W's start publish and under off
};
//! the stereo mode only (5.V1-8)
void PublishGuiInputs(const GuiInputs& inputs);
//! the published stereo mode, counted against today's GC read; today's read before main's
//! first publish and under displaystate_off
RenderStereoMode GuiStereoMode(RenderStereoMode today);
// 5.B3-3, each a no-op without hotplugworker (OUTPUT::HotplugWorkerOn), logged "GUIINPUTS ..."
//! at W's start, after the hotplug latch: every field
void PublishGuiInputsAtStart(bool nativeGui);
//! CDisplaySettings::SetCurrentResolution (any thread): guiResolution from the current
//! resolution
void PublishGuiResolution();
//! the DISABLEGUISCALING branch, before the table is rebuilt (OnSettingChanging, the incoming
//! value), and OnSettingChanged (the committed one); any thread
void PublishNativeGui(bool nativeGui);
//! main, MessagePump: videoPlaying and videoResolution, published when they change
void PublishGuiPlayback();
//! the published inputs (W's hotplug job, from B3-7b); published false before the start publish
GuiInputs GetGuiInputs();

// 5.B2-8 (D11, D12): a DV-mode publish posts OutputChanged; main's handler asks RM to re-resolve
// the GUI/OSD encoding at its next render pass
//! the switch: ModeSetsOnW and no outputdrive_skip "B2-8", latched at its first read after W runs
bool DvModePublishOn();
//! the writers of the DV output mode (aml_dv_set_output_mode)
enum DvWriter : unsigned int
{
  DV_WRITER_VS10 = 1,
  DV_WRITER_VS10_FOLLOW = 2,
  DV_WRITER_DV_APPLY = 4, //!< a decoder open: its first picture configures
  DV_WRITER_TEARDOWN = 8, //!< a decoder close: the next decoder's first picture configures
};
//! W, inside the writer's job: the epoch bump (no commit count, no timing bump) and the post
void PublishDvMode(unsigned int oldMode, unsigned int mode, DvWriter writer);
//! main, from MessagePump: the posts since the last call; sets the re-encode request unless only
//! decoder writers posted
void HandleOutputChanged();
//! main, RM's render pass: the pending request's epoch, consumed; 0 when none
uint64_t TakeReencodeRequest();

// main only: the shadows' readers only count
//! logs new DIFF/FAIL details, and the counters every 10 s
void Tick();
//! the counters changed since their last line
void LogCounters();

} // namespace DISPLAYSTATE
