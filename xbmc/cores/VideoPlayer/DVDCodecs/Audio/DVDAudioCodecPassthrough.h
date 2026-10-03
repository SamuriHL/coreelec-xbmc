/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  The "LAV Audio" passthrough A/V sync is derived from LAV Filters by Hendrik
 *  Leppkes (Nevcairiel): https://github.com/Nevcairiel/LAVFilters
 *  It is always on for normal playback; only realtime/PVR streams are excluded.
 */

#pragma once

#include "DVDAudioCodec.h"
#include "DVDStreamInfo.h"
#include "FloatingAverage.h"
#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Utils/AEBitstreamPacker.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "settings/lib/ISettingCallback.h"

#include <atomic>

#include <deque>
#include <list>
#include <memory>
#include <vector>

class CProcessInfo;
class CPackerMAT;

class CDVDAudioCodecPassthrough : public CDVDAudioCodec, public ISettingCallback
{
public:
  CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType);
  ~CDVDAudioCodecPassthrough() override;

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  void Dispose() override;
  bool AddData(const DemuxPacket &packet) override;
  void GetData(DVDAudioFrame &frame) override;
  void Reset() override;
  AEAudioFormat GetFormat() override { return m_format; }
  bool NeedPassthrough() override { return true; }
  std::string GetName() override { return m_codecName; }
  int GetBufferSize() override;

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

  //============================================================================
  // LAV Audio passthrough A/V sync (OFF by default)
  // Based on LAV Filters by Hendrik Leppkes (Nevcairiel).
  //============================================================================

  // Enable/disable the LAV Audio internal-clock + jitter sync path.
  void SetLavStyleSyncEnabled(bool enabled);
  bool IsLavStyleSyncEnabled() const { return m_lavStyleSyncEnabled; }
  void ClearBoundary();

  // Reset LAV sync state (for GENERAL_RESYNC without a full codec reset).
  void ResetLavSyncState();

  // Sync the internal clock to VideoPlayer's coordinated RESYNC timestamp. This
  // is the authoritative clock value that accounts for both audio and video;
  // call it from the GENERAL_RESYNC handler AFTER ResetLavSyncState().
  void SyncToResyncPts(double pts);

private:
  int GetData(uint8_t** dst);
  void UpdateDialNormSettings();
  unsigned int PackTrueHD();
  CDVDStreamInfo m_hints;
  CAEStreamParser m_parser;
  uint8_t* m_buffer = nullptr;
  unsigned int m_bufferSize = 0;
  unsigned int m_dataSize = 0;
  AEAudioFormat m_format;
  uint8_t *m_backlogBuffer = nullptr;
  unsigned int m_backlogBufferSize = 0;
  unsigned int m_backlogSize = 0;
  double m_currentPts = DVD_NOPTS_VALUE;
  double m_nextPts = DVD_NOPTS_VALUE;
  std::string m_codecName;

  // TrueHD specifics
  std::unique_ptr<CPackerMAT> m_packerMAT;
  std::vector<uint8_t> m_trueHDBuffer;
  unsigned int m_trueHDoffset = 0;
  unsigned int m_trueHDframes = 0;
  bool m_deviceIsRAW{false};

  // Dialogue normalisation defeat, cached from the settings callback (settings
  // thread) and applied to the parser on the audio thread.
  std::atomic<bool> m_defeatAC3DialNorm{false};
  std::atomic<bool> m_defeatEAC3AtmosDialNorm{false};
  std::atomic<bool> m_defeatTrueHDDialNorm{false};
  std::atomic<bool> m_defeatDTSDialNorm{false};
  bool m_isEAC3JOC{false};

  //============================================================================
  // LAV Audio A/V Sync state (only used when m_lavStyleSyncEnabled == true)
  //============================================================================
  // Based on LAV Filters by Hendrik Leppkes (Nevcairiel).
  //
  // We maintain our own internal clock (m_internalClock) that:
  //  - syncs to the RESYNC PTS from VideoPlayer (the coordinated A/V clock),
  //  - outputs PTS from our clock, not the demuxer,
  //  - continuously corrects any timing jitter/drift against the demuxer PTS
  //    that exceeds the threshold (not only at seamless branch points).
  // This isolates us from demuxer PTS chaos, including during seamless branching.
  //============================================================================
  bool m_lavStyleSyncEnabled{false};

  // Sentinel for "no valid PTS": we use -1.0 rather than DVD_NOPTS_VALUE, which
  // when cast to double becomes ~1.8e19 — the exact garbage value the demuxer
  // can emit during seamless branching.
  static constexpr double LOCAL_NOPTS = -1.0;

  // TrueHD timestamp caching: cache the PTS of the first frame in a MAT assembly.
  double m_truehdPtsCache{LOCAL_NOPTS};
  bool m_truehdPtsCacheValid{false};
  // the TrueHD seed (after a reset): the latest packet with its own pts among
  // the access units of the first MAT frame, and how many units came before it
  double m_seedRefPts{LOCAL_NOPTS};
  int m_seedRefUnits{0};
  int m_seedUnits{0};
  int m_matUnits{0}; // units in the MAT frame being packed
  // MAT frames held after a reset until a packet with its own pts dates them
  std::deque<std::vector<uint8_t>> m_heldMat;
  bool m_holdDone{false};
  double m_heldFirstPts{LOCAL_NOPTS};
  static constexpr size_t MAX_HELD_MAT = 8;

  // Jitter tracking using the LAV FloatingAverage (min-abs correction).
  static constexpr size_t JITTER_WINDOW_SIZE = 256;
  CFloatingAverage<double, JITTER_WINDOW_SIZE> m_jitterTracker;

  // Jitter correction thresholds (DVD_TIME_BASE units = microseconds). TrueHD/DTS
  // use a looser threshold for bitstreaming tolerance (whole frames only, no resample).
  static constexpr double JITTER_THRESHOLD_TRUEHD_DTS = 100000.0; // 100 ms
  static constexpr double JITTER_THRESHOLD_DEFAULT = 10000.0; // 10 ms
  double m_jitterThreshold{JITTER_THRESHOLD_DEFAULT};

  // Running output timestamp (like LAV's m_rtStart) and its resync flag.
  double m_internalClock{LOCAL_NOPTS};
  bool m_needsResync{true};

  // Rate limiter for the standing-jitter trace (see GetData).
  unsigned int m_jitterTraceCount{0};

  // A timeline correction (a Blu-ray seam or menu loop wrap) is marked by a
  // change in the packets' total offset; the step it leaves in the labels
  // against the content already emitted is realised at the output.
  double m_lastOffsetCorrection{0.0};
  bool m_haveOffsetCorrection{false};
  bool m_boundaryEnabled{true};
  bool m_boundaryMarkPending{false}; // the changed packet had no pts
  bool m_currentMark{false}; // m_currentPts / m_nextPts open the corrected timeline
  bool m_nextMark{false};
  unsigned int m_boundaryDrops{0}; // overlapped incoming frames still to drop
  double m_boundaryPad{0.0}; // the gap to pad before the next frame kept
  bool m_boundaryKeep{false}; // the next frame is the first kept after drops
  double m_boundaryJitter{0.0}; // the standing jitter before the boundary
  double m_lastJitter{0.0};
  // pts less the label of the content emitted (TIMEKEEPER pins by label)
  double m_labelLag{0.0};
};
