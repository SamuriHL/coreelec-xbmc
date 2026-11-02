/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoGeometry.h"

#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "cores/VideoSettings.h"
#include "utils/MathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace VIDEOGEOMETRY
{

int ClampViewMode(int viewMode)
{
  if (viewMode < ViewModeNormal || viewMode > ViewModeZoom110Width)
    viewMode = ViewModeNormal;
  return viewMode;
}

bool ViewModeIs43(int viewMode, float sourceFrameRatio)
{
  // HEAD's sqrt and pow resolve to the float overloads (static_assert in BaseRenderer.cpp)
  return sourceFrameRatio < 8.f / (3.f * std::sqrt(3.f)) && viewMode == ViewModeNormal;
}

SViewModeOut ComputeViewMode(const SViewModeIn& in)
{
  SViewModeOut out;
  out.viewMode = ClampViewMode(in.viewMode);

  float screenWidth = (float)(in.overscanRight - in.overscanLeft);
  float screenHeight = (float)(in.overscanBottom - in.overscanTop);
  const float sourceFrameRatio = in.sourceFrameRatio;
  const bool is43 = ViewModeIs43(out.viewMode, sourceFrameRatio);

  // Splitres scaling factor
  float xscale = (float)in.screenWidth / (float)in.width;
  float yscale = (float)in.screenHeight / (float)in.height;
  screenWidth *= xscale;
  screenHeight *= yscale;

  out.verticalShift = 0.0f;
  out.nonLinearStretched = false;
  const int mode = out.viewMode;

  if (mode == ViewModeZoom || (is43 && in.stretch43 == ViewModeZoom))
  { // zoom image so no black bars
    out.pixelRatio = 1.0;
    float outputFrameRatio = sourceFrameRatio * out.pixelRatio / in.guiPixelRatio;
    float newHeight = screenHeight;
    float newWidth = newHeight * outputFrameRatio;
    out.zoomAmount = newWidth / screenWidth;
    if (newWidth < screenWidth)
    { // zoom to full width
      newWidth = screenWidth;
      newHeight = newWidth / outputFrameRatio;
      out.zoomAmount = newHeight / screenHeight;
    }
  }
  else if (mode == ViewModeStretch4x3)
  { // stretch image to 4:3 ratio
    out.zoomAmount = 1.0;
    out.pixelRatio = (4.0f / 3.0f) / sourceFrameRatio;
  }
  else if (mode == ViewModeWideZoom || (is43 && in.stretch43 == ViewModeWideZoom))
  { // super zoom
    float stretchAmount = (screenWidth / screenHeight) * in.guiPixelRatio / sourceFrameRatio;
    out.pixelRatio = std::pow(stretchAmount, float(2.0 / 3.0));
    out.zoomAmount = std::pow(stretchAmount, float((stretchAmount < 1.0f) ? -1.0 / 3.0 : 1.0 / 3.0));
    out.nonLinearStretched = true;
  }
  else if (mode == ViewModeStretch16x9 || mode == ViewModeStretch16x9Nonlin ||
           (is43 && (in.stretch43 == ViewModeStretch16x9 ||
                     in.stretch43 == ViewModeStretch16x9Nonlin)))
  { // stretch image to 16:9 ratio
    out.zoomAmount = 1.0;
    out.pixelRatio = (screenWidth / screenHeight) * in.guiPixelRatio / sourceFrameRatio;
    out.nonLinearStretched = (is43 && in.stretch43 == ViewModeStretch16x9Nonlin) ||
                             mode == ViewModeStretch16x9Nonlin;
  }
  else if (mode == ViewModeOriginal)
  { // zoom image so that the height is the original size
    out.pixelRatio = 1.0;
    float outputFrameRatio = sourceFrameRatio * out.pixelRatio / in.guiPixelRatio;
    float newHeight = screenWidth / outputFrameRatio;
    if (newHeight > screenHeight)
      newHeight = screenHeight;
    out.zoomAmount = in.sourceHeight / newHeight;
  }
  else if (mode == ViewModeCustom)
  {
    out.zoomAmount = in.customZoomAmount;
    out.pixelRatio = in.customPixelRatio;
    out.nonLinearStretched = in.customNonLinStretch;
    out.verticalShift = in.customVerticalShift;
  }
  else if (mode == ViewModeZoom120Width)
  {
    float fitHeightZoom = sourceFrameRatio * screenHeight / (in.guiPixelRatio * screenWidth);
    out.pixelRatio = 1.0f;
    out.zoomAmount = fitHeightZoom < 1.0f ? 1.0f : (fitHeightZoom > 1.2f ? 1.2f : fitHeightZoom);
  }
  else if (mode == ViewModeZoom110Width)
  {
    float fitHeightZoom = sourceFrameRatio * screenHeight / (in.guiPixelRatio * screenWidth);
    out.pixelRatio = 1.0f;
    out.zoomAmount = fitHeightZoom < 1.0f ? 1.0f : (fitHeightZoom > 1.1f ? 1.1f : fitHeightZoom);
  }
  else
  {
    out.pixelRatio = 1.0;
    out.zoomAmount = 1.0;
  }
  return out;
}

CRect CalcDestRect(float offsetX,
                   float offsetY,
                   float width,
                   float height,
                   float inputFrameRatio,
                   float zoomAmount,
                   float verticalShift,
                   float guiPixelRatio,
                   int errorInAspect,
                   unsigned int orientation)
{
  CRect destRect;
  if (height == 0 || width == 0)
  {
    destRect.SetRect(0.0f, 0.0f, 0.0f, 0.0f);
    return destRect;
  }

  float outputFrameRatio = inputFrameRatio / guiPixelRatio;

  // allow a certain error to maximize size of render area
  float fCorrection = width / height / outputFrameRatio - 1.0f;
  float fAllowed = errorInAspect * 0.01f;
  if (fCorrection > fAllowed)
    fCorrection = fAllowed;
  if (fCorrection < -fAllowed)
    fCorrection = - fAllowed;

  outputFrameRatio *= 1.0f + fCorrection;

  const bool isRotated = orientation == 90 || orientation == 270;

  float newWidth;
  float newHeight;

  if (!isRotated)
  {
    // maximize the movie width
    newWidth = width;
    newHeight = newWidth / outputFrameRatio;
    if (newHeight > height)
    {
      newHeight = height;
      newWidth = newHeight * outputFrameRatio;
    }
  }
  else
  {
    // maximize the movie height
    newHeight = std::min(width, height);
    newWidth = newHeight / outputFrameRatio;
    if (newWidth > width)
    {
      newWidth = std::min(width, height);
      newHeight = newWidth * outputFrameRatio;
    }
  }

  newWidth *= zoomAmount;
  newHeight *= zoomAmount;

  // if we are less than one pixel off use the complete screen instead
  if (std::abs(newWidth - width) < 1.0f)
    newWidth = width;
  if (std::abs(newHeight - height) < 1.0f)
    newHeight = height;

  float posY = (height - newHeight) / 2;
  float posX = (width - newWidth) / 2;

  // vertical shift range -1 to 1 shifts within the top and bottom black bars
  float blackBarSize = std::max((height - newHeight) / 2.0f, 0.0f);
  posY += blackBarSize * std::max(std::min(verticalShift, 1.0f), -1.0f);

  // vertical shift ranges -2 to -1 and 1 to 2 shift the image out of the screen
  float shiftRange = std::min(newHeight, newHeight - (newHeight - height) / 2.0f);
  if (verticalShift > 1.0f)
    posY += shiftRange * (verticalShift - 1.0f);
  else if (verticalShift < -1.0f)
    posY += shiftRange * (verticalShift + 1.0f);

  destRect.x1 = static_cast<float>(MathUtils::round_int(static_cast<double>(posX + offsetX)));
  destRect.x2 = destRect.x1 + MathUtils::round_int(static_cast<double>(newWidth));
  destRect.y1 = static_cast<float>(MathUtils::round_int(static_cast<double>(posY + offsetY)));
  destRect.y2 = destRect.y1 + MathUtils::round_int(static_cast<double>(newHeight));
  return destRect;
}

SRenderAreaOut ComputeRenderArea(const SRenderAreaIn& in)
{
  SRenderAreaOut out;
  CRect& sourceRect = out.sourceRect;
  sourceRect.x1 = 0.0f;
  sourceRect.y1 = 0.0f;
  sourceRect.x2 = (float)in.sourceWidth;
  sourceRect.y2 = (float)in.sourceHeight;

  unsigned int stereo_mode = CONF_FLAGS_STEREO_MODE_MASK(in.flags);
  auto stereo_view = in.stereoView;
  float inputFrameRatio = in.aspectRatio;
  float ratio = sourceRect.x2 / sourceRect.y2;

  if (CONF_FLAGS_STEREO_CADENCE(in.flags) == CONF_FLAGS_STEREO_CADANCE_RIGHT_LEFT)
  {
    if (stereo_view == RenderStereoView::LEFT)
      stereo_view = RenderStereoView::RIGHT;
    else if (stereo_view == RenderStereoView::RIGHT)
      stereo_view = RenderStereoView::LEFT;
  }

  switch (stereo_mode)
  {
    case CONF_FLAGS_STEREO_MODE_TAB:
      if (stereo_view == RenderStereoView::LEFT)
        sourceRect.y2 *= 0.5f;
      else if (stereo_view == RenderStereoView::RIGHT)
        sourceRect.y1 += sourceRect.y2 * 0.5f;

      if (inputFrameRatio == ratio)
        inputFrameRatio *= (sourceRect.y2 / 1080.0f);
      break;

    case CONF_FLAGS_STEREO_MODE_SBS:
      if (stereo_view == RenderStereoView::LEFT)
        sourceRect.x2 *= 0.5f;
      else if (stereo_view == RenderStereoView::RIGHT)
        sourceRect.x1 += sourceRect.x2 * 0.5f;

      if (inputFrameRatio == ratio)
        inputFrameRatio /= (sourceRect.x2 / 1920.0f);
      break;

    default:
      break;
  }

  const float offsetX = in.viewRect.x1;
  const float offsetY = in.viewRect.y1;
  const float width = in.viewRect.Width();
  const float height = in.viewRect.Height();
  out.destRect = CalcDestRect(offsetX, offsetY, width, height, inputFrameRatio * in.pixelRatio,
                              in.zoomAmount, in.verticalShift, in.guiPixelRatio,
                              in.errorInAspect, in.orientation);
  if (height == 0 || width == 0)
    return out;

  if (in.clip)
  {
    CRect original(out.destRect);
    out.destRect.Intersect(CRect(offsetX, offsetY, offsetX + width, offsetY + height));
    if (out.destRect != original)
    {
      float scaleX = sourceRect.Width() / original.Width();
      float scaleY = sourceRect.Height() / original.Height();
      sourceRect.x1 += (out.destRect.x1 - original.x1) * scaleX;
      sourceRect.y1 += (out.destRect.y1 - original.y1) * scaleY;
      sourceRect.x2 += (out.destRect.x2 - original.x2) * scaleX;
      sourceRect.y2 += (out.destRect.y2 - original.y2) * scaleY;
    }
  }
  out.reorder = true;
  return out;
}

CRect AmlRotatedDest(const CRect& destRect, int rotationDegree)
{
  CRect dst_rect = destRect;
  switch (rotationDegree)
  {
    case 0:
    case 2:
      break;

    case 1:
    case 3:
    {
      float scale = static_cast<float>(dst_rect.Height()) / dst_rect.Width();
      int diff = (int)((dst_rect.Height() * scale - dst_rect.Width()) / 2);
      dst_rect = CRect(destRect.x1 - diff, destRect.y1, destRect.x2 + diff, destRect.y2);
    }
  }
  return dst_rect;
}

SAmlAxisOut ComputeAmlAxis(const SAmlAxisIn& in)
{
  SAmlAxisOut out;
  CRect dst_rect = in.dest;
  const CRect& display = in.screen;
  out.gui = CRect(0, 0, in.guiWidth, in.guiHeight);

  if (out.gui != display)
  {
    float xscale = display.Width() / out.gui.Width();
    float yscale = display.Height() / out.gui.Height();
    dst_rect.x1 *= xscale;
    dst_rect.x2 *= xscale;
    dst_rect.y1 *= yscale;
    dst_rect.y2 *= yscale;
  }

  if (in.stereoMode == RenderStereoMode::MONO)
  {
    if (in.videoStereoMode == "left_right" || in.videoStereoMode == "right_left")
      dst_rect.x2 *= 2.0f;
    else if (in.videoStereoMode == "top_bottom" || in.videoStereoMode == "bottom_top")
      dst_rect.y2 *= 2.0f;
  }
  else if (in.stereoMode == RenderStereoMode::SPLIT_VERTICAL)
  {
    dst_rect.x2 *= 2.0f;
  }
  else if (in.stereoMode == RenderStereoMode::SPLIT_HORIZONTAL)
  {
    dst_rect.y2 *= 2.0f;
  }
  else if (in.stereoMode == RenderStereoMode::HARDWAREBASED)
  {
    // 3D frame packed output: the screen height from the graphic context (fullscreen only)
    dst_rect.y2 = in.hwHeight * 2 + in.hwBlanking;
  }
  out.dest = dst_rect;

  // goofy 0/1 based difference in aml axis coordinates
  dst_rect.x2--;
  dst_rect.y2--;
  out.axis[0] = (int)dst_rect.x1;
  out.axis[1] = (int)dst_rect.y1;
  out.axis[2] = (int)dst_rect.x2;
  out.axis[3] = (int)dst_rect.y2;
  return out;
}

} // namespace VIDEOGEOMETRY
