/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// 5.V3-2 (docs/phase5_v3_plan.md §3 V3-2): the video geometry as pure functions of their inputs.
// Nothing here reads or writes Kodi state, so the same code can later run from published inputs
// on the presentation coordinator (V3-4). Today main calls them with the values HEAD's code read
// at the same points, in HEAD's order; shadow A (geometrytick_shadow) compares them with HEAD's
// own bodies, kept in CBaseRenderer and CAMLCodec.

#include "rendering/RenderSystemTypes.h"
#include "utils/Geometry.h"

#include <string>

namespace VIDEOGEOMETRY
{

//! CBaseRenderer::SetViewMode's inputs: the GUI row's facts, the source and the per-file custom
//! values
struct SViewModeIn
{
  int viewMode = 0; //!< as passed in; ComputeViewMode clamps it
  int overscanLeft = 0;
  int overscanTop = 0;
  int overscanRight = 0;
  int overscanBottom = 0;
  int width = 0; //!< the GUI row's iWidth
  int height = 0; //!< iHeight
  int screenWidth = 0; //!< iScreenWidth
  int screenHeight = 0; //!< iScreenHeight
  float guiPixelRatio = 1.0f; //!< the GUI row's fPixelRatio
  float sourceFrameRatio = 1.0f;
  unsigned int sourceHeight = 0;
  float customZoomAmount = 1.0f;
  float customPixelRatio = 1.0f;
  float customVerticalShift = 0.0f;
  bool customNonLinStretch = false;
  int stretch43 = 0; //!< SETTING_VIDEOPLAYER_STRETCH43, read only when ViewModeIs43
};

//! SetViewMode's outputs: the clamped view mode and the four CDisplaySettings values
struct SViewModeOut
{
  int viewMode = 0;
  float zoomAmount = 1.0f;
  float pixelRatio = 1.0f;
  float verticalShift = 0.0f;
  bool nonLinearStretched = false;
};

int ClampViewMode(int viewMode);
//! a 4:3 source in Normal (STRETCH43 then applies); viewMode already clamped
bool ViewModeIs43(int viewMode, float sourceFrameRatio);
SViewModeOut ComputeViewMode(const SViewModeIn& in);

//! CBaseRenderer::CalcDestRect: the destination for a view window; an empty window gives an
//! empty rect
CRect CalcDestRect(float offsetX,
                   float offsetY,
                   float width,
                   float height,
                   float inputFrameRatio,
                   float zoomAmount,
                   float verticalShift,
                   float guiPixelRatio,
                   int errorInAspect,
                   unsigned int orientation);

//! CBaseRenderer::ManageRenderArea's inputs
struct SRenderAreaIn
{
  CRect viewRect;
  unsigned int sourceWidth = 0;
  unsigned int sourceHeight = 0;
  unsigned int flags = 0; //!< the renderer's CONF_FLAGS
  RenderStereoView stereoView = RenderStereoView::OFF;
  float aspectRatio = 1.0f; //!< GetAspectRatio()
  float pixelRatio = 1.0f; //!< CDisplaySettings
  float zoomAmount = 1.0f;
  float verticalShift = 0.0f;
  float guiPixelRatio = 1.0f; //!< the GUI row's fPixelRatio
  int errorInAspect = 0; //!< SETTING_VIDEOPLAYER_ERRORINASPECT
  unsigned int orientation = 0;
  bool clip = false; //!< always-clip, or neither fullscreen video nor calibrating
};
// guiPixelRatio, errorInAspect and clip are used (and read by HEAD) only for a non-empty view

struct SRenderAreaOut
{
  CRect sourceRect;
  CRect destRect;
  bool reorder = false; //!< a non-empty view: HEAD then reorders the draw points
};

SRenderAreaOut ComputeRenderArea(const SRenderAreaIn& in);

//! CAMLCodec::ApplyVideoGeometry: the destination turned for a 90/270 degree rotation (the value
//! its change detection compares)
CRect AmlRotatedDest(const CRect& destRect, int rotationDegree);

//! the VD1 axis inputs, taken when a change was detected
struct SAmlAxisIn
{
  CRect dest; //!< AmlRotatedDest's result
  CRect screen; //!< the video row's screen size
  int guiWidth = 0; //!< the video row's iWidth
  int guiHeight = 0;
  RenderStereoMode stereoMode = RenderStereoMode::OFF;
  std::string videoStereoMode; //!< read only for MONO
  int hwHeight = 0; //!< GetResInfo().iHeight, read only for HARDWAREBASED
  int hwBlanking = 0; //!< GetResInfo().iBlanking, read only for HARDWAREBASED
};

struct SAmlAxisOut
{
  CRect gui; //!< for the log
  CRect dest; //!< scaled to the screen, stereo applied (the log's dst_rect)
  int axis[4] = {}; //!< /sys/class/video/axis (the 0/1-based fix applied)
};

SAmlAxisOut ComputeAmlAxis(const SAmlAxisIn& in);

} // namespace VIDEOGEOMETRY
