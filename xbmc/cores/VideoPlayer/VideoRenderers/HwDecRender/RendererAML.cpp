/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererAML.h"

#include "cores/VideoPlayer/DVDCodecs/Video/AMLCodec.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/amlogic/WinSystemAmlogic.h"
#include "windowing/WinSystem.h"

CRendererAML::CRendererAML()
 : m_prevVPts(DVD_NOPTS_VALUE)
 , m_bConfigured(false)
{
  CLog::Log(LOGINFO, "Constructing CRendererAML");
}

CRendererAML::~CRendererAML()
{
  Reset();
  // GUI returns to sRGB - tear down the HDR FBO composite (if any), clear the
  // per-primitive PQ flag, and restore the matching core2 graphics declaration.
  CServiceBroker::GetWinSystem()->SetGuiCompositing(0);
  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(false);
  CSysfsPath("/sys/class/amdolby_vision/graphic_fmt", 2 /* FORMAT_SDR */);
}

CBaseRenderer* CRendererAML::Create(CVideoBuffer *buffer)
{
  if (buffer && dynamic_cast<CAMLVideoBuffer*>(buffer))
    return new CRendererAML();
  return nullptr;
}

bool CRendererAML::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("amlogic", CRendererAML::Create);
  return true;
}

// Single source of truth for the GUI/OSD encoding decision, shared by Configure
// (which applies it) and ConfigChanged (which detects that it has gone stale).
// Deliberately side-effect free: it reports the encoding the output REQUIRES,
// never whether the composite could actually be built.
CRendererAML::GuiEncoding CRendererAML::ResolveGuiEncoding(const VideoPicture& picture,
                                                          unsigned int dvOutputMode,
                                                          bool isHdrDisplay)
{
  const bool core_is_pq(dvOutputMode == DOLBY_VISION_OUTPUT_MODE_IPT ||
                        dvOutputMode == DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL ||
                        dvOutputMode == DOLBY_VISION_OUTPUT_MODE_HDR10);
  if (core_is_pq)
    return GuiEncoding::Composite;

  // BYPASS = no DV-core forcing: native path, PQ only when the stream itself is
  // HDR PQ/HLG on an HDR display.
  const bool native_is_pq(dvOutputMode == DOLBY_VISION_OUTPUT_MODE_BYPASS &&
    (picture.hdrType == StreamHdrType::HDR_TYPE_HLG || picture.color_transfer == AVCOL_TRC_SMPTE2084) &&
    isHdrDisplay);
  return native_is_pq ? GuiEncoding::Scalar : GuiEncoding::Srgb;
}

bool CRendererAML::ConfigChanged(const VideoPicture& picture)
{
  // A decoder swap can change the resolved DV output mode while every picture
  // parameter stays identical (BD menu-domain segments are force-mapped to DV,
  // feature titles are not). IsSameParams cannot see that, so ask for the
  // reconfigure here - otherwise the GUI keeps the previous segment's encoding
  // for the rest of playback. Only a change of ENCODING CLASS reconfigures: a
  // benign mode reshuffle within the same class (IPT <-> IPT_TUNNEL) does not,
  // so this adds no renderer churn during steady playback, where the mode is
  // constant anyway.
  if (!m_bConfigured)
    return false;

  const unsigned int dv_output_mode(aml_dv_get_output_mode());

  // A DV disc session holds the wire in DV across the no-source gaps of every
  // decoder swap (the kernel VSIF hold), and CloseDecoder parks the published
  // mode at BYPASS for the whole of each gap. A BYPASS reading during a session
  // is therefore the volatile teardown value, not a real output change: acting
  // on it would recreate the renderer twice per decoder swap - and briefly run a
  // DV menu through the scalar path, which is not the transform the BD-J
  // pre-inversion assumes. Wait for the session's next real Configure instead.
  if (dv_output_mode == DOLBY_VISION_OUTPUT_MODE_BYPASS && aml_dv_disc_session())
    return false;

  return ResolveGuiEncoding(picture, dv_output_mode, m_isHdrDisplay) != m_guiEncoding;
}

bool CRendererAML::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  // Configure the GUI/OSD encoding to match the ACTUAL video-plane output the
  // sink receives this stream, not the source hdrType. CAMLCodec::OpenDecoder
  // resolved and published the output mode (after VS10 tunnel resolution and
  // non-DV-display coercion): PQ for a DV tunnel (IPT/IPT_TUNNEL) or a
  // VS10/DV-converted HDR10 output, SDR for SDR10/SDR8, BYPASS for native
  // passthrough. Keying on the resolved output fixes the two mismatches VS10
  // could otherwise produce - HDR10 output on a non-DV display whose source
  // wasn't tagged PQ (OSD left sRGB, dim/desaturated), and a DV source forced to
  // SDR on an HDR display (OSD PQ-encoded then tone-mapped a second time).
  const unsigned int dv_output_mode(aml_dv_get_output_mode());
  m_isHdrDisplay = CServiceBroker::GetWinSystem()->IsHDRDisplay();
  m_guiEncoding = ResolveGuiEncoding(picture, dv_output_mode, m_isHdrDisplay);
  const bool core_is_pq(m_guiEncoding == GuiEncoding::Composite);
  const bool gui_is_pq(m_guiEncoding != GuiEncoding::Srgb);

  // WHO OWNS the OSD plane's sRGB -> BT.2020 PQ encode decides what Kodi may do,
  // and the two output classes differ:
  //
  //  - DV core engaged (core_is_pq): the kernel's vpp_matrix_update() returns
  //    early while is_amdv_on() on VD1 (amvecm/amcsc.c), so the VPP never
  //    programs its OSD transfer stage; DV core2 consumes the OSD plane as
  //    declared by graphic_fmt below. Nothing else converts the GUI, so Kodi
  //    must do the full transform itself - the FBO composite.
  //
  //  - DV core idle (native_is_pq, i.e. BYPASS native HDR10/HLG passthrough):
  //    the VPP encodes the OSD plane itself. On G12A+ (all our SoCs bar TL1)
  //    amvecm/amcsc_pip.c video_post_process() programs OSD1_HDR with SDR_HDR
  //    for an HDR10 source at PROC_BYPASS, and SDR_HLG for HLG - a real EOTF ->
  //    BT.709->BT.2020 -> ST2084 encode in hardware. A Kodi-side transform on
  //    top of it encodes TWICE: gamut compressed twice (desaturated) and
  //    midtones lifted several stops with white unchanged, i.e. a washed,
  //    over-bright GUI. So on this path Kodi must NOT transform; it keeps
  //    upstream CoreELEC's behaviour of only trimming GUI brightness with the
  //    m_sdrPeak scalar (guipeakluminance), which is what that setting exists
  //    for on a kernel-encoded OSD plane.
  //    HDR10, HDR10+ and HLG at PROC_BYPASS all take that OSD encode. The one
  //    exception is hdr_policy==2 with target_format BT_BYPASS, where the VPP
  //    leaves OSD1_HDR at HDR_BYPASS and the GUI would be left under-encoded
  //    (dim) instead. That corner needs the DV core forced to a non-bypass
  //    target, which makes is_amdv_on() true and lands us in the core_is_pq
  //    branch above instead, so it is not reachable from here in practice.
  //
  // Hence the composite is gated on core_is_pq, NOT on gui_is_pq. If its
  // shader/LUTs fail to build, fall back to the per-primitive encode so a DV GUI
  // stays visible rather than black.
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  const bool composite(core_is_pq && winSystem->SetGuiCompositing(AVCOL_TRC_SMPTE2084));
  if (!core_is_pq)
    winSystem->SetGuiCompositing(0);

  // Per-primitive m_sdrPeak encode: the intended path for native BYPASS output
  // (scalar trim only, the VPP does the transform), and the fallback when a DV
  // composite could not be built. Never alongside an active composite - that
  // would encode twice, once per primitive into the FBO and once in the pass.
  const bool per_primitive_pq(gui_is_pq && !composite);
  if (core_is_pq && !composite)
    CLog::Log(LOGWARNING, "CRendererAML::Configure - GUI composite unavailable, "
                          "falling back to the m_sdrPeak scalar encode");
  winSystem->GetGfxContext().SetTransferPQ(per_primitive_pq);

  // Report the GUI transform and the graphic_fmt actually written below, so an
  // OSD-brightness complaint can be triaged from the log alone.
  CLog::Log(LOGDEBUG, "CRendererAML::Configure - resolved DV output mode {}, GUI transform {} ({})",
    dv_output_mode,
    !gui_is_pq ? "none, plain sRGB" : composite ? "sRGB->BT.2020 PQ, FBO composite"
      : core_is_pq ? "m_sdrPeak scalar, composite unavailable"
                   : "m_sdrPeak scalar only, the VPP encodes the OSD plane",
    gui_is_pq ? "graphic_fmt=9 FORMAT_HDR8" : "graphic_fmt=2 FORMAT_SDR");

  // The DV core2 graphics-input declaration must match the GUI encoding set
  // above. When the GUI plane is PQ-encoded and the DV core composites it
  // (DV output, or VS10 conversions), core2 has to read it as FORMAT_HDR8
  // (PQ 8-bit) - its FORMAT_SDR default assumes sRGB and applies a second
  // SDR->DV mapping on top of the PQ encode, rendering menus/subtitles/OSD
  // dim (~29% white) and desaturated. When the GUI stays sRGB (SDR output,
  // e.g. VS10 to an SDR display) core2 must keep the sRGB assumption. With
  // the DV core inactive the value is unread, so pairing it unconditionally
  // with the transfer flag is always safe.
  //
  // This stays keyed on gui_is_pq, NOT on composite: the m_sdrPeak scalar path
  // must also be declared FORMAT_HDR8. That looks wrong - a scalar is not a PQ
  // encode - but it is the validated pairing, and deliberately so: the default
  // peak maps to (0.7*40+30)/100 = 0.58, which is the ST2084 code for ~203 nits,
  // so core2 reading the scalar output AS PQ lands GUI white on BT.2408
  // reference white. Declaring FORMAT_SDR here instead makes core2 apply its
  // sRGB EOTF to that same 0.58, i.e. 0.58^2.4-ish = ~29% white - exactly the
  // dim, desaturated OSD that commit 27bb0c4791 fixed and had confirmed on-box
  // against a UB820. So the scalar path is only ever safe under FORMAT_HDR8.
  CSysfsPath("/sys/class/amdolby_vision/graphic_fmt",
             gui_is_pq ? 9 /* FORMAT_HDR8 */ : 2 /* FORMAT_SDR */);

  m_bConfigured = true;

  return true;
}

CRenderInfo CRendererAML::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_numRenderBuffers;
  info.opaque_pointer = (void *)this;
  return info;
}

void CRendererAML::AddVideoPicture(const VideoPicture &picture, int index)
{
  ReleaseBuffer(index);

  BUFFER &buf(m_buffers[index]);
  if (picture.videoBuffer)
  {
    buf.videoBuffer = picture.videoBuffer;
    buf.videoBuffer->Acquire();
  }
}

void CRendererAML::ReleaseBuffer(int idx)
{
  BUFFER &buf(m_buffers[idx]);
  if (buf.videoBuffer)
  {
    CAMLVideoBuffer *amli(dynamic_cast<CAMLVideoBuffer*>(buf.videoBuffer));
    if (amli)
    {
      if (amli->m_amlCodec)
      {
        amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, true, amli->m_sessionGen);
        amli->m_amlCodec = nullptr; // Released
      }
      amli->Release();
    }
    buf.videoBuffer = nullptr;
  }
}

bool CRendererAML::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_NONLINSTRETCH ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_ROTATION)
    return true;

  return false;
}

void CRendererAML::Reset()
{
  std::array<int, 2> reset_arr[m_numRenderBuffers];
  m_prevVPts = DVD_NOPTS_VALUE;

  for (int i = 0 ; i < m_numRenderBuffers ; ++i)
  {
    reset_arr[i][0] = i;

    if (m_buffers[i].videoBuffer)
      reset_arr[i][1] = dynamic_cast<CAMLVideoBuffer *>(m_buffers[i].videoBuffer)->m_bufferIndex;
    else
      reset_arr[i][1] = 0;
  }

  std::sort(std::begin(reset_arr), std::end(reset_arr),
    [](const std::array<int, 2>& u, const std::array<int, 2>& v)
    {
      return u[1] < v[1];
    });

  for (int i = 0; i < m_numRenderBuffers; ++i)
  {
    if (m_buffers[reset_arr[i][0]].videoBuffer)
    {
      m_buffers[reset_arr[i][0]].videoBuffer->Release();
      m_buffers[reset_arr[i][0]].videoBuffer = nullptr;
    }
  }
}

bool CRendererAML::Flush(bool saveBuffers)
{
  Reset();
  return saveBuffers;
};

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  ManageRenderArea();

  CAMLVideoBuffer *amli = dynamic_cast<CAMLVideoBuffer *>(m_buffers[index].videoBuffer);
  if(amli && amli->m_amlCodec)
  {
    uint64_t pts = amli->m_omxPts;
    if (pts != m_prevVPts)
    {
      amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, m_prevVPts == DVD_NOPTS_VALUE,
                                     amli->m_sessionGen);
      amli->m_amlCodec->SetVideoRect(m_sourceRect, m_destRect);
      amli->m_amlCodec = nullptr; //Mark frame as processed
      m_prevVPts = pts;
    }
  }
  CAMLCodec::PollFrame();
}
