/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// 5.V3-3 (docs/phase5_v3_plan.md §1.1, Q7): main's GUI state for the video geometry, published
// by main as one snapshot at the end of every CApplication::FrameMove (a) and after Flip on
// rendered passes (b), each followed by C's wake when the content changed. Read by C (from V3-4)
// and by shadow B, at most three attempts per read. Everything here runs only with the 5.V3
// latch on (aml_geometrytick_on).

#include "rendering/RenderSystemTypes.h"
#include "utils/Geometry.h"

#include <cstdint>

struct RESOLUTION_INFO;
namespace DISPLAYSTATE
{
struct DisplayState;
}

namespace GUISNAPSHOT
{

//! the RESOLUTION_INFO fields the geometry reads
struct SRow
{
  int overscanLeft = 0;
  int overscanTop = 0;
  int overscanRight = 0;
  int overscanBottom = 0;
  int width = 0; //!< iWidth
  int height = 0; //!< iHeight
  int blanking = 0;
  int screenWidth = 0;
  int screenHeight = 0;
  int subtitles = 0;
  float pixelRatio = 1.0f;
  uint32_t flags = 0; //!< dwFlags
  float refreshRate = 0.0f;
  int pad = 0;
};

SRow RowOf(const RESOLUTION_INFO& info);
//! W's committed GUI row (DisplayState's res, written by FillMode)
SRow WRowOf(const DISPLAYSTATE::DisplayState& state);
//! CGraphicContext::GetResInfo's stereo rule (GC:688-735) for one stereo mode value; the FPS
//! override is the caller's
SRow StereoRow(SRow row, RenderStereoMode mode);

struct SSnapshot
{
  uint64_t noticeSeqDone = 0; //!< the largest finished fullscreen notice seq
  uint64_t modeSeqSeen = 0; //!< the largest W mode publish main has taken a reply of
  CRect videoRect; //!< GC's m_videoRect
  SRow rawRow; //!< the GUI row of m_Resolution (CDisplaySettings), also the video row
  SRow row; //!< rawRow with the stereo rule for stereoMode, and the FPS override
  int stereoMode = 0; //!< RenderStereoMode, read once
  bool fullScreenVideo = false;
  bool calibrating = false;
  bool rowMoved = false; //!< GC's row is DisplayState's committed W row (by value, at (a))
  bool valid = false; //!< false: nothing published yet
};

//! main, at the end of CApplication::FrameMove
void PublishFrameMove();
//! main, right before RenderEx: the flags, the stereo mode and the rows publish (b) pairs with
void CaptureAtRenderEx();
//! main, after Flip on a rendered pass: the captured state with this pass's m_videoRect
void PublishAfterFlip();

//! main: a finished fullscreen notice (applied, already set or refused)
void NoteNoticeDone(uint64_t seq);
//! main: a W reply's mode publish seq (0: W did not commit)
void NoteModeSeq(uint64_t seq);

//! any thread: the latest snapshot; false when none is published or three reads were torn (the
//! caller keeps its last good copy; counted as GEOMDIAG torn)
bool Read(SSnapshot& snapshot);

//! main: counters and the read time, logged every 10 s while they change
void Tick();

} // namespace GUISNAPSHOT
