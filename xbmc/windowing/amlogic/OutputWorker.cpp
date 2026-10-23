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
#include <optional>
#include <set>
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
        CLog::Log(LOGINFO, "OUTPUT: {} timed job(s) dropped at the stop (first: {})",
                  m_timed.size(), m_timed.front().job.name);
      m_timed.clear();
      break;
    }
    Job job = std::move(lane->front());
    lane->pop_front();
    ++m_jobsStarted;
    lock.unlock();
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
    lock.lock();
    if (job.result)
    {
      job.result->error = error;
      job.result->done = true;
      m_doneCond.notify_all();
    }
  }
}

namespace
{
// the first line of special://profile/<name>, or nullopt without the file
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

namespace
{
// a "<substep>[,<substep>]" bisect file (outputdrive_skip, hotplugworker_skip), logged by tag
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
} // namespace

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

bool TestHotplugCapsFail()
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
