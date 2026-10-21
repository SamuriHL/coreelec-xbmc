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
    m_cond.wait(lock, [&] { return m_stop || next() != nullptr; });
    // a stop drains what is queued first: a waiting caller is never left without its job
    std::deque<Job>* lane = next();
    if (!lane)
      break;
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

bool OutputDriveSkip(const std::string& substep)
{
  static const std::vector<std::string> skipped = []
  {
    std::vector<std::string> list;
    if (const auto line = ReadTestSwitch("outputdrive_skip"))
    {
      size_t begin = 0;
      while (begin <= line->size())
      {
        const size_t end = std::min(line->find(',', begin), line->size());
        std::string name = line->substr(begin, end - begin);
        StringUtils::Trim(name);
        if (!name.empty())
          list.push_back(name);
        begin = end + 1;
      }
      CLog::Log(LOGWARNING, "OUTPUTDRIVE skip {}", *line);
    }
    return list;
  }();
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

DriveReply LastDriveReply(uint64_t& seq)
{
  seq = s_replySeq;
  return s_lastReply;
}

CDriveOriginRm::CDriveOriginRm()
{
  ++s_originRm;
}

CDriveOriginRm::~CDriveOriginRm()
{
  --s_originRm;
}

bool DriveOriginRm()
{
  return s_originRm > 0;
}

void NoteDriveResolution(int res)
{
  s_driveResolution = res;
}

int DriveResolution()
{
  return s_driveResolution;
}

} // namespace OUTPUT
