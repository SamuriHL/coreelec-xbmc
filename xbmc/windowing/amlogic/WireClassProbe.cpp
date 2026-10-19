/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WireClassProbe.h"

#include "ServiceBroker.h"
#include "filesystem/File.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/log.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace std::chrono_literals;

namespace WIRECLASS
{
namespace
{
constexpr int DV_MODE_IPT = 0;
constexpr int DV_MODE_IPT_TUNNEL = 1;
constexpr int DV_MODE_HDR10 = 2;
constexpr int DV_MODE_SDR10 = 3;
constexpr int DV_MODE_SDR8 = 4;
constexpr int DV_MODE_BYPASS = 5;
constexpr int DV_POLICY_FORCE = 2;
// amcsc.c:90 on the shipped kernel (common_drivers f9af0b41d6); not readable from user space
constexpr bool LIMIT_8BIT_HDR10 = true;

const std::string ST2084 = "HDR10-GAMMA_ST2084";
const std::string HLG = "HDR10-GAMMA_HLG";
const std::string SDR = "SDR";
const std::string ANY = "any";

std::string ReadFile(const char* path)
{
  std::ifstream file(path);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

int ReadParam(const char* name)
{
  const std::string text = ReadFile(
      (std::string("/sys/module/aml_media/parameters/") + name).c_str());
  if (text.empty())
    return -1;
  if (text[0] == 'Y')
    return 1;
  if (text[0] == 'N')
    return 0;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  return end == text.c_str() ? -1 : static_cast<int>(value);
}

// "SMPTE ST 2084: 1" in hdr_cap's text
int CapFlag(const std::string& text, const char* key)
{
  const size_t at = text.find(key);
  if (at == std::string::npos)
    return -1;
  const size_t colon = text.find(':', at);
  if (colon == std::string::npos || colon + 2 >= text.size())
    return -1;
  return text[colon + 2] == '1' ? 1 : 0;
}

std::string Status()
{
  std::string text = ReadFile("/sys/class/amhdmitx/amhdmitx0/hdmi_hdr_status");
  while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
    text.pop_back();
  return text.empty() ? "unreadable" : text;
}
} // namespace

const char* SourceName(Source s)
{
  switch (s)
  {
    case Source::SDR:
      return "SDR";
    case Source::HDR10:
      return "HDR10";
    case Source::HLG:
      return "HLG";
    case Source::HDR10PLUS:
      return "HDR10+";
    case Source::DV:
      return "DV";
    default:
      return "unknown";
  }
}

Prediction Expected(const Inputs& in)
{
  // the DV core decides while it is enabled and not forced to bypass (get_hdr_policy,
  // amcsc.c:639-660); hdmi_hdr_status reports the HDR10+ VSIF, then DV, then HDR, else SDR
  // (hdmitx21 hdmi_tx_main.c:2225-2266, hdmitx20 :1933-1974)
  const bool dvDecides = in.dvEnable && !(in.dvPolicy == DV_POLICY_FORCE &&
                                          in.dvTargetMode == DV_MODE_BYPASS);
  if (in.dvEnable && in.dvTargetMode >= 0 && in.dvTargetMode != DV_MODE_BYPASS)
  {
    switch (in.dvTargetMode)
    {
      case DV_MODE_IPT_TUNNEL:
        return {"DolbyVision-Std", "dv-tunnel", "measured"};
      case DV_MODE_IPT:
        if (in.dvLlPolicy > 0)
          return {"DolbyVision-Lowlatency", "dv-ll", "measured"};
        return {ANY, "dv-ipt-ll0", "unmodelled"};
      case DV_MODE_HDR10:
        return {ST2084, "dv-hdr10", "code"};
      case DV_MODE_SDR10:
      case DV_MODE_SDR8:
        return {SDR, "dv-sdr", "code"};
      default:
        return {ANY, "dv-mode", "unmodelled"};
    }
  }

  const int policy = dvDecides ? in.dvPolicy : in.hdrPolicy;
  if (in.source == Source::UNKNOWN || in.source == Source::DV || policy < 0 || policy > 1 ||
      in.hdrMode < 0 || in.hdrMode > 1 || in.sdrMode < 0 || in.sdrMode > 1 || in.sinkPq < 0 ||
      in.wireBits < 0)
    return {ANY, "inputs", "unmodelled"};

  // an 8-bit wire clears the sink's HDR caps unless DV follows the source to a DV sink with LL
  // policy 0 (amcsc.c:4123, 4170-4177); both policies then send no HDR packet (8131-8138,
  // 8896-8907)
  const bool stripped = LIMIT_8BIT_HDR10 && in.wireBits == 8 &&
                        !(in.dvEnable && policy == 1 && in.sinkDv == 1 && in.dvLlPolicy == 0);
  const bool sinkPq = !stripped && in.sinkPq == 1;
  const bool sinkHlg = !stripped && in.sinkHlg == 1;
  const bool sinkHdr10plus = !stripped && in.sinkHdr10plus == 1 && in.enableHdr10plus == 1;

  if (policy == 0)
  {
    // follow sink (amcsc.c:8896-8907)
    if (stripped)
      return {SDR, "p0-8bit", "measured"};
    if (in.source == Source::SDR || in.source == Source::HDR10)
      return sinkPq ? Prediction{ST2084, "p0-pq", "measured"}
                    : Prediction{ANY, "p0-nopq", "unmodelled"};
    // amcsc_pip.c:869-873
    if (in.source == Source::HLG && sinkHlg)
      return {HLG, "p0-hlg", "measured"};
    return {ANY, "p0-hlg-hdr10+", "unmodelled"};
  }

  // follow source
  if (in.source == Source::SDR)
  {
    if (in.sdrMode == 1)
      return sinkPq ? Prediction{ST2084, "p1-sdr2hdr", "code"}
                    : Prediction{ANY, "p1-sdr2hdr-nopq", "unmodelled"};
    return {SDR, "p1-sdr", "measured"};
  }
  if (in.hdrMode == 1)
    return {SDR, "p1-hdr2sdr", in.source == Source::HDR10PLUS ? "code" : "measured"};
  if (stripped)
    return {SDR, "p1-8bit", "measured"};
  switch (in.source)
  {
    case Source::HDR10:
      return sinkPq ? Prediction{ST2084, "p1-hdr10", "measured"}
                    : Prediction{SDR, "p1-hdr10-nopq", "code"};
    case Source::HLG:
      // amcsc.c:7220-7230
      if (sinkHlg)
        return {HLG, "p1-hlg", "measured"};
      return sinkPq ? Prediction{ST2084, "p1-hlg-pq", "code"}
                    : Prediction{SDR, "p1-hlg-sdr", "code"};
    case Source::HDR10PLUS:
      // the VSIF needs VSDB application version 1 (amcsc.c:4156-4158), which hdr_cap does not
      // show; without the VSDB hdmitx sends ST2084 (hdmitx21 hdmi_tx_main.c:961-967)
      if (sinkHdr10plus)
        return {ANY, "p1-hdr10+-vsdb", "unmodelled"};
      return sinkPq ? Prediction{ST2084, "p1-hdr10+-pq", "code"}
                    : Prediction{SDR, "p1-hdr10+-sdr", "code"};
    default:
      return {ANY, "source", "unmodelled"};
  }
}

CWireClassProbe& CWireClassProbe::Get()
{
  static CWireClassProbe probe;
  return probe;
}

void CWireClassProbe::Start()
{
  std::unique_lock lock(m_mutex);
  if (m_running)
    return;
  if (XFILE::CFile::Exists("special://profile/wireprobe_off"))
  {
    CLog::Log(LOGINFO, "WIRECLASS: off (wireprobe_off)");
    return;
  }
  m_stop = false;
  m_running = true;
  m_thread = std::thread(&CWireClassProbe::Run, this);
}

void CWireClassProbe::Stop()
{
  {
    std::unique_lock lock(m_mutex);
    if (!m_running)
      return;
    m_stop = true;
    m_running = false;
  }
  m_cond.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void CWireClassProbe::Notify(const char* origin, Source gfxSource)
{
  {
    std::unique_lock lock(m_mutex);
    if (!m_running)
      return;
    m_serial++;
    m_origin = origin;
    if (gfxSource != Source::UNKNOWN)
      m_gfxSource = gfxSource;
  }
  m_cond.notify_all();
}

void CWireClassProbe::Run()
{
  std::unique_lock lock(m_mutex);
  while (!m_stop)
  {
    m_cond.wait(lock, [this] { return m_stop || m_serial != m_doneSerial; });
    if (m_stop)
      break;
    m_doneSerial = m_serial;
    const char* origin = m_origin;
    const Source gfxSource = m_gfxSource;
    const uint64_t serial = m_serial;
    lock.unlock();
    Check(origin, gfxSource, serial);
    lock.lock();
  }
}

Inputs CWireClassProbe::ReadInputs(Source gfxSource) const
{
  Inputs in;
  in.dvEnable = ReadParam("dolby_vision_enable") == 1;
  in.dvTargetMode = ReadParam("amdv_target_mode");
  in.dvPolicy = ReadParam("dolby_vision_policy");
  in.dvLlPolicy = ReadParam("dolby_vision_ll_policy");
  in.hdrPolicy = ReadParam("hdr_policy");
  in.hdrMode = ReadParam("hdr_mode");
  in.sdrMode = ReadParam("sdr_mode");
  const std::string e10p = ReadFile("/sys/class/amvecm/enable_hdr10plus");
  in.enableHdr10plus = e10p.empty() ? -1 : (e10p[0] == '1' ? 1 : 0);
  const Source decoder = m_decoderSource.load(std::memory_order_relaxed);
  in.source = decoder != Source::UNKNOWN ? decoder : gfxSource;
  const std::string hdrCap = ReadFile("/sys/class/amhdmitx/amhdmitx0/hdr_cap");
  in.sinkPq = CapFlag(hdrCap, "SMPTE ST 2084");
  in.sinkHlg = CapFlag(hdrCap, "Hybrid Log-Gamma");
  in.sinkHdr10plus = CapFlag(hdrCap, "HDR10Plus Supported");
  const std::string dvCap = ReadFile("/sys/class/amhdmitx/amhdmitx0/dv_cap");
  in.sinkDv = dvCap.empty() ? -1 : (dvCap.find("RX support list") != std::string::npos ? 1 : 0);
  // config's depth is the link's; attr holds only what was last written to it
  const std::string config = ReadFile("/sys/class/amhdmitx/amhdmitx0/config");
  const size_t depth = config.find("Colour depth: ");
  in.wireBits = depth == std::string::npos ? -1 : std::atoi(config.c_str() + depth + 14);
  if (in.wireBits != 8 && in.wireBits != 10 && in.wireBits != 12)
    in.wireBits = -1;
  return in;
}

void CWireClassProbe::Check(const char* origin, Source gfxSource, uint64_t serial)
{
  // as SETTLE will: from the commit, the delay plus 100 ms, then until the class matches, capped
  // at the delay plus 3 s (§4.4)
  const auto start = std::chrono::steady_clock::now();
  const auto settings = CServiceBroker::GetSettingsComponent();
  const int delayMs =
      settings && settings->GetSettings()
          ? 100 * settings->GetSettings()->GetInt("videoscreen.delayrefreshchange")
          : 0;
  const auto first = start + std::chrono::milliseconds(delayMs + 100);
  const auto cap = start + std::chrono::milliseconds(delayMs + 3000);

  std::unique_lock lock(m_mutex);
  auto superseded = [this, serial] { return m_stop || m_serial != serial; };
  if (m_cond.wait_until(lock, first, superseded))
  {
    const bool stopped = m_stop;
    lock.unlock();
    if (!stopped)
      CLog::Log(LOGINFO, "WIRECLASS {}: superseded before the first read", origin);
    return;
  }

  std::string firstActual;
  while (true)
  {
    lock.unlock();
    const Inputs in = ReadInputs(gfxSource);
    const Prediction p = Expected(in);
    const std::string actual = Status();
    lock.lock();
    if (firstActual.empty())
      firstActual = actual;
    const auto now = std::chrono::steady_clock::now();
    // an unmodelled prediction can become modelled once the DV core has seen a frame
    const bool match = p.expected == actual;
    if (match || now >= cap)
    {
      lock.unlock();
      const long ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      CLog::Log(LOGINFO,
                "WIRECLASS {}: {} predicted {} ({}, {}) actual {} at +{} ms (first {}) | src {} "
                "dv {} mode {} policy {} ll {} | hdr_policy {} hdr_mode {} sdr_mode {} "
                "enable_hdr10plus {} | sink pq {} hlg {} hdr10+ {} dv {} | wire {} bit",
                origin, p.expected == ANY ? "UNMODELLED" : (match ? "MATCH" : "DIFF"),
                p.expected, p.row, p.basis, actual, ms, firstActual, SourceName(in.source),
                in.dvEnable ? 1 : 0, in.dvTargetMode, in.dvPolicy, in.dvLlPolicy, in.hdrPolicy,
                in.hdrMode, in.sdrMode, in.enableHdr10plus, in.sinkPq, in.sinkHlg,
                in.sinkHdr10plus, in.sinkDv, in.wireBits);
      return;
    }
    if (m_cond.wait_for(lock, 50ms, superseded))
    {
      const bool stopped = m_stop;
      lock.unlock();
      if (!stopped)
        CLog::Log(LOGINFO, "WIRECLASS {}: superseded while waiting for {} (last {})", origin,
                  p.expected, actual);
      return;
    }
  }
}

} // namespace WIRECLASS
