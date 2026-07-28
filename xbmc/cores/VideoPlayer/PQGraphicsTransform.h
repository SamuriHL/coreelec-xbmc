/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
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
//
// PQ is DISPLAY-REFERRED: a code carries an absolute luminance. The decode is
// therefore the true inverse of the downstream encode only if BOTH use the same
// reference white, so the transform resolves the GUI reference white at
// construction from the very source the composite uses
// (CWinSystem::GetGuiSdrPeakLuminance, via PeakFromPQCode) instead of assuming
// one. A hardcoded 80-nit white here against the composite's ~199 amplified
// every overlay ~2.5x in linear light - which merely looked "a bit bright" on
// discs authoring dim menus, but clipped brightly-authored ones (measured up to
// 294 nits) into a desaturated near-white.
namespace PQGRAPHICS
{
/*!
 * \brief Convert a PQ-signal-domain GUI peak code to PQ-normalized luminance.
 *
 * The legacy Amlogic GUI peak (CWinSystem::GetGuiSdrPeakLuminance) is a PQ
 * CODE, not nits. Returns nits/10000, clamped to 1000 nits.
 *
 * Lives here, beside the ST.2084 constants, so the composite shader and this
 * transform cannot drift apart; CGuiCompositeShaderGLES::PeakFromPQCode
 * delegates to it.
 */
double PeakFromPQCode(double code);

/*!
 * \brief Converts pixels authored in BT.2020 PQ to sRGB BT.709.
 *
 * Construct ONCE per composition and reuse it across that composition's pixels
 * or palette entries: the constructor resolves the reference white and bakes it
 * into a decode LUT, so the per-pixel cost is table lookups plus a 3x3 matrix.
 *
 * Constructing per composition rather than caching globally is deliberate - it
 * picks up a live videoscreen.guipeakluminance change on the next overlay,
 * exactly as the composite does, and keeps no shared mutable state between the
 * BD-J graphics thread and the subtitle codec thread.
 */
class CPQGraphicsTransform
{
public:
  CPQGraphicsTransform();

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
  uint32_t Convert(uint32_t px) const;

  /*! \brief The resolved reference white, PQ-normalized (nits / 10000). */
  double ReferenceWhite() const { return m_referenceWhite; }

private:
  double m_referenceWhite;
  //! PQ code [0..255] -> linear light normalized to m_referenceWhite.
  std::array<double, 256> m_pqDecode;
};
} // namespace PQGRAPHICS
