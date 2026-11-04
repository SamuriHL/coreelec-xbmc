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
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "cores/VideoPlayer/VideoRenderers/VideoGeometry.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/WinSystemAmlogic.h"
#include "windowing/WinSystem.h"

#include <iterator>

CRendererAML::CRendererAML()
 : m_prevVPts(DVD_NOPTS_VALUE)
 , m_bConfigured(false)
{
  CLog::Log(LOGINFO, "Constructing CRendererAML");
}

namespace
{
// RenderStereoMode's names, for 5.V3-1's counter
constexpr const char* STEREO_MODE_NAMES[] = {"OFF",
                                             "SPLIT_HORIZONTAL",
                                             "SPLIT_VERTICAL",
                                             "ANAGLYPH_RED_CYAN",
                                             "ANAGLYPH_GREEN_MAGENTA",
                                             "ANAGLYPH_YELLOW_BLUE",
                                             "INTERLACED",
                                             "CHECKERBOARD",
                                             "HARDWAREBASED",
                                             "MONO"};
static_assert(std::size(STEREO_MODE_NAMES) == static_cast<size_t>(RenderStereoMode::COUNT));
} // namespace

CRendererAML::~CRendererAML()
{
  Reset();
  // 5.V3-1 (Q4's plan gate): 0 not-OFF calls in every mode tried
  for (int mode = 0; mode < STEREO_MODES; mode++)
  {
    if (const uint32_t calls = m_stereoCalls[mode].load(std::memory_order_relaxed))
      CLog::Log(LOGINFO, "GEOMDIAG stereo view in RenderUpdate: mode {} calls {} not off {}",
                STEREO_MODE_NAMES[mode], calls,
                m_stereoNotOff[mode].load(std::memory_order_relaxed));
  }
  if (m_shadowB.evals)
    CLog::Log(LOGINFO,
              "GEOMDIAG shadowB renderer: evals {} match {} PENDING {} GIVEN_UP {} windowed {} "
              "unexplained {} no snapshot {} stereo string {}",
              m_shadowB.evals, m_shadowB.match, m_shadowB.pending, m_shadowB.givenUp,
              m_shadowB.windowed, m_shadowB.unexplained, m_shadowB.noSnapshot,
              m_shadowB.stereoString);
}

void CRendererAML::EndRenderSession()
{
  // GUI returns to sRGB - tear down the HDR FBO composite (if any), clear the
  // per-primitive PQ flag, and restore the matching core2 graphics declaration.
  //
  // Only when playback ends, never in the destructor: CRenderManager::Configure
  // deletes and recreates the renderer for every new video segment, and doing
  // this there flipped graphic_fmt to SDR and OSD passthrough off mid-session,
  // freed both GUI FBOs and recompiled the composite, with the last PQ menu or
  // subtitle frame still on the OSD plane - dimmed on DV, re-encoded on HDR10
  // until the next Configure put it back. The next segment's Configure sets
  // the regime it needs; a change of regime is written there.
  CServiceBroker::GetWinSystem()->SetGuiCompositing(0);
  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(false);
  CSysfsPath("/sys/class/amdolby_vision/graphic_fmt", 2 /* FORMAT_SDR */);
  CSysfsPath(OSD_PQ_PASSTHROUGH, 0);
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

// Kernel switch (common_drivers patch 0016): with it set, the VPP leaves the OSD
// planes' transfer alone on native PQ output and applies only the BT.2020
// RGB->YUV matrix, so Kodi can hand it an already PQ-encoded GUI.
const char* const CRendererAML::OSD_PQ_PASSTHROUGH =
    "/sys/module/aml_media/parameters/osd_pq_passthrough";

bool CRendererAML::OsdPqPassthroughAvailable()
{
  static const bool available = CSysfsPath(OSD_PQ_PASSTHROUGH).Exists();
  return available;
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
  if (!native_is_pq)
    return GuiEncoding::Srgb;

  // Native PQ output composites like the DV core does when the kernel can be
  // told to pass the OSD through (see Configure). HLG output keeps the VPP's
  // own SDR->HLG encode and the scalar trim.
  const bool native_hlg(picture.hdrType == StreamHdrType::HDR_TYPE_HLG &&
                        picture.color_transfer != AVCOL_TRC_SMPTE2084);
  return !native_hlg && OsdPqPassthroughAvailable() ? GuiEncoding::Composite
                                                    : GuiEncoding::Scalar;
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
  // DV menu through the scalar path, where its PQ graphics are tone-mapped to
  // sRGB instead of drawn as authored. Wait for the session's next real
  // Configure instead.
  if (dv_output_mode == DOLBY_VISION_OUTPUT_MODE_BYPASS && aml_dv_disc_session())
    return false;

  const SGuiState state = m_guiState;
  return ResolveGuiEncoding(picture, dv_output_mode, state.isHdrDisplay) != state.encoding;
}

bool CRendererAML::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  // one read of the DV mode and the sink for both halves
  const unsigned int dv_output_mode(aml_dv_get_output_mode());
  const bool isHdrDisplay =
      DISPLAYSTATE::HdrDisplay(CServiceBroker::GetWinSystem()->IsHDRDisplay());
  ConfigureVideo(picture, fps, orientation, isHdrDisplay, dv_output_mode);
  ApplyGeometry();
  ApplyGuiEncoding(picture, dv_output_mode, isHdrDisplay);
  return true;
}

void CRendererAML::ConfigureVideo(const VideoPicture& picture,
                                  float fps,
                                  unsigned int orientation,
                                  bool isHdrDisplay,
                                  unsigned int dvOutputMode)
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
  if (aml_geometrytick_inputs_on())
    m_videoStereoMode = picture.stereoMode;
  StoreGuiEncoding(picture, dvOutputMode, isHdrDisplay);
  m_bConfigured = true;
}

void CRendererAML::GetConfigureFacts(SGeometryIn& in) const
{
  in.sourceWidth = m_sourceWidth;
  in.sourceHeight = m_sourceHeight;
  in.flags = m_iFlags;
  in.orientation = m_renderOrientation;
  in.aspectRatio = GetAspectRatio();
  in.alwaysClip = AlwaysClip();
  in.videoStereoMode = m_videoStereoMode;
}

void CRendererAML::ApplyGeometry()
{
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();
  m_geometryApplied = true;
}

void CRendererAML::StoreGuiEncoding(const VideoPicture& picture,
                                    unsigned int dvOutputMode,
                                    bool isHdrDisplay)
{
  m_guiState = SGuiState{ResolveGuiEncoding(picture, dvOutputMode, isHdrDisplay), isHdrDisplay};
}

const char* CRendererAML::GuiEncodingName() const
{
  switch (m_guiState.load().encoding)
  {
    case GuiEncoding::Srgb:
      return "srgb";
    case GuiEncoding::Scalar:
      return "scalar";
    case GuiEncoding::Composite:
      return "composite";
  }
  return "unknown";
}

void CRendererAML::ApplyGuiEncoding(const VideoPicture& picture,
                                    unsigned int dv_output_mode,
                                    bool isHdrDisplay)
{
  // Configure the GUI/OSD encoding to match the ACTUAL video-plane output the
  // sink receives this stream, not the source hdrType. CAMLCodec::OpenDecoder
  // resolved and published the output mode (after VS10 tunnel resolution and
  // non-DV-display coercion): PQ for a DV tunnel (IPT/IPT_TUNNEL) or a
  // VS10/DV-converted HDR10 output, SDR for SDR10/SDR8, BYPASS for native
  // passthrough. Keying on the resolved output fixes the two mismatches VS10
  // could otherwise produce - HDR10 output on a non-DV display whose source
  // wasn't tagged PQ (OSD left sRGB, dim/desaturated), and a DV source forced to
  // SDR on an HDR display (OSD PQ-encoded then tone-mapped a second time).
  const GuiEncoding encoding = ResolveGuiEncoding(picture, dv_output_mode, isHdrDisplay);
  const bool core_is_pq(encoding == GuiEncoding::Composite);
  const bool gui_is_pq(encoding != GuiEncoding::Srgb);
  const bool native_output(dv_output_mode == DOLBY_VISION_OUTPUT_MODE_BYPASS);

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
  //    The VPP encode cannot carry PQ-authored graphics (Blu-ray PGS): its
  //    input is sRGB, so nothing outside BT.709 or above SDR white survives.
  //    So when the kernel has osd_pq_passthrough (patch 0016), native PQ output
  //    resolves to Composite as well, and the switch below turns the VPP's OSD
  //    SDR->HDR encode into a pass-through with the BT.2020 matrix. HLG, and a
  //    kernel without the switch, stay on the scalar path described here.
  //
  // Hence the composite is gated on core_is_pq (Kodi owns the transform), NOT
  // on gui_is_pq. If its shader/LUTs fail to build, fall back to the
  // per-primitive encode so a DV GUI stays visible rather than black.
  CWinSystemBase* const winSystem = CServiceBroker::GetWinSystem();
  const bool composite(core_is_pq && winSystem->SetGuiCompositing(AVCOL_TRC_SMPTE2084));
  if (!core_is_pq)
    winSystem->SetGuiCompositing(0);

  // Only a native composite hands the VPP a PQ plane; with the DV core engaged
  // the VPP OSD stage is not programmed at all, and a failed composite falls
  // back to the scalar trim, which needs the VPP's own encode.
  const bool osd_pq_passthrough(composite && native_output);
  CSysfsPath(OSD_PQ_PASSTHROUGH, osd_pq_passthrough ? 1 : 0);

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
    !gui_is_pq ? "none, plain sRGB"
      : osd_pq_passthrough ? "sRGB->BT.2020 PQ, FBO composite, VPP OSD passthrough"
      : composite ? "sRGB->BT.2020 PQ, FBO composite"
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
  if (!saveBuffers)
    Reset();
  return saveBuffers;
};

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  // 5.V2-4: main must apply a swapped-in renderer's geometry before it draws it
  if (!m_geometryApplied && !m_geometryErrorLogged)
  {
    m_geometryErrorLogged = true;
    CLog::Log(LOGERROR, "VCONFIGURE drawn before its duties: SetVideoRect before SetViewMode");
  }
  // 5.V3-1 (log only): the GUI stereo view main's geometry evaluation runs under, per mode
  {
    CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
    const int mode = static_cast<int>(gfx.GetStereoMode());
    if (mode >= 0 && mode < STEREO_MODES)
    {
      m_stereoCalls[mode].fetch_add(1, std::memory_order_relaxed);
      const RenderStereoView view = gfx.GetStereoView();
      if (view != RenderStereoView::OFF &&
          m_stereoNotOff[mode].fetch_add(1, std::memory_order_relaxed) == 0)
        CLog::Log(LOGWARNING, "GEOMDIAG stereo view {} in RenderUpdate, mode {}",
                  view == RenderStereoView::LEFT ? "LEFT" : "RIGHT", STEREO_MODE_NAMES[mode]);
    }
  }
  ManageRenderArea();

  if (m_offThreadPresent)
  {
    std::shared_ptr<CAMLCodec> codec;
    {
      std::lock_guard<std::mutex> lock(m_rectMutex);
      codec = std::move(m_rectCodec);
    }
    if (codec)
    {
      codec->SetVideoRect(m_sourceRect, m_destRect);
      m_geometryCodec = codec;
    }
    // paused, or between frames: a zoom, view mode or window change shows now
    else if (const auto shown = m_geometryCodec.lock();
             shown && shown->IsOpened() && aml_presentation_validated())
      shown->ApplyVideoGeometry(m_sourceRect, m_destRect, false);
    return;
  }

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

void CRendererAML::ShadowB(uint64_t request)
{
  using namespace VIDEOGEOMETRY;
  m_shadowB.evals++;
  GUISNAPSHOT::SSnapshot read;
  if (GUISNAPSHOT::Read(read))
    m_shadowSnap = read;
  if (!m_shadowSnap.valid)
  {
    m_shadowB.noSnapshot++;
    return;
  }
  const GUISNAPSHOT::SSnapshot& snap = m_shadowSnap;
  const auto state = DISPLAYSTATE::Get();
  if (!state)
  {
    m_shadowB.noSnapshot++;
    return;
  }

  // the latest mode publish n against main's echo (plan §1.1: PENDING, TAKEN, GIVEN_UP)
  enum class Mode
  {
    TAKEN,
    PENDING,
    GIVEN_UP,
  };
  const uint64_t n = state->modeSeq;
  const Mode mode = n == 0                    ? Mode::TAKEN
                    : snap.modeSeqSeen < n    ? Mode::PENDING
                    : snap.rowMoved           ? Mode::TAKEN
                                              : Mode::GIVEN_UP;
  // a request is honoured until main finished its notice
  const bool requested = (request & 1) && (request >> 1) > snap.noticeSeqDone;
  const bool fullScreen = requested || snap.fullScreenVideo;
  const RenderStereoMode stereo = static_cast<RenderStereoMode>(snap.stereoMode);
  GUISNAPSHOT::SRow rawRow = snap.rawRow;
  GUISNAPSHOT::SRow row = snap.row;
  if (fullScreen && mode != Mode::TAKEN)
  {
    rawRow = GUISNAPSHOT::WRowOf(*state);
    row = GUISNAPSHOT::StereoRow(rawRow, stereo);
  }

  // C's render area
  SRenderAreaIn area;
  area.viewRect = fullScreen || snap.calibrating
                      ? CRect(row.overscanLeft, row.overscanTop, row.overscanRight,
                              row.overscanBottom)
                      : snap.videoRect;
  area.sourceWidth = m_sourceWidth;
  area.sourceHeight = m_sourceHeight;
  area.flags = m_iFlags;
  area.aspectRatio = GetAspectRatio();
  area.pixelRatio = CDisplaySettings::GetInstance().GetPixelRatio();
  area.zoomAmount = CDisplaySettings::GetInstance().GetZoomAmount();
  area.verticalShift = CDisplaySettings::GetInstance().GetVerticalShift();
  area.guiPixelRatio = row.pixelRatio;
  area.errorInAspect =
      OUTPUT::CSettingsMirror::Get().GetInt(CSettings::SETTING_VIDEOPLAYER_ERRORINASPECT);
  area.orientation = m_renderOrientation;
  area.clip = AlwaysClip() || !(fullScreen || snap.calibrating);
  const SRenderAreaOut c = ComputeRenderArea(area);

  // main's VD1 axis inputs, as ApplyVideoGeometry reads them, and C's
  CGraphicContext& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
  const int rotation = static_cast<int>(m_renderOrientation / 90) % 4;
  const auto shown = m_geometryCodec.lock();
  const std::string liveStereo = shown ? shown->VideoStereoMode() : m_videoStereoMode;
  if (liveStereo != m_videoStereoMode)
    m_shadowB.stereoString++;
  const RESOLUTION_INFO& videoRow =
      CDisplaySettings::GetInstance().GetResolutionInfo(gfx.GetVideoResolution());
  SAmlAxisIn mainAxisIn;
  mainAxisIn.dest = AmlRotatedDest(m_destRect, rotation);
  mainAxisIn.screen = CRect(0, 0, videoRow.iScreenWidth, videoRow.iScreenHeight);
  mainAxisIn.guiWidth = videoRow.iWidth;
  mainAxisIn.guiHeight = videoRow.iHeight;
  mainAxisIn.stereoMode = gfx.GetStereoMode();
  mainAxisIn.videoStereoMode = liveStereo;
  const RESOLUTION_INFO resInfo = gfx.GetResInfo();
  mainAxisIn.hwHeight = resInfo.iHeight;
  mainAxisIn.hwBlanking = resInfo.iBlanking;
  SAmlAxisIn cAxisIn;
  cAxisIn.dest = AmlRotatedDest(c.destRect, rotation);
  cAxisIn.screen = CRect(0, 0, state->screenWidth, state->screenHeight);
  cAxisIn.guiWidth = rawRow.width;
  cAxisIn.guiHeight = rawRow.height;
  cAxisIn.stereoMode = stereo;
  cAxisIn.videoStereoMode = m_videoStereoMode;
  cAxisIn.hwHeight = row.height;
  cAxisIn.hwBlanking = row.blanking;
  const SAmlAxisOut mainAxis = ComputeAmlAxis(mainAxisIn);
  const SAmlAxisOut cAxis = ComputeAmlAxis(cAxisIn);

  const bool same = c.sourceRect == m_sourceRect && c.destRect == m_destRect &&
                    std::equal(std::begin(cAxis.axis), std::end(cAxis.axis),
                               std::begin(mainAxis.axis));
  if (same)
  {
    m_shadowB.match++;
    return;
  }
  const char* kind;
  if (mode == Mode::PENDING)
  {
    m_shadowB.pending++;
    kind = "PENDING";
  }
  else if (mode == Mode::GIVEN_UP && fullScreen)
  {
    m_shadowB.givenUp++;
    kind = "GIVEN_UP";
  }
  else if (!fullScreen && !snap.calibrating && gfx.GetVideoRect() != snap.videoRect)
  {
    m_shadowB.windowed++;
    kind = "windowed";
  }
  else
    kind = m_shadowB.unexplained++ < 10 ? "unexplained" : nullptr;
  if (kind)
    CLog::Log(kind[0] == 'u' ? LOGWARNING : LOGINFO,
              "GEOMDIAG shadowB DIFF {}: main dst {:.0f},{:.0f},{:.0f},{:.0f} axis {} {} {} {} "
              "fs {}; C dst {:.0f},{:.0f},{:.0f},{:.0f} axis {} {} {} {} fs {} (request {} "
              "notice done {}) modeSeq {} seen {} rowMoved {} calibrating {} stereo {}",
              kind, m_destRect.x1, m_destRect.y1, m_destRect.x2, m_destRect.y2,
              mainAxis.axis[0], mainAxis.axis[1], mainAxis.axis[2], mainAxis.axis[3],
              gfx.IsFullScreenVideo(), c.destRect.x1, c.destRect.y1, c.destRect.x2,
              c.destRect.y2, cAxis.axis[0], cAxis.axis[1], cAxis.axis[2], cAxis.axis[3],
              fullScreen, request >> 1, snap.noticeSeqDone, n, snap.modeSeqSeen, snap.rowMoved,
              snap.calibrating, snap.stereoMode);
}

void CRendererAML::PresentFrame(int index)
{
  CAMLVideoBuffer* amli = dynamic_cast<CAMLVideoBuffer*>(m_buffers[index].videoBuffer);
  if (!amli || !amli->m_amlCodec)
    return;

  uint64_t pts = amli->m_omxPts;
  if (pts == m_prevVPts)
    return;

  amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, m_prevVPts == DVD_NOPTS_VALUE,
                                 amli->m_sessionGen);
  {
    std::lock_guard<std::mutex> lock(m_rectMutex);
    m_rectCodec = std::move(amli->m_amlCodec); // also marks the frame processed
  }
  m_prevVPts = pts;
}
