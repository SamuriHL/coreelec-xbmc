/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/StreamDetails.h"

#include <cstdint>
#include <string>

enum AML_SUPPORT_H264_4K2K
{
  AML_SUPPORT_H264_4K2K_UNINIT = -1,
  AML_NO_H264_4K2K,
  AML_HAS_H264_4K2K,
  AML_HAS_H264_4K2K_SAME_PROFILE
};

enum AML_DISPLAY_DV_LED
{
  AML_DV_TV_LED = 0,
  AML_DV_PLAYER_LED
};

#define HDR10_PLUS_CAP      (int)(1<<0)
#define HDR10_CAP           (int)(1<<2)
#define SMPTE_ST_2084_CAP   (int)(1<<3)
#define HLG_CAP             (int)(1<<4)

#define DV_2160p60Hz        (int)(1<<2)
#define DV_RGB_444_8BIT     (int)(1<<3)
#define LL_YCbCr_422_12BIT  (int)(1<<5)

// Dolby Vision VS10 engine output modes (kernel amdv output-mode enum). The
// VS10 engine converts the incoming HDR flavor to one of these outputs; BYPASS
// means no VS10 conversion (native passthrough).
#define DOLBY_VISION_OUTPUT_MODE_IPT         (unsigned int)(0)
#define DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL  (unsigned int)(1)
#define DOLBY_VISION_OUTPUT_MODE_HDR10       (unsigned int)(2)
#define DOLBY_VISION_OUTPUT_MODE_SDR10       (unsigned int)(3)
#define DOLBY_VISION_OUTPUT_MODE_SDR8        (unsigned int)(4)
#define DOLBY_VISION_OUTPUT_MODE_BYPASS      (unsigned int)(5)

#define AML_GXBB    0x1F
#define AML_GXL     0x21
#define AML_GXM     0x22
#define AML_G12A    0x28
#define AML_G12B    0x29
#define AML_SM1     0x2B
#define AML_SC2     0x32
#define AML_T7      0x36
#define AML_S4      0x37
#define AML_S5      0x3E
#define AML_S7      0x46
#define AML_S7D     0x47
#define AML_S6      0x48

int  aml_get_cpufamily_id();
// the SoCs the presentation timeline work is validated on (S6, G12B): elsewhere
// the player keeps today's behaviour
bool aml_presentation_validated();
//! 5.C: the decoder keeps the stream's speed across its open and a display
//! hold pauses it; the DV wait is re-armed per file (validated, A/B switch)
bool aml_codec_speed_fix();
std::string aml_get_cpufamily_name(int cpuid = -1);
bool aml_support_hevc();
bool aml_support_hevc_4k2k();
bool aml_support_hevc_8k4k();
bool aml_support_hevc_10bit();
bool aml_support_h266();
AML_SUPPORT_H264_4K2K aml_support_h264_4k2k();
bool aml_support_vp9();
bool aml_support_av1();
bool aml_support_avs2();
bool aml_support_avs3();
bool aml_support_dolby_vision();
bool aml_dolby_vision_enabled();
bool aml_dv_core_active();
// True when this stream will leave the box as player-led low-latency Dolby
// Vision (LLDV): the DV LED setting is player-led AND the output stays Dolby
// Vision (i.e. VS10 is not converting it to HDR10/SDR). In that mode the sink
// re-acquires DV on any mid-stream CM version change, so the per-frame Smart
// CMv4.0 decision has to be pinned.
bool aml_dv_lldv_output_active();
// True when this stream will leave the box as TV-led (display-led) Dolby
// Vision: the DV LED setting is TV-led AND the output stays Dolby Vision. That
// is the only path where the sink itself parses the RPU, so it is the only one
// where rewriting the RPU's CM version can change what the TV does.
bool aml_dv_tvled_output_active();
bool aml_dv_source_engages_core();
bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType);
bool aml_display_support_dv();
bool aml_display_support_3d();
bool aml_display_support_hdr_pq();
bool aml_display_support_hdr_hlg();
bool aml_display_support_hdr10plus();
// HDMI sink audio capabilities parsed from amhdmitx0/aud_cap (read live at
// each call - the EDID can change with the sink). Channel counts: 0 = codec
// absent, -1 = present but no channel field advertised. valid=false = node
// absent (HDMI down / non-Amlogic build); callers must not treat that as
// "no capability".
struct AMLHdmiAudioCaps
{
  bool valid{false};
  int pcm_ch{0};          // LPCM
  bool pcm_192k{false};   // optional 192kHz rate on the LPCM line
  int truehd_ch{0};       // Dolby TrueHD / Atmos-MAT
  int ddp_ch{0};          // Dolby Digital Plus
  bool ddp_atmos{false};  // DD+ Atmos (JOC) substream
  int ac3_ch{0};          // Dolby Digital
  int dtshd_ch{0};        // DTS-HD (MA / High-Res)
  int dts_ch{0};          // plain DTS (core only)
};
AMLHdmiAudioCaps aml_get_hdmi_audio_caps();
// Dolby Vision VS10 engine.
unsigned int aml_vs10_by_setting(const std::string& setting);
unsigned int aml_vs10_by_hdrtype(StreamHdrType hdrType, unsigned int bitDepth);
// Carries the VS10 output mode resolved at stream-open (from the real source
// hdrType, before VideoPlayer fakes it to DOLBYVISION) down to CAMLCodec, which
// applies it. BYPASS means "no VS10 conversion for this stream".
void aml_dv_set_vs10_pending(unsigned int mode);
unsigned int aml_dv_get_vs10_pending();
// The resolved video-plane output mode for the current stream, recorded by
// CAMLCodec::OpenDecoder and read by CRendererAML::Configure so the GUI/OSD PQ
// encoding follows the actual output (IPT/IPT_TUNNEL/HDR10 = PQ, SDR10/SDR8 =
// SDR) rather than the source hdrType. BYPASS = DV core not forcing an output.
void aml_dv_set_output_mode(unsigned int mode);
unsigned int aml_dv_get_output_mode();
bool aml_dv_wire_format_mismatch();
bool aml_dv_wire_format_is_lldv();
// Disc-session DV latch: active while a Blu-ray whose content includes Dolby
// Vision plays in navigation (menu) mode on a DV-capable display. Menu-domain
// segments without their own DV stream (FirstPlay bumpers, menu video loops)
// are VS10-mapped into the DV output instead of dropping the HDMI DV
// signalling at every DV/non-DV segment boundary - each drop is a multi-second
// TV resync (black screen) that the segment's audio plays straight through.
// Explicitly selected titles keep their native format (S&M-style format-demo
// discs must output HDR10/HDR10+ demos untouched).
void aml_dv_set_disc_session(bool active);
bool aml_dv_disc_session();
// Request the disc-session DV output engage. It is applied by the next mode set
// carrying a DV picture (aml_dv_engage_pending_disc_session), so the sink locks
// into DV once, at the film's display mode.
void aml_dv_pre_engage_disc_session();
// Called by CreateNewWindow once the display mode is set: applies a pending
// engage when the window now shows a DV picture.
void aml_dv_engage_pending_disc_session(bool dvPicture);
bool aml_dv_disc_engage_pending();
// The same engage for a native DV file, which has no disc session: requested at
// its decoder open, applied by its mode set, released by its decoder close. The
// sink then switches into DV inside the held start, not after the clock starts.
void aml_dv_request_file_engage();
// Whether a file's engage is applied. Kept apart from aml_dv_disc_engaged(), whose
// session guards (held wire, suppressed live VS10 BYPASS) do not apply to a file.
bool aml_dv_file_engaged();
// The file's release: the ordered teardown, run by CloseDecoder once the decoder
// is closed and the video is off. Clears a request that was never applied.
void aml_dv_release_file_engage();
// Drops a file's request its mode set has not applied (no teardown).
void aml_dv_cancel_file_engage_request();
// MIXED discs (DV present on the disc, but the selected title is native
// HDR10/SDR with conform off): actively release the pre-engaged DV output -
// drop the session VSIF hold and return amdv to follow-source - so the title's
// native EOTF can signal. The hold otherwise blocks the DV signal drop this
// case needs, leaving the sink in DV against a non-DV decode (black screen).
// A later DV-output segment re-engages via aml_dv_pre_engage_disc_session().
void aml_dv_release_disc_engage();
// Whether the disc-session DV output engage is currently applied.
bool aml_dv_disc_engaged();
// Clear a DV core left engaged by a previous run that died before its teardown
// (crash, OOM, the JVM SIGABRT after BD-J). Call once at window-system init.
void aml_dv_recover_stale_disc_session();
unsigned int aml_dv_dolby_vision_mode();
// What the kernel's DV core puts on the wire right now (enabled and in a DV
// tunnel mode), independent of the mode Kodi last published.
bool aml_dv_core_outputs_dv();
// True when a re-decided HDMI attr would be the native one rather than the DV
// tunnel format (core disabled, or forced to HDR10/SDR output).
bool aml_dv_core_leaves_native_wire();
void aml_dv_set_vs10_mode(unsigned int mode);
// Resolve the stored "Dolby Vision" VS10 option (IPT) to the tunnel form the
// current TV-led / player-led output setting actually needs.
unsigned int aml_dv_resolve_tunnel_mode(unsigned int mode);

// What CAMLCodec::OpenDecoder decides for a stream's DV output, as a function of
// inputs taken before it (phase 5 step 5.E1b; DECIDE uses it from 5.E2)
struct AmlDvInputs
{
  bool deviceSupportsDv = false;
  bool userDisabled = false;
  bool streamIsDv = false; // the hint's HDR type after the codec's VS10/HDR10+ rewrite
  int dvProfile = 0;
  bool displaySupportsDv = false;
  bool displaySupportsPq = false;
  unsigned int vs10Pending = DOLBY_VISION_OUTPUT_MODE_BYPASS;
  bool playerLed = false;
  bool stockConvert = false;
  bool discSession = false;
  bool presentationValidated = false;
  bool fileEngageOff = false;
};

struct AmlDvDecision
{
  bool enable = false;
  unsigned int llPolicy = 0;
  int policy = -1; // the dolby_vision_policy written, -1 for none
  unsigned int outputMode = DOLBY_VISION_OUTPUT_MODE_BYPASS;
  bool fileEngage = false;
};

AmlDvDecision aml_dv_resolve_output(const AmlDvInputs& in);

// The HDR type a stream is presented to the decoder as, in today's two stages
// (phase 5 step 5.E1b): at the stream open (CVideoPlayer::OpenStream) and at the
// first keyframe (CDVDVideoCodecAmlogic), where HDR10+ is known
struct AmlHdrOpenInputs
{
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  unsigned int vs10Mode = DOLBY_VISION_OUTPUT_MODE_BYPASS; // for this type, after the disc-session mapping
  bool convertByVsEngine = false; // the legacy HDR2DV/SDR2DV toggle for this type
  bool deviceSupportsDv = false;
  bool displaySupportsDv = false;
  bool hdr10plusConvert = false;
};

struct AmlHdrOpenDecision
{
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  unsigned int vs10Pending = DOLBY_VISION_OUTPUT_MODE_BYPASS;
};

AmlHdrOpenDecision aml_dv_resolve_stream_open(const AmlHdrOpenInputs& in);

// the codec arms the HDR10+ -> DV 8.1 conversion at its open
bool aml_dv_hdr10plus_candidate(bool deviceSupportsDv,
                                bool displaySupportsDv,
                                int dvProfile,
                                StreamHdrType hdrType,
                                bool hdr10plusConvert);

struct AmlHdrFirstFrameDecision
{
  bool toProfile81 = false; // presented as DV 8.1 from the stream's HDR10+
  bool toVs10 = false;      // presented as DV for the VS10 mapping, with vs10Pending
  unsigned int vs10Pending = DOLBY_VISION_OUTPUT_MODE_BYPASS;
};

// an armed candidate at its first keyframe; vs10ForType/convertByVsEngine for its HDR type
AmlHdrFirstFrameDecision aml_dv_resolve_first_frame(bool isHdr10Plus,
                                                    unsigned int vs10ForType,
                                                    bool convertByVsEngine);
// HDR10-output mastering peak override restated to the sink by kernel patch 0011
// (dovi.ko itself ignores it); silently no-ops on a stock kernel.
void aml_dv_apply_target_overrides(unsigned int mode);
void aml_dv_set_hdr10_osd_brightness(int nits);
// Push SETTING_COREELEC_AMLOGIC_DV_GUI_SINK_PEAK to the kernel (read per frame).
void aml_dv_apply_graphics_from_sink();
// Dolby Vision VSVDB max-luminance override: patch the display's advertised v2
// VSVDB block and inject it via aml_media force_vsvdb/vsvdb_data. apply() reads
// the settings and enables/disables accordingly; clear() disables injection.
void aml_dv_apply_vsvdb();
void aml_dv_clear_vsvdb();
// Drop the cached genuine VSVDB; call when the attached display may have changed.
void aml_display_vsvdb_invalidate();
//! W: reads the panel's VSVDB for the decoder side to take (5.B1-3)
void aml_display_vsvdb_refill();
//! D10 (5.B1-4): writes SDR2HDR/HDR2SDR's kernel parameters, or defers them while a player is open
void aml_hdr_conversion_refresh(const char* origin);
//! VideoPlayer's start and exit; at exit a deferred change is applied
void aml_hdr_conversion_player_open(bool open);
// display-mode hold for menu-domain segments of a disc session (Sony-class
// sinks re-train on every re-clock; see AMLUtils.cpp)
void aml_set_disc_mode_hold(bool hold);
bool aml_disc_mode_hold();
//! main thread: the mode set now running is a content start that may re-clock a
//! 1000/1001 rate difference at an unchanged mode string
void aml_set_frac_reclock_request(bool request);
bool aml_frac_reclock_request();

// Disc-session resolution anchor. False until the first video segment of a disc
// session has chosen a resolution from its own content; the mode hold consults it
// so it never anchors the session to the GUI's mode. Reset at player teardown.
bool aml_disc_mode_anchored();
void aml_set_disc_mode_anchored(bool anchored);
// Display's GENUINE VSVDB v2 max luminance in nits (0 = unavailable/not v2).
// Injection-aware: reads the cached panel block, not dv_cap (which reports
// the injected block while an override is live).
int aml_display_vsvdb_max_nits();
// CMv4.0 append settings (mode / Smart threshold / display peak) live-apply.
// The settings thread bumps a generation counter; the codec compares it per
// packet and re-pushes the settings to its CBitstreamConverter when it moves.
// A counter rather than the values themselves because re-pushing the mode
// resets the per-decision logging sentinels - it must happen on change only,
// not every packet.
void aml_dv_cmv40_settings_changed();
unsigned int aml_dv_cmv40_settings_generation();
// Dolby Vision L5 active-area detection: a background thread software-decodes the
// playing file and finds the letterbox/pillarbox bars from luma; the offsets are
// injected into the DV RPU by CBitstreamConverter (no kernel params).
enum DvDetectState
{
  DV_DETECT_FAILED = 0,
  DV_DETECT_SKIP_NON16X9 = 1,
  DV_DETECT_SKIP_IMAX = 2,
  DV_DETECT_SKIPPED = 3,
  DV_DETECT_OK = 4,
  DV_DETECT_RUNNING = 5,
  DV_DETECT_INACTIVE = 6
};
void aml_dv_detect_set_file(const std::string& path);
void aml_dv_detect_active_area_start();
void aml_dv_detect_active_area_stop();
// Geometric letterbox for hard-cropped (non-16:9 coded) content: derive the
// display-scaling bars from the coded resolution and publish them as the
// active-area offsets (no pixel scan). Applied in Source/Auto (not Zero).
void aml_dv_set_geometric_active_area(int codedW, int codedH);
int aml_dv_detect_active_area_state();
// Returns true when detection has completed (state OK); fills the offsets either way.
bool aml_dv_detect_active_area_get(uint16_t& top, uint16_t& bottom, uint16_t& left,
                                   uint16_t& right);
// DV L5 "osdst": GUI thread reports OSD/subtitle visibility; the codec reads the
// combined state and pushes it to the bitstream so L5 masking can be lifted while
// an overlay is on screen (keeps OSD/subs in the letterbox bars visible).
void aml_dv_set_osd_visible(bool visible);
void aml_dv_set_subtitles_visible(bool visible);
bool aml_dv_l5_overlay_visible();
bool aml_video_started();
//! the sink's own TMDS lock (SCDC, hdmitx sink_lock): 1 locked, 0 not, -1 unknown
int aml_hdmi_sink_locked();
//! real_player 4a (design 4.3): keep the last picture across a decoder reinit
//! at a disc join. The player arms the next close; the kernel keeps the frame
//! (blackout_policy=0) until the next decoder's first frame replaces it.
void aml_keep_frame_arm(bool arm);
bool aml_keep_frame_take_arm();
void aml_set_frame_kept(bool kept);
bool aml_frame_kept();
//! blank a kept frame now (stop, no video in the next segment, open failure)
void aml_drop_kept_frame(const char* why);
//! bumped whenever disable_video is written outside CAMLCodec::ShowMainVideo
unsigned int aml_disable_video_generation();
// real_player Phase 0 measurement: the vsync-wait return time (steady_clock us)
// and whether the last present swapped a GUI frame.
void aml_note_poll_return();
int64_t aml_poll_return_us();
void aml_note_present(bool rendered);
bool aml_last_present_rendered();
// real_player Phase 2: the presentation coordinator's display fence. The display
// executor holds it across a display transaction (a mode set, the DV engage);
// the coordinator parks - no frame selection, no QBUF - until it is released.
// Acquire returns once the coordinator has acknowledged (bounded); every
// acquire and release starts a new display epoch.
//! site names the hold in the coordinator's TIMING lines
//! true once the coordinator acknowledged (or is not running); false when the 2 s bound passed
//! and the hold is held unacknowledged. epoch: the display epoch this acquire started.
bool aml_presenter_hold_acquire(const char* site, unsigned int* epoch = nullptr);
void aml_presenter_hold_release();
//! 5.B2-5b (Q5): waits up to ms more for the ack of a hold still held (the same epoch: no
//! release, no new acquire); true once acknowledged
bool aml_presenter_hold_rewait(unsigned int epoch, int ms);
struct CAmlPresenterHold
{
  explicit CAmlPresenterHold(const char* site) : m_parked(aml_presenter_hold_acquire(site, &m_epoch))
  {
  }
  ~CAmlPresenterHold() { aml_presenter_hold_release(); }
  CAmlPresenterHold(const CAmlPresenterHold&) = delete;
  CAmlPresenterHold& operator=(const CAmlPresenterHold&) = delete;
  bool Parked() const { return m_parked; }
  bool Rewait(int ms) { return m_parked = m_parked || aml_presenter_hold_rewait(m_epoch, ms); }

private:
  unsigned int m_epoch = 0;
  bool m_parked;
};
//! 5.B2-5b (S1', R4-M1): a hold is held and the coordinator's ack is for an epoch at or after
//! the outer mode set hold's acquire (a nested hold's release leaves C held on an older ack);
//! true too when the coordinator is not running
bool aml_presenter_parked();
//! the hold whose release started a display epoch; site null = not started by a hold release
struct AmlEpochHold
{
  const char* site = nullptr;
  int tid = 0;
  int64_t releaseNs = 0; //!< steady_clock
};
// coordinator side: true while held; acknowledges the hold when park is set
// (nothing of its own in flight); epoch = the current display epoch, hold = its hold
bool aml_presenter_check_hold(unsigned int& epoch, bool park = true, AmlEpochHold* hold = nullptr);
unsigned int aml_presenter_epoch();
// 5.V1-4 (MC-23): writers of the render manager's display timing inputs bump this after the
// write; the coordinator skips the wait for main after a hold only while it is unchanged since
// main's last publish. displaystate_off (or outputworker_off): no bump, today's wait.
void aml_displaystate_init(); // at W's start
bool aml_displaystate_on();
void aml_timing_inputs_bump();
uint64_t aml_timing_inputs_gen();

// A frame queued to amvideo after vblank S is on VD1 from S+2: the vsync ISR
// takes it at S+1 and applies it through vsync RDMA at the next vsync (traced
// on S6: v4l2_qbuf, amvideo_vf_peek, drm_vblank_event, 718 of 718 frames).
constexpr int AML_VD1_PRESENT_VBLANKS = 2;
void aml_presenter_set_running(bool running);
//! 5.B2-1 test (outputdrive_cstall): withhold the ack of a mode set's park for ms
void aml_presenter_set_test_cstall(int ms);
//! an eventfd the coordinator polls, woken by every hold acquire; -1 = none
void aml_presenter_set_wake_fd(int fd);
// true while a coordinator releases the video plane, so the GUI loop paces
// itself instead of relying on the video release's vsync wait
void aml_set_video_presenter_active(bool active);
bool aml_video_presenter_active();
int aml_amdv_wait(StreamHdrType hdrType);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);
