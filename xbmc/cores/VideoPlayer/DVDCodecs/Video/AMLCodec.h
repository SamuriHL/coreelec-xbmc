/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDVideoCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/IPlayer.h"
#include "windowing/Resolution.h"
#include "rendering/RenderSystem.h"
#include "utils/BitstreamConverter.h"
#include "utils/Geometry.h"

#include <deque>
#include <atomic>
#include <mutex>

typedef struct am_private_t am_private_t;

class DllLibAmCodec;

class PosixFile;
typedef std::shared_ptr<PosixFile> PosixFilePtr;

class CProcessInfo;

struct vpp_pq_ctrl_s {
	unsigned int length;
	union {
		void *ptr;/*point to pq_ctrl_s*/
		long long ptr_length;
	};
};

struct pq_ctrl_s {
	unsigned char sharpness0_en;
	unsigned char sharpness1_en;
	unsigned char dnlp_en;
	unsigned char cm_en;
	unsigned char vadj1_en;
	unsigned char vd1_ctrst_en;
	unsigned char vadj2_en;
	unsigned char post_ctrst_en;
	unsigned char wb_en;
	unsigned char gamma_en;
	unsigned char lc_en;
	unsigned char black_ext_en;
	unsigned char chroma_cor_en;
	unsigned char reserved;
};

#define _VE_CM  'C'
#define AMVECM_IOC_S_PQ_CTRL  _IOW(_VE_CM, 0x69, struct vpp_pq_ctrl_s)
#define AMVECM_IOC_G_PQ_CTRL  _IOR(_VE_CM, 0x6a, struct vpp_pq_ctrl_s)

class CAMLCodec
{
public:
  CAMLCodec(CProcessInfo &processInfo);
  virtual ~CAMLCodec();

  bool          OpenDecoder(CDVDStreamInfo &hints, bool doviIsFEL, bool isDualStream = false);
  bool          Enable_vadj1();
  void          CloseDecoder();
  void          Reset();

  bool          AddData(uint8_t *pData, size_t size, double dts, double pts);
  int           AddHDR10PData(uint8_t *pData, size_t iSize);
  CDVDVideoCodec::VCReturn GetPicture(VideoPicture* pVideoPicture);

  void          SetSpeed(int speed);
  void          SetDrain(bool drain){m_drain = drain;};
  void          SetVideoRect(const CRect &SrcRect, const CRect &DestRect);
  void          SetVideoRate(int videoRate);
  uint64_t      GetOMXPts() const { return m_cur_pts; }
  uint32_t      GetBufferIndex() const { return m_bufferIndex; };
  float         GetBufferLevel(int new_chunk, int &data_len, int &free_len, int &size);
  static float  OMXPtsToSeconds(int omxpts);
  static int    OMXDurationToNs(int duration);
  int           GetAmlDuration() const;
  int           ReleaseFrame(const uint32_t index, bool bDrop = false,
                             uint32_t sessionGen = UINT32_MAX);
  // decode-session generation, bumped at every OpenDecoder: buffers carry
  // the generation they were decoded in, and ReleaseFrame refuses to QBUF a
  // previous session's index into the new v4l session (VC_REOPEN made
  // mid-stream close/open a recurring runtime path - review finding F7)
  uint32_t      GetSessionGeneration() const { return m_sessionGen.load(); }

  static int    PollFrame();
  static void   SetPollDevice(int device);

private:
  void          ShowMainVideo(const bool show);
  void          SetVideoZoom(const float zoom);
  void          SetVideoContrast(const int contrast);
  void          SetVideoBrightness(const int brightness);
  void          SetVideoSaturation(const int saturation);
  bool          OpenAmlVideo(const CDVDStreamInfo &hints);
  void          CloseAmlVideo();
  std::string   GetVfmMap(const std::string &name);
  void          SetVfmMap(const std::string &name, const std::string &map);
  int           DequeueBuffer();
  unsigned int  GetDecoderVideoRate();
  std::string   GetHDRStaticMetadata(bool dv_enable);

  DllLibAmCodec   *m_dll;
  bool             m_opened;
  bool             m_drain = false;
  am_private_t    *am_private;
  CDVDStreamInfo   m_hints;
  int              m_speed;
  uint64_t         m_cur_pts;
  uint64_t         m_last_pts;
  uint32_t         m_bufferIndex;

  CRect            m_dst_rect;
  CRect            m_display_rect;

  int              m_view_mode = -1;
  RenderStereoMode m_guiStereoMode = RenderStereoMode::OFF;
  RenderStereoView m_guiStereoView = RenderStereoView::OFF;
  float            m_zoom = -1.0f;
  int              m_contrast = -1;
  int              m_brightness = -1;
  bool             m_vadj1_enabled = false;
  RESOLUTION       m_video_res = RES_INVALID;

  static const unsigned int STATE_PREFILLED  = 1;
  static const unsigned int STATE_HASPTS     = 2;

  unsigned int m_state;

  PosixFilePtr     m_amlVideoFile;
  // serializes render-thread ReleaseFrame against video-thread
  // CloseAmlVideo's reset of m_amlVideoFile (shared_ptr read/reset race)
  std::mutex       m_videoFileMutex;
  std::atomic<uint32_t> m_sessionGen{0};
  std::string      m_defaultVfmMap;

  static std::atomic_flag  m_pollSync;
  static int m_pollDevice;
  static double m_ttd;
  CProcessInfo &m_processInfo;
  int m_decoder_timeout;
  std::chrono::time_point<std::chrono::system_clock> m_tp_last_frame;

  bool            m_buffer_level_ready;
  float           m_minimum_buffer_level;
  // stream-buffer data_len at the previous GetPicture call: the parked
  // stall clock only engages while this is UNCHANGED (idle input); a
  // changing value with no frames out is a consume-without-output wedge
  int             m_park_last_data_len{-1};
  // Dual-stream (BL+EL) DV opened in stream dec_mode: skip the stream-buffer
  // fill gate in AddData. Covers disc playitems AND file-played BL+EL m2ts
  // rips - both are fed in short segments that can EOS below any fill
  // threshold, wedging the gate in a starve/reopen loop.
  bool            m_skipBufferFillGate = false;
};
