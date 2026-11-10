/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OutputWorker.h"

#include "ServiceBroker.h"
#include "ThreadRole.h"
#include "filesystem/File.h"
#include "messaging/ApplicationMessenger.h"
#include "platform/linux/SysfsPath.h"
#include "utils/AMLUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <pthread.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace OUTPUT
{
namespace
{
// the nodes that carry link or DV state; the GUI plane's encoding and the picture controls are
// presentation, not link state
bool IsOutputTarget(const std::string& path)
{
  static const char* const prefixes[] = {
      "/sys/module/aml_media/parameters/",
      "/sys/class/amdolby_vision/",
      "/sys/class/amhdmitx/",
      "/sys/class/display/",
  };
  static const char* const excluded[] = {
      "/sys/class/amdolby_vision/graphic_fmt",
      "/sys/module/aml_media/parameters/osd_pq_passthrough",
      "/sys/class/amdolby_vision/debug", // the decoder's layer switches (I3's decoder-side exception)
  };
  for (const char* p : excluded)
    if (path == p)
      return false;
  for (const char* p : prefixes)
    if (path.rfind(p, 0) == 0)
      return true;
  return false;
}

void ObserveSysfsWrite(const std::string& path)
{
  if (IsOutputTarget(path))
    NoteWrite(path);
}
} // namespace

void NoteWrite(const std::string& target)
{
  // each thread reports a target once, and once more after W starts, so a boot write cannot
  // hide a later stray of the same target
  const bool active = COutputWorker::Get().Active();
  thread_local std::set<std::string> reportedHere;
  const std::string key = active ? target : target + " (W not running)";
  if (reportedHere.count(key))
    return;
  reportedHere.insert(key);

  char name[16] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  const bool onW = COutputWorker::Get().OnWorker();
  // with the tid: a name over 15 characters cannot be set, so VideoPlayerVideo shows as VideoPlayer
  CLog::Log(LOGINFO, "OUTPUT write: {} by {}{} tid {}", target, onW ? "W" : name,
            onW ? "" : (active ? " (not W)" : " (not W, W not running)"), gettid());
}

void StartAccounting()
{
  CSysfsPath::SetWriteObserver(&ObserveSysfsWrite);
}

void StopAccounting()
{
  CSysfsPath::SetWriteObserver(nullptr);
}

COutputWorker& COutputWorker::Get()
{
  static COutputWorker worker;
  return worker;
}

namespace
{
constexpr uint32_t WORD_T_MASK = (1u << 24) - 1;
constexpr uint32_t WORD_F_MASK = (1u << 10) - 1;
constexpr uint32_t WORD_E_MASK = (1u << 24) - 1;

const char* WordStateName(COutputWorker::WordState state)
{
  switch (state)
  {
    using enum COutputWorker::WordState;
    case NONE:
      return "NONE";
    case STARTING:
      return "STARTING";
    case LEADING:
      return "LEADING";
    case FREEZING:
      return "FREEZING";
    case LOST:
      return "LOST";
    case CONVERTING:
      return "CONVERTING";
    case PAUSED:
      return "PAUSED";
    case RESET_OWED:
      return "RESET_OWED";
    case RETURNING:
      return "RETURNING";
  }
  return "?";
}

bool WordFrozenState(COutputWorker::WordState state)
{
  using enum COutputWorker::WordState;
  return state == LOST || state == CONVERTING || state == PAUSED || state == RESET_OWED ||
         state == RETURNING;
}

std::string WordOwned(const char* states, uint32_t ownT)
{
  return StringUtils::Format("{}{{T={}}}", states, ownT);
}
} // namespace

uint64_t COutputWorker::PackWord(const SWord& word)
{
  return static_cast<uint64_t>(static_cast<uint8_t>(word.state) & 0xf) |
         (static_cast<uint64_t>(word.sub ? 1 : 0) << 4) |
         (static_cast<uint64_t>(word.t & WORD_T_MASK) << 5) |
         (static_cast<uint64_t>(word.f & WORD_F_MASK) << 29) |
         (static_cast<uint64_t>(word.e & WORD_E_MASK) << 39) |
         (static_cast<uint64_t>(word.spanOpen ? 1 : 0) << 63);
}

COutputWorker::SWord COutputWorker::UnpackWord(uint64_t value)
{
  SWord word;
  word.state = static_cast<WordState>(value & 0xf);
  word.sub = (value >> 4) & 1;
  word.t = static_cast<uint32_t>(value >> 5) & WORD_T_MASK;
  word.f = static_cast<uint32_t>(value >> 29) & WORD_F_MASK;
  word.e = static_cast<uint32_t>(value >> 39) & WORD_E_MASK;
  word.spanOpen = (value >> 63) & 1;
  return word;
}

std::string COutputWorker::WordText(uint64_t value)
{
  const SWord w = UnpackWord(value);
  const char* sub = "";
  if (w.sub)
    sub = w.state == WordState::FREEZING ? " parked" : " held";
  else if (w.state == WordState::FREEZING)
    sub = " claimed";
  return StringUtils::Format("{}{{T={} F={} e={}{}{}}}", WordStateName(w.state), w.t, w.f, w.e,
                             sub, w.spanOpen ? " span" : "");
}

bool COutputWorker::WordApply(const std::function<SWordStep(const SWord&)>& decide,
                              const SWordHead* head)
{
  // design §4.7: CASes are read-and-retry loops (fc20 A1); a lost race re-reads and re-decides
  constexpr int TRIES = 4;
  const std::string headText =
      head ? StringUtils::Format(" lossState={}", head->name ? head->name : "?") : "";
  for (int attempt = 1;; ++attempt)
  {
    const uint64_t old = LoadWord();
    const SWordStep step = decide(UnpackWord(old));
    if (step.kind == SWordStep::Kind::NOTHING)
      return false;
    if (step.kind == SWordStep::Kind::SKIP)
    {
      // a precondition miss, not a race: HEAD's state beside it says whether HEAD moved on too
      const bool moved = head && head->moved;
      CLog::Log(moved ? LOGINFO : LOGWARNING, "WORD skip (row {}): expected {} found {}{} ({})",
                step.row, step.expected, WordText(old), headText,
                moved ? "HEAD moved on" : "unexplained");
      return false;
    }
    const uint64_t desired = PackWord(step.next);
    uint64_t found = old;
    if (m_word.compare_exchange_strong(found, desired, std::memory_order_acq_rel))
    {
      CLog::Log(LOGINFO, "WORD {} -> {} (row {}){}{}{}", WordText(old), WordText(desired),
                step.row, step.note ? " " : "", step.note ? step.note : "",
                attempt > 1 ? StringUtils::Format(" (try {})", attempt) : std::string());
      return true;
    }
    if (attempt == TRIES)
    {
      CLog::Log(LOGWARNING, "WORD CAS failed (row {}): expected {} found {} after {} tries{}",
                step.row, WordText(old), WordText(found), TRIES, headText);
      return false;
    }
    CLog::Log(LOGINFO, "WORD race (row {}) retried: expected {} found {}{}", step.row,
              WordText(old), WordText(found), headText);
  }
}

namespace
{
using Step = COutputWorker::SWordStep;

Step Apply(const COutputWorker::SWord& next, const char* row, const char* note = nullptr)
{
  Step step;
  step.kind = Step::Kind::APPLY;
  step.next = next;
  step.row = row;
  step.note = note;
  return step;
}

Step Skip(const char* row, std::string expected)
{
  Step step;
  step.kind = Step::Kind::SKIP;
  step.row = row;
  step.expected = std::move(expected);
  return step;
}

// a held player's states (rows 15, 17, 19, 20)
bool WordHeldState(COutputWorker::WordState state)
{
  using enum COutputWorker::WordState;
  return state == LOST || state == CONVERTING || state == PAUSED;
}

constexpr const char* ROW_NOT_HELD = "none (HEAD: player not held)";
} // namespace

void COutputWorker::WordLoss(bool walk)
{
  WordApply(
      [walk](const SWord& w)
      {
        SWord n = w;
        n.e = (w.e + 1) & WORD_E_MASK;
        n.spanOpen = true;
        const char* note = walk ? "loss" : "loss inside the open span";
        switch (w.state)
        {
          using enum WordState;
          case NONE:
            if (!w.f)
              return Apply(n, "27", note);
            if (walk)
            {
              // a Lost job's claim; the walk's FREEZE body parks it
              n.state = FREEZING;
              n.sub = false;
              return Apply(n, "11", note);
            }
            return Apply(n, ROW_NOT_HELD, note);
          case RETURNING:
            n.state = FREEZING;
            n.sub = false;
            return Apply(n, "11", note);
          case LOST:
            return Apply(n, w.spanOpen ? "19" : "17", note);
          case CONVERTING:
          case PAUSED:
            return Apply(n, "19", note);
          case FREEZING:
            return Apply(n, w.f ? "11" : "27", "loss while a freeze is claimed");
          case STARTING:
            return Apply(n, "9", note);
          case LEADING:
            n.state = FREEZING;
            n.sub = false;
            return Apply(n, "8", note);
          case RESET_OWED:
            break;
        }
        // nothing in E2-3 stores RESET_OWED
        return Apply(n, "unreachable", note);
      });
}

void COutputWorker::WordLossWalked()
{
  WordApply(
      [](const SWord& w)
      {
        if (w.state != WordState::FREEZING)
          return Step{};
        SWord n = w;
        n.state = WordState::NONE;
        n.sub = false;
        // rows 13 and 14: P stopped during the walk (its stop keeps FREEZING with F 0)
        if (!w.f)
          return Apply(n, w.sub ? "14" : "13", "P stopped meanwhile");
        if (!w.sub)
          return Apply(n, "13", "no player parked the claim");
        return Step{};
      });
}

void COutputWorker::WordCommitted(bool notConnected)
{
  const char* note = notConnected ? "commit, not-committed" : "commit";
  WordApply(
      [note](const SWord& w)
      {
        SWord n = w;
        n.e = (w.e + 1) & WORD_E_MASK;
        if (!w.f)
          return Apply(n, "27", note);
        if (WordHeldState(w.state))
          return Apply(n, w.spanOpen ? "19" : "15", note);
        return Apply(n, ROW_NOT_HELD, note);
      });
}

void COutputWorker::WordSpanClosed()
{
  WordApply(
      [](const SWord& w)
      {
        if (!w.spanOpen)
          return Step{};
        SWord n = w;
        n.spanOpen = false;
        if (!w.f)
          return Apply(n, "27", "span closed");
        if (WordHeldState(w.state))
          return Apply(n, "20", "span closed before the player's return");
        return Apply(n, ROW_NOT_HELD, "span closed");
      });
}

uint32_t COutputWorker::WordOpen(uint32_t ownT, WordState mirror)
{
  uint32_t t = (m_wordT.fetch_add(1) + 1) & WORD_T_MASK;
  if (!t)
    t = (m_wordT.fetch_add(1) + 1) & WORD_T_MASK;
  uint32_t f = (m_wordF.fetch_add(1) + 1) & WORD_F_MASK;
  if (!f)
    f = (m_wordF.fetch_add(1) + 1) & WORD_F_MASK;
  const bool opened = WordApply(
      [=](const SWord& w)
      {
        SWord n = w;
        n.t = t;
        n.f = f;
        n.state = mirror;
        n.sub = false;
        if (w.f && ownT && w.t == ownT)
          return Apply(n, "28+29", "next file on this player");
        if (w.f)
          return Apply(n, "28+29", "the previous player not stopped yet");
        return Apply(n, "29", "open");
      });
  return opened ? t : 0;
}

void COutputWorker::WordFreezeBody(uint32_t ownT, bool newcomer, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        const char* row = newcomer ? "29" : "11+12";
        if (w.t != ownT ||
            !(w.state == WordState::NONE || (w.state == WordState::FREEZING && !w.sub)))
          return Skip(row, WordOwned("NONE or FREEZING claimed", ownT));
        SWord n = w;
        n.state = WordState::FREEZING;
        n.sub = true;
        if (w.state == WordState::FREEZING)
          return Apply(n, "12", "the FREEZE body begins");
        return Apply(n, row,
                     newcomer ? "the newcomer freeze's body"
                              : "a request's walk, claimed and parked at once");
      },
      &head);
}

void COutputWorker::WordFrozen(uint32_t ownT, bool newcomer, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        const char* row = newcomer ? "29" : "14";
        if (w.t != ownT || w.state != WordState::FREEZING || !w.sub)
          return Skip(row, WordOwned("FREEZING parked", ownT));
        SWord n = w;
        n.state = WordState::LOST;
        n.sub = false;
        return Apply(n, row, "after the FREEZE body");
      },
      &head);
}

void COutputWorker::WordConverting(uint32_t ownT, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        if (w.t != ownT || w.state != WordState::LOST)
          return Skip("18", WordOwned("LOST", ownT));
        SWord n = w;
        n.state = WordState::CONVERTING;
        return Apply(n, "18", "conversion begins");
      },
      &head);
}

void COutputWorker::WordPaused(uint32_t ownT, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        if (w.t != ownT || w.state != WordState::CONVERTING)
          return Skip("18", WordOwned("CONVERTING", ownT));
        SWord n = w;
        n.state = WordState::PAUSED;
        return Apply(n, "18", "conversion done");
      },
      &head);
}

void COutputWorker::WordReturning(uint32_t ownT, WordState headFrom, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        // the row comes from HEAD's state after the spin; the word may be one P hook behind it
        const char* row = "20";
        if (headFrom == WordState::CONVERTING)
          row = "21+22";
        else if (headFrom == WordState::LOST && !w.spanOpen)
          row = "16";
        if (w.t != ownT || !WordHeldState(w.state))
          return Skip(row, WordOwned("LOST, CONVERTING or PAUSED", ownT));
        SWord n = w;
        n.state = WordState::RETURNING;
        n.spanOpen = false;
        if (headFrom == WordState::CONVERTING)
          return Apply(n, row, "RETURNING stored over CONVERTING after the 1 s wait");
        if (w.state != headFrom)
          return Apply(n, row, "display reset; the word one P hook behind HEAD");
        return Apply(n, row, "display reset");
      },
      &head);
}

void COutputWorker::WordReturned(uint32_t ownT, const SWordHead& head)
{
  WordApply(
      [=](const SWord& w)
      {
        if (w.t != ownT || w.state != WordState::RETURNING)
          return Skip("23", WordOwned("RETURNING", ownT));
        SWord n = w;
        n.state = WordState::NONE;
        return Apply(n, "23", "the reset's resume");
      },
      &head);
}

void COutputWorker::WordStop(uint32_t ownT)
{
  WordApply(
      [=](const SWord& w)
      {
        if (w.t != ownT || !w.f)
        {
          // a next player's open took the word first (row 28+29 logged there)
          CLog::Log(LOGINFO, "WORD stop T={}: the word is {} (row 28 taken by the next open)",
                    ownT, WordText(PackWord(w)));
          return Step{};
        }
        SWord n = w;
        n.f = 0;
        // a pending claim stays FREEZING (released after the walk, row 13 or 14)
        if (w.state != WordState::FREEZING)
          n.state = w.sub ? WordState::FREEZING : WordState::NONE;
        return Apply(n, "28", WordFrozenState(w.state) ? "stop while frozen" : "stop");
      });
}


// 5.E2-4: W's request lists as a shadow (design §8.10)
namespace
{
const char* LaneName(Lane lane)
{
  switch (lane)
  {
    case Lane::DECODER:
      return "decoder";
    case Lane::RESTORE:
      return "restore";
    case Lane::START:
      return "start";
    case Lane::MIDPLAY:
      return "midplay";
    case Lane::SETTINGS:
      return "settings";
    case Lane::COUNT:
      break;
  }
  return "?";
}

const char* StartPhaseName(COutputWorker::StartPhase phase)
{
  switch (phase)
  {
    case COutputWorker::StartPhase::NONE:
      return "NONE";
    case COutputWorker::StartPhase::STARTING:
      return "STARTING";
    case COutputWorker::StartPhase::LEADING:
      return "LEADING";
  }
  return "?";
}

int64_t MonoNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// a request job's origin from its name and the poster's scope; nullptr: not a request
const char* RequestOrigin(const char* name)
{
  const std::string_view job(name);
  if (job == "vs10")
    return "vs10";
  if (job == "hdr conversion" || job == "gui sink peak" || job == "vsvdb setting")
    return "setting";
  if (job == "mode set")
  {
    // posted on main, where RM's scope is counted
    if (DriveOriginRm())
      return DriveFilmStart() ? "start-decision" : "main-decision";
    if (const char* origin = CurrentRequestOrigin())
      return origin;
    return "main (unscoped)";
  }
  return nullptr;
}
} // namespace

void COutputWorker::TagRequestLocked(Lane lane, Job& job)
{
  if ((lane != Lane::MIDPLAY && lane != Lane::SETTINGS) || !aml_transaction_shadow_on())
    return;
  job.origin = RequestOrigin(job.name);
  if (!job.origin)
    return;
  job.reqF = UnpackWord(LoadWord()).f;
  std::lock_guard listLock(m_listMutex);
  job.reqId = ++m_reqNext;
}

uint64_t COutputWorker::LowestRequestLocked(Lane* lane) const
{
  uint64_t lowest = 0;
  for (const Lane l : {Lane::MIDPLAY, Lane::SETTINGS})
    for (const Job& waiting : m_lanes[static_cast<size_t>(l)])
      if (waiting.reqId && (!lowest || waiting.reqId < lowest))
      {
        lowest = waiting.reqId;
        *lane = l;
      }
  return lowest;
}

COutputWorker::StartPhase COutputWorker::ShadowPhaseNow() const
{
  const auto phase = static_cast<StartPhase>(m_startPhase.load());
  if (phase == StartPhase::LEADING && MonoNs() >= m_leadEndNs.load())
    return StartPhase::NONE;
  return phase;
}

void COutputWorker::ShadowStartPhase(StartPhase phase, int64_t leadEndNs, const char* why, int from)
{
  const int to = static_cast<int>(phase);
  int previous = m_startPhase.load();
  if (from >= 0 && previous != from)
    return;
  // the lead's end before the phase, so a LEADING read never sees an old end
  m_leadEndNs = leadEndNs;
  if (from >= 0)
  {
    if (!m_startPhase.compare_exchange_strong(previous, to))
      return;
  }
  else
    previous = m_startPhase.exchange(to);
  if (previous == to && phase != StartPhase::LEADING)
    return;
  if (phase == StartPhase::LEADING)
    CLog::Log(LOGINFO, "WINDOW shadow {} -> LEADING for {:.0f} ms ({})",
              StartPhaseName(static_cast<StartPhase>(previous)),
              (leadEndNs - MonoNs()) / 1e6, why);
  else
    CLog::Log(LOGINFO, "WINDOW shadow {} -> {} ({})",
              StartPhaseName(static_cast<StartPhase>(previous)), StartPhaseName(phase), why);
}

std::string COutputWorker::ShadowVerdict(const Job& job, const SWord& word, StartPhase phase) const
{
  using enum WordState;
  const std::string_view origin(job.origin);
  // RM's decision for a start the player holds: DECIDE's Start job (lane 3) from E2-8, not a
  // request
  if (origin == "start-decision")
    return "run (the start's own decision: the Start job from E2-8)";
  // a setting job's pending rule (§4.4): at once with no player, else at the next Start job's
  // step 5 or the first job boundary after the stop
  if (origin == "setting")
    return word.f ? "pending (setting: a player is open; HEAD applies it now)" : "run (no player)";
  // the ABORT rule: a request of another file, or of a file since stopped, is dropped
  if (job.reqF && job.reqF != word.f)
    return word.f ? StringUtils::Format("dropped (ABORT: made for F={}, F={} open)", job.reqF,
                                        word.f)
                  : StringUtils::Format("dropped (ABORT: F={} stopped)", job.reqF);
  // a desktop set made with no player, dequeued while a file is open
  if (!job.reqF && word.f)
    return StringUtils::Format("dropped (desktop set, F={} open)", word.f);
  if (word.f && phase != StartPhase::NONE)
    return StringUtils::Format("deferred under T={} ({})", word.t, StartPhaseName(phase));
  const bool frozen = word.state == LOST || word.state == CONVERTING || word.state == PAUSED;
  if (word.state == RETURNING || word.state == RESET_OWED || (frozen && word.spanOpen) ||
      (word.state == NONE && !word.f && word.spanOpen))
    return "held (after-list: the span is open)";
  if (frozen)
    return "run (inside the MidPlay freeze, no span: row 26)";
  return "run";
}

void COutputWorker::ShadowSettleDone(uint64_t request)
{
  const uint64_t value = LoadWord();
  const SWord word = UnpackWord(value);
  const StartPhase phase = ShadowPhaseNow();
  std::lock_guard listLock(m_listMutex);
  CLog::Log(LOGINFO, "SettleDone shadow request={} T={} e={} {} (lists: deferred {}, after {})",
            request, word.t, word.e, StartPhaseName(phase), m_deferred.size(), m_after.size());
}

void COutputWorker::Start()
{
  std::unique_lock lock(m_mutex);
  if (m_running || m_stopping)
    return;
  if (SwitchedOff())
  {
    CLog::Log(LOGINFO, "OUTPUT: worker off (outputworker_off), jobs run on their callers");
    return;
  }
  m_stop = false;
  OutputDriveSkip("");
  {
    XFILE::CFile file;
    std::vector<uint8_t> buf;
    if (XFILE::CFile::Exists("special://profile/outputworker_stall") &&
        file.LoadFile("special://profile/outputworker_stall", buf) > 0)
    {
      std::string line(buf.begin(), buf.end());
      line = line.substr(0, line.find('\n'));
      const auto space = line.rfind(' ');
      if (space != std::string::npos)
      {
        m_stallJob = line.substr(0, space);
        m_stallMs = std::atoi(line.c_str() + space + 1);
        CLog::Log(LOGWARNING, "OUTPUT: test stall of {} ms before every \"{}\" job", m_stallMs,
                  m_stallJob);
      }
    }
  }
  m_thread = std::thread(&COutputWorker::Process, this);
  m_workerId = m_thread.get_id();
  m_running = true;
  CLog::Log(LOGINFO, "OUTPUT: worker started");
}

void COutputWorker::Stop()
{
  {
    std::unique_lock lock(m_mutex);
    // a job cannot stop the thread it runs on
    if (!m_running || m_stopping || std::this_thread::get_id() == m_workerId)
      return;
    m_stop = true;
    m_stopping = true;
  }
  m_cond.notify_all();
  if (m_thread.joinable())
    m_thread.join();
  std::unique_lock lock(m_mutex);
  m_running = false;
  m_stopping = false;
  m_workerId = {};
}

bool COutputWorker::SwitchedOff()
{
  static const bool off = XFILE::CFile::Exists("special://profile/outputworker_off");
  return off;
}

bool COutputWorker::Active() const
{
  std::unique_lock lock(m_mutex);
  return m_running && !m_stop;
}

bool COutputWorker::OnWorker() const
{
  std::unique_lock lock(m_mutex);
  return m_running && std::this_thread::get_id() == m_workerId;
}

void COutputWorker::Run(Lane lane,
                        const char* name,
                        std::function<void()> fn,
                        uint64_t generation,
                        std::chrono::milliseconds errorAfter)
{
  // Lock rule (5.B2-5b review S4): a caller of this synchronous wait must hold none of RM's
  // m_statelock / m_presentlock, the player clock's lock or the player's message queue locks:
  // W's mode set job takes them (the lost walk: OnLostDisplay -> GetResumePts, the clock, the
  // players' queues). No cheap owner check exists for those CCriticalSections (their counts are
  // not per thread), so the rule is kept by the callers, listed in the plan's lock table.
  std::unique_lock lock(m_mutex);
  if (!m_running || m_stop || std::this_thread::get_id() == m_workerId)
  {
    lock.unlock();
    fn();
    return;
  }
  auto result = std::make_shared<Result>();
  const uint64_t id = m_nextId++;
  m_lanes[static_cast<size_t>(lane)].push_back({name, std::move(fn), generation, id, result});
  TagRequestLocked(lane, m_lanes[static_cast<size_t>(lane)].back());
  m_cond.notify_all();

  const auto start = std::chrono::steady_clock::now();
  if (!m_doneCond.wait_for(lock, errorAfter, [&result] { return result->done; }))
  {
    CLog::Log(LOGERROR, "OUTPUT: {} job {} still waiting after {} ms", name, id,
              errorAfter.count());
    m_doneCond.wait(lock, [&result] { return result->done; });
    CLog::Log(LOGERROR, "OUTPUT: {} job {} done after {} ms", name, id,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
  }
  if (result->error)
    std::rethrow_exception(result->error);
}

void COutputWorker::Post(Lane lane, const char* name, std::function<void()> fn, uint64_t generation)
{
  std::unique_lock lock(m_mutex);
  if (!m_running || m_stop)
  {
    lock.unlock();
    fn();
    return;
  }
  m_lanes[static_cast<size_t>(lane)].push_back({name, std::move(fn), generation, m_nextId++, {}});
  TagRequestLocked(lane, m_lanes[static_cast<size_t>(lane)].back());
  m_cond.notify_all();
}

bool COutputWorker::PostIfRunning(Lane lane,
                                  const char* name,
                                  std::function<void()> fn,
                                  uint64_t generation)
{
  std::unique_lock lock(m_mutex);
  if (!m_running || m_stop)
    return false;
  m_lanes[static_cast<size_t>(lane)].push_back({name, std::move(fn), generation, m_nextId++, {}});
  TagRequestLocked(lane, m_lanes[static_cast<size_t>(lane)].back());
  m_cond.notify_all();
  return true;
}

bool COutputWorker::PostAfter(Lane lane,
                              const char* name,
                              std::chrono::milliseconds after,
                              std::function<void()> fn)
{
  std::unique_lock lock(m_mutex);
  if (!m_running || m_stop)
    return false;
  m_timed.push_back({std::chrono::steady_clock::now() + after, lane,
                     {name, std::move(fn), 0, m_nextId++, {}}});
  TagRequestLocked(lane, m_timed.back().job);
  m_cond.notify_all();
  return true;
}

std::optional<std::chrono::steady_clock::time_point> COutputWorker::ReleaseDueLocked()
{
  std::optional<std::chrono::steady_clock::time_point> earliest;
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_timed.begin(); it != m_timed.end();)
  {
    if (it->due <= now)
    {
      m_lanes[static_cast<size_t>(it->lane)].push_back(std::move(it->job));
      it = m_timed.erase(it);
      continue;
    }
    if (!earliest || it->due < *earliest)
      earliest = it->due;
    ++it;
  }
  return earliest;
}

void COutputWorker::Process()
{
  pthread_setname_np(pthread_self(), "OutputWorker");
  SetThreadRole("W");
  std::unique_lock lock(m_mutex);
  while (true)
  {
    auto next = [this]() -> std::deque<Job>* {
      for (auto& lane : m_lanes)
        if (!lane.empty())
          return &lane;
      return nullptr;
    };
    // timed jobs join their lane when due; a stop drops the ones not yet due
    const auto timedDue = ReleaseDueLocked();
    if (!m_stop && !next())
    {
      if (timedDue)
        m_cond.wait_until(lock, *timedDue);
      else
        m_cond.wait(lock);
      continue;
    }
    // a stop drains what is queued first: a waiting caller is never left without its job
    std::deque<Job>* lane = next();
    if (!lane)
    {
      if (!m_timed.empty())
      {
        std::vector<std::string> names;
        for (const TimedJob& timed : m_timed)
          names.emplace_back(timed.job.name);
        CLog::Log(LOGINFO, "OUTPUT: {} timed job(s) dropped at the stop ({})", m_timed.size(),
                  StringUtils::Join(names, ", "));
      }
      m_timed.clear();
      break;
    }
    Job job = std::move(lane->front());
    lane->pop_front();
    ++m_jobsStarted;
    // 5.E2-4 (shadow): the lowest request id still waiting, read with the dequeue
    const bool shadow = aml_transaction_shadow_on();
    Lane lowestLane = Lane::MIDPLAY;
    const uint64_t lowest = shadow && job.reqId ? LowestRequestLocked(&lowestLane) : 0;
    const Lane jobLane = static_cast<Lane>(lane - m_lanes.data());
    lock.unlock();
    uint32_t eBefore = 0;
    if (shadow)
    {
      const uint64_t value = LoadWord();
      const SWord word = UnpackWord(value);
      eBefore = word.e;
      {
        // W's busy mark, under the list lock (design §8.10)
        std::lock_guard listLock(m_listMutex);
        m_busy = true;
        m_busyT = word.t;
      }
      if (job.reqId)
      {
        if (lowest && lowest < job.reqId)
          CLog::Log(LOGWARNING, "ORDER shadow: W took request {} ({}, {}) while request {} ({}) waits",
                    job.reqId, LaneName(jobLane), job.origin, lowest, LaneName(lowestLane));
        const StartPhase phase = ShadowPhaseNow();
        CLog::Log(LOGINFO, "DEFER shadow {} id={} F={} T={} {} word {} -> {}", job.origin,
                  job.reqId, job.reqF, word.t, StartPhaseName(phase), WordText(value),
                  ShadowVerdict(job, word, phase));
      }
    }
    if (m_stallMs > 0 && m_stallJob == job.name)
      std::this_thread::sleep_for(std::chrono::milliseconds(m_stallMs));
    std::exception_ptr error;
    try
    {
      job.fn();
    }
    catch (...)
    {
      error = std::current_exception();
      if (!job.result)
        CLog::Log(LOGERROR, "OUTPUT: posted {} job {} threw", job.name, job.id);
    }
    if (shadow)
    {
      // JobEnded{T, e}: a request that made no e+1 while the start window is open, in one step
      // with the busy mark's clear
      const SWord word = UnpackWord(LoadWord());
      const StartPhase phase = ShadowPhaseNow();
      std::lock_guard listLock(m_listMutex);
      if (job.reqId && word.e == eBefore && phase != StartPhase::NONE)
        CLog::Log(LOGINFO, "JobEnded shadow T={} e={} request {} ({}) {}: no commit", word.t,
                  word.e, job.reqId, job.origin, StartPhaseName(phase));
      m_busy = false;
      m_busyT = 0;
    }
    lock.lock();
    if (job.result)
    {
      job.result->error = error;
      job.result->done = true;
      m_doneCond.notify_all();
    }
  }
}

std::optional<std::string> ReadTestSwitch(const char* name)
{
  const std::string path = std::string("special://profile/") + name;
  if (!XFILE::CFile::Exists(path))
    return std::nullopt;
  XFILE::CFile file;
  std::vector<uint8_t> buf;
  file.LoadFile(path, buf);
  std::string line(buf.begin(), buf.end());
  return line.substr(0, line.find('\n'));
}

namespace
{
std::atomic_bool s_demuxBlockArmed{false};
} // namespace

void TestStopAt(const char* step)
{
  static const auto stop = []
  {
    std::pair<std::string, int> result{"", 0};
    if (const auto line = ReadTestSwitch("outputdrive_stopat"))
    {
      const auto space = line->find(' ');
      result = {line->substr(0, space),
                space == std::string::npos ? 0 : std::atoi(line->c_str() + space + 1)};
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test stop at step \"{}\", then {} ms there",
                result.first, result.second);
    }
    return result;
  }();
  if (stop.first.empty() || stop.first != step)
    return;
  CLog::Log(LOGWARNING, "OUTPUTDRIVE: test stop posted at step {}", step);
  if (auto messenger = CServiceBroker::GetAppMessenger())
    messenger->PostMsg(TMSG_MEDIA_STOP);
  if (stop.second > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(stop.second));
}

bool TestCommitFail()
{
  static std::atomic_int remaining{[]
  {
    const auto line = ReadTestSwitch("outputdrive_commitfail");
    if (!line)
      return 0;
    const int n = std::max(1, std::atoi(line->c_str()));
    CLog::Log(LOGWARNING, "OUTPUTDRIVE: test failure after the commit of mode set {}", n);
    return n;
  }()};
  if (remaining.load() <= 0 || --remaining != 0)
    return false;
  CLog::Log(LOGWARNING, "OUTPUTDRIVE: test failure after the commit");
  return true;
}

namespace
{
int DemuxBlockMs()
{
  static const int ms = []
  {
    const auto line = ReadTestSwitch("outputdrive_demuxblock");
    const int value = line ? std::atoi(line->c_str()) : 0;
    if (value > 0)
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test demux block of {} ms after each lost walk", value);
    return value;
  }();
  return ms;
}
} // namespace

void ArmDemuxBlock()
{
  if (DemuxBlockMs() > 0)
    s_demuxBlockArmed = true;
}

void DisarmDemuxBlock()
{
  s_demuxBlockArmed = false;
}

void TestDemuxBlock()
{
  if (!s_demuxBlockArmed.load(std::memory_order_relaxed) || !s_demuxBlockArmed.exchange(false))
    return;
  CLog::Log(LOGWARNING, "OUTPUTDRIVE: test demux block {} ms", DemuxBlockMs());
  std::this_thread::sleep_for(std::chrono::milliseconds(DemuxBlockMs()));
}

namespace
{
std::pair<int, int> CStall()
{
  static const std::pair<int, int> stall = []
  {
    const auto line = ReadTestSwitch("outputdrive_cstall");
    const int value = line ? std::atoi(line->c_str()) : 0;
    const auto space = line ? line->find(' ') : std::string::npos;
    const int nth = space == std::string::npos ? 0 : std::max(0, std::atoi(line->c_str() + space));
    if (value > 0 && nth > 0)
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test park ack withheld {} ms at mode set {}", value, nth);
    else if (value > 0)
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test park ack withheld {} ms at every mode set", value);
    return std::pair<int, int>{value, nth};
  }();
  return stall;
}
} // namespace

int TestCStallMs()
{
  return CStall().first;
}

int TestCStallNth()
{
  return CStall().second;
}

bool TestOrderToday()
{
  static const bool today = []
  {
    auto line = ReadTestSwitch("outputdrive_order");
    if (!line)
      return false;
    StringUtils::Trim(*line);
    const bool result = *line == "today";
    if (result)
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test order today: the lost walk after the planes off");
    else
      CLog::Log(LOGWARNING, "OUTPUTDRIVE: test order \"{}\" not known, ignored", *line);
    return result;
  }();
  return today;
}

std::vector<std::string> ReadSkipList(const char* name, const char* tag)
{
  std::vector<std::string> list;
  if (const auto line = ReadTestSwitch(name))
  {
    size_t begin = 0;
    while (begin <= line->size())
    {
      const size_t end = std::min(line->find(',', begin), line->size());
      std::string entry = line->substr(begin, end - begin);
      StringUtils::Trim(entry);
      if (!entry.empty())
        list.push_back(entry);
      begin = end + 1;
    }
    CLog::Log(LOGWARNING, "{} skip {}", tag, *line);
  }
  return list;
}

bool OutputDriveSkip(const std::string& substep)
{
  static const std::vector<std::string> skipped = ReadSkipList("outputdrive_skip", "OUTPUTDRIVE");
  return !substep.empty() && std::find(skipped.begin(), skipped.end(), substep) != skipped.end();
}

namespace
{
std::atomic_bool s_coordinatorStarted{false};
std::atomic_bool s_coordinatorNoted{false};
} // namespace

void NoteCoordinatorStarted(bool started)
{
  s_coordinatorStarted = started;
  s_coordinatorNoted = true;
}

bool OutputDriveOn()
{
  static const bool on = []
  {
    aml_displaystate_init();
    const bool displayState = aml_displaystate_on();
    // a caller before GLES init latches off for good
    if (!s_coordinatorNoted)
      CLog::Log(LOGERROR, "OUTPUTDRIVE: switch read before the coordinator was noted");
    const bool coordinator = s_coordinatorStarted.load();
    const bool result = displayState && aml_presentation_validated() && coordinator &&
                        !XFILE::CFile::Exists("special://profile/outputdrive_off");
    CLog::Log(LOGINFO, "OUTPUTDRIVE {} (display state {}, coordinator {})", result ? "on" : "off",
              displayState ? "on" : "off", coordinator ? "started" : "not started");
    return result;
  }();
  return on;
}

namespace
{
std::atomic_bool s_modeSetsOnW{false};
// main only
DriveReply s_lastReply{DriveReply::NONE};
uint64_t s_replySeq{0};
int s_originRm{0};
int s_originStartHeld{0};
int s_driveResolution{-1};
} // namespace

void NoteModeSetsOnW(bool on)
{
  s_modeSetsOnW = on;
}

bool ModeSetsOnW()
{
  return s_modeSetsOnW.load(std::memory_order_relaxed);
}

bool SettleOnW()
{
  if (!ModeSetsOnW())
    return false;
  static const bool on = []
  {
    const bool result = !OutputDriveSkip("B2-6");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-6: SETTLE on W's clock, the reset ledger on W");
    return result;
  }();
  return on;
}

bool SettleGatesOnW()
{
  // HotplugOnW is latched at W's start, vconfigure at its own latch: not cached here
  if (!SettleOnW() || !HotplugOnW() || !aml_vconfigure_on() || aml_vconfigure_skip("V2-10"))
    return false;
  static std::once_flag logged;
  std::call_once(logged,
                 []
                 {
                   CLog::Log(LOGINFO, "VCONFIGURE V2-10: SETTLE on W's own facts, armed at "
                                      "every delay");
                 });
  return true;
}

bool LostFirstOn()
{
  return ModeSetsOnW() && !OutputDriveSkip("B2-5c") && !TestOrderToday();
}

bool PlaneOffOnC()
{
  if (!ModeSetsOnW())
    return false;
  static const bool on = []
  {
    const bool result = !OutputDriveSkip("B2-7");
    if (result)
      CLog::Log(LOGINFO, "OUTPUTDRIVE B2-7: C runs the plane-off commits; no hold outside W's "
                         "park");
    return result;
  }();
  return on;
}

const char* DriveReplyName(DriveReply reply)
{
  switch (reply)
  {
    case DriveReply::DONE:
      return "done";
    case DriveReply::DONE_NO_CHANGE:
      return "done(noChange)";
    case DriveReply::NOT_COMMITTED:
      return "not-committed";
    case DriveReply::FAILED:
      return "failed";
    case DriveReply::FAILED_PARK:
      return "failed(park)";
    case DriveReply::GAVE_UP:
      return "gave-up";
    case DriveReply::HELD:
      return "held";
    case DriveReply::DEFERRED:
      return "deferred";
    case DriveReply::DROPPED:
      return "dropped";
    default:
      return "none";
  }
}

void NoteDriveReply(DriveReply reply)
{
  s_lastReply = reply;
  ++s_replySeq;
}

uint64_t NextDriveId()
{
  // one id space for every poster (O2): main only until B3-7b, so today's request numbers
  static std::atomic<uint64_t> s_driveIds{0};
  return ++s_driveIds;
}

DriveReply LastDriveReply(uint64_t& seq)
{
  seq = s_replySeq;
  return s_lastReply;
}

CDriveOriginRm::CDriveOriginRm(bool startHeld) : m_startHeld(startHeld)
{
  ++s_originRm;
  if (m_startHeld)
    ++s_originStartHeld;
}

CDriveOriginRm::~CDriveOriginRm()
{
  --s_originRm;
  if (m_startHeld)
    --s_originStartHeld;
}

bool DriveOriginRm()
{
  return s_originRm > 0;
}

namespace
{
thread_local const char* t_requestOrigin = nullptr;
constexpr const char* REQUEST_ORIGINS[] = {"user-pick", "window-rule", "test", "recovery", "boot"};
} // namespace

CRequestOrigin::CRequestOrigin(const char* origin) : m_previous(t_requestOrigin)
{
  t_requestOrigin = origin;
}

CRequestOrigin::~CRequestOrigin()
{
  t_requestOrigin = m_previous;
}

const char* CurrentRequestOrigin()
{
  return t_requestOrigin;
}

const char* InternRequestOrigin(const std::string& origin)
{
  for (const char* known : REQUEST_ORIGINS)
    if (origin == known)
      return known;
  return "tmsg";
}

bool DriveFilmStart()
{
  return s_originStartHeld > 0;
}

void NoteDriveResolution(int res)
{
  s_driveResolution = res;
}

int DriveResolution()
{
  return s_driveResolution;
}

namespace
{
std::atomic_bool s_hotplugWorker{false};
std::atomic_bool s_hpdNoRestore{false};
} // namespace

void LatchHotplugWorker()
{
  static std::atomic_bool latched{false};
  if (latched.exchange(true))
    return;
  const char* reason = "";
  bool on = false;
  if (COutputWorker::SwitchedOff())
    reason = "outputworker_off";
  else if (!ModeSetsOnW())
    reason = "it needs outputdrive and W's mode sets";
  else if (XFILE::CFile::Exists("special://profile/hotplugworker_off"))
    reason = "hotplugworker_off";
  else
    on = true;
  // primed here so a bisect file is logged at W's start, as OutputDriveSkip's
  HotplugWorkerSkip("");
  const bool restorePresent = aml_hpd_restore_present();
  const std::optional<bool> restore = aml_hpd_restore();
  const bool cb = aml_hpd_cb_present() && !(on && TestHotplugNoHpdCb());
  bool noRestore = false;
  if (on)
  {
    if (XFILE::CFile::Exists("special://profile/hpdnorestore_off"))
      reason = "hpdnorestore_off";
    else if (TestHotplugNoHpdCb())
      // the stand-in for a kernel without 5.KH's parameters: today's restore, no N
      reason = "hpdnorestore: nohpdcb (no parameter)";
    else if (!restorePresent)
      reason = "hpdnorestore: no parameter";
    else if (!restore)
      reason = "hpdnorestore: parameter unreadable";
    else
    {
      reason = "hpdnorestore";
      noRestore = true;
    }
  }
  // W already runs: hotplugworker is stored before hpdnorestore (release, acquire loads), so
  // a reader that sees hpdnorestore also sees hotplugworker
  s_hotplugWorker.store(on, std::memory_order_release);
  s_hpdNoRestore.store(noRestore, std::memory_order_release);
  CLog::Log(LOGINFO, "HOTPLUGWORKER {} ({}) hpdRestore {} hpdCb {}", on ? "on" : "off", reason,
            restore ? (*restore ? "Y" : "N") : (restorePresent ? "unreadable" : "absent"),
            cb ? "present" : "absent");
}

bool HotplugWorkerOn()
{
  return s_hotplugWorker.load(std::memory_order_acquire);
}

bool HpdNoRestoreOn()
{
  return s_hpdNoRestore.load(std::memory_order_acquire);
}

bool HotplugWorkerSkip(const std::string& substep)
{
  static const std::vector<std::string> skipped =
      ReadSkipList("hotplugworker_skip", "HOTPLUGWORKER");
  return !substep.empty() && std::find(skipped.begin(), skipped.end(), substep) != skipped.end();
}

bool DetectShadowOn()
{
  if (!HotplugWorkerOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-6");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-6: W shadows the hotplug detection (log only)");
    return result;
  }();
  return on;
}

bool OrderingOn()
{
  if (!HotplugWorkerOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-7a");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-7a: resets re-check the lost generation under the "
                         "resource lock; the FD thread posts only to a running W");
    return result;
  }();
  return on;
}

bool HotplugOnW()
{
  if (!OrderingOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-7b");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-7b: W runs the hotplug (Lost, re-init, caps, Restore); "
                         "main applies its replies");
    return result;
  }();
  return on;
}

bool RestoreRow2On()
{
  if (!HotplugOnW())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-8");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-8: W's Restore takes row 2's target (the video's "
                         "mode, else the GUI resolution, desktop as desktopMode, else PREFERRED)");
    return result;
  }();
  return on;
}

bool DarkRoutingOn()
{
  if (!HotplugOnW())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-9a");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-9a: W's Restore routes an unrouted connector (the "
                         "dark branch, no hotplug flags)");
    return result;
  }();
  return on;
}

namespace
{
std::atomic_bool s_refusalConfirm{false};
} // namespace

bool RefusalConfirmOn()
{
  if (!LinkRequestOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-10a");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-10a: C latches a graphics refusal only when its "
                         "retries after GUI-only commits are refused three times");
    s_refusalConfirm.store(result, std::memory_order_release);
    return result;
  }();
  return on;
}

bool RefusalConfirmLive()
{
  return s_refusalConfirm.load(std::memory_order_acquire);
}

bool DesktopSlotOn()
{
  if (!RefusalConfirmOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-9c");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-9c: a placeholder RES_DESKTOP takes desktopMode at a "
                         "reconnect with no live mode");
    return result;
  }();
  return on;
}

namespace
{
std::mutex s_hpdWriteMutex;
bool s_hpdNWritten{false};
bool s_hpdExit{false};
bool s_hpdRevertLogged{false};

const char* HpdRestoreText(std::optional<bool> value)
{
  return value ? (*value ? "Y" : "N") : "unreadable";
}
} // namespace

bool HpdNoRestoreWriteOn()
{
  if (!DesktopSlotOn() || !HpdNoRestoreOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-10b") && !HotplugWorkerSkip("B3-10c");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-10c: W writes hpd_restore N at its start (a plug-in "
                         "stays dark for W's Restore), Y at exit");
    return result;
  }();
  return on;
}

bool VideoRecordOnActOn()
{
  if (!HpdNoRestoreWriteOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-10d");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-10d: W records the video's mode only in a job that "
                         "acts on its edge");
    return result;
  }();
  return on;
}

void WriteHpdNoRestore()
{
  std::unique_lock lock(s_hpdWriteMutex);
  if (s_hpdExit)
    return;
  aml_set_hpd_restore(false);
  const std::optional<bool> after = aml_hpd_restore();
  if (after && !*after)
  {
    s_hpdNWritten = true;
    CLog::Log(LOGINFO, "hpdRestore N (W's start)");
  }
  else
    CLog::Log(LOGERROR, "hpdRestore not N after W's write (reads {})", HpdRestoreText(after));
}

void CheckHpdNoRestore()
{
  std::unique_lock lock(s_hpdWriteMutex);
  if (!s_hpdNWritten || s_hpdExit || s_hpdRevertLogged)
    return;
  const std::optional<bool> value = aml_hpd_restore();
  if (value && !*value)
    return;
  s_hpdRevertLogged = true;
  CLog::Log(LOGERROR, "hpdRestore reverted under W (reads {})", HpdRestoreText(value));
}

void ExitHpdNoRestore()
{
  std::unique_lock lock(s_hpdWriteMutex);
  s_hpdExit = true;
  if (!s_hpdNWritten)
    return;
  aml_set_hpd_restore(true);
  const std::optional<bool> after = aml_hpd_restore();
  if (after && *after)
    CLog::Log(LOGINFO, "hpdRestore Y (exit)");
  else
    CLog::Log(LOGERROR, "hpdRestore not Y after the exit's write (reads {})",
              HpdRestoreText(after));
}

bool LinkRequestOn()
{
  if (!DarkRoutingOn())
    return false;
  static const bool on = []
  {
    const bool result = !HotplugWorkerSkip("B3-9b");
    if (result)
      CLog::Log(LOGINFO, "HOTPLUGWORKER B3-9b: W's Restore makes one re-train (no UPDATE after "
                         "its mode commit)");
    return result;
  }();
  return on;
}

namespace
{
std::atomic_bool s_hotplugJobsOnW{false};
} // namespace

void NoteHotplugJobsOnW()
{
  s_hotplugJobsOnW = true;
}

bool HotplugJobsOnW()
{
  return s_hotplugJobsOnW.load();
}

namespace
{
// the first number of a hotplugworker_* file; fallback without the file
int HotplugTestValue(const char* name, int fallback)
{
  const auto line = ReadTestSwitch(name);
  return line ? std::atoi(line->c_str()) : fallback;
}
} // namespace

int TestHotplugHoldMs()
{
  static const int ms = []
  {
    const int value = std::max(0, HotplugTestValue("hotplugworker_hold", 0));
    if (value > 0)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test hold of {} ms before each {} job's HPD read",
                value, HotplugOnW() ? "hotplug" : "shadow");
    return value;
  }();
  return ms;
}

int TestGraphicsRefuseAt()
{
  static const int n = []
  {
    const int value = std::max(0, HotplugTestValue("hotplugworker_gfxrefuse", 0));
    if (value > 0)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test graphics refusal from graphics submit {}", value);
    return value;
  }();
  return n;
}

bool TestHotplugHoldHpd()
{
  static const bool on = []
  {
    const bool result = XFILE::CFile::Exists("special://profile/hotplugworker_holdhpd");
    if (result)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test hold at a plug-out until hpd_state reads 1");
    return result;
  }();
  return on;
}

int TestHotplugRetryMs()
{
  static const int ms = []
  {
    const int value = std::max(0, HotplugTestValue("hotplugworker_retry", 0));
    if (value > 0)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test retry timer's first period {} ms", value);
    return value;
  }();
  return ms;
}

namespace
{
std::atomic_int& CapsFailRemaining()
{
  static std::atomic_int remaining{[]
  {
    const auto line = ReadTestSwitch("hotplugworker_capsfail");
    if (!line)
      return 0;
    const int n = std::max(1, std::atoi(line->c_str()));
    CLog::Log(LOGWARNING, "HOTPLUGWORKER: test one-mode caps at reconnect {}", n);
    return n;
  }()};
  return remaining;
}
} // namespace

void PrimeTestHotplugCapsFail()
{
  CapsFailRemaining();
}

bool TestHotplugCapsFail()
{
  std::atomic_int& remaining = CapsFailRemaining();
  if (remaining.load() <= 0 || --remaining != 0)
    return false;
  CLog::Log(LOGWARNING, "HOTPLUGWORKER: test one-mode caps");
  return true;
}

std::pair<int, int> TestHotplugReinit()
{
  static const std::pair<int, int> reinit = []
  {
    std::pair<int, int> result{0, 0};
    if (const auto line = ReadTestSwitch("hotplugworker_reinit"))
    {
      const auto space = line->find(' ');
      const int period = std::atoi(line->c_str());
      const int seconds = space == std::string::npos ? 0 : std::atoi(line->c_str() + space + 1);
      if (period > 0 && seconds > 0)
      {
        result = {period, seconds};
        CLog::Log(LOGWARNING, "HOTPLUGWORKER: test re-init on W every {} ms for {} s", period,
                  seconds);
      }
      else
        CLog::Log(LOGWARNING, "HOTPLUGWORKER: test re-init \"{}\" not understood, ignored", *line);
    }
    return result;
  }();
  return reinit;
}

bool TestHotplugNoHpdCb()
{
  static const bool on = []
  {
    const bool result = XFILE::CFile::Exists("special://profile/hotplugworker_nohpdcb");
    if (result)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test hpd_cb_state/hpd_cb_seq taken as absent");
    return result;
  }();
  return on;
}

int TestHotplugSettleGapMs()
{
  static const int ms = []
  {
    int value = std::max(0, HotplugTestValue("hotplugworker_settlegap", 0));
    // every lane, and main's synchronous Runs, wait out the gap: a long one would turn a set
    // posted meanwhile into the 5 s ERROR or the 15 s give-up, a different test
    if (value > 1000)
    {
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test gap {} ms clamped to 1000 ms", value);
      value = 1000;
    }
    if (value > 0)
      CLog::Log(LOGWARNING,
                "HOTPLUGWORKER: test gap of {} ms between SETTLE's reset test and its lock",
                value);
    return value;
  }();
  return ms;
}

int TestHandlerStallMs()
{
  static const int ms = []
  {
    const int value = std::max(0, HotplugTestValue("vconfigure_handlerstall", 0));
    if (value > 0)
      CLog::Log(LOGWARNING, "VCONFIGURE test: main sleeps {} ms before each set's handler", value);
    return value;
  }();
  return ms;
}

int TestHotplugRaceGapMs()
{
  static const int ms = []
  {
    const int value = std::max(0, HotplugTestValue("hotplugworker_racegap", 0));
    if (value > 0)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test gap of {} ms before W reads the receipts",
                value);
    return value;
  }();
  return ms;
}

namespace
{
struct SApplyThrow
{
  int n = 0;
  int times = 0;
  const char* at = "after table";
};

const SApplyThrow& ApplyThrow()
{
  static const SApplyThrow config = []
  {
    SApplyThrow result;
    if (const auto line = ReadTestSwitch("hotplugworker_applythrow"))
    {
      const std::vector<std::string> words = StringUtils::Split(*line, ' ');
      result.n = std::max(1, std::atoi(line->c_str()));
      result.times = words.size() > 1 ? std::max(1, std::atoi(words[1].c_str())) : 1;
      if (words.size() > 2 && words[2] == "table")
        result.at = "table";
      else if (words.size() > 2 && words[2] == "adopt")
        result.at = "adopt";
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test throw in reply apply {} ({} times, {})", result.n,
                result.times, result.at);
    }
    return result;
  }();
  return config;
}
} // namespace

const char* TestHotplugApplyThrowAt()
{
  return ApplyThrow().at;
}

bool TestHotplugApplyThrow()
{
  const std::pair<int, int> range{ApplyThrow().n, ApplyThrow().times};
  // main only
  static int applies = 0;
  if (range.first <= 0)
    return false;
  ++applies;
  if (applies < range.first || applies >= range.first + range.second)
    return false;
  CLog::Log(LOGWARNING, "HOTPLUGWORKER: test throw in reply apply {}", applies);
  return true;
}

int TestHotplugSetAfterMs()
{
  static const int ms = []
  {
    const int value = HotplugTestValue("hotplugworker_setafter", -1);
    if (value >= 0)
      CLog::Log(LOGWARNING, "HOTPLUGWORKER: test settings resolution change {} ms after a "
                            "plug-in's receipt",
                value);
    return value;
  }();
  return ms;
}

} // namespace OUTPUT
