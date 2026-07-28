/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PQGraphicsTransform.h"

#include "PlatformDefs.h"
#include "ServiceBroker.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <atomic>
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

// BT.2408 reference white, the fallback when no windowing system can supply a
// GUI peak (non-Amlogic platforms return 0 from the base GetGuiSdrPeakLuminance).
constexpr double BT2408_REFERENCE_WHITE = 203.0 / 10000.0;

// BT.2020 -> BT.709 (numeric inverse of the composite's printed bt709_to_bt2020).
constexpr double bt2020_to_bt709[3][3] = {
    {1.660511, -0.587711, -0.072801},
    {-0.124561, 1.132961, -0.008399},
    {-0.018168, -0.100561, 1.118728},
};

// linear light [0..1] -> sRGB 8-bit (IEC 61966-2-1 OETF), LUT to avoid per-pixel
// pow on full-screen BD-J redraws. Independent of the reference white, so it is
// built once and shared.
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

//! Resolve the GUI reference white the downstream encode is actually using.
double ResolveReferenceWhite()
{
  double peak = 0.0;

  if (CWinSystemBase* winSystem = CServiceBroker::GetWinSystem())
    peak = PQGRAPHICS::PeakFromPQCode(winSystem->GetGuiSdrPeakLuminance());

  if (!(peak > 0.0))
    peak = BT2408_REFERENCE_WHITE;

  // Log once, and again whenever it changes, so a field log always states the
  // white the overlays were actually inverted against - the single number that
  // decides whether brightly-authored menus clip.
  static std::atomic<double> lastLogged{-1.0};
  if (lastLogged.exchange(peak) != peak)
    CLog::Log(LOGDEBUG, "PQGraphicsTransform - GUI reference white {:.1f} nits", peak * 10000.0);

  return peak;
}
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

CPQGraphicsTransform::CPQGraphicsTransform() : m_referenceWhite(ResolveReferenceWhite())
{
  for (int i = 0; i < 256; ++i)
  {
    const double N = std::pow(i / 255.0, 1.0 / ST2084_m2);
    const double L =
        std::pow(std::max(N - ST2084_c1, 0.0) / (ST2084_c2 - ST2084_c3 * N), 1.0 / ST2084_m1);
    const double rel = L / m_referenceWhite;
    m_pqDecode[i] = rel < 0.0 ? 0.0 : (rel > 1.0 ? 1.0 : rel);
  }
}

uint32_t CPQGraphicsTransform::Convert(uint32_t px) const
{
  const double c0 = m_pqDecode[(px >> PIXEL_RSHIFT) & 0xffu];
  const double c1 = m_pqDecode[(px >> PIXEL_GSHIFT) & 0xffu];
  const double c2 = m_pqDecode[(px >> PIXEL_BSHIFT) & 0xffu];
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
