/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PQGraphicsTransform.h"

#include <algorithm>
#include <cmath>

namespace
{
// ST.2084 (PQ) EOTF constants, matching CGuiCompositeShaderGLES.
constexpr double ST2084_m1 = 0.1593017578125;
constexpr double ST2084_m2 = 78.84375;
constexpr double ST2084_c1 = 0.8359375;
constexpr double ST2084_c2 = 18.8515625;
constexpr double ST2084_c3 = 18.6875;
} // namespace

namespace PQGRAPHICS
{
double PeakFromPQCode(double code)
{
  // The legacy Amlogic GUI peak is a PQ CODE, not nits: the scalar-encoded OSD
  // plane is declared FORMAT_HDR8 so the DV core reads it as PQ, which is why
  // the default (0.7*40+30)/100 = 0.58 lands GUI white on ~199 nits, within 2%
  // of the 203-nit BT.2408 reference. Decoding the code makes the same setting
  // mean the same luminance on every path that consumes it.
  //
  // Clamped to 1000 nits, matching the composite: the raw curve reaches 10000
  // nits at the top of the slider, which no panel can show, and stretching a
  // fixed-size LUT that far crushes GUI shadows badly.
  if (code <= 0.0)
    return 0.0;

  const double Em2 = std::pow(std::min(code, 1.0), 1.0 / ST2084_m2);
  const double num = std::max(Em2 - ST2084_c1, 0.0);
  const double den = ST2084_c2 - ST2084_c3 * Em2;
  if (den <= 0.0)
    return 0.1;

  return std::min(std::pow(num / den, 1.0 / ST2084_m1), 0.1);
}
} // namespace PQGRAPHICS
