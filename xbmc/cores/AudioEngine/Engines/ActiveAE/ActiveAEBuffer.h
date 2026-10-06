/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include <cmath>
#include <deque>
#include <memory>

extern "C" {
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

namespace ActiveAE
{

/**
 * the variables here follow ffmpeg naming
 */
class CSoundPacket
{
public:
  CSoundPacket(const SampleConfig& conf, int samples);
  ~CSoundPacket();
  uint8_t **data;                        // array with pointers to planes of data
  SampleConfig config;
  int bytes_per_sample;                  // bytes per sample and per channel
  int linesize;                          // see ffmpeg, required for planar formats
  int planes;                            // 1 for non planar formats, #channels for planar
  int nb_samples;                        // number of frames used
  int max_nb_samples;                    // max number of frames this packet can hold
  int pause_burst_ms;
};

class CActiveAEBufferPool;

class CSampleBuffer
{
public:
  CSampleBuffer() = default;
  ~CSampleBuffer() = default;
  CSampleBuffer *Acquire();
  void Return();
  std::unique_ptr<CSoundPacket> pkt;
  CActiveAEBufferPool *pool = nullptr;
  int64_t timestamp = 0;
  // the pts of the sample at pkt_start_offset in µs (timestamp keeps the ms value
  // the sync measurement uses); 0 = none. Set by the stream, carried through the
  // resample and atempo pools.
  int64_t ptsUs = 0;
  // a scheduled start: CLOCK_MONOTONIC at which this buffer's first sample must
  // leave the output, and the start it belongs to; 0 = play when it comes
  int64_t landNs = 0;
  unsigned int landEpoch = 0;
  // silence (a pause, for a bitstream) the output writes before this buffer: a
  // step in the labels realised at the output
  double padMs = 0;
  // pts less the label of the content (DVDAudioFrame::labelLag)
  double labelLagMs = 0;
  int pkt_start_offset = 0;
  // the engine holds a reference for the output, which keeps the buffer until
  // it has played (a pause takes back what has not)
  bool sinkHold = false;
  int refCount = 0;
  double centerMixLevel;
};

class CActiveAEBufferPool
{
public:
  explicit CActiveAEBufferPool(const AEAudioFormat& format);
  virtual ~CActiveAEBufferPool();
  virtual bool Create(unsigned int totaltime);
  CSampleBuffer *GetFreeBuffer();
  void ReturnBuffer(CSampleBuffer *buffer);
  AEAudioFormat m_format;
  std::deque<CSampleBuffer*> m_allSamples;
  std::deque<CSampleBuffer*> m_freeSamples;
};

class IAEResample;

class CActiveAEBufferPoolResample : public CActiveAEBufferPool
{
public:
  CActiveAEBufferPoolResample(const AEAudioFormat& inputFormat, const AEAudioFormat& outputFormat, AEQuality quality);
  ~CActiveAEBufferPoolResample() override;
  using CActiveAEBufferPool::Create;
  bool Create(
      unsigned int totaltime, bool remap, bool upmix, bool normalize = true, float sublevel = 0.0);
  bool ResampleBuffers(int64_t timestamp = 0);
  void ConfigureResampler(bool normalizelevels,
                          bool stereoupmix,
                          AEQuality quality,
                          float sublevel);
  float GetDelay();
  void Flush();
  void SetDrain(bool drain);
  void SetRR(double rr);
  double GetRR() const;
  void FillBuffer();
  bool DoesNormalize() const;
  void ForceResampler(bool force);
  AEAudioFormat m_inputFormat;
  std::deque<CSampleBuffer*> m_inputSamples;
  std::deque<CSampleBuffer*> m_outputSamples;

protected:
  void ChangeResampler();

  uint8_t *m_planes[16];
  bool m_empty = true;
  bool m_drain = false;
  int64_t m_lastSamplePts = 0;
  double m_lastSamplePtsUs = 0.0; // the same in µs, unrounded; 0 = none
  bool m_remap = false;
  CSampleBuffer *m_procSample = nullptr;
  std::unique_ptr<IAEResample> m_resampler;
  double m_resampleRatio = 1.0;
  double m_centerMixLevel = M_SQRT1_2;
  bool m_fillPackets = false;
  bool m_normalize = true;
  float m_mixSubLevel = 0.0f;
  bool m_changeResampler = false;
  bool m_forceResampler = false;
  AEQuality m_resampleQuality;
  bool m_stereoUpmix = false;
};

class CActiveAEFilter;

class CActiveAEBufferPoolAtempo : public CActiveAEBufferPool
{
public:
  explicit CActiveAEBufferPoolAtempo(const AEAudioFormat& format);
  ~CActiveAEBufferPoolAtempo() override;
  bool Create(unsigned int totaltime) override;
  bool ProcessBuffers();
  float GetDelay();
  void Flush();
  void SetTempo(float tempo);
  float GetTempo() const;
  void FillBuffer();
  void SetDrain(bool drain);
  std::deque<CSampleBuffer*> m_inputSamples;
  std::deque<CSampleBuffer*> m_outputSamples;

protected:
  void ChangeFilter();
  std::unique_ptr<CActiveAEFilter> m_pTempoFilter;
  uint8_t *m_planes[16];
  CSampleBuffer *m_procSample;
  bool m_empty;
  bool m_drain;
  bool m_changeFilter;
  float m_tempo;
  int64_t m_lastSamplePts = 0;
  double m_lastSamplePtsUs = 0.0; // the same in µs, unrounded; 0 = none
  bool m_fillPackets;
};

}
