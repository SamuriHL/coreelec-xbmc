/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <cstdint>

// Inverse of the forward sRGB -> BT.2020 -> PQ encode that the OSD plane
// receives downstream (either CGuiCompositeShaderGLES, or the Amlogic VPP's
// OSD1_HDR stage when the DV core is idle).
//
// UHD Blu-ray discs author their graphics assets - BD-J overlays and PG
// (subtitle) palettes alike - in BT.2020 with the ST.2084 transfer function.
// Kodi's graphics pipeline treats every overlay as sRGB, so on an HDR title
// those assets reach the plane already PQ-encoded and are then encoded a
// SECOND time. The result is gamut compressed twice (desaturated) and, because
// a PQ code is numerically much smaller than the sRGB value it gets mistaken
// for, badly crushed: measured on-box, subtitle white landed near 29 nits
// instead of ~203.
//
// Pre-inverting the asset back to sRGB BT.709 leaves exactly one forward encode
// in the chain, which is what the downstream stage expects.
namespace PQGRAPHICS
{
/*!
 * \brief Convert one packed pixel authored in BT.2020 PQ to sRGB BT.709.
 *
 * Input and output are in the platform's PIXEL_ASHIFT/RSHIFT/GSHIFT/BSHIFT
 * layout. Alpha is passed through untouched.
 *
 * Must be applied to NON-premultiplied values: PQ decoding is non-linear, so a
 * premultiplied pixel decodes to the wrong luminance. For subtitle palettes
 * that means transforming at decode time, before OVERLAY::build_rgba() folds
 * alpha in.
 */
uint32_t PQ2020ToSrgb709(uint32_t px);
} // namespace PQGRAPHICS
