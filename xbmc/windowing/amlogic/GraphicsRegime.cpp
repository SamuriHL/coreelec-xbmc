/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GraphicsRegime.h"

#include "DisplayState.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "GraphicsTick.h"
#include "OutputWorker.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/PresentationCoordinator.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "filesystem/File.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>

namespace DISPLAYSTATE
{
namespace
{
std::atomic_bool s_latched{false};
std::atomic_bool s_on{false};
// GT-6
std::atomic_bool s_knobsAllowed{false};
std::atomic_bool s_knobOwnerC{false};
//! main's latest wanted knobs: seq << 8 | PackKnobs (0: none yet)
std::atomic<uint64_t> s_mainWanted{0};
//! the knobs as last written: PackKnobs (0: unknown)
std::atomic<uint32_t> s_knobsWritten{0};
std::atomic<int64_t> s_stopOwedNs{0};
std::atomic<uint64_t> s_stopPublishes{0};
std::atomic<uint64_t> s_stopPublishesAtOwed{0};

uint32_t PackKnobs(SKnobs knobs)
{
  return 1u | (knobs.osdPq ? 2u : 0u) | (knobs.hdr8 ? 4u : 0u);
}

SKnobs UnpackKnobs(uint32_t packed)
{
  return SKnobs{(packed & 2u) != 0, (packed & 4u) != 0};
}

int64_t SteadyNowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

//! RC24 follow-up: a published cell its readers copy with no lock (G reads the regime per
//! composition; a std::mutex holder descheduled on main/W stalled G the way atomic<shared_ptr>'s
//! lock stalled C in RC23). Two slots, each with a sequence: the writer (under its own lock)
//! fills the slot readers are not pointed at, then points them at it, so a reader retries only
//! when two publishes complete during its copy, never on a writer descheduled mid-write
template<typename T>
class CRtCell
{
  static_assert(std::is_trivially_copyable_v<T>);
  static constexpr size_t WORDS = (sizeof(T) + 7) / 8;

public:
  //! writers only, serialised by their own lock
  void Publish(const T& value)
  {
    const int next = 1 - m_index.load(std::memory_order_relaxed);
    Slot& slot = m_slots[next];
    std::array<uint64_t, WORDS> words{};
    std::memcpy(words.data(), &value, sizeof(T));
    const uint32_t seq = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < WORDS; i++)
      slot.words[i].store(words[i], std::memory_order_relaxed);
    slot.seq.store(seq + 2, std::memory_order_release);
    m_index.store(next, std::memory_order_release);
  }
  //! any thread, no lock
  T Read() const
  {
    for (;;)
    {
      const Slot& slot = m_slots[m_index.load(std::memory_order_acquire)];
      const uint32_t before = slot.seq.load(std::memory_order_acquire);
      if (before & 1)
        continue;
      std::array<uint64_t, WORDS> words;
      for (size_t i = 0; i < WORDS; i++)
        words[i] = slot.words[i].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (slot.seq.load(std::memory_order_relaxed) != before)
        continue;
      T value;
      std::memcpy(&value, words.data(), sizeof(T));
      return value;
    }
  }

private:
  struct Slot
  {
    std::atomic<uint32_t> seq{0};
    std::array<std::atomic<uint64_t>, WORDS> words{};
  };
  std::array<Slot, 2> m_slots{};
  std::atomic<int> m_index{0};
};

CRtCell<GraphicsRegime> s_pubCell; //!< s_pub as published, for GetRegime
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
  s_pubCell.Publish(s_pub);
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

//! GT-6, on W after a publish: a change of the tag gets C's cleared buffers made for this mode,
//! so C can clear an old-encoding buffer at the new regime's first slot without waiting
void ArmClears(uint8_t before, uint8_t after)
{
  if (before == after || !KnobOwnerC() || !OUTPUT::COutputWorker::Get().OnWorker())
    return;
  if (CPresentationCoordinator* coordinator = CPresentationCoordinator::Get())
    coordinator->ArmKnobClears();
}

void RepublishOnW(uint64_t n, const char* why)
{
  const unsigned int mode = aml_dv_get_output_mode();
  const bool isHdrDisplay = PublishedHdrDisplay();
  std::string publish;
  std::string shadow;
  uint8_t before;
  uint8_t after;
  {
    std::unique_lock lock(s_mutex);
    before = RegimeTag(s_pub);
    GraphicsRegime next = s_pub;
    next.encoding = CRendererAML::ResolveGuiEncoding(s_stream.hdrType, s_stream.transfer, mode,
                                                     isHdrDisplay);
    next.dvOutputMode = mode;
    next.isHdrDisplay = isHdrDisplay;
    PublishLocked(next, why, publish);
    after = RegimeTag(s_pub);
    if (publish.empty())
      publish = StringUtils::Format("GREGIME republish {} request={}: unchanged epoch={}", why, n,
                                    s_pub.epoch);
    ServeLocked(n, shadow);
  }
  Log(publish);
  Log(shadow);
  ArmClears(before, after);
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
  // no lock (G per composition): the cell PublishLocked fills under s_mutex
  return s_pubCell.Read();
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
    // GT-6: the knobs move to C only on graphicstick route sessions
    if (GRAPHICSTICK::Requested())
    {
      const bool gknobOff = XFILE::CFile::Exists("special://profile/gknob_off");
      s_knobsAllowed = !off && !gknobOff;
      CLog::Log(LOGINFO, "REGIME knobs: {}",
                s_knobsAllowed ? "C writes them on a route session"
                : gknobOff     ? "main writes them (gknob_off)"
                               : "main writes them (no regime)");
    }
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
  uint8_t before;
  uint8_t after;
  {
    std::unique_lock lock(s_mutex);
    before = RegimeTag(s_pub);
    GraphicsRegime next = s_pub;
    next.encoding = CRendererAML::ResolveGuiEncoding(s_stream.hdrType, s_stream.transfer, mode,
                                                     isHdrDisplay);
    next.dvOutputMode = mode;
    next.isHdrDisplay = isHdrDisplay;
    PublishLocked(next, WriterName(writer), line);
    after = RegimeTag(s_pub);
  }
  Log(line);
  ArmClears(before, after);
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
                uint8_t before;
                uint8_t after;
                {
                  std::unique_lock lock(s_mutex);
                  before = RegimeTag(s_pub);
                  s_stream = {};
                  GraphicsRegime next = s_pub;
                  next.encoding = GuiEncoding::Srgb;
                  next.dvOutputMode = aml_dv_get_output_mode();
                  PublishLocked(next, "stop", publish);
                  after = RegimeTag(s_pub);
                  ServeLocked(request, shadow);
                }
                s_stopPublishes.fetch_add(1);
                Log(publish);
                Log(shadow);
                ArmClears(before, after);
              });
}

namespace
{
std::mutex s_appliedMutex;
MainApplied s_applied;
CRtCell<MainApplied> s_appliedCell; //!< s_applied as published, for GetMainApplied
} // namespace

void PublishMainApplied(GuiEncoding encoding, bool limited, SKnobs knobs)
{
  const uint8_t tag = KnobTag(encoding, knobs);
  std::unique_lock lock(s_appliedMutex);
  if (s_applied.epoch != 0 && s_applied.encoding == encoding && s_applied.limited == limited &&
      s_applied.tag == tag)
    return;
  s_applied.epoch++;
  s_applied.encoding = encoding;
  s_applied.limited = limited;
  s_applied.tag = tag;
  s_appliedCell.Publish(s_applied);
}

MainApplied GetMainApplied()
{
  // no lock (G per composition): the cell PublishMainApplied fills under s_appliedMutex
  return s_appliedCell.Read();
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

uint8_t KnobTag(GuiEncoding effective, SKnobs knobs)
{
  return static_cast<uint8_t>(1u | (static_cast<unsigned int>(effective) << 1) |
                              (knobs.osdPq ? 8u : 0u) | (knobs.hdr8 ? 16u : 0u));
}

SKnobs TagKnobs(uint8_t tag)
{
  return SKnobs{(tag & 8u) != 0, (tag & 16u) != 0};
}

GuiEncoding TagEncoding(uint8_t tag)
{
  return static_cast<GuiEncoding>((tag >> 1) & 3u);
}

uint8_t RegimeTag(const GraphicsRegime& regime)
{
  // CRendererAML::ApplyGuiEncoding's rules: a failed composite falls back to the scalar trim,
  // osd_pq_passthrough only for a composite on native output, graphic_fmt from the class
  const GuiEncoding effective =
      regime.encoding == GuiEncoding::Composite && !regime.compositeAvailable
          ? GuiEncoding::Scalar
          : regime.encoding;
  SKnobs knobs;
  knobs.osdPq = effective == GuiEncoding::Composite &&
                regime.dvOutputMode == DOLBY_VISION_OUTPUT_MODE_BYPASS;
  knobs.hdr8 = regime.encoding != GuiEncoding::Srgb;
  return KnobTag(effective, knobs);
}

std::string TagName(uint8_t tag)
{
  if (!tag)
    return "none";
  const SKnobs knobs = TagKnobs(tag);
  return StringUtils::Format("{}/{}/{}", GuiEncodingName(TagEncoding(tag)), knobs.osdPq ? 1 : 0,
                             knobs.hdr8 ? 9 : 2);
}

bool KnobsAllowed()
{
  return s_latched.load(std::memory_order_acquire) && s_knobsAllowed.load();
}

bool KnobOwnerC()
{
  return s_knobOwnerC.load();
}

void KnobRouteOn()
{
  if (!KnobsAllowed())
    return;
  s_stopOwedNs = 0;
  if (!s_knobOwnerC.exchange(true))
    CLog::Log(LOGINFO, "REGIME knobs to C (route on)");
}

void KnobStopOwed()
{
  if (!KnobOwnerC())
    return;
  s_stopPublishesAtOwed = s_stopPublishes.load();
  s_stopOwedNs = SteadyNowNs();
}

bool MainWritesKnobs(SKnobs knobs)
{
  uint64_t old = s_mainWanted.load();
  uint64_t next;
  do
    next = (((old >> 8) + 1) << 8) | PackKnobs(knobs);
  while (!s_mainWanted.compare_exchange_weak(old, next));
  // after the store: C's hand-back reads the wanted after its own store (one of the two writes)
  return !s_knobOwnerC.load();
}

bool KnobHandBack(SKnobs& wanted)
{
  s_knobOwnerC.store(false);
  s_stopOwedNs = 0;
  const uint64_t latest = s_mainWanted.load();
  if (!latest)
    return false;
  wanted = UnpackKnobs(static_cast<uint32_t>(latest & 0xff));
  return true;
}

int64_t KnobStopOwedNs()
{
  return s_stopOwedNs.load();
}

bool KnobStopPublished()
{
  return s_stopPublishes.load() > s_stopPublishesAtOwed.load();
}

void NoteKnobsWritten(SKnobs knobs)
{
  s_knobsWritten = PackKnobs(knobs);
}

bool KnobsWritten(SKnobs& knobs)
{
  const uint32_t packed = s_knobsWritten.load();
  if (!packed)
    return false;
  knobs = UnpackKnobs(packed);
  return true;
}

uint8_t PredictVs10Tag(unsigned int mode)
{
  GraphicsRegime next;
  SStream stream;
  {
    std::unique_lock lock(s_mutex);
    next = s_pub;
    stream = s_stream;
  }
  const uint8_t now = RegimeTag(next);
  // aml_dv_set_vs10_mode_now's refusals write nothing
  if (mode == DOLBY_VISION_OUTPUT_MODE_BYPASS && aml_dv_disc_engaged())
    return now;
  if (mode != DOLBY_VISION_OUTPUT_MODE_BYPASS &&
      !CServiceBroker::GetDataCacheCore().IsVideoHwDecoder())
    return now;
  const unsigned int current = aml_dv_get_output_mode();
  unsigned int after = mode;
  if (mode == DOLBY_VISION_OUTPUT_MODE_BYPASS)
  {
    if (current == DOLBY_VISION_OUTPUT_MODE_BYPASS)
      return now;
    // follow source: a DV source stays on the core in a PQ mode, any other returns to native
    const bool pq = current == DOLBY_VISION_OUTPUT_MODE_IPT ||
                    current == DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL ||
                    current == DOLBY_VISION_OUTPUT_MODE_HDR10;
    if (aml_dv_source_is_dv())
      after = pq ? current : DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL;
  }
  next.encoding = CRendererAML::ResolveGuiEncoding(stream.hdrType, stream.transfer, after,
                                                   PublishedHdrDisplay());
  next.dvOutputMode = after;
  return RegimeTag(next);
}

void RunVs10Write(unsigned int mode)
{
  COsdHoldScope hold(PredictVs10Tag(mode), "vs10", mode, 2000);
  aml_dv_set_vs10_mode_now(mode);
}

uint8_t PredictStreamTag(StreamHdrType hdrType,
                         AVColorTransferCharacteristic transfer,
                         unsigned int mode)
{
  GraphicsRegime next;
  {
    std::unique_lock lock(s_mutex);
    next = s_pub;
  }
  next.encoding =
      CRendererAML::ResolveGuiEncoding(hdrType, transfer, mode, PublishedHdrDisplay());
  next.dvOutputMode = mode;
  return RegimeTag(next);
}

namespace
{
std::atomic<uint64_t> s_predictChecked{0};
std::atomic<uint64_t> s_predictMissed{0};
} // namespace

void TakePredictCounts(uint64_t& checked, uint64_t& missed)
{
  checked = s_predictChecked.exchange(0);
  missed = s_predictMissed.exchange(0);
}

COsdHoldScope::COsdHoldScope(uint8_t predicted, const char* why, unsigned int mode, int boundMs)
  : m_predicted(predicted), m_why(why), m_mode(mode)
{
  CPresentationCoordinator* coordinator = CPresentationCoordinator::Get();
  m_route = coordinator && KnobOwnerC() && GRAPHICSTICK::RouteOn() &&
            OUTPUT::COutputWorker::Get().OnWorker();
  if (!m_route)
    return;
  m_from = RegimeTag(GetRegime());
  if (m_from == m_predicted)
    return;
  m_held = true;
  m_beginNs = SteadyNowNs();
  coordinator->ArmKnobClears();
  const bool flipped = coordinator->OsdHoldBegin(std::chrono::milliseconds(boundMs));
  const int64_t waitedMs = (SteadyNowNs() - m_beginNs) / 1000000;
  if (flipped)
    CLog::Log(LOGINFO, "REGIME osd hold begin why={} mode={} from={} to={} flip_wait_ms={}", why,
              mode, TagName(m_from), TagName(m_predicted), waitedMs);
  else if (waitedMs >= boundMs)
    CLog::Log(LOGERROR,
              "REGIME osd hold: C's cleared buffers not flipped in {} ms, the DV writes go ahead "
              "(why={} mode={})",
              waitedMs, why, mode);
  else
    CLog::Log(LOGWARNING,
              "REGIME osd hold not taken (no cleared buffer or C not writing the knobs), why={} "
              "mode={}",
              why, mode);
}

COsdHoldScope::~COsdHoldScope()
{
  if (!m_route)
    return;
  const GraphicsRegime published = GetRegime();
  const uint8_t actual = RegimeTag(published);
  s_predictChecked.fetch_add(1);
  if (actual != m_predicted)
  {
    s_predictMissed.fetch_add(1);
    GRAPHICSTICK::GTLogF(LOGWARNING,
                         "REGIME predict miss why={} mode={} from={} predicted={} actual={} "
                         "epoch={} held={}",
                         m_why, m_mode, TagName(m_from), TagName(m_predicted), TagName(actual),
                         published.epoch, m_held ? 1 : 0);
  }
  if (!m_held)
    return;
  // C ends the hold once it has read this epoch, so the change is under way before any drawn
  // buffer goes up again
  if (CPresentationCoordinator* coordinator = CPresentationCoordinator::Get())
    coordinator->OsdHoldEnd(published.epoch);
  CLog::Log(LOGINFO, "REGIME osd hold release why={} mode={} published={} held_ms={}", m_why,
            m_mode, TagName(actual), (SteadyNowNs() - m_beginNs) / 1000000);
}

} // namespace DISPLAYSTATE
