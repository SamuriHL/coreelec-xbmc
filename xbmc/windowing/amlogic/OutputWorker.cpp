/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "OutputWorker.h"

#include "filesystem/File.h"
#include "platform/linux/SysfsPath.h"
#include "utils/log.h"

#include <pthread.h>
#include <set>
#include <utility>

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
  // each thread remembers what it has reported, so a repeat costs no lock
  thread_local std::set<std::string> reportedHere;
  if (reportedHere.count(target))
    return;
  reportedHere.insert(target);

  char name[16] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  const bool onW = COutputWorker::Get().OnWorker();
  CLog::Log(LOGINFO, "OUTPUT write: {} by {}{}", target, onW ? "W" : name, onW ? "" : " (not W)");
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
  if (XFILE::CFile::Exists("special://profile/outputworker_off"))
  {
    CLog::Log(LOGINFO, "OUTPUT: worker off (outputworker_off), jobs run on their callers");
    return;
  }
  m_stop = false;
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
    lock.unlock();
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

} // namespace OUTPUT
