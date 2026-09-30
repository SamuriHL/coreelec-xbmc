/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Geometry.h"

#include <memory>
#include <vector>

class CDVDOverlay;

//! The HDR disc graphics of one GUI frame, for a platform plane that shows
//! them apart from the GUI plane.
struct SHdrGraphics
{
  //! CDVDOverlayImage with m_isHDROverlay, bottom first; never modified once published
  std::vector<std::shared_ptr<CDVDOverlay>> images;
  CRect source, dest, view; //!< the video rects, as for COverlay::CRenderer::SetVideoRect
  float width = 0; //!< the space the rects are in
  float height = 0;
  //! of the video frame they go with; DVD_NOPTS_VALUE without one, or when any
  //! image is shown at once (a disc menu) rather than with the frame
  double pts = 0;
  bool limited = false; //!< limited-range output, as the GUI plane's
};
