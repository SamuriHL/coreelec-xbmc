/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

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
bool aml_convert_to_dv_by_vs_engine(StreamHdrType hdrType);
bool aml_display_support_hdr_pq();
bool aml_display_support_hdr_hlg();
bool aml_display_support_hdr10plus();
// Dolby Vision VS10 engine.
unsigned int aml_vs10_by_setting(const std::string& setting);
unsigned int aml_vs10_by_hdrtype(StreamHdrType hdrType, unsigned int bitDepth);
// Carries the VS10 output mode resolved at stream-open (from the real source
// hdrType, before VideoPlayer fakes it to DOLBYVISION) down to CAMLCodec, which
// applies it. BYPASS means "no VS10 conversion for this stream".
void aml_dv_set_vs10_pending(unsigned int mode);
unsigned int aml_dv_get_vs10_pending();
unsigned int aml_dv_dolby_vision_mode();
void aml_dv_set_vs10_mode(unsigned int mode);
// Resolve the stored "Dolby Vision" VS10 option (IPT) to the tunnel form the
// current TV-led / player-led output setting actually needs.
unsigned int aml_dv_resolve_tunnel_mode(unsigned int mode);
// DM target min/max overrides for VS10-converted output (needs the samurihl
// common_drivers kernel patch; silently no-ops on a stock kernel).
void aml_dv_apply_target_overrides(unsigned int mode);
void aml_dv_set_hdr10_osd_brightness(int nits);
// Dolby Vision VSVDB max-luminance override: patch the display's advertised v2
// VSVDB block and inject it via aml_media force_vsvdb/vsvdb_data. apply() reads
// the settings and enables/disables accordingly; clear() disables injection.
void aml_dv_apply_vsvdb();
void aml_dv_clear_vsvdb();
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
int aml_amdv_wait(StreamHdrType hdrType);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);
