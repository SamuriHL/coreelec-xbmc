/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// 5.G2 GT-3 gselftest (docs/phase5_gt3_gate.md): G's renderer against an exact CPU reference.
// The reference is today's texel build (CGraphicsPlaneAML::UpdateTextures, with the regime's
// conversion and trim, diag E's P5CpuTexels) placed and blended in integers as the 8-bit target
// stores each draw (diag G's G-X4r):
//   image (ONE, ONE_MINUS_SRC_ALPHA), every channel: out = round(s + d * (255 - sa) / 255)
//   glyph (SRC_ALPHA, ONE_MINUS_SRC_ALPHA; alpha ONE, ONE_MINUS_SRC_ALPHA), k = a * t / 65025:
//     colour out = round(c * k + d * (1 - k)), alpha out = round(255 * k + d * (1 - k))
// A placement scaled by a whole factor is sampled bilinearly at the plane pixel centres, which
// at 2x are the exact weights 1/4 and 3/4. The GPU works in float: the rule allows one code.

#include "GraphicsRasterAML.h"

#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRendererUtil.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <ass/ass.h>

using DISPLAYSTATE::GuiEncoding;

namespace
{
using SEncode = CGraphicsRasterAML::SEncode;
using SRegime = CGraphicsRasterAML::SRegime;

//! synthetic regimes' epochs: never one W or main publishes
constexpr uint64_t SELFTEST_EPOCH = 0xfff0000000000000ULL;

//! today's CPU texel build: the box (width, height, crop) and its R, G, B, A bytes
void CpuTexels(const CDVDOverlayImage& o,
               const SEncode& e,
               int& width,
               int& height,
               CRect& crop,
               std::vector<uint8_t>& rgba)
{
  std::vector<uint32_t> packed;
  crop = CRect();
  if (o.palette.empty())
  {
    width = o.width;
    height = o.height;
    packed.resize(static_cast<size_t>(o.width) * o.height);
    // as COverlayTextureGLES: each distinct colour converted once
    std::unordered_map<uint32_t, uint32_t> converted;
    std::vector<uint32_t> one(1);
    for (int y = 0; y < o.height; y++)
    {
      const auto* row = reinterpret_cast<const uint32_t*>(o.pixels.data() + y * o.linesize);
      for (int x = 0; x < o.width; x++)
      {
        uint32_t px = row[x];
        if (e.toSrgb && ((px >> PIXEL_ASHIFT) & 0xff))
        {
          auto it = converted.find(px);
          if (it == converted.end())
          {
            one[0] = px;
            OVERLAY::ConvertPQPaletteToSRGB(one);
            it = converted.emplace(px, one[0]).first;
          }
          px = it->second;
        }
        const uint32_t a = (px >> PIXEL_ASHIFT) & 0xff;
        const uint32_t r = (((px >> PIXEL_RSHIFT) & 0xff) * a + 127) / 255;
        const uint32_t g = (((px >> PIXEL_GSHIFT) & 0xff) * a + 127) / 255;
        const uint32_t b = (((px >> PIXEL_BSHIFT) & 0xff) * a + 127) / 255;
        packed[static_cast<size_t>(y) * o.width + x] = (a << PIXEL_ASHIFT) | (r << PIXEL_RSHIFT) |
                                                       (g << PIXEL_GSHIFT) | (b << PIXEL_BSHIFT);
      }
    }
  }
  else
  {
    uint32_t lut[256];
    if (e.toSrgb)
    {
      std::vector<uint32_t> palette = o.palette;
      OVERLAY::ConvertPQPaletteToSRGB(palette);
      OVERLAY::BuildRGBALut(palette, true, lut);
    }
    else
      OVERLAY::BuildRGBALut(o.palette, true, lut);
    int x0 = 0;
    int y0 = 0;
    int x1 = o.width;
    int y1 = o.height;
    OVERLAY::FindVisibleBox(o.pixels.data(), o.linesize, o.width, o.height, lut, x0, y0, x1, y1);
    width = x1 - x0;
    height = y1 - y0;
    if (width != o.width || height != o.height)
      crop = CRect(x0, y0, x1, y1);
    packed.resize(static_cast<size_t>(width) * height);
    OVERLAY::ConvertIndices(o.pixels.data() + y0 * o.linesize + x0, o.linesize, width, height, lut,
                            packed.data());
  }

  rgba.resize(packed.size() * 4);
  for (size_t i = 0; i < packed.size(); i++)
  {
    const uint32_t px = packed[i];
    const uint32_t a = (px >> PIXEL_ASHIFT) & 0xff;
    uint32_t c[3] = {(px >> PIXEL_RSHIFT) & 0xff, (px >> PIXEL_GSHIFT) & 0xff,
                     (px >> PIXEL_BSHIFT) & 0xff};
    for (uint32_t& v : c)
    {
      if (e.limited)
        v = (v * 219 + a * 16 + 127) / 255;
      if (e.scalar)
        v = std::min<uint32_t>(a, static_cast<uint32_t>(std::lround(v * e.peak)));
    }
    rgba[i * 4 + 0] = static_cast<uint8_t>(c[0]);
    rgba[i * 4 + 1] = static_cast<uint8_t>(c[1]);
    rgba[i * 4 + 2] = static_cast<uint8_t>(c[2]);
    rgba[i * 4 + 3] = static_cast<uint8_t>(a);
  }
}

//! r is the box scaled by a whole factor (1 = 1:1) from a whole plane pixel; x, y its origin
bool WholeScale(const CRect& r, int width, int height, int& scale, int& x, int& y)
{
  if (r.IsEmpty() || width <= 0 || height <= 0)
    return false;
  const int s = static_cast<int>(std::lround(r.Width() / width));
  if (s < 1 || std::lround(r.Height() / height) != s)
    return false;
  const float eps = CGraphicsRasterAML::ALIGN_EPS;
  const float rx = std::round(r.x1);
  const float ry = std::round(r.y1);
  if (std::fabs(r.Width() - static_cast<float>(s * width)) > eps ||
      std::fabs(r.Height() - static_cast<float>(s * height)) > eps ||
      std::fabs(r.x1 - rx) > eps || std::fabs(r.y1 - ry) > eps)
    return false;
  scale = s;
  x = static_cast<int>(rx);
  y = static_cast<int>(ry);
  return true;
}

//! bilinear (clamp to edge) of channel c of a w x h buffer of n channels at texel-space X, Y
double Sample(const uint8_t* data, int w, int h, int n, int c, double X, double Y)
{
  const double x = X - 0.5;
  const double y = Y - 0.5;
  const int i0 = static_cast<int>(std::floor(x));
  const int j0 = static_cast<int>(std::floor(y));
  const double fx = x - i0;
  const double fy = y - j0;
  const auto at = [&](int i, int j)
  {
    i = std::clamp(i, 0, w - 1);
    j = std::clamp(j, 0, h - 1);
    return static_cast<double>(data[(static_cast<size_t>(j) * w + i) * n + c]);
  };
  return (at(i0, j0) * (1 - fx) + at(i0 + 1, j0) * fx) * (1 - fy) +
         (at(i0, j0 + 1) * (1 - fx) + at(i0 + 1, j0 + 1) * fx) * fy;
}

uint8_t Round8(double v)
{
  return static_cast<uint8_t>(std::clamp(std::floor(v + 0.5), 0.0, 255.0));
}

std::string RegimeName(const SRegime& r)
{
  return StringUtils::Format("{} limited={} peak={:.2f}", DISPLAYSTATE::GuiEncodingName(r.encoding),
                             r.limited ? 1 : 0, r.peak);
}

// the 40-case images of diag E: ARGB with alpha ramps over every code, colour sweeps (outside
// BT.709 and above reference white as PQ) and a padded row; a PGS palette image with a
// transparent border to crop
std::shared_ptr<CDVDOverlayImage> MakeArgb()
{
  auto argb = std::make_shared<CDVDOverlayImage>();
  argb->width = 512;
  argb->height = 256;
  argb->linesize = 512 * 4 + 64;
  argb->pixels.assign(static_cast<size_t>(argb->linesize) * argb->height, 0);
  for (int y = 0; y < argb->height; y++)
    for (int x = 0; x < argb->width; x++)
    {
      const uint32_t a = x < 256 ? static_cast<uint32_t>(x) : 255;
      const uint32_t r = x < 256 ? static_cast<uint32_t>(y) : static_cast<uint32_t>(x - 256);
      const uint32_t g = x < 256 ? static_cast<uint32_t>(255 - y) : static_cast<uint32_t>(y);
      const uint32_t b = x < 256 ? static_cast<uint32_t>((x * 3 + y) & 0xff)
                                 : static_cast<uint32_t>(((x - 256) ^ y) & 0xff);
      const uint32_t px =
          (a << PIXEL_ASHIFT) | (r << PIXEL_RSHIFT) | (g << PIXEL_GSHIFT) | (b << PIXEL_BSHIFT);
      std::memcpy(argb->pixels.data() + static_cast<size_t>(y) * argb->linesize + x * 4, &px, 4);
    }
  return argb;
}

std::shared_ptr<CDVDOverlayImage> MakePgs()
{
  auto pal = std::make_shared<CDVDOverlayImage>();
  pal->m_isPGS = true;
  pal->width = 320;
  pal->height = 200;
  pal->linesize = 336;
  pal->pixels.assign(static_cast<size_t>(pal->linesize) * pal->height, 0);
  for (int y = 20; y < pal->height - 20; y++)
    for (int x = 20; x < pal->width - 20; x++)
      pal->pixels[static_cast<size_t>(y) * pal->linesize + x] =
          static_cast<uint8_t>((x + y * 7) % 255 + 1);
  pal->palette.assign(256, 0);
  for (uint32_t i = 1; i < 256; i++)
  {
    const uint32_t a = i < 128 ? i * 2 : 255;
    pal->palette[i] = (a << PIXEL_ASHIFT) | (((i * 53) & 0xff) << PIXEL_RSHIFT) |
                      (((i * 101) & 0xff) << PIXEL_GSHIFT) | (((i * 197) & 0xff) << PIXEL_BSHIFT);
  }
  return pal;
}

//! libass-shaped glyphs (coverage ramps and hard edges, overlapping, one half-transparent),
//! through today's convert_quad, as COverlayGlyphGLES gets them
std::shared_ptr<const OVERLAY::SQuads> MakeGlyphs(int frameWidth, int originX, int originY)
{
  constexpr int COUNT = 8;
  std::vector<std::vector<uint8_t>> bitmaps(COUNT);
  std::vector<ASS_Image> images(COUNT);
  for (int g = 0; g < COUNT; g++)
  {
    const int w = 30 + 5 * g;
    const int h = 40 + 3 * g;
    bitmaps[g].assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
      {
        const double dx = (x + 0.5 - w / 2.0) / (w / 2.0);
        const double dy = (y + 0.5 - h / 2.0) / (h / 2.0);
        if (dx * dx + dy * dy <= 1.0)
          bitmaps[g][static_cast<size_t>(y) * w + x] =
              static_cast<uint8_t>(x < w / 3 ? 255 : (x * 37 + y * 11 + g * 50) & 0xff);
      }
    ASS_Image& img = images[g];
    std::memset(&img, 0, sizeof(img));
    img.w = w;
    img.h = h;
    img.stride = w;
    img.bitmap = bitmaps[g].data();
    // RGBA with the alpha inverted (0 opaque)
    const uint32_t r = (g * 97 + 40) & 0xff;
    const uint32_t gr = (g * 151 + 200) & 0xff;
    const uint32_t b = (g * 59 + 120) & 0xff;
    img.color = (r << 24) | (gr << 16) | (b << 8) | (g == 5 ? 0x80u : 0u);
    img.dst_x = originX + g * 25;
    img.dst_y = originY + (g % 2) * 10;
    img.next = g + 1 < COUNT ? &images[g + 1] : nullptr;
  }
  auto quads = std::make_shared<OVERLAY::SQuads>();
  if (!OVERLAY::convert_quad(images.data(), *quads, frameWidth))
    return nullptr;
  return quads;
}

CRect PlaceAt(const CGraphicsRasterAML::STexture& t, const CDVDOverlayImage& o, int x, int y, int s)
{
  if (t.crop.IsEmpty())
    return CRect(x, y, x + o.width * s, y + o.height * s);
  return CRect(x + t.crop.x1 * s, y + t.crop.y1 * s, x + t.crop.x2 * s, y + t.crop.y2 * s);
}
} // namespace

bool CGraphicsRasterAML::EnsureCheckTarget()
{
  if (m_checkFbo)
    return true;
  glGenTextures(1, &m_checkTexture);
  glBindTexture(GL_TEXTURE_2D, m_checkTexture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &m_checkFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_checkFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_checkTexture, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status == GL_FRAMEBUFFER_COMPLETE)
    return true;
  CLog::Log(LOGERROR, "GSELFTEST check target incomplete ({:#x})", status);
  glDeleteFramebuffers(1, &m_checkFbo);
  glDeleteTextures(1, &m_checkTexture);
  m_checkFbo = m_checkTexture = 0;
  return false;
}

GLuint CGraphicsRasterAML::DrawCheck(const std::vector<SImage>& images,
                                     const std::vector<SGlyphs>& glyphs)
{
  if (!EnsureCheckTarget())
    return 0;
  glBindFramebuffer(GL_FRAMEBUFFER, m_checkFbo);
  glViewport(0, 0, m_width, m_height);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  Draw(images, glyphs, m_checkFbo);
  return m_checkFbo;
}

bool CGraphicsRasterAML::CheckPass1(const CDVDOverlayImage& o,
                                    const SEncode& e,
                                    const std::string& name)
{
  int width = 0;
  int height = 0;
  CRect crop;
  std::vector<uint8_t> want;
  CpuTexels(o, e, width, height, crop, want);
  STexture gpu;
  if (!Pass1(o, e, gpu))
  {
    CLog::Log(LOGINFO, "GSELFTEST case={} regime={} maxdiff=- alpha=- FAIL why=no_pass1", name,
              RegimeName(m_regime));
    return false;
  }
  std::vector<uint8_t> got(static_cast<size_t>(gpu.width) * gpu.height * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, m_convFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu.texture, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, gpu.width, gpu.height, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteTextures(1, &gpu.texture);

  // a texel's row in the read-back is its row in the image: pass 1 draws row y at GL y
  const bool sameBox = gpu.width == width && gpu.height == height && gpu.crop == crop &&
                       got.size() == want.size();
  int maxDelta = 0;
  int maxAlpha = 0;
  int64_t diffBytes = 0;
  for (size_t i = 0; sameBox && i < want.size(); i++)
  {
    const int delta = std::abs(static_cast<int>(want[i]) - static_cast<int>(got[i]));
    if (!delta)
      continue;
    diffBytes++;
    if (i % 4 == 3)
      maxAlpha = std::max(maxAlpha, delta);
    else if (want[i / 4 * 4 + 3] > 0)
      maxDelta = std::max(maxDelta, delta);
  }
  const bool pass = sameBox && maxDelta <= 1 && maxAlpha <= 1;
  CLog::Log(LOGINFO, "GSELFTEST case={} regime={} maxdiff={} alpha={} {} box={} px={} diff_bytes={}",
            name, RegimeName(m_regime), maxDelta, maxAlpha, pass ? "PASS" : "FAIL",
            sameBox ? "same" : "DIFFERS", static_cast<int64_t>(width) * height, diffBytes);
  return pass;
}

bool CGraphicsRasterAML::CheckLayer(const std::string& name,
                                    const char* layer,
                                    GLuint fbo,
                                    const std::vector<const CDVDOverlayImage*>& images,
                                    const std::vector<SImage>& placed,
                                    const std::vector<SGlyphs>& glyphs)
{
  const bool composited = std::strcmp(layer, "fbo") == 0;
  const bool composite = m_regime.encoding == GuiEncoding::Composite;
  std::vector<uint8_t> ref(static_cast<size_t>(m_width) * m_height * 4, 0);
  const char* why = nullptr;
  std::string placements;
  const auto note = [&placements](int scale)
  {
    const std::string p = scale == 1 ? "1to1" : std::to_string(scale) + "x";
    if (placements.find(p) == std::string::npos)
      placements += (placements.empty() ? "" : ",") + p;
  };

  for (size_t n = 0; n < placed.size() && n < images.size(); n++)
  {
    const STexture* t = placed[n].texture;
    if (!t || !t->texture || t->encode.composited != composited || placed[n].rect.IsEmpty())
      continue;
    int width = 0;
    int height = 0;
    CRect crop;
    std::vector<uint8_t> rgba;
    CpuTexels(*images[n], t->encode, width, height, crop, rgba);
    if (width != t->width || height != t->height || !(crop == t->crop))
    {
      why = "box";
      continue;
    }
    int s = 0;
    int x0 = 0;
    int y0 = 0;
    if (!WholeScale(placed[n].rect, width, height, s, x0, y0))
    {
      why = "scaled";
      continue;
    }
    note(s);
    for (int j = 0; j < height * s; j++)
    {
      const int py = y0 + j;
      if (py < 0 || py >= m_height)
        continue;
      for (int i = 0; i < width * s; i++)
      {
        const int px = x0 + i;
        if (px < 0 || px >= m_width)
          continue;
        double src[4];
        for (int c = 0; c < 4; c++)
          src[c] = s == 1 ? rgba[(static_cast<size_t>(j) * width + i) * 4 + c]
                          : Sample(rgba.data(), width, height, 4, c, (i + 0.5) / s, (j + 0.5) / s);
        uint8_t* d = &ref[(static_cast<size_t>(py) * m_width + px) * 4];
        for (int c = 0; c < 4; c++)
          d[c] = Round8(src[c] + d[c] * (255.0 - src[3]) / 255.0);
      }
    }
  }

  // glyphs: in the pre-encode buffer in Composite, on the plane otherwise
  if (composited == composite)
  {
    const SEncode e = GlyphEncodeFor(m_regime);
    for (const SGlyphs& item : glyphs)
    {
      const OVERLAY::SQuads* quads = item.quads.get();
      if (!quads || quads->quad.empty() || item.frameWidth <= 0 || item.frameHeight <= 0)
        continue;
      int s = 0;
      int ox = 0;
      int oy = 0;
      if (!WholeScale(item.view, static_cast<int>(std::lround(item.frameWidth)),
                      static_cast<int>(std::lround(item.frameHeight)), s, ox, oy))
      {
        why = "glyphs_scaled";
        continue;
      }
      note(s);
      for (const OVERLAY::SQuad& q : quads->quad)
      {
        double colour[3] = {static_cast<double>(q.r), static_cast<double>(q.g),
                            static_cast<double>(q.b)};
        for (double& c : colour)
        {
          if (e.limited)
            c = c * 219.0 / 255.0 + 16.0;
          if (e.scalar)
            c *= e.peak;
        }
        for (int j = 0; j < q.h * s; j++)
        {
          const int py = oy + q.y * s + j;
          if (py < 0 || py >= m_height)
            continue;
          for (int i = 0; i < q.w * s; i++)
          {
            const int px = ox + q.x * s + i;
            if (px < 0 || px >= m_width)
              continue;
            const double t =
                s == 1 ? quads->texture[static_cast<size_t>(q.v + j) * quads->size_x + q.u + i]
                       : Sample(quads->texture.data(), quads->size_x, quads->size_y, 1, 0,
                                q.u + (i + 0.5) / s, q.v + (j + 0.5) / s);
            const double k = q.a * t / 65025.0;
            uint8_t* d = &ref[(static_cast<size_t>(py) * m_width + px) * 4];
            for (int c = 0; c < 3; c++)
              d[c] = Round8(colour[c] * k + d[c] * (1.0 - k));
            d[3] = Round8(255.0 * k + d[3] * (1.0 - k));
          }
        }
      }
    }
  }

  // G's buffer, rows from the bottom
  std::vector<uint8_t> got(ref.size());
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  while (glGetError() != GL_NO_ERROR)
  {
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, got.data());
  const GLenum error = glGetError();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  int maxDelta = 0;
  int maxAlpha = 0;
  int64_t diffPx = 0;
  int64_t drawnPx = 0;
  int64_t gotPx = 0;
  std::string first = "-";
  for (int y = 0; y < m_height; y++)
    for (int x = 0; x < m_width; x++)
    {
      const uint8_t* want = &ref[(static_cast<size_t>(y) * m_width + x) * 4];
      const uint8_t* have = &got[(static_cast<size_t>(m_height - 1 - y) * m_width + x) * 4];
      drawnPx += want[3] != 0;
      gotPx += have[3] != 0;
      bool differs = false;
      for (int c = 0; c < 4; c++)
      {
        const int delta = std::abs(static_cast<int>(want[c]) - static_cast<int>(have[c]));
        if (!delta)
          continue;
        differs = true;
        if (c == 3)
          maxAlpha = std::max(maxAlpha, delta);
        else
          maxDelta = std::max(maxDelta, delta);
      }
      if (differs && diffPx++ == 0)
        first = StringUtils::Format("{},{} ref={},{},{},{} got={},{},{},{}", x, y, want[0],
                                    want[1], want[2], want[3], have[0], have[1], have[2],
                                    have[3]);
    }

  const char* verdict = "PASS";
  if (error != GL_NO_ERROR)
  {
    verdict = "FAIL";
    why = "gl_error";
  }
  else if (why)
    verdict = "SKIP";
  else if (!drawnPx || !gotPx)
  {
    verdict = "FAIL";
    why = "empty";
  }
  else if (maxDelta > 1 || maxAlpha > 1)
    verdict = "FAIL";
  CLog::Log(LOGINFO,
            "GSELFTEST case={} regime={} maxdiff={} alpha={} {} why={} layer={} placement={} "
            "drawn_px={} got_px={} diff_px={} first={}",
            name, RegimeName(m_regime), maxDelta, maxAlpha, verdict, why ? why : "-", layer,
            placements.empty() ? "-" : placements, drawnPx, gotPx, diffPx, first);
  return std::strcmp(verdict, "FAIL") != 0;
}

bool CGraphicsRasterAML::CheckComposition(const std::string& name,
                                          const std::vector<std::shared_ptr<CDVDOverlay>>& images,
                                          const std::vector<SImage>& placed,
                                          const std::vector<SGlyphs>& glyphs)
{
  std::vector<const CDVDOverlayImage*> raw;
  raw.reserve(images.size());
  for (const auto& image : images)
    raw.push_back(static_cast<const CDVDOverlayImage*>(image.get()));
  const bool srgbItems =
      !glyphs.empty() || std::any_of(placed.begin(), placed.end(), [](const SImage& image)
                                     { return image.texture && image.texture->encode.composited; });
  if (m_regime.encoding == GuiEncoding::Composite && srgbItems)
  {
    // the encoded output has no exact CPU reference: the buffer the composite pass reads is
    // checked, and the PQ images over the output only when there are none
    bool pass = CheckLayer(name, "fbo", m_preFbo, raw, placed, glyphs);
    const bool pq = std::any_of(placed.begin(), placed.end(), [](const SImage& image)
                                { return image.texture && !image.texture->encode.composited; });
    if (pq)
      CLog::Log(LOGINFO,
                "GSELFTEST case={} regime={} maxdiff=- alpha=- SKIP why=composite_encode "
                "layer=plane",
                name, RegimeName(m_regime));
    return pass;
  }
  return CheckLayer(name, "plane", m_checkFbo, raw, placed, glyphs);
}

void CGraphicsRasterAML::SelfTestPlane(int& total)
{
  auto pgs = MakePgs();
  pgs->m_isHDROverlay = true;
  auto menu = MakeArgb();
  menu->m_isHDROverlay = true;
  // a 1080p frame at 1:1, and a 720x480 frame at 2x (an SD video's libass frame)
  const auto glyphs1 = MakeGlyphs(1920, 600, 700);
  const auto glyphs2 = MakeGlyphs(720, 40, 300);

  struct SRegimeCase
  {
    GuiEncoding encoding;
    float peak;
  };
  const SRegimeCase regimes[] = {
      {GuiEncoding::Srgb, 1.0f}, {GuiEncoding::Scalar, 0.58f}, {GuiEncoding::Composite, 0.58f}};
  uint64_t epoch = SELFTEST_EPOCH + 0x1000;
  for (const SRegimeCase& rc : regimes)
  {
    SRegime regime;
    regime.epoch = ++epoch;
    regime.encoding = rc.encoding;
    regime.peak = rc.peak;
    SetRegime(regime);
    for (const int scale : {1, 2})
    {
      const std::string placement = scale == 1 ? "1to1" : "2x";
      for (const char* kind : {"pgs", "menu", "ass"})
      {
        std::vector<std::shared_ptr<CDVDOverlay>> images;
        std::vector<SImage> placed;
        std::vector<SGlyphs> glyphs;
        if (std::strcmp(kind, "ass") == 0)
        {
          SGlyphs g;
          g.quads = scale == 1 ? glyphs1 : glyphs2;
          g.frameWidth = scale == 1 ? 1920.0f : 720.0f;
          g.frameHeight = scale == 1 ? 1080.0f : 480.0f;
          g.view = scale == 1 ? CRect(0, 0, 1920, 1080) : CRect(240, 60, 1680, 1020);
          if (g.quads)
            glyphs.push_back(g);
        }
        else
        {
          const auto& o = std::strcmp(kind, "pgs") == 0 ? pgs : menu;
          const STexture* t = Expand(o);
          if (t)
          {
            images.push_back(o);
            placed.push_back({t, PlaceAt(*t, *o, scale == 1 ? 801 : 200, scale == 1 ? 403 : 100,
                                         scale)});
          }
        }
        total++;
        const std::string name = "plane/" + std::string(kind) + "/" + placement;
        if (images.empty() && glyphs.empty())
        {
          CLog::Log(LOGINFO, "GSELFTEST case={} regime={} maxdiff=- alpha=- FAIL why=no_content",
                    name, RegimeName(m_regime));
          m_selfTestFailed++;
          continue;
        }
        if (!DrawCheck(placed, glyphs) || !CheckComposition(name, images, placed, glyphs))
          m_selfTestFailed++;
        Retain({}, {});
      }
    }
  }
}

void CGraphicsRasterAML::SelfTest()
{
  if (!m_ready)
    return;
  const auto start = std::chrono::steady_clock::now();
  m_selfTestFailed = 0;
  int total = 0;

  auto argb = MakeArgb();
  auto pgs = MakePgs();
  struct SCase
  {
    GuiEncoding encoding;
    float peak;
  };
  // Composite's peak is the composite pass's, never pass 1's: its two cases show none leaks in
  const SCase cases[] = {{GuiEncoding::Srgb, 1.0f},
                         {GuiEncoding::Scalar, 0.58f},
                         {GuiEncoding::Scalar, 1.0f},
                         {GuiEncoding::Composite, 1.0f},
                         {GuiEncoding::Composite, 0.58f}};
  uint64_t epoch = SELFTEST_EPOCH;
  for (const auto& image : {argb, pgs})
    for (const bool pq : {true, false})
    {
      image->m_isHDROverlay = pq;
      for (const SCase& c : cases)
        for (const bool limited : {false, true})
        {
          SRegime regime;
          regime.epoch = ++epoch;
          regime.encoding = c.encoding;
          regime.limited = limited;
          regime.peak = c.peak;
          SetRegime(regime);
          total++;
          const std::string name = StringUtils::Format(
              "p1/{}/{}/{}/peak={:.2f}", image->palette.empty() ? "argb" : "pgs",
              pq ? "pq" : "srgb", limited ? "limited" : "full", c.peak);
          if (!CheckPass1(*image, EncodeFor(*image, regime), name))
            m_selfTestFailed++;
        }
    }

  SelfTestPlane(total);

  ClearCache();
  m_regimeSet = false;
  CLog::Log(LOGINFO, "GSELFTEST summary cases={} failed={} took_ms={} {}", total,
            m_selfTestFailed,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count(),
            m_selfTestFailed ? "FAIL" : "PASS");
}
