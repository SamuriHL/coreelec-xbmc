/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Interface/StreamInfo.h"

#include <cstdint>

extern "C"
{
#include <libavutil/pixfmt.h>
}

// Phase 5 step 5.G2 GT-2 (design draft42 §10.2 item 9, D-G2-3 first half;
// docs/phase5_g2_plan.md GT-2): the graphics regime as published state, in shadow.
//
// W publishes the class (the GUI/OSD encoding the output requires) where it writes the DV
// output mode, so the regime changes with the output and not when main gets round to it. Main
// publishes the GUI-only fields (limited, peak, compositeAvailable) where they change (M6). A
// class change main's per-picture check sees that W did not publish (a DV write off W, a picture
// whose facts differ from its decoder's open) is posted to W, which re-publishes it (M7). Each
// change is a new epoch: from GT-3 G draws in it, from GT-6 C writes the knobs for it.
//
// Until then nothing reads it for behaviour. Main still resolves and writes the knobs exactly as
// today; a shadow compares each of main's applies with the published regime and logs
// "GREGIME shadow ... agree|DISAGREE(<field>)". Validated SoCs with W running only.
// special://profile/gregime_off: nothing is published, posted, compared or logged.

namespace DISPLAYSTATE
{
//! the W writer of a DV output mode (DisplayState.h)
enum DvWriter : unsigned int;

//! the GUI/OSD encoding class, as CRendererAML::ResolveGuiEncoding resolves it
enum class GuiEncoding : uint8_t
{
  Srgb, // plain sRGB: nothing downstream encodes the OSD plane
  Scalar, // m_sdrPeak trim only; the VPP encodes the OSD plane itself
  Composite, // Kodi owns the full sRGB->BT.2020 PQ transform (DV core reads the
             // plane, or the VPP passes it through on native PQ output)
};
const char* GuiEncodingName(GuiEncoding encoding);

//! videoscreen.guipeakluminance as the PQ code the GUI is trimmed to: one rule for main's read
//! (CWinSystemAmlogic::GetGuiSdrPeakLuminance) and the published peak, so the two compare exactly
inline float GuiPeakCode(int setting)
{
  return (0.7f * setting + 30.0f) / 100.0f;
}

struct GraphicsRegime
{
  uint64_t epoch = 0; //!< 0: never published (and under off)
  GuiEncoding encoding = GuiEncoding::Srgb; //!< the design's `class`
  bool limited = false;
  float peak = 0.0f; //!< GuiPeakCode of the setting
  bool compositeAvailable = false;
  // the inputs W resolved the class from: for the logs, no epoch of their own
  unsigned int dvOutputMode = 0;
  bool isHdrDisplay = false;
};

//! latched by RegimeInit; false before it
bool RegimeOn();
//! the published regime (epoch 0 before the first publish and under off)
GraphicsRegime GetRegime();

// main's publishes (GUI-only epochs)
//! GLES InitRenderSystem (validated SoCs), after the composite build: latches the switch at the
//! first call and publishes the seed (Srgb, the GUI at boot); a later call (a render-system
//! re-init) publishes the build's result only
void RegimeInit(bool compositeAvailable, bool limited, float peak);
//! the composite's build or LUTs failed, or built again (SetGuiCompositing)
void PublishRegimeComposite(bool available, const char* why);
//! the settings mirror, at guipeakluminance's store or a profile load
void PublishRegimePeak(int setting);

// W's publishes
//! the decoder open's source facts (DvApply), before its DV output-mode write
void NoteRegimeStream(StreamHdrType hdrType, AVColorTransferCharacteristic transfer);
//! aml_dv_set_output_mode: the class for the new mode (writer: a single DvWriter bit)
void PublishRegimeDvMode(unsigned int mode, DvWriter writer);

// main's requests to W
//! M7: a class change from main's per-picture ConfigChanged (V's Configure, B2-8): posted to W,
//! which re-resolves from its own stream facts (DvApply's alone: a posted picture may be an old
//! decoder's), the current DV mode and caps
void RequestRegimeRepublish(const char* why);
//! EndRenderSession: main's stop regime (Srgb), compared with W's publish of it
void RegimeStop();

// the shadows (log only)
//! main, at the end of each ApplyGuiEncoding: what it applied. composite: the composite is
//! active (read only when encoding is Composite)
void ShadowMainRegime(const char* site,
                      GuiEncoding encoding,
                      bool composite,
                      bool limited,
                      unsigned int dvOutputMode,
                      bool isHdrDisplay);
//! main, at the per-frame peak read of the composite: logged on a change only
void ShadowMainPeak(float peak);

} // namespace DISPLAYSTATE
