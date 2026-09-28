/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

// ST.2084 helpers for PQ graphics.
//
// This file used to hold CPQGraphicsTransform, which pre-inverted PQ-authored
// disc graphics back to sRGB so the downstream encode would apply only once.
// That route was retired: PQ-authored menu and subtitle graphics are now drawn
// as HDR overlays and passed through raw (see OverlayRendererGLES and the OSD
// PQ passthrough), and nothing converts them PQ -> SDR any more. What remains
// is the one conversion the GUI composite still shares with other code.
namespace PQGRAPHICS
{
/*!
 * \brief Convert a PQ-signal-domain GUI peak code to PQ-normalized luminance.
 *
 * The legacy Amlogic GUI peak (CWinSystem::GetGuiSdrPeakLuminance) is a PQ
 * CODE, not nits. Returns nits/10000, clamped to 1000 nits.
 * CGuiCompositeShaderGLES::PeakFromPQCode delegates to it.
 */
double PeakFromPQCode(double code);
} // namespace PQGRAPHICS
