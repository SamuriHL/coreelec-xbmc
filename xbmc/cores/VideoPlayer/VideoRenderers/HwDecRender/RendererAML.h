/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/BaseRenderer.h"

class CRendererAML : public CBaseRenderer
{
public:
  CRendererAML();
  virtual ~CRendererAML();

  // Registration
  static CBaseRenderer* Create(CVideoBuffer *buffer);
  static bool Register();

  virtual void AddVideoPicture(const VideoPicture &picture, int index) override;
  virtual void ReleaseBuffer(int idx) override;
  virtual bool Configure(const VideoPicture &picture, float fps, unsigned int orientation) override;
  virtual bool IsConfigured() override { return m_bConfigured; };
  bool ConfigChanged(const VideoPicture& picture) override;
  virtual CRenderInfo GetRenderInfo() override;
  virtual void UnInit() override {};
  virtual void Update() override {};
  virtual void RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  virtual bool SupportsMultiPassRendering()override { return false; };
  virtual bool Flush(bool saveBuffers) override;

  // Player functions
  virtual bool IsGuiLayer() override { return false; };

  // Feature support
  virtual bool Supports(ESCALINGMETHOD method) const override { return false; };
  virtual bool Supports(ERENDERFEATURE feature) const override;

private:
  void Reset();

  // Which GUI/OSD encoding the resolved video output requires. Kept so
  // ConfigChanged() can force a reconfigure when a decoder swap changes the
  // output CLASS without changing any picture parameter - e.g. a BD menu-domain
  // segment force-mapped to DV followed by a feature title at the same
  // resolution, hdrType and color_transfer. VideoPicture::IsSameParams compares
  // only picture properties, so CRenderManager::Configure would otherwise skip
  // the reconfigure and leave the GUI encoded for the previous segment's output.
  enum class GuiEncoding
  {
    Srgb, // plain sRGB: nothing downstream encodes the OSD plane
    Scalar, // m_sdrPeak trim only; the VPP encodes the OSD plane itself
    Composite, // Kodi owns the full sRGB->BT.2020 PQ transform (DV core reads the plane)
  };
  static GuiEncoding ResolveGuiEncoding(const VideoPicture& picture,
                                        unsigned int dvOutputMode,
                                        bool isHdrDisplay);

  GuiEncoding m_guiEncoding{GuiEncoding::Srgb};
  // Sink HDR capability, sampled in Configure. CWinSystemAmlogic::IsHDRDisplay()
  // is NOT a cached accessor - it stats and reads two amhdmitx sysfs nodes and
  // mutates m_hdr_caps - so it must never be called from ConfigChanged, which
  // runs once per output picture on the VideoPlayerVideo thread while holding
  // CRenderManager's state lock. Caching it also keeps ConfigChanged and
  // Configure deciding from identical inputs, which is what stops them
  // disagreeing and reconfiguring in a loop.
  bool m_isHdrDisplay{false};

  static const int m_numRenderBuffers = NUM_BUFFERS;

  struct BUFFER
  {
    BUFFER() : videoBuffer(nullptr) {};
    CVideoBuffer *videoBuffer;
    int duration;
  } m_buffers[m_numRenderBuffers];

  uint64_t m_prevVPts;
  bool m_bConfigured;
};
