/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AMLFrameMetadata.h"
#include "DVDVideoCodec.h"
#include "DVDStreamInfo.h"
#include "threads/CriticalSection.h"
#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "utils/BitstreamConverter.h"

#include <set>
#include <atomic>
#include <chrono>

class CAMLCodec;
struct mpeg2_sequence;
struct h264_sequence;
class CBitstreamParser;
class CBitstreamConverter;

class CDVDVideoCodecAmlogic;

// data, size, isELPackage, dts
typedef std::tuple<uint8_t*, uint32_t, bool, double> DLDemuxPacket;

class CAMLVideoBuffer : public CVideoBuffer
{
public:
  CAMLVideoBuffer(int id) : CVideoBuffer(id) {};
  void Set(CDVDVideoCodecAmlogic* codec,
           std::shared_ptr<CAMLCodec> amlcodec,
           uint64_t omxPts,
           int amlDuration,
           uint32_t bufferIndex,
           uint32_t sessionGen)
  {
    m_codec = codec;
    m_amlCodec = amlcodec;
    m_omxPts = omxPts;
    m_amlDuration = amlDuration;
    m_bufferIndex = bufferIndex;
    m_sessionGen = sessionGen;
  }

  CDVDVideoCodecAmlogic* m_codec;
  std::shared_ptr<CAMLCodec> m_amlCodec;
  uint64_t m_omxPts;
  int m_amlDuration;
  uint32_t m_bufferIndex;
  // decode session the buffer belongs to (CAMLCodec::GetSessionGeneration);
  // ReleaseFrame drops indices from closed sessions
  uint32_t m_sessionGen{UINT32_MAX};
};

class CAMLVideoBufferPool : public IVideoBufferPool
{
public:
  virtual ~CAMLVideoBufferPool();

  virtual CVideoBuffer* Get() override;
  virtual void Return(int id) override;

private:
  CCriticalSection m_criticalSection;;
  std::vector<CAMLVideoBuffer*> m_videoBuffers;
  std::vector<int> m_freeBuffers;
};

class CDVDVideoCodecAmlogic : public CDVDVideoCodec
{
public:
  CDVDVideoCodecAmlogic(CProcessInfo &processInfo);
  virtual ~CDVDVideoCodecAmlogic();

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static bool Register();

  // Required overrides
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  virtual bool AddData(const DemuxPacket &packet) override;
  virtual void Reset() override;
  virtual void Reopen() override;
  virtual VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  virtual void SetSpeed(int iSpeed) override;
  virtual void SetCodecControl(int flags) override;
  virtual const char* GetName(void) override { return (const char*)m_pFormatName; }
  virtual bool SupportsExtention() { return true; }
  virtual int GetDataLevel() const override;
  // Non-zero so VideoPlayerVideo buffers recent packets and REPLAYS them
  // after VC_FLUSHED/VC_REOPEN. Without this (base returns 0) a reopened
  // decoder stayed closed until the NEXT demuxer packet - on a still or a
  // drain there is none, so recovery never happened (review finding A5).
  // ~one GOP of packets; the player also time-caps the buffer at 10s.
  unsigned GetConvergeCount() override { return m_opened ? 30 : 0; }

protected:
  void            Close(void);
  void            DrainMetadataToClock();
  double          RenderDisplayLatency();
  void            FrameRateTracking(uint8_t *pData, int iSize, double dts, double pts);
  // Read the CMv4.0 append settings (mode / Smart threshold / display peak) and
  // push them to m_bitstream. Called at stream open and again from AddData when
  // the settings generation moves, so changing them mid-playback takes effect.
  void            ApplyCmv40Settings();
  //void            RemoveInfo(CDVDAmlogicInfo* info);

  std::shared_ptr<CAMLCodec> m_Codec;

  const char     *m_pFormatName;
  VideoPicture m_videobuffer;
  bool            m_opened;
  int             m_codecControlFlags;
  int             m_timeoutFlushCount{0};
  // reopen-escalation bound + staleness decay (review findings A6/F10)
  int             m_reopenCount{0};
  std::chrono::steady_clock::time_point m_lastTimeoutFlush{};
  CDVDStreamInfo  m_hints;
  double          m_framerate;
  int             m_video_rate;
  float           m_aspect_ratio;
  mpeg2_sequence *m_mpeg2_sequence;
  double          m_mpeg2_sequence_pts;
  h264_sequence  *m_h264_sequence;
  double          m_h264_sequence_pts;
  bool            m_has_keyframe;
  // HDR10+ -> DV 8.1 convert armed at Open; the DV engage decision is deferred to
  // AddData once the bitstream confirms HDR10+ (covers file sources too).
  bool            m_hdr10plusToDvCandidate = false;
  // CMv4.0 append live-apply: false until the stream configures CMv4.0 at all
  // (non-DV / DV-disabled streams never re-push), plus the settings generation
  // this codec last consumed.
  bool            m_cmv40Configured{false};
  unsigned int    m_cmv40SettingsGen{0};
  bool            m_cmv40SmartPinnedLogged{false};

  CBitstreamParser *m_bitparser;
  CBitstreamConverter *m_bitstream;
private:
  std::shared_ptr<CAMLVideoBufferPool> m_videoBufferPool;
  static std::atomic<bool> m_InstanceGuard;

  // BL/EL packets awaiting their partner. Bounded by m_packagesBytes: if
  // pairing never converges this list would otherwise grow until the process
  // is OOM-killed.
  std::list<DLDemuxPacket> m_packages;

  uint32_t m_metadataToken{0};
  bool m_metaLeadLogged{false};
  bool m_stripHdr10Plus{false};
  bool m_dualLayer{false};
  int m_nalLengthSize{0};
  double m_lastCommitPts{0.0};
  AMLFrameMetadata m_streamMeta;
  AMLFrameMetadata m_pendingMeta;
  AMLFrameMetadata m_lastMeta;
  CAMLFrameMetadataSequencer m_metadataSequencer;
  size_t m_packagesBytes = 0;
  bool m_packagesOverflowLogged = false;
  // Free the head of m_packages, keeping m_packagesBytes in step.
  void PopPackageFront();
};
