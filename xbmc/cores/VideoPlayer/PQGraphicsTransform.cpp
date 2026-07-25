/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PQGraphicsTransform.h"

#include "PlatformDefs.h"

#include <array>
#include <cmath>

namespace
{
// ST.2084 (PQ) EOTF constants, matching CGuiCompositeShaderGLES so this decode
// is the exact inverse of the composite's encode.
constexpr double ST2084_m1 = 0.1593017578125;
constexpr double ST2084_m2 = 78.84375;
constexpr double ST2084_c1 = 0.8359375;
constexpr double ST2084_c2 = 18.8515625;
constexpr double ST2084_c3 = 18.6875;

// Authored graphics reference white, PQ-normalized (nits/10000). 80 nits = the
// IEC 61966-2-1 sRGB reference display, and the measured fit for BD-J menus on
// the discs this has been validated against (Superman, Batman Begins). Tuning
// this only trims graphics luminance - hue is peak-independent, saturation
// nearly so.
//
// It is deliberately shared by BD-J overlays and PG palettes: both are authored
// by the same house to the same reference on a given disc, so a single constant
// keeps the two graphics layers consistent with each other. Splitting them
// would let a disc's menu and its subtitles disagree.
constexpr double GRAPHICS_WHITE = 80.0 / 10000.0;

// BT.2020 -> BT.709 (numeric inverse of the composite's printed bt709_to_bt2020).
constexpr double bt2020_to_bt709[3][3] = {
    {1.660511, -0.587711, -0.072801},
    {-0.124561, 1.132961, -0.008399},
    {-0.018168, -0.100561, 1.118728},
};

// PQ code [0..255] -> linear light normalized to the authored graphics white.
const std::array<double, 256>& pq_decode_lut()
{
  static const std::array<double, 256> lut = []
  {
    std::array<double, 256> t{};
    for (int i = 0; i < 256; ++i)
    {
      const double N = std::pow(i / 255.0, 1.0 / ST2084_m2);
      const double L =
          std::pow(std::max(N - ST2084_c1, 0.0) / (ST2084_c2 - ST2084_c3 * N), 1.0 / ST2084_m1);
      const double rel = L / GRAPHICS_WHITE;
      t[i] = rel < 0.0 ? 0.0 : (rel > 1.0 ? 1.0 : rel);
    }
    return t;
  }();
  return lut;
}

// linear light [0..1] -> sRGB 8-bit (IEC 61966-2-1 OETF), LUT to avoid per-pixel
// pow on full-screen BD-J redraws.
const std::array<uint8_t, 1025>& srgb_oetf_lut()
{
  static const std::array<uint8_t, 1025> lut = []
  {
    std::array<uint8_t, 1025> t{};
    for (int i = 0; i <= 1024; ++i)
    {
      const double l = i / 1024.0;
      const double s = l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
      const int v = static_cast<int>(s * 255.0 + 0.5);
      t[i] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    return t;
  }();
  return lut;
}

inline uint8_t oetf8(double l)
{
  l = l < 0.0 ? 0.0 : (l > 1.0 ? 1.0 : l);
  return srgb_oetf_lut()[static_cast<int>(l * 1024.0 + 0.5)];
}
} // namespace

namespace PQGRAPHICS
{
uint32_t PQ2020ToSrgb709(uint32_t px)
{
  const auto& dec = pq_decode_lut();
  const double c0 = dec[(px >> PIXEL_RSHIFT) & 0xffu];
  const double c1 = dec[(px >> PIXEL_GSHIFT) & 0xffu];
  const double c2 = dec[(px >> PIXEL_BSHIFT) & 0xffu];
  const double r =
      bt2020_to_bt709[0][0] * c0 + bt2020_to_bt709[0][1] * c1 + bt2020_to_bt709[0][2] * c2;
  const double g =
      bt2020_to_bt709[1][0] * c0 + bt2020_to_bt709[1][1] * c1 + bt2020_to_bt709[1][2] * c2;
  const double b =
      bt2020_to_bt709[2][0] * c0 + bt2020_to_bt709[2][1] * c1 + bt2020_to_bt709[2][2] * c2;
  return (px & (0xffu << PIXEL_ASHIFT)) |
         (static_cast<uint32_t>(oetf8(r)) << PIXEL_RSHIFT) |
         (static_cast<uint32_t>(oetf8(g)) << PIXEL_GSHIFT) |
         (static_cast<uint32_t>(oetf8(b)) << PIXEL_BSHIFT);
}
} // namespace PQGRAPHICS
