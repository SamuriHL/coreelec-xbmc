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
#include <functional>
#include <string>

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

//! 5.G2 GT-4 (plan M3): the encoding main applied last, as it wrote the knobs for it. Until GT-6
//! moves the knobs to C, G draws in this one (not in W's published class), so the graphics change
//! encoding when main writes the knobs, as main's own image cache does today. The peak is not
//! here: main's GUI reads it live from the settings mirror, and G reads it from there too.
//! Published whether or not the regime above is on (gregime_off); nothing is logged.
struct MainApplied
{
  uint64_t epoch = 0; //!< 0: main has applied nothing yet
  GuiEncoding encoding = GuiEncoding::Srgb; //!< in effect: Scalar when the composite failed
  bool limited = false;
  uint8_t tag = 0; //!< GT-6: KnobTag of what main draws in now (0 before its first apply)
};

// 5.G2 GT-6 (design §10.2 item 9, D-G2-3 second half; plan "GT-6"): on a graphicstick route
// session C writes the two plane-encoding knobs (osd_pq_passthrough, graphic_fmt) for W's
// published regime, one tick before the first OSD buffer drawn in it is committed; main applies
// only its GL side and tags its GUI buffers. special://profile/gknob_off: main writes the knobs
// and G follows main's applied encoding (GT-5).

//! the knob values of a regime
struct SKnobs
{
  bool osdPq = false; //!< osd_pq_passthrough 1
  bool hdr8 = false; //!< graphic_fmt 9 (FORMAT_HDR8), else 2 (FORMAT_SDR)
  bool operator==(const SKnobs& o) const { return osdPq == o.osdPq && hdr8 == o.hdr8; }
  bool operator!=(const SKnobs& o) const { return !(*this == o); }
};
//! what an OSD buffer is drawn for: the class in effect and the knobs it is right under. C
//! commits a tagged buffer only under its own tag; 0 is knob-neutral (a cleared buffer)
uint8_t KnobTag(GuiEncoding effective, SKnobs knobs);
SKnobs TagKnobs(uint8_t tag);
GuiEncoding TagEncoding(uint8_t tag);
//! W's published regime as a tag (the composite's availability decides the class in effect)
uint8_t RegimeTag(const GraphicsRegime& regime);
//! "<class>/<osd_pq>/<graphic_fmt>", for the logs
std::string TagName(uint8_t tag);

//! latched at RegimeInit: the regime is on, the route requested, no gknob_off
bool KnobsAllowed();
//! C writes the knobs now (main does not)
bool KnobOwnerC();
//! RM, when a route session starts: the knobs go to C
void KnobRouteOn();
//! RM, at the stop of a route session (before the route ends): C keeps the knobs until it has
//! written the stop regime (bounded)
void KnobStopOwed();
//! main (ApplyGuiEncoding, EndRenderSession), every apply: what it would write. true: main
//! writes them now (C does not own them); then it calls NoteKnobsWritten
bool MainWritesKnobs(SKnobs knobs);
//! C: the knobs go back to main; false when a route came on meanwhile (they stay C's). haveWanted
//! with main's latest wanted (C writes it when it is not on the kernel already, so an apply main
//! skipped while C owned them is not lost)
bool KnobHandBack(SKnobs& wanted, bool& haveWanted);
//! the stop C owes, for the hand-back: owed since (0: none) and whether W published the stop
//! regime since
int64_t KnobStopOwedNs();
bool KnobStopPublished();
//! any writer, after its write
void NoteKnobsWritten(SKnobs knobs);
//! the knobs as last written; false before the first write
bool KnobsWritten(SKnobs& knobs);
//! C: main's GUI is to be drawn again (C's cleared buffer is up, or main caught up with the
//! presented regime); main's PresentRender reads the count and repaints a few frames
void RequestGuiRepaint();
uint64_t GuiRepaints();
//! W, before a no-op job's DV writes: the tag W's publish will carry after
//! aml_dv_set_vs10_mode_now(mode) (its own refusals included); the current one when it cannot
//! change
uint8_t PredictVs10Tag(unsigned int mode);
//! W, before a decoder open's DV writes (DvApply): the tag W's publish will carry for the new
//! stream's facts and the mode the pure resolver (aml_dv_resolve_output) predicts
uint8_t PredictStreamTag(StreamHdrType hdrType,
                         AVColorTransferCharacteristic transfer,
                         unsigned int mode);
//! W: today's unparked VS10 write. On a route session with the knobs on C, a write that changes
//! the regime runs under C's OSD hold (design §4.2): the OSD blank from before the DV writes to
//! the new regime's first slot, instead of old-encoding OSD under the new core output
void RunVs10Write(unsigned int mode);

/*!
 * \brief W, around a no-op job's DV writes (a VS10 write, a decoder open's DvApply): on a route
 * session with the knobs on C, C's OSD hold while the predicted tag differs from the published
 * one (waits at most bound for C's cleared buffers to flip). At the end the prediction is checked
 * against what W published: a miss is logged ("REGIME predict miss") and counted. The hold only
 * blanks: C orders the knobs from the published regime alone, so a miss costs one blank too many
 * or one missing, never the order.
 */
class COsdHoldScope
{
public:
  //! predict runs only on a route session with the knobs on C (off it, nothing is read)
  COsdHoldScope(const std::function<uint8_t()>& predict,
                const char* why,
                unsigned int mode,
                int boundMs);
  ~COsdHoldScope();
  COsdHoldScope(const COsdHoldScope&) = delete;
  COsdHoldScope& operator=(const COsdHoldScope&) = delete;

private:
  bool m_route = false;
  bool m_held = false;
  uint8_t m_from = 0;
  uint8_t m_predicted = 0;
  const char* m_why = "";
  unsigned int m_mode = 0;
  int64_t m_beginNs = 0;
};
//! C's report: predictions checked and missed since the last call (reset)
void TakePredictCounts(uint64_t& checked, uint64_t& missed);

//! main, at the end of each ApplyGuiEncoding and at EndRenderSession's stop knobs
void PublishMainApplied(GuiEncoding encoding, bool limited, SKnobs knobs);
//! any thread
MainApplied GetMainApplied();

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
