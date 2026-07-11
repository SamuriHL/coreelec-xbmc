/*
 *  Copyright (C) 2024 Team Kodi
 *  Copyright (C) 2010-2021 Hendrik Leppkes
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  The TrueHD seamless-branch handling (output-timing discontinuity detection
 *  and padding carry-forward) is derived from the TrueHD MAT packer in LAV
 *  Filters by Hendrik Leppkes (Nevcairiel), decoder/LAVAudio/BitstreamMAT.cpp.
 */

#pragma once

#include <deque>
#include <stdint.h>
#include <vector>

enum class Type
{
  PADDING,
  DATA,
};

class CPackerMAT
{
public:
  CPackerMAT();
  ~CPackerMAT() = default;

  bool PackTrueHD(const uint8_t* data, int size);
  std::vector<uint8_t> GetOutputFrame();

  // Samples offset carried by the last GetOutputFrame() MAT frame, for TrueHD
  // sub-MAT-frame drift compensation by the caller.
  int GetSamplesOffset() const { return m_lastOutputSamplesOffset; }

  // True when the last GetOutputFrame() MAT frame spanned a detected stream
  // discontinuity (seamless branch point).
  bool HadDiscontinuity() const { return m_lastOutputHadDiscontinuity; }

  // Fully reset packer state (used by the caller on codec reset/seek).
  void Reset();

private:
  struct MATState
  {
    bool init; // differentiates the first header

    // audio_sampling_frequency:
    //  0 -> 48 kHz
    //  1 -> 96 kHz
    //  2 -> 192 kHz
    //  8 -> 44.1 kHz
    //  9 -> 88.2 kHz
    // 10 -> 176.4 kHz
    int ratebits;

    // output timing parsed from the TrueHD major sync restart header (when
    // present) or inferred by advancing a counter. Used to detect a seamless
    // branch point: the value jumps when the stream branches.
    uint16_t outputTiming;
    bool outputTimingValid;

    // input timing of previous audio unit used to calculate padding bytes
    uint16_t prevFrametime;
    bool prevFrametimeValid;

    uint32_t matFramesize; // size in bytes of current MAT frame
    uint32_t prevMatFramesize; // size in bytes of previous MAT frame

    uint32_t padding; // padding bytes pending to write

    // frame-time to output-time offset, used to size the padding carry-forward
    // on the next branch point.
    int32_t nOutputTimeOffset;

    // MAT-frame sample accounting used to derive the samples offset.
    uint32_t samples; // number of samples accumulated in current MAT frame
    int32_t numberOfSamplesOffset; // offset vs samples in a standard MAT frame (40 * 24)
  };

  void WriteHeader();
  void WritePadding();
  void AppendData(const uint8_t* data, int size, Type type);
  uint32_t GetCount() const { return m_bufferCount; }
  int FillDataBuffer(const uint8_t* data, int size, Type type);
  void FlushPacket();

  MATState m_state{};

  uint32_t m_bufferCount{0};
  std::vector<uint8_t> m_buffer;
  std::deque<std::vector<uint8_t>> m_outputQueue;

  // Per-MAT-frame samples offset / discontinuity flag, queued as frames are
  // produced and surfaced via GetSamplesOffset() / HadDiscontinuity().
  int m_lastOutputSamplesOffset{0};
  bool m_lastOutputHadDiscontinuity{false};
  std::deque<int> m_offsetQueue;
  std::deque<bool> m_discontinuityQueue;
  bool m_pendingDiscontinuity{false};
};
