/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/Geometry.h"
#include "windowing/amlogic/GraphicsRegime.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <GLES2/gl2.h>

class CDVDOverlay;
class CDVDOverlayImage;
class CGuiCompositeShaderGLES;
namespace OVERLAY
{
struct SQuads;
}

// 5.G2 GT-3 (design draft42 §10.2 item 6): G's renderer, writing today's bytes in every regime
// on the GPU:
//  - pass 1 expands each image once at its own size into an RGBA8 texture (highp, GL_NEAREST at
//    texel centres): a palette image is a lookup in a LUT built by today's CPU functions; an ARGB
//    image runs the whole ConvertPQPaletteToSRGB in the shader where the regime converts;
//  - pass 2 draws that texture, GL_NEAREST where it lands 1:1 on plane pixels, GL_LINEAR scaled;
//  - libass glyphs take the fonts shader's maths with its limited-range and Scalar steps;
//  - Composite: the sRGB-authored items into a pre-encode buffer encoded by a
//    CGuiCompositeShaderGLES in G's context, then the PQ-authored images raw on top.
// The texture cache is keyed on the regime epoch. No SPU (Q5).
//
// G's thread only, with G's context current.
class CGraphicsRasterAML
{
public:
  //! the regime fields a draw reads (the published GraphicsRegime's)
  struct SRegime
  {
    uint64_t epoch = 0; //!< the texture cache's key
    DISPLAYSTATE::GuiEncoding encoding = DISPLAYSTATE::GuiEncoding::Srgb;
    bool limited = false;
    float peak = 1.0f; //!< GuiPeakCode: Scalar's m_sdrPeak, Composite's reference white
  };

  //! how one image's texels are encoded in a regime: pass 1's and the CPU reference's one rule
  struct SEncode
  {
    bool toSrgb = false; //!< ConvertPQPaletteToSRGB (a PQ-authored image in Srgb or Scalar)
    bool limited = false; //!< (v*219 + a*16)/255 on the premultiplied value
    bool scalar = false; //!< min(a, lround(v*peak)), after the range, as the GUI shaders
    float peak = 1.0f;
    bool composited = false; //!< into the pre-encode buffer; the composite pass encodes it
  };

  //! one image's pass-1 texture
  struct STexture
  {
    GLuint texture = 0;
    int width = 0; //!< the visible box of the image
    int height = 0;
    CRect crop; //!< that box in the image, empty when it is the whole image
    mutable bool nearest = false; //!< its filter now (set per draw)
    SEncode encode;
  };

  //! an image of a composition, placed in plane pixels (rows down)
  struct SImage
  {
    const STexture* texture = nullptr;
    CRect rect;
  };

  //! libass output of one overlay, as convert_quad gives it, over its view in plane pixels
  struct SGlyphs
  {
    std::shared_ptr<const OVERLAY::SQuads> quads;
    float frameWidth = 0; //!< the frame libass rendered (the quads' units)
    float frameHeight = 0;
    CRect view;
  };

  CGraphicsRasterAML(int width, int height);
  ~CGraphicsRasterAML();

  //! compiles and links every program and builds the composite in both ranges, with LUTs at
  //! peak; logs "GPROG ready ..." or "GPROG failed (<why>)". failCompile (gselftest_failcompile)
  //! breaks pass 1's source, so the failure path runs as a compile failure would
  bool Init(float peak, bool failCompile);
  //! frees every GL object (the context must be current)
  void Deinit();
  bool Ready() const { return m_ready; }

  //! the regime of the next draws: an epoch change empties the texture cache, a peak change
  //! rebuilds the composite's LUTs (M6). true when either happened (a cold composition)
  bool SetRegime(const SRegime& regime);
  //! the encode rule (item 6): Srgb and Scalar convert PQ-authored images; Scalar trims, with raw
  //! PQ exempted as COverlayTextureGLES::Render does (never reached: Scalar converts PQ);
  //! Composite takes PQ-authored images raw and composites the sRGB-authored ones
  static SEncode EncodeFor(const CDVDOverlayImage& o, const SRegime& regime);
  //! the glyphs' steps in a regime: limited and scalar as the fonts shader, composited in Composite
  static SEncode GlyphEncodeFor(const SRegime& regime);

  //! the image's pass-1 texture in the current regime, built on a miss; nullptr when it has
  //! nothing to draw or pass 1 failed. misses counts the builds
  const STexture* Expand(const std::shared_ptr<CDVDOverlay>& image);
  //! drops the textures and atlases of everything not in these lists
  void Retain(const std::vector<std::shared_ptr<CDVDOverlay>>& images,
              const std::vector<SGlyphs>& glyphs);
  int Misses() const { return m_misses; }
  int64_t ExpandedTexels() const { return m_expandedTexels; }
  void ResetCounters();

  //! draws a composition into target (an FBO of width x height; 0 the window surface), which
  //! the caller has cleared: images bottom first, then the glyphs; in Composite the composited
  //! ones through the pre-encode buffer and the composite pass first, then the raw PQ images.
  //! Leaves target bound, the viewport full size, blending off
  void Draw(const std::vector<SImage>& images, const std::vector<SGlyphs>& glyphs, GLuint target);

  //! the placed rect r is width x height on whole plane pixels (within ALIGN_EPS); x, y its
  //! top-left plane pixel (diag F's P5Aligned, diag G's P5OneToOne)
  static bool OneToOne(const CRect& r, int width, int height, int& x, int& y);
  //! the glyphs' frame pixels are plane pixels; x, y: where frame pixel 0,0 lands
  static bool GlyphsOneToOne(const SGlyphs& glyphs, int& x, int& y);

  //! 1/4096 plane pixel: well inside a 1080p plane's float precision
  static constexpr float ALIGN_EPS = 1.0f / 4096.0f;

  // gselftest only (GraphicsSelfTestAML.cpp): "GSELFTEST case= regime= maxdiff= alpha= PASS|FAIL"
  //! the 40-case pass-1 set and the plane set (Srgb/Scalar/Composite x 1:1 and 2x x PGS/menu
  //! image/ASS glyph); leaves the cache empty and the regime unset
  void SelfTest();
  //! the composition just drawn into the check target by DrawCheck, against the CPU reference
  bool CheckComposition(const std::string& name,
                        const std::vector<std::shared_ptr<CDVDOverlay>>& images,
                        const std::vector<SImage>& placed,
                        const std::vector<SGlyphs>& glyphs);
  //! Draw into the check target (cleared first); 0 when it cannot be made
  GLuint DrawCheck(const std::vector<SImage>& images, const std::vector<SGlyphs>& glyphs);

private:
  struct SCached
  {
    std::shared_ptr<CDVDOverlay> image; //!< keeps the key alive
    STexture texture;
  };
  struct SAtlas
  {
    std::shared_ptr<const OVERLAY::SQuads> quads; //!< keeps the key alive
    GLuint texture = 0;
    bool nearest = false;
  };

  bool InitPrograms(bool failCompile, std::string& why);
  bool InitComposite(float peak, std::string& why);
  bool BuildLUTs(float peak);
  bool EnsurePreEncode();
  //! pass 1 of one image with this encode; false leaves no texture
  bool Pass1(const CDVDOverlayImage& o, const SEncode& e, STexture& texture);
  void DrawImages(const std::vector<SImage>& images, bool composited);
  void DrawGlyphs(const std::vector<SGlyphs>& glyphs);
  void CompositePass();
  void ClearCache();

  // gselftest
  //! pass 1 against the CPU reference, read back; one GSELFTEST line
  bool CheckPass1(const CDVDOverlayImage& o, const SEncode& e, const std::string& name);
  //! a layer of the target (or the pre-encode buffer) against the CPU reference
  bool CheckLayer(const std::string& name,
                  const char* layer,
                  GLuint fbo,
                  const std::vector<const CDVDOverlayImage*>& images,
                  const std::vector<SImage>& placed,
                  const std::vector<SGlyphs>& glyphs);
  void SelfTestPlane(int& total);
  bool EnsureCheckTarget();

  const int m_width;
  const int m_height;
  bool m_ready = false;
  SRegime m_regime;
  bool m_regimeSet = false;

  GLuint m_argbProgram = 0; //!< pass 1, ARGB
  GLint m_argbPos = -1;
  GLint m_argbScale = -1;
  GLint m_argbToSrgb = -1;
  GLint m_argbLimited = -1;
  GLint m_argbScalar = -1;
  GLint m_argbPeak = -1;
  GLuint m_paletteProgram = 0; //!< pass 1, palette
  GLint m_palettePos = -1;
  GLint m_paletteScale = -1;
  GLuint m_drawProgram = 0; //!< pass 2: today's draw
  GLint m_drawPos = -1;
  GLint m_drawTex = -1;
  GLuint m_glyphProgram = 0;
  GLint m_glyphPos = -1;
  GLint m_glyphCol = -1;
  GLint m_glyphTex = -1;
  GLint m_glyphLimited = -1;
  GLint m_glyphScalar = -1;
  GLint m_glyphPeak = -1;
  GLuint m_convFbo = 0; //!< pass 1's target, re-attached per image
  //! [0] full range, [1] limited: the range is compiled in, as main's BuildCompositeShader
  std::unique_ptr<CGuiCompositeShaderGLES> m_composite[2];
  float m_lutPeak = -1.0f; //!< the GuiPeakCode both composites' LUTs are built for
  bool m_compositeFailed = false;
  GLuint m_preFbo = 0; //!< Composite's pre-encode buffer (GUI FBO's role), GL_NEAREST read
  GLuint m_preTexture = 0;

  std::map<const CDVDOverlay*, SCached> m_textures;
  uint64_t m_cacheEpoch = 0;
  std::map<const OVERLAY::SQuads*, SAtlas> m_atlases;
  int m_misses = 0;
  int64_t m_expandedTexels = 0;

  GLuint m_checkFbo = 0;
  GLuint m_checkTexture = 0;
  int m_selfTestFailed = 0;
};
