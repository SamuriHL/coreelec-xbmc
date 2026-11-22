/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsRasterAML.h"

#include "cores/VideoPlayer/DVDCodecs/Overlay/DVDOverlayImage.h"
#include "cores/VideoPlayer/VideoRenderers/OverlayRendererUtil.h"
#include "rendering/gles/GuiCompositeShaderGLES.h"
#include "utils/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C"
{
#include <libavutil/pixfmt.h>
}

using DISPLAYSTATE::GuiEncoding;

namespace
{
GLuint Compile(GLenum type, const char* source, std::string& why)
{
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    why = log;
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

GLuint Link(const char* vertexSource, const char* fragmentSource, std::string& why)
{
  const GLuint vertex = Compile(GL_VERTEX_SHADER, vertexSource, why);
  const GLuint fragment = vertex ? Compile(GL_FRAGMENT_SHADER, fragmentSource, why) : 0;
  GLuint program = 0;
  GLint linked = GL_FALSE;
  if (vertex && fragment)
  {
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
      char log[512] = {};
      glGetProgramInfoLog(program, sizeof(log), nullptr, log);
      why = log;
      glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex)
    glDeleteShader(vertex);
  if (fragment)
    glDeleteShader(fragment);
  return program;
}

GLuint NewTexture(GLint filter)
{
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  return texture;
}

//! the texture bound to unit 0 takes this filter when it has another
void SetFilter(bool nearest, bool& current)
{
  if (nearest == current)
    return;
  const GLint filter = nearest ? GL_NEAREST : GL_LINEAR;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  current = nearest;
}

//! one premultiplied LUT entry (PIXEL_*SHIFT) to its final bytes R, G, B, A in memory order
uint32_t EncodeEntry(uint32_t px, const CGraphicsRasterAML::SEncode& e)
{
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
  const uint8_t bytes[4] = {static_cast<uint8_t>(c[0]), static_cast<uint8_t>(c[1]),
                            static_cast<uint8_t>(c[2]), static_cast<uint8_t>(a)};
  uint32_t out = 0;
  std::memcpy(&out, bytes, 4);
  return out;
}

const char* PASS1_VERTEX = "attribute vec2 a_pos;\n"
                           "void main()\n"
                           "{\n"
                           "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                           "}\n";

// gl_FragCoord.xy is the centre of the target texel, which is the image texel. Each rounding
// step is an explicit floor(x + 0.5) on integer codes, as the CPU's integer maths; mediump
// (fp16, about +-0.12 code at 255) is not enough for that, so no highp is a compile failure
const char* PASS1_ARGB =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "#error no highp float in the fragment shader\n"
    "#endif\n"
    "uniform highp sampler2D u_src;\n"
    "uniform vec2 u_scale;\n"
    "uniform float u_toSrgb;\n"
    "uniform float u_limited;\n"
    "uniform float u_scalar;\n"
    "uniform float u_peak;\n"
    "uniform float u_pq[256];\n"
    "float Oetf(float c)\n"
    "{\n"
    "  c = clamp(c, 0.0, 1.0);\n"
    "  return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055;\n"
    "}\n"
    "void main()\n"
    "{\n"
    "  vec4 t = floor(texture2D(u_src, gl_FragCoord.xy * u_scale) * 255.0 + 0.5);\n"
    "  float a = t.a;\n"
    // the bytes are B, G, R, A (PIXEL_*SHIFT)
    "  vec3 c = t.bgr;\n"
    "  if (u_toSrgb > 0.5 && a > 0.0)\n"
    "  {\n"
    // ConvertPQPaletteToSRGB whole: per-channel decode x 10000/203, the BT.2020->709 matrix
    // (it mixes channels, so no per-channel LUT can stand in), clamp, the hue-keeping divide,
    // the OETF and its rounding
    "    vec3 l = vec3(u_pq[int(c.r)], u_pq[int(c.g)], u_pq[int(c.b)]);\n"
    "    vec3 m = vec3(1.660491002 * l.r + -0.587641139 * l.g + -0.072849863 * l.b,\n"
    "                  -0.124550474 * l.r + 1.132899897 * l.g + -0.008349423 * l.b,\n"
    "                  -0.018150763 * l.r + -0.100578897 * l.g + 1.118729660 * l.b);\n"
    "    m = max(m, vec3(0.0));\n"
    "    float mx = max(max(m.r, m.g), m.b);\n"
    "    if (mx > 1.0)\n"
    "      m = m / mx;\n"
    "    c = floor(vec3(Oetf(m.r), Oetf(m.g), Oetf(m.b)) * 255.0 + 0.5);\n"
    "  }\n"
    // (n + 127) / 255 in integers is floor((n + 127.5) / 255) for integer n
    "  c = floor((c * a + 127.5) / 255.0);\n"
    "  if (u_limited > 0.5)\n"
    "    c = floor((c * 219.0 + a * 16.0 + 127.5) / 255.0);\n"
    "  if (u_scalar > 0.5)\n"
    "  {\n"
    "    vec3 x = c * u_peak;\n"
    "    vec3 f = floor(x);\n"
    "    c = min(vec3(a), f + step(0.5, x - f));\n"
    "  }\n"
    // + 0.25: the same code whether the store rounds or truncates
    "  gl_FragColor = (vec4(c, a) + 0.25) / 255.0;\n"
    "}\n";

const char* PASS1_PALETTE =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "#error no highp float in the fragment shader\n"
    "#endif\n"
    "uniform highp sampler2D u_src;\n"
    "uniform highp sampler2D u_pal;\n"
    "uniform vec2 u_scale;\n"
    "void main()\n"
    "{\n"
    "  float i = floor(texture2D(u_src, gl_FragCoord.xy * u_scale).r * 255.0 + 0.5);\n"
    "  vec4 p = floor(texture2D(u_pal, vec2((i + 0.5) / 256.0, 0.5)) * 255.0 + 0.5);\n"
    "  gl_FragColor = (p + 0.25) / 255.0;\n"
    "}\n";

// CGraphicsPlaneAML's draw of premultiplied texels
const char* DRAW_VERTEX = "attribute vec2 a_pos;\n"
                          "attribute vec2 a_tex;\n"
                          "varying vec2 v_tex;\n"
                          "void main()\n"
                          "{\n"
                          "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                          "  v_tex = a_tex;\n"
                          "}\n";
const char* DRAW_FRAGMENT = "precision mediump float;\n"
                            "uniform sampler2D u_tex;\n"
                            "varying vec2 v_tex;\n"
                            "void main()\n"
                            "{\n"
                            "  gl_FragColor = texture2D(u_tex, v_tex);\n"
                            "}\n";

// gles_shader_fonts.frag's maths with its two defines as uniforms: straight colour, the
// limited scale unweighted by alpha, then the m_sdrPeak trim
const char* GLYPH_VERTEX = "attribute vec2 a_pos;\n"
                           "attribute vec4 a_col;\n"
                           "attribute vec2 a_tex;\n"
                           "varying vec4 v_col;\n"
                           "varying vec2 v_tex;\n"
                           "void main()\n"
                           "{\n"
                           "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
                           "  v_col = a_col;\n"
                           "  v_tex = a_tex;\n"
                           "}\n";
const char* GLYPH_FRAGMENT = "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
                             "precision highp float;\n"
                             "#else\n"
                             "precision mediump float;\n"
                             "#endif\n"
                             "uniform sampler2D u_tex;\n"
                             "uniform float u_limited;\n"
                             "uniform float u_scalar;\n"
                             "uniform float u_peak;\n"
                             "varying vec4 v_col;\n"
                             "varying vec2 v_tex;\n"
                             "void main()\n"
                             "{\n"
                             "  vec3 c = v_col.rgb;\n"
                             "  if (u_limited > 0.5)\n"
                             "    c = c * ((235.0 - 16.0) / 255.0) + 16.0 / 255.0;\n"
                             "  if (u_scalar > 0.5)\n"
                             "    c *= u_peak;\n"
                             "  gl_FragColor = vec4(c, v_col.a * texture2D(u_tex, v_tex).a);\n"
                             "}\n";
} // namespace

CGraphicsRasterAML::CGraphicsRasterAML(int width, int height) : m_width(width), m_height(height)
{
}

CGraphicsRasterAML::~CGraphicsRasterAML() = default;

bool CGraphicsRasterAML::Init(float peak, bool failCompile)
{
  const auto start = std::chrono::steady_clock::now();
  std::string why;
  m_ready = InitPrograms(failCompile, why) && InitComposite(peak, why) && EnsurePreEncode();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  if (!m_ready)
  {
    CLog::Log(LOGERROR, "GPROG failed ({}) after {} ms", why.empty() ? "pre-encode buffer" : why,
              ms);
    Deinit();
    return false;
  }
  GLint range[2] = {};
  GLint precision = 0;
  glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
  CLog::Log(LOGINFO,
            "GPROG ready compile_ms={} programs=pass1_argb,pass1_palette,draw,glyph,"
            "composite_full,composite_limited highp_precision={} peak={:.4f}",
            ms, precision, peak);
  return true;
}

bool CGraphicsRasterAML::InitPrograms(bool failCompile, std::string& why)
{
  GLint range[2] = {};
  GLint precision = 0;
  glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
  GLint uniforms = 0;
  glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &uniforms);
  if (precision < 23)
  {
    why = "no fp32 highp in the fragment shader, precision " + std::to_string(precision);
    return false;
  }
  if (uniforms < 256 + 8)
  {
    why = "fragment uniform vectors " + std::to_string(uniforms) + " < 264 for the PQ table";
    return false;
  }

  // gselftest_failcompile: a real compile error in a real program, so the failure path is the
  // one a driver's refusal takes
  const std::string argbSource =
      std::string(failCompile ? "#error gselftest_failcompile\n" : "") + PASS1_ARGB;
  std::string error;
  if (!(m_argbProgram = Link(PASS1_VERTEX, argbSource.c_str(), error)))
  {
    why = "pass1_argb: " + error;
    return false;
  }
  if (!(m_paletteProgram = Link(PASS1_VERTEX, PASS1_PALETTE, error)))
  {
    why = "pass1_palette: " + error;
    return false;
  }
  if (!(m_drawProgram = Link(DRAW_VERTEX, DRAW_FRAGMENT, error)))
  {
    why = "draw: " + error;
    return false;
  }
  if (!(m_glyphProgram = Link(GLYPH_VERTEX, GLYPH_FRAGMENT, error)))
  {
    why = "glyph: " + error;
    return false;
  }

  // the per-channel PQ decode x 10000/203, from ConvertPQPaletteToSRGB's own DecodePQ
  float decode[256];
  constexpr float whiteScale = 10000.0f / 203.0f;
  for (int i = 0; i < 256; i++)
    decode[i] = OVERLAY::DecodePQ(i / 255.0f) * whiteScale;
  glUseProgram(m_argbProgram);
  glUniform1i(glGetUniformLocation(m_argbProgram, "u_src"), 0);
  glUniform1fv(glGetUniformLocation(m_argbProgram, "u_pq"), 256, decode);
  m_argbPos = glGetAttribLocation(m_argbProgram, "a_pos");
  m_argbScale = glGetUniformLocation(m_argbProgram, "u_scale");
  m_argbToSrgb = glGetUniformLocation(m_argbProgram, "u_toSrgb");
  m_argbLimited = glGetUniformLocation(m_argbProgram, "u_limited");
  m_argbScalar = glGetUniformLocation(m_argbProgram, "u_scalar");
  m_argbPeak = glGetUniformLocation(m_argbProgram, "u_peak");
  glUseProgram(m_paletteProgram);
  glUniform1i(glGetUniformLocation(m_paletteProgram, "u_src"), 0);
  glUniform1i(glGetUniformLocation(m_paletteProgram, "u_pal"), 1);
  m_palettePos = glGetAttribLocation(m_paletteProgram, "a_pos");
  m_paletteScale = glGetUniformLocation(m_paletteProgram, "u_scale");
  glUseProgram(m_drawProgram);
  glUniform1i(glGetUniformLocation(m_drawProgram, "u_tex"), 0);
  m_drawPos = glGetAttribLocation(m_drawProgram, "a_pos");
  m_drawTex = glGetAttribLocation(m_drawProgram, "a_tex");
  glUseProgram(m_glyphProgram);
  glUniform1i(glGetUniformLocation(m_glyphProgram, "u_tex"), 0);
  m_glyphPos = glGetAttribLocation(m_glyphProgram, "a_pos");
  m_glyphCol = glGetAttribLocation(m_glyphProgram, "a_col");
  m_glyphTex = glGetAttribLocation(m_glyphProgram, "a_tex");
  m_glyphLimited = glGetUniformLocation(m_glyphProgram, "u_limited");
  m_glyphScalar = glGetUniformLocation(m_glyphProgram, "u_scalar");
  m_glyphPeak = glGetUniformLocation(m_glyphProgram, "u_peak");
  glUseProgram(0);

  glGenFramebuffers(1, &m_convFbo);
  if (!m_convFbo)
  {
    why = "no framebuffer object";
    return false;
  }
  return true;
}

// both ranges at init (GPROG), not at the first Composite composition: a compile there would
// land in a composition's draw
bool CGraphicsRasterAML::InitComposite(float peak, std::string& why)
{
  for (int limited = 0; limited < 2; limited++)
  {
    m_composite[limited] =
        std::make_unique<CGuiCompositeShaderGLES>(limited ? "#define KODI_LIMITED_RANGE 1\n" : "");
    if (!m_composite[limited]->CompileAndLink())
    {
      why = limited ? "composite_limited" : "composite_full";
      return false;
    }
  }
  if (!BuildLUTs(peak))
  {
    why = "composite luts";
    return false;
  }
  return true;
}

bool CGraphicsRasterAML::BuildLUTs(float peak)
{
  const float nits = CGuiCompositeShaderGLES::PeakFromPQCode(peak);
  for (auto& composite : m_composite)
  {
    if (!composite)
      return false;
    composite->SetSdrPeak(nits);
    if (!composite->CreateLUTs(AVCOL_TRC_SMPTE2084))
      return false;
  }
  m_lutPeak = peak;
  return true;
}

bool CGraphicsRasterAML::EnsurePreEncode()
{
  if (m_preFbo)
    return true;
  // read GL_NEAREST by the composite pass: a 1:1 read (G-X4e)
  m_preTexture = NewTexture(GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  glGenFramebuffers(1, &m_preFbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_preFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_preTexture, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (status == GL_FRAMEBUFFER_COMPLETE)
    return true;
  CLog::Log(LOGERROR, "GPROG pre-encode buffer {}x{} incomplete ({:#x})", m_width, m_height,
            status);
  glDeleteFramebuffers(1, &m_preFbo);
  glDeleteTextures(1, &m_preTexture);
  m_preFbo = m_preTexture = 0;
  return false;
}

void CGraphicsRasterAML::Deinit()
{
  ClearCache();
  for (GLuint* program : {&m_argbProgram, &m_paletteProgram, &m_drawProgram, &m_glyphProgram})
  {
    if (*program)
      glDeleteProgram(*program);
    *program = 0;
  }
  for (GLuint* fbo : {&m_convFbo, &m_preFbo, &m_checkFbo})
  {
    if (*fbo)
      glDeleteFramebuffers(1, fbo);
    *fbo = 0;
  }
  for (GLuint* texture : {&m_preTexture, &m_checkTexture})
  {
    if (*texture)
      glDeleteTextures(1, texture);
    *texture = 0;
  }
  for (auto& composite : m_composite)
    composite.reset();
  m_lutPeak = -1.0f;
  m_compositeFailed = false;
  m_regimeSet = false;
  m_ready = false;
}

void CGraphicsRasterAML::ClearCache()
{
  for (auto& [key, cached] : m_textures)
    if (cached.texture.texture)
      glDeleteTextures(1, &cached.texture.texture);
  m_textures.clear();
  for (auto& [key, atlas] : m_atlases)
    glDeleteTextures(1, &atlas.texture);
  m_atlases.clear();
}

bool CGraphicsRasterAML::SetRegime(const SRegime& regime)
{
  bool cold = false;
  if (!m_regimeSet || regime.epoch != m_cacheEpoch)
  {
    // the atlases hold coverage only, which no regime changes
    for (auto& [key, cached] : m_textures)
      if (cached.texture.texture)
        glDeleteTextures(1, &cached.texture.texture);
    m_textures.clear();
    m_cacheEpoch = regime.epoch;
    cold = true;
  }
  if (regime.peak != m_lutPeak)
  {
    // CreateLUTs keeps the previous LUTs on a failure; without a peak to trust, Composite
    // draws no composited item until a rebuild succeeds (Draw retries per composition)
    m_compositeFailed = !BuildLUTs(regime.peak);
    if (m_compositeFailed)
      CLog::Log(LOGERROR, "GPROG composite luts failed at peak {:.4f}", regime.peak);
    cold = true;
  }
  m_regime = regime;
  m_regimeSet = true;
  return cold;
}

CGraphicsRasterAML::SEncode CGraphicsRasterAML::EncodeFor(const CDVDOverlayImage& o,
                                                          const SRegime& regime)
{
  SEncode e;
  e.peak = regime.peak;
  const bool pq = o.m_isHDROverlay;
  switch (regime.encoding)
  {
    case GuiEncoding::Srgb:
      e.toSrgb = pq;
      e.limited = regime.limited;
      break;
    case GuiEncoding::Scalar:
      e.toSrgb = pq;
      e.limited = regime.limited;
      e.scalar = !(pq && !e.toSrgb);
      break;
    case GuiEncoding::Composite:
      e.composited = !pq;
      e.limited = regime.limited && pq;
      break;
  }
  return e;
}

CGraphicsRasterAML::SEncode CGraphicsRasterAML::GlyphEncodeFor(const SRegime& regime)
{
  SEncode e;
  e.peak = regime.peak;
  e.composited = regime.encoding == GuiEncoding::Composite;
  e.limited = !e.composited && regime.limited;
  e.scalar = regime.encoding == GuiEncoding::Scalar;
  return e;
}

void CGraphicsRasterAML::ResetCounters()
{
  m_misses = 0;
  m_expandedTexels = 0;
}

const CGraphicsRasterAML::STexture* CGraphicsRasterAML::Expand(
    const std::shared_ptr<CDVDOverlay>& image)
{
  if (!m_ready || !m_regimeSet || !image || !image->IsOverlayType(DVDOVERLAY_TYPE_IMAGE))
    return nullptr;
  auto it = m_textures.find(image.get());
  if (it == m_textures.end())
  {
    const auto& o = static_cast<const CDVDOverlayImage&>(*image);
    SCached& cached = m_textures[image.get()];
    cached.image = image;
    if (o.width > 0 && o.height > 0 && !o.pixels.empty() &&
        Pass1(o, EncodeFor(o, m_regime), cached.texture))
    {
      m_misses++;
      m_expandedTexels += static_cast<int64_t>(cached.texture.width) * cached.texture.height;
    }
    it = m_textures.find(image.get());
  }
  return it->second.texture.texture ? &it->second.texture : nullptr;
}

void CGraphicsRasterAML::Retain(const std::vector<std::shared_ptr<CDVDOverlay>>& images,
                                const std::vector<SGlyphs>& glyphs)
{
  for (auto it = m_textures.begin(); it != m_textures.end();)
  {
    if (std::find(images.begin(), images.end(), it->second.image) == images.end())
    {
      if (it->second.texture.texture)
        glDeleteTextures(1, &it->second.texture.texture);
      it = m_textures.erase(it);
    }
    else
      ++it;
  }
  for (auto it = m_atlases.begin(); it != m_atlases.end();)
  {
    const bool kept = std::any_of(glyphs.begin(), glyphs.end(), [&](const SGlyphs& g)
                                  { return g.quads.get() == it->first; });
    if (!kept)
    {
      glDeleteTextures(1, &it->second.texture);
      it = m_atlases.erase(it);
    }
    else
      ++it;
  }
}

bool CGraphicsRasterAML::Pass1(const CDVDOverlayImage& o, const SEncode& e, STexture& texture)
{
  int width = o.width;
  int height = o.height;
  CRect crop;
  int srcWidth = width; //!< of the source texture: a padded row is uploaded whole
  GLenum srcFormat = GL_RGBA;
  const uint8_t* srcData = o.pixels.data();
  std::vector<uint8_t> copy;
  uint32_t encoded[256] = {};
  if (o.palette.empty())
  {
    // GLES 2 has no GL_UNPACK_ROW_LENGTH
    if (o.linesize != width * 4)
    {
      if (o.linesize % 4 == 0 && o.linesize / 4 >= width)
        srcWidth = o.linesize / 4;
      else
      {
        copy.resize(static_cast<size_t>(width) * height * 4);
        for (int y = 0; y < height; y++)
          std::memcpy(copy.data() + static_cast<size_t>(y) * width * 4,
                      o.pixels.data() + static_cast<size_t>(y) * o.linesize,
                      static_cast<size_t>(width) * 4);
        srcData = copy.data();
      }
    }
  }
  else
  {
    // today's functions, once per palette: no per-pixel CPU work
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
    srcWidth = width;
    if (width != o.width || height != o.height)
      crop = CRect(x0, y0, x1, y1);
    for (int i = 0; i < 256; i++)
      encoded[i] = EncodeEntry(lut[i], e);
    const uint8_t* first = o.pixels.data() + y0 * o.linesize + x0;
    copy.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++)
      std::memcpy(copy.data() + static_cast<size_t>(y) * width, first + y * o.linesize,
                  static_cast<size_t>(width));
    srcData = copy.data();
    srcFormat = GL_LUMINANCE;
  }

  const GLuint src = NewTexture(GL_NEAREST);
  glPixelStorei(GL_UNPACK_ALIGNMENT, srcFormat == GL_LUMINANCE ? 1 : 4);
  glTexImage2D(GL_TEXTURE_2D, 0, srcFormat, srcWidth, height, 0, srcFormat, GL_UNSIGNED_BYTE,
               srcData);
  GLuint pal = 0;
  if (srcFormat == GL_LUMINANCE)
  {
    pal = NewTexture(GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, encoded);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

  const GLuint target = NewTexture(GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, m_convFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status == GL_FRAMEBUFFER_COMPLETE)
  {
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    const bool palette = srcFormat == GL_LUMINANCE;
    const GLint pos = palette ? m_palettePos : m_argbPos;
    glUseProgram(palette ? m_paletteProgram : m_argbProgram);
    if (palette)
    {
      glUniform2f(m_paletteScale, 1.0f / static_cast<float>(srcWidth),
                  1.0f / static_cast<float>(height));
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, pal);
    }
    else
    {
      glUniform2f(m_argbScale, 1.0f / static_cast<float>(srcWidth),
                  1.0f / static_cast<float>(height));
      glUniform1f(m_argbToSrgb, e.toSrgb ? 1.0f : 0.0f);
      glUniform1f(m_argbLimited, e.limited ? 1.0f : 0.0f);
      glUniform1f(m_argbScalar, e.scalar ? 1.0f : 0.0f);
      glUniform1f(m_argbPeak, e.peak);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    const GLfloat quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(pos);
    if (palette)
    {
      glActiveTexture(GL_TEXTURE1);
      glBindTexture(GL_TEXTURE_2D, 0);
      glActiveTexture(GL_TEXTURE0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
  }
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, m_width, m_height);
  glDeleteTextures(1, &src);
  if (pal)
    glDeleteTextures(1, &pal);
  if (status != GL_FRAMEBUFFER_COMPLETE)
  {
    CLog::Log(LOGERROR, "GPROG pass-1 target {}x{} incomplete ({:#x})", width, height, status);
    glDeleteTextures(1, &target);
    return false;
  }
  texture.texture = target;
  texture.width = width;
  texture.height = height;
  texture.crop = crop;
  texture.nearest = false;
  texture.encode = e;
  return true;
}

bool CGraphicsRasterAML::OneToOne(const CRect& r, int width, int height, int& x, int& y)
{
  if (r.IsEmpty())
    return false;
  const float rx = std::round(r.x1);
  const float ry = std::round(r.y1);
  if (std::fabs(r.Width() - static_cast<float>(width)) > ALIGN_EPS ||
      std::fabs(r.Height() - static_cast<float>(height)) > ALIGN_EPS ||
      std::fabs(r.x1 - rx) > ALIGN_EPS || std::fabs(r.y1 - ry) > ALIGN_EPS)
    return false;
  x = static_cast<int>(rx);
  y = static_cast<int>(ry);
  return true;
}

bool CGraphicsRasterAML::GlyphsOneToOne(const SGlyphs& glyphs, int& x, int& y)
{
  // frame pixel fx lands on plane pixel x + fx over the whole frame when the view is frameWidth
  // plane pixels wide from a whole pixel (the quads are whole frame pixels)
  if (glyphs.frameWidth <= 0 || glyphs.frameHeight <= 0)
    return false;
  return OneToOne(glyphs.view, static_cast<int>(std::lround(glyphs.frameWidth)),
                  static_cast<int>(std::lround(glyphs.frameHeight)), x, y) &&
         std::fabs(glyphs.frameWidth - std::round(glyphs.frameWidth)) <= ALIGN_EPS &&
         std::fabs(glyphs.frameHeight - std::round(glyphs.frameHeight)) <= ALIGN_EPS;
}

void CGraphicsRasterAML::DrawImages(const std::vector<SImage>& images, bool composited)
{
  glUseProgram(m_drawProgram);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glActiveTexture(GL_TEXTURE0);
  glEnableVertexAttribArray(m_drawPos);
  glEnableVertexAttribArray(m_drawTex);
  for (const SImage& image : images)
  {
    const STexture* texture = image.texture;
    if (!texture || !texture->texture || texture->encode.composited != composited ||
        image.rect.IsEmpty())
      continue;
    // GL's y runs up, the plane's rows down
    const CRect& r = image.rect;
    const GLfloat left = r.x1 / m_width * 2.0f - 1.0f;
    const GLfloat right = r.x2 / m_width * 2.0f - 1.0f;
    const GLfloat top = NdcY(r.y1);
    const GLfloat bottom = NdcY(r.y2);
    const GLfloat pos[] = {left, top, right, top, left, bottom, right, bottom};
    const GLfloat tex[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    glBindTexture(GL_TEXTURE_2D, texture->texture);
    // at 1:1 GL_LINEAR's coordinate lands a hair off the texel centre and mixes in the
    // neighbour at strong edges (G-X4r); scaled, the filter runs on premultiplied bytes
    int x = 0;
    int y = 0;
    SetFilter(OneToOne(r, texture->width, texture->height, x, y), texture->nearest);
    glVertexAttribPointer(m_drawPos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(m_drawTex, 2, GL_FLOAT, GL_FALSE, 0, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glDisableVertexAttribArray(m_drawPos);
  glDisableVertexAttribArray(m_drawTex);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
  glUseProgram(0);
}

void CGraphicsRasterAML::DrawGlyphs(const std::vector<SGlyphs>& glyphs)
{
  struct Vertex
  {
    GLfloat x, y, u, v;
    GLubyte r, g, b, a;
  };
  const SEncode e = GlyphEncodeFor(m_regime);
  glUseProgram(m_glyphProgram);
  glUniform1f(m_glyphLimited, e.limited ? 1.0f : 0.0f);
  glUniform1f(m_glyphScalar, e.scalar ? 1.0f : 0.0f);
  glUniform1f(m_glyphPeak, e.peak);
  glEnable(GL_BLEND);
  // as COverlayGlyphGLES::Render
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glActiveTexture(GL_TEXTURE0);
  glEnableVertexAttribArray(m_glyphPos);
  glEnableVertexAttribArray(m_glyphCol);
  glEnableVertexAttribArray(m_glyphTex);
  std::vector<Vertex> vertices;
  for (const SGlyphs& item : glyphs)
  {
    const OVERLAY::SQuads* quads = item.quads.get();
    if (!quads || quads->size_x <= 0 || quads->size_y <= 0 || quads->quad.empty() ||
        item.frameWidth <= 0 || item.frameHeight <= 0 || item.view.IsEmpty())
      continue;
    SAtlas& atlas = m_atlases[quads];
    if (!atlas.texture)
    {
      // as LoadTexture uploads it: GL_ALPHA, unpadded
      atlas.quads = item.quads;
      atlas.texture = NewTexture(GL_LINEAR);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, quads->size_x, quads->size_y, 0, GL_ALPHA,
                   GL_UNSIGNED_BYTE, quads->texture.data());
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }
    glBindTexture(GL_TEXTURE_2D, atlas.texture);
    // libass places each glyph at whole pixels, so at 1:1 GL_NEAREST is libass's own image;
    // scaled, the atlas's one-texel gap keeps the neighbours out (convert_quad)
    int ox = 0;
    int oy = 0;
    SetFilter(GlyphsOneToOne(item, ox, oy), atlas.nearest);

    const CRect& view = item.view;
    const auto corner = [&](const OVERLAY::SQuad& q, int dx, int dy)
    {
      const float sx = view.x1 + (q.x + dx * q.w) / item.frameWidth * view.Width();
      const float sy = view.y1 + (q.y + dy * q.h) / item.frameHeight * view.Height();
      return Vertex{sx / m_width * 2.0f - 1.0f,
                    NdcY(sy),
                    static_cast<GLfloat>(q.u + dx * q.w) / quads->size_x,
                    static_cast<GLfloat>(q.v + dy * q.h) / quads->size_y,
                    q.r,
                    q.g,
                    q.b,
                    q.a};
    };
    vertices.clear();
    vertices.reserve(quads->quad.size() * 6);
    for (const OVERLAY::SQuad& q : quads->quad)
    {
      const Vertex v0 = corner(q, 0, 0);
      const Vertex v1 = corner(q, 0, 1);
      const Vertex v2 = corner(q, 1, 0);
      const Vertex v3 = corner(q, 1, 1);
      vertices.insert(vertices.end(), {v0, v1, v2, v1, v3, v2});
    }
    glVertexAttribPointer(m_glyphPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].x);
    glVertexAttribPointer(m_glyphTex, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].u);
    glVertexAttribPointer(m_glyphCol, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                          &vertices[0].r);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
  }
  glDisableVertexAttribArray(m_glyphPos);
  glDisableVertexAttribArray(m_glyphCol);
  glDisableVertexAttribArray(m_glyphTex);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
  glUseProgram(0);
}

// CWinSystemAmlogicGLESContext::CompositeGui's pass over G's pre-encode buffer, into the bound
// target
void CGraphicsRasterAML::CompositePass()
{
  CGuiCompositeShaderGLES& composite = *m_composite[m_regime.limited ? 1 : 0];
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_preTexture);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  const float w = static_cast<float>(m_width);
  const float h = static_cast<float>(m_height);
  const GLfloat proj[16] = {2.0f / w, 0, 0, 0, 0, -2.0f / h, 0, 0, 0, 0, -1, 0, -1.0f, 1.0f, 0, 1};
  composite.SetProjection(proj);
  composite.SetHdrTexture(0);
  composite.Enable();
  const GLint posLoc = composite.GetPosLoc();
  const GLint texLoc = composite.GetTexLoc();
  const GLfloat vert[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
  const GLfloat tex[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  const GLubyte idx[4] = {0, 1, 3, 2};
  glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, vert);
  glVertexAttribPointer(texLoc, 2, GL_FLOAT, GL_FALSE, 0, tex);
  glEnableVertexAttribArray(posLoc);
  glEnableVertexAttribArray(texLoc);
  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, idx);
  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);
  composite.Disable();
  composite.SetProjection(nullptr);
  // its LUTs stay bound on units 1 and 2 otherwise
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_BLEND);
}

void CGraphicsRasterAML::Draw(const std::vector<SImage>& images,
                              const std::vector<SGlyphs>& glyphs,
                              GLuint target)
{
  if (!m_ready || !m_regimeSet)
    return;
  if (m_compositeFailed)
    m_compositeFailed = !BuildLUTs(m_regime.peak);
  glDisable(GL_SCISSOR_TEST);
  if (m_regime.encoding == GuiEncoding::Composite)
  {
    const bool srgb =
        !glyphs.empty() || std::any_of(images.begin(), images.end(), [](const SImage& image)
                                       { return image.texture && image.texture->encode.composited; });
    if (srgb && !m_compositeFailed)
    {
      glBindFramebuffer(GL_FRAMEBUFFER, m_preFbo);
      glViewport(0, 0, m_width, m_height);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      DrawImages(images, true);
      DrawGlyphs(glyphs);
      glBindFramebuffer(GL_FRAMEBUFFER, target);
      glViewport(0, 0, m_width, m_height);
      CompositePass();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, m_width, m_height);
    // over the encoded output: raw, as route C draws them
    DrawImages(images, false);
    return;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, target);
  glViewport(0, 0, m_width, m_height);
  DrawImages(images, false);
  DrawGlyphs(glyphs);
}
