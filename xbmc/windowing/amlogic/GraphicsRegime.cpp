/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsRegime.h"

#include "DisplayState.h"
#include "OutputWorker.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace DISPLAYSTATE
{
namespace
{
std::atomic_bool s_latched{false};
std::atomic_bool s_on{false};

struct SStream
{
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
};

// what main applied last, until it is compared
struct SMain
{
  const char* site = "";
  GuiEncoding encoding = GuiEncoding::Srgb;
  bool composite = false;
  bool limited = false;
  unsigned int dvOutputMode = 0;
  bool isHdrDisplay = false;
  bool stop = false; //!< the stop regime: the class only
  uint64_t afterRequest = 0; //!< the requests posted before it
  bool owed = false; //!< compared at W's publish of afterRequest
};

struct SPeakShadow
{
  bool logged = false;
  float main = 0.0f;
  float pub = 0.0f;
  bool held = false;
  float heldMain = 0.0f;
  float heldPub = 0.0f;
};

std::mutex s_mutex;
GraphicsRegime s_pub;
SStream s_stream;
// main's requests to W (M7, stop) posted, and the newest W has published
uint64_t s_requested = 0;
uint64_t s_served = 0;
SMain s_main;
SPeakShadow s_peak;

const char* WriterName(DvWriter writer)
{
  switch (writer)
  {
    case DV_WRITER_VS10:
      return "vs10";
    case DV_WRITER_VS10_FOLLOW:
      return "vs10_follow";
    case DV_WRITER_DV_APPLY:
      return "dv_apply";
    case DV_WRITER_TEARDOWN:
      return "teardown_pre";
  }
  return "?";
}

//! a new epoch when a field of the regime changed (the inputs alone make none); its log line in
//! line
void PublishLocked(GraphicsRegime next, const char* by, std::string& line)
{
  const bool changed = s_pub.epoch == 0 || next.encoding != s_pub.encoding ||
                       next.limited != s_pub.limited || next.peak != s_pub.peak ||
                       next.compositeAvailable != s_pub.compositeAvailable;
  next.epoch = s_pub.epoch + (changed ? 1 : 0);
  s_pub = next;
  if (changed)
    line = StringUtils::Format(
        "GREGIME publish epoch={} class={} limited={} peak={:.4f} composite={} by={} dv={} "
        "hdrdisplay={}",
        s_pub.epoch, GuiEncodingName(s_pub.encoding), s_pub.limited ? 1 : 0, s_pub.peak,
        s_pub.compositeAvailable ? 1 : 0, by, s_pub.dvOutputMode, s_pub.isHdrDisplay ? 1 : 0);
}

std::string CompareLocked(const char* when)
{
  const SMain& m = s_main;
  std::string fields;
  const auto differ = [&fields](const char* field)
  { fields += (fields.empty() ? "" : ",") + std::string(field); };
  if (m.encoding != s_pub.encoding)
    differ("class");
  if (!m.stop)
  {
    if (m.limited != s_pub.limited)
      differ("limited");
    if (m.encoding == GuiEncoding::Composite && s_pub.encoding == GuiEncoding::Composite &&
        m.composite != s_pub.compositeAvailable)
      differ("composite");
  }
  return StringUtils::Format(
      "GREGIME shadow main={} pub={}/{} {} at={} when={} dv={}/{} hdrdisplay={}/{} limited={}/{} "
      "composite={}/{}",
      GuiEncodingName(m.encoding), GuiEncodingName(s_pub.encoding), s_pub.epoch,
      fields.empty() ? "agree" : "DISAGREE(" + fields + ")", m.site, when, m.dvOutputMode,
      s_pub.dvOutputMode, m.isHdrDisplay ? 1 : 0, s_pub.isHdrDisplay ? 1 : 0, m.limited ? 1 : 0,
      s_pub.limited ? 1 : 0, m.composite ? 1 : 0, s_pub.compositeAvailable ? 1 : 0);
}

//! request n has been published (or will never be): main's apply that waited for it is compared
void ServeLocked(uint64_t n, std::string& line)
{
  s_served = std::max(s_served, n);
  if (s_main.owed && s_main.afterRequest <= s_served)
  {
    s_main.owed = false;
    line = CompareLocked("late");
  }
}

void Log(const std::string& line)
{
  if (!line.empty())
    CLog::Log(LOGINFO, "{}", line);
}

void RepublishOnW(uint64_t n, const char* why)
{
  const unsigned int mode = aml_dv_get_output_mode();
  const bool isHdrDisplay = PublishedHdrDisplay();
  std::string publish;
  std::string shadow;
  {
    std::unique_lock lock(s_mutex);
    GraphicsRegime next = s_pub;
    next.encoding = CRendererAML::ResolveGuiEncoding(s_stream.hdrType, s_stream.transfer, mode,
                                                     isHdrDisplay);
    next.dvOutputMode = mode;
    next.isHdrDisplay = isHdrDisplay;
    PublishLocked(next, why, publish);
    if (publish.empty())
      publish = StringUtils::Format("GREGIME republish {} request={}: unchanged epoch={}", why, n,
                                    s_pub.epoch);
    ServeLocked(n, shadow);
  }
  Log(publish);
  Log(shadow);
}

//! posts fn(n) to W; when W does not run, n is served at once (nothing will publish it)
template<typename Fn>
void PostRequest(uint64_t n, const char* name, Fn fn)
{
  if (OUTPUT::COutputWorker::Get().PostIfRunning(OUTPUT::Lane::DECODER, name, [n, fn] { fn(n); }))
    return;
  std::string shadow;
  {
    std::unique_lock lock(s_mutex);
    ServeLocked(n, shadow);
  }
  CLog::Log(LOGWARNING, "GREGIME {} request={} not posted (W not running)", name, n);
  Log(shadow);
}
} // namespace

const char* GuiEncodingName(GuiEncoding encoding)
{
  switch (encoding)
  {
    case GuiEncoding::Srgb:
      return "srgb";
    case GuiEncoding::Scalar:
      return "scalar";
    case GuiEncoding::Composite:
      return "composite";
  }
  return "unknown";
}

bool RegimeOn()
{
  return s_latched.load(std::memory_order_acquire) && s_on.load(std::memory_order_relaxed);
}

GraphicsRegime GetRegime()
{
  std::unique_lock lock(s_mutex);
  return s_pub;
}

void RegimeInit(bool compositeAvailable, bool limited, float peak)
{
  if (!s_latched.load(std::memory_order_acquire))
  {
    // W publishes the class: without it, or without DisplayState's caps, there is nothing to
    // compare main with
    const char* off = nullptr;
    if (!aml_presentation_validated())
      off = "unvalidated SoC";
    else if (!OUTPUT::COutputWorker::Get().Active())
      off = "W not running";
    else if (!aml_displaystate_on())
      off = "displaystate_off";
    else if (XFILE::CFile::Exists("special://profile/gregime_off"))
      off = "gregime_off";
    s_on = !off;
    s_latched.store(true, std::memory_order_release);
    if (off)
      CLog::Log(LOGINFO, "GREGIME off ({})", off);
    else
      CLog::Log(LOGINFO, "GREGIME on: W publishes the graphics regime, main's applies are "
                         "compared (log only)");
  }
  if (!RegimeOn())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    GraphicsRegime next = s_pub;
    next.compositeAvailable = compositeAvailable;
    next.limited = limited;
    if (next.epoch == 0)
      next.peak = peak;
    PublishLocked(next, "init", line);
  }
  Log(line);
}

void PublishRegimeComposite(bool available, const char* why)
{
  if (!RegimeOn())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    GraphicsRegime next = s_pub;
    next.compositeAvailable = available;
    PublishLocked(next, why, line);
  }
  Log(line);
}

void PublishRegimePeak(int setting)
{
  if (!RegimeOn())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    GraphicsRegime next = s_pub;
    next.peak = GuiPeakCode(setting);
    PublishLocked(next, "peak", line);
  }
  Log(line);
}

void NoteRegimeStream(StreamHdrType hdrType, AVColorTransferCharacteristic transfer)
{
  if (!RegimeOn() || !OUTPUT::COutputWorker::Get().OnWorker())
    return;
  std::unique_lock lock(s_mutex);
  s_stream = {hdrType, transfer};
}

void PublishRegimeDvMode(unsigned int mode, DvWriter writer)
{
  if (!RegimeOn())
    return;
  // the decoder's close: main keeps its encoding until the next open's picture configures, or
  // returns to sRGB at EndRenderSession, which at a stop runs before the decoder closes (VP
  // CloseFile); in a disc session this BYPASS is the volatile gap value ConfigChanged ignores
  if (writer == DV_WRITER_TEARDOWN)
    return;
  if (!OUTPUT::COutputWorker::Get().OnWorker())
  {
    static std::atomic_bool logged{false};
    if (!logged.exchange(true))
      CLog::Log(LOGWARNING, "GREGIME publish off W (writer={}): not published", WriterName(writer));
    return;
  }
  const bool isHdrDisplay = PublishedHdrDisplay();
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    GraphicsRegime next = s_pub;
    next.encoding = CRendererAML::ResolveGuiEncoding(s_stream.hdrType, s_stream.transfer, mode,
                                                     isHdrDisplay);
    next.dvOutputMode = mode;
    next.isHdrDisplay = isHdrDisplay;
    PublishLocked(next, WriterName(writer), line);
  }
  Log(line);
}

void RequestRegimeRepublish(const char* why)
{
  if (!RegimeOn())
    return;
  uint64_t n;
  {
    std::unique_lock lock(s_mutex);
    n = ++s_requested;
  }
  PostRequest(n, "gregime republish", [why](uint64_t request) { RepublishOnW(request, why); });
}

void RegimeStop()
{
  if (!RegimeOn())
    return;
  uint64_t n;
  {
    std::unique_lock lock(s_mutex);
    // the request first, so main's stop is compared with the publish of it
    n = ++s_requested;
    s_main = SMain{};
    s_main.site = "stop";
    s_main.stop = true;
    s_main.dvOutputMode = aml_dv_get_output_mode();
    s_main.afterRequest = n;
    s_main.owed = true;
  }
  PostRequest(n, "gregime stop",
              [](uint64_t request)
              {
                std::string publish;
                std::string shadow;
                {
                  std::unique_lock lock(s_mutex);
                  s_stream = {};
                  GraphicsRegime next = s_pub;
                  next.encoding = GuiEncoding::Srgb;
                  next.dvOutputMode = aml_dv_get_output_mode();
                  PublishLocked(next, "stop", publish);
                  ServeLocked(request, shadow);
                }
                Log(publish);
                Log(shadow);
              });
}

void ShadowMainRegime(const char* site,
                      GuiEncoding encoding,
                      bool composite,
                      bool limited,
                      unsigned int dvOutputMode,
                      bool isHdrDisplay)
{
  if (!RegimeOn())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    s_main = SMain{site, encoding, composite, limited, dvOutputMode, isHdrDisplay, false,
                   s_requested, false};
    // a re-publish main posted before this apply is compared when W has published it
    if (s_served >= s_requested)
      line = CompareLocked("now");
    else
      s_main.owed = true;
  }
  Log(line);
}

void ShadowMainPeak(float peak)
{
  if (!RegimeOn())
    return;
  std::string line;
  {
    std::unique_lock lock(s_mutex);
    SPeakShadow& p = s_peak;
    const float pub = s_pub.peak;
    if (p.logged && p.main == peak && p.pub == pub)
      return;
    // the mirror's store and the publish are two steps on the setting's thread: a difference
    // is one only when the next read still sees it
    if (peak != pub && !(p.held && p.heldMain == peak && p.heldPub == pub))
    {
      p.held = true;
      p.heldMain = peak;
      p.heldPub = pub;
      return;
    }
    p.held = false;
    p.logged = true;
    p.main = peak;
    p.pub = pub;
    line = StringUtils::Format("GREGIME shadow peak main={:.4f} pub={:.4f}/{} {}", peak, pub,
                               s_pub.epoch, peak == pub ? "agree" : "DISAGREE(peak)");
  }
  Log(line);
}

} // namespace DISPLAYSTATE
