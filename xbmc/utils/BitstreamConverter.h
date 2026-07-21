/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/FFmpeg.h"

#include <stdint.h>

extern "C"
{
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavcodec/avcodec.h>

#ifdef HAVE_LIBDOVI
#include <libdovi/rpu_parser.h>
#endif
}

// HDR10+ -> Dolby Vision profile 8.1 conversion: SEI extractors + RPU synthesis.
#include "HevcSei.h"
#include "HDR10PlusConvert.h"

typedef struct
{
  const uint8_t* data;
  const uint8_t* end;
  int head;
  uint64_t cache;
} nal_bitstream;

typedef struct mpeg2_sequence
{
  uint32_t width;
  uint32_t height;
  uint32_t fps_rate;
  uint32_t fps_scale;
  float ratio;
  uint32_t ratio_info;
} mpeg2_sequence;

typedef struct h264_sequence
{
  uint32_t  width;
  uint32_t  height;
  float     ratio;
  uint32_t  ratio_info;
} h264_sequence;

typedef struct
{
  int profile_idc;
  int level_idc;
  int sps_id;

  int chroma_format_idc;
  int separate_colour_plane_flag;
  int bit_depth_luma_minus8;
  int bit_depth_chroma_minus8;
  int qpprime_y_zero_transform_bypass_flag;
  int seq_scaling_matrix_present_flag;

  int log2_max_frame_num_minus4;
  int pic_order_cnt_type;
  int log2_max_pic_order_cnt_lsb_minus4;

  int max_num_ref_frames;
  int gaps_in_frame_num_value_allowed_flag;
  int pic_width_in_mbs_minus1;
  int pic_height_in_map_units_minus1;

  int frame_mbs_only_flag;
  int mb_adaptive_frame_field_flag;

  int direct_8x8_inference_flag;

  int frame_cropping_flag;
  int frame_crop_left_offset;
  int frame_crop_right_offset;
  int frame_crop_top_offset;
  int frame_crop_bottom_offset;
} sps_info_struct;

class CBitstreamParser
{
public:
  CBitstreamParser();
  ~CBitstreamParser() = default;

  static bool Open() { return true; }
  static void Close();
  static bool CanStartDecode(const uint8_t* buf, int buf_size);
};

// Dolby Vision CMv4.0 append mode (coreelec.amlogic.dolbyvision.cmv40.append).
// Appends CMv4.0 metadata to CMv2.9 titles. SMART decides per-frame whether to
// append (see CBitstreamConverter::processDoviRpu).
enum DOVICMv40Mode : int
{
  CMV40_NONE = 0,   // Off - never append
  CMV40_NO_L2,      // append only when the stream lacks L2 trims
  CMV40_ALWAYS,     // always append
  CMV40_SMART,      // per-frame: append unless content peak > display*(1+pct)
};

// DV Level 5 (active-area / letterbox) handling for the RPU. See
// CBitstreamConverter::processDoviRpu.
enum DOVIL5Mode : int
{
  DOVI_L5_SOURCE = 0, // leave the RPU's L5 offsets as authored
  DOVI_L5_ZERO,       // force 0,0,0,0 (full frame active - old dovizerolevel5=on)
  DOVI_L5_DETECT,     // inject detected active-area offsets when source L5 is empty
};

class CBitstreamConverter
{
public:
  CBitstreamConverter();
  ~CBitstreamConverter();

  bool Open(enum AVCodecID codec, uint8_t* in_extradata, int in_extrasize, bool to_annexb);
  void Close();
  bool NeedConvert() const { return m_convert_bitstream; }
  bool Convert(uint8_t* pData, int iSize);
  bool Convert(uint8_t *pData_bl, int iSize_bl, uint8_t *pData_el, int iSize_el);
  uint8_t* GetConvertBuffer() const;
  int GetConvertSize() const;
  uint8_t* GetExtraData();
  const uint8_t* GetExtraData() const;
  int GetExtraSize() const;
  void ResetStartDecode();
  bool CanStartDecode() const;
  void SetConvertDovi(bool value) { m_convert_dovi = value; }
  void SetRemoveDovi(bool value) { m_removeDovi = value; }
  void SetRemoveHdr10Plus(bool value) { m_removeHdr10Plus = value; }
  // HDR10+ -> Dolby Vision profile 8.1: synthesize a DV RPU from HDR10+ dynamic
  // metadata and inject it into the stream (see ProcessSeiPrefix / the h265 loop).
  void SetConvertHdr10Plus(bool value) { m_convert_Hdr10Plus = value; }
  void SetConvertHdr10PlusPeakBrightnessSource(enum PeakBrightnessSource value)
  {
    m_convert_Hdr10Plus_peak_brightness_source = value;
  }
  // Compat shim for the other platforms (WebOS/Android) that still drive the
  // stock boolean: on -> Zero, off -> Source.
  void SetDoviZeroLevel5(bool value) { m_doviL5Mode = value ? DOVI_L5_ZERO : DOVI_L5_SOURCE; }
  void SetDoviL5Mode(int mode) { m_doviL5Mode = mode; }
  // Detected active-area offsets (valid only when the detector has finished);
  // used in DOVI_L5_DETECT mode when the source RPU carries no L5.
  void SetDoviL5DetectedOffsets(bool valid, uint16_t top, uint16_t bottom, uint16_t left,
                                uint16_t right)
  {
    m_doviL5DetectedValid = valid;
    m_doviL5DetTop = top;
    m_doviL5DetBottom = bottom;
    m_doviL5DetLeft = left;
    m_doviL5DetRight = right;
  }
  // L5 "osdst": when the osd-unmask setting is on AND an overlay (OSD/subtitle) is
  // on screen, force L5 to zero so the letterbox bars are not masked over the
  // overlay. m_doviL5OverlayVisible is refreshed per-packet from the GUI state.
  void SetDoviL5OsdUnmask(bool value) { m_doviL5OsdUnmask = value; }
  void SetDoviL5OverlayVisible(bool value) { m_doviL5OverlayVisible = value; }
  // Geometric letterbox for hard-cropped content: the detected offsets describe
  // display-scaling bars (not baked-in bars), so inject them even in SOURCE mode
  // (still never overriding a real source L5).
  void SetDoviL5Geometric(bool value) { m_doviL5Geometric = value; }
  // CMv4.0 append: mode + smart-bypass inputs. Set the two bypass inputs
  // BEFORE SetAppendCMv40 (it resets the per-decision logging sentinel). The
  // bypass inputs are only consulted when the mode is CMV40_SMART.
  void SetAppendCMv40(enum DOVICMv40Mode value) { m_append_cmv40 = value; m_smart_last_effective = CMV40_SMART; m_cmv40_native_logged = false; }
  void SetSmartBypassDisplayNits(int nits) { m_smart_display_nits = nits; }
  void SetSmartBypassThresholdPct(int pct) { m_smart_threshold_pct = pct; }
  bool GetDoviIsFEL() const { return m_doviIsFEL; }
  bool GetIsHdrPlus() const { return m_IsHdr10Plus; }

  static bool mpeg2_sequence_header(const uint8_t* data,
                                    const uint32_t size,
                                    mpeg2_sequence* sequence);
  static bool h264_sequence_header(const uint8_t *data,
                                   const uint32_t size,
                                   h264_sequence *sequence);

protected:
  static int avc_parse_nal_units(AVIOContext* pb, const uint8_t* buf_in, int size);
  static int avc_parse_nal_units_buf(const uint8_t* buf_in, uint8_t** buf, int* size);
  int isom_write_avcc(AVIOContext* pb, const uint8_t* data, int len);
  // bitstream to bytestream (Annex B) conversion support.
  bool IsIDR(uint8_t unit_type);
  bool IsSlice(uint8_t unit_type);
  bool BitstreamConvertInitAVC(void* in_extradata, int in_extrasize);
  bool BitstreamConvertInitHEVC(void* in_extradata, int in_extrasize);
  bool BitstreamConvertInitVVC(void* in_extradata, int in_extrasize);
  bool BitstreamConvert(uint8_t* pData, int iSize, uint8_t** poutbuf, int* poutbuf_size);
  static void BitstreamAllocAndCopy(uint8_t** poutbuf,
                                    int* poutbuf_size,
                                    const uint8_t* sps_pps,
                                    uint32_t sps_pps_size,
                                    const uint8_t* in,
                                    uint32_t in_size,
                                    uint8_t nal_type);
  static void BitstreamAllocAndCopy(uint8_t** poutbuf,
                                    uint32_t* poutbuf_size,
                                    const uint8_t* in,
                                    uint32_t in_size,
                                    uint8_t nal_type);

#ifdef HAVE_LIBDOVI
  const DoviData* processDoviRpu(uint8_t* buf, uint32_t nalSize);
#endif

  // HDR10+ -> DV 8.1: accumulate the source's static HDR metadata (mastering
  // display + content light level) from SEI, used to build the synthesized RPU.
  void ApplyMasteringDisplayColourVolume(const MasteringDisplayColourVolume& metadata);
  void ApplyContentLightLevel(const ContentLightLevel& metadata);

  typedef struct omx_bitstream_ctx {
      uint8_t  length_size;
      uint8_t  first_idr;
      uint8_t  idr_sps_pps_seen;
      uint8_t *sps_pps_data;
      uint32_t size;
  } omx_bitstream_ctx;

  uint8_t* m_convertBuffer;
  int m_convertSize;
  uint8_t* m_inputBuffer;
  int m_inputSize;

  uint32_t m_sps_pps_size;
  omx_bitstream_ctx m_sps_pps_context;
  bool m_convert_bitstream;
  bool m_to_annexb;
  bool m_combine;

  FFmpegExtraData m_extraData;
  bool m_convert_3byteTo4byteNALSize;
  bool m_convert_bytestream;
  AVCodecID m_codec;
  bool m_start_decode;
  bool m_convert_dovi;
  bool m_removeDovi;
  bool m_removeHdr10Plus;
  int m_doviL5Mode{DOVI_L5_SOURCE};
  bool m_doviL5DetectedValid{false};
  uint16_t m_doviL5DetTop{0};
  uint16_t m_doviL5DetBottom{0};
  uint16_t m_doviL5DetLeft{0};
  uint16_t m_doviL5DetRight{0};
  bool m_doviL5OsdUnmask{false};
  bool m_doviL5OverlayVisible{false};
  bool m_doviL5Geometric{false};
  enum DOVICMv40Mode m_append_cmv40{CMV40_NONE};
  int m_smart_display_nits{0};
  int m_smart_threshold_pct{20};
  DOVICMv40Mode m_smart_last_effective{CMV40_SMART};
  bool m_cmv40_native_logged{false};
  bool m_doviIsFEL{false};
  bool m_doviELTested{false};
  bool m_IsHdr10Plus{false};
  bool m_Hdr10PlusTested{false};
  bool m_convert_Hdr10Plus{false};
  enum PeakBrightnessSource m_convert_Hdr10Plus_peak_brightness_source{PeakBrightnessSource::MaxScl};
  HDRStaticMetadataInfo m_hdrStaticMetadataInfo;
};
