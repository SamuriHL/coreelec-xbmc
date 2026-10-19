/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/Interface/StreamInfo.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Phase 5 step 5.E1 (log only): the HDR class the kernel should put on the wire, predicted from
// the content and the kernel's own policy inputs (design §4.4 ExpectedWireClass), against the
// `hdmi_hdr_status` it reports, after every mode set, decoder open and live VS10 change. A wrong
// row surfaces as a logged DIFF before 5.E2 makes the class a SETTLE condition. Reads sysfs and
// module parameters only, on its own thread; writes nothing; nobody waits on it.
// special://profile/wireprobe_off turns it off.

namespace WIRECLASS
{

enum class Source
{
  UNKNOWN,
  SDR,
  HDR10,
  HLG,
  HDR10PLUS,
  DV,
};

struct Inputs
{
  bool dvEnable = false;  // dolby_vision_enable
  int dvTargetMode = -1;  // amdv_target_mode: the mode the DV core chose (5 = BYPASS)
  int dvPolicy = -1;      // dolby_vision_policy
  int dvLlPolicy = -1;    // dolby_vision_ll_policy
  int hdrPolicy = -1;     // hdr_policy
  int hdrMode = -1;       // hdr_mode
  int sdrMode = -1;       // sdr_mode
  int enableHdr10plus = -1;
  Source source = Source::UNKNOWN;
  int sinkPq = -1;        // hdr_cap: SMPTE ST 2084
  int sinkHlg = -1;       // hdr_cap: Hybrid Log-Gamma
  int sinkHdr10plus = -1; // hdr_cap: HDR10Plus Supported (the VSDB is present, version not parsed)
  int sinkDv = -1;        // dv_cap lists RX support
  int wireBits = -1;      // config: the link's depth, 8, 10 or 12
};

struct Prediction
{
  std::string expected; // an hdmi_hdr_status string, or "any"
  const char* row;      // which rule decided it
  const char* basis;    // "measured", "code" or "unmodelled"
};

// pure: no I/O
Prediction Expected(const Inputs& in);
const char* SourceName(Source s);

inline Source FromHdrType(StreamHdrType type)
{
  switch (type)
  {
    case StreamHdrType::HDR_TYPE_NONE:
      return Source::SDR;
    case StreamHdrType::HDR_TYPE_HDR10:
      return Source::HDR10;
    case StreamHdrType::HDR_TYPE_HLG:
      return Source::HLG;
    case StreamHdrType::HDR_TYPE_HDR10PLUS:
      return Source::HDR10PLUS;
    case StreamHdrType::HDR_TYPE_DOLBYVISION:
      return Source::DV;
    default:
      return Source::UNKNOWN;
  }
}

class CWireClassProbe
{
public:
  static CWireClassProbe& Get();
  ~CWireClassProbe() { Stop(); }

  void Start();
  void Stop();

  // any thread, never blocks: a check after the output has had time to settle; a newer one
  // supersedes an older one that has not finished
  void Notify(const char* origin, Source gfxSource);
  // the AML decoder's content class at its open (UNKNOWN when it closes)
  void SetDecoderSource(Source s) { m_decoderSource.store(s, std::memory_order_relaxed); }

private:
  CWireClassProbe() = default;
  void Run();
  void Check(const char* origin, Source gfxSource, uint64_t serial);
  Inputs ReadInputs(Source gfxSource) const;

  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cond;
  bool m_running = false;
  bool m_stop = false;
  uint64_t m_serial = 0;      // bumps per Notify
  uint64_t m_doneSerial = 0;  // the last one taken
  const char* m_origin = "";
  Source m_gfxSource = Source::UNKNOWN;
  std::atomic<Source> m_decoderSource{Source::UNKNOWN};
};

} // namespace WIRECLASS
