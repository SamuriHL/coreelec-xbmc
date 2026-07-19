/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDMessageQueue.h"

#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/SystemClock.h"
#include "utils/log.h"

#include <algorithm>
#include <limits>
#include <math.h>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

CDVDMessageQueue::CDVDMessageQueue(const std::string &owner) : m_hEvent(true), m_owner(owner)
{
  m_iDataSize     = 0;
  m_bInitialized = false;

  m_TimeBack = DVD_NOPTS_VALUE;
  m_TimeFront = DVD_NOPTS_VALUE;
  m_TimeSize = 1.0 / 4.0; /* 4 seconds */
  m_iMaxDataSize = 0;
}

CDVDMessageQueue::~CDVDMessageQueue()
{
  // remove all remaining messages
  Flush(CDVDMsg::NONE);
}

void CDVDMessageQueue::Init()
{
  m_iDataSize = 0;
  m_bAbortRequest = false;
  m_bInitialized = true;
  m_TimeBack = DVD_NOPTS_VALUE;
  m_TimeFront = DVD_NOPTS_VALUE;
  m_drain = false;
}

void CDVDMessageQueue::Flush(CDVDMsg::Message type)
{
  std::unique_lock lock(m_section);

  m_messages.remove_if([type](const DVDMessageListItem &item){
    return type == CDVDMsg::NONE || item.message->IsType(type);
  });

  m_prioMessages.remove_if([type](const DVDMessageListItem &item){
    return type == CDVDMsg::NONE || item.message->IsType(type);
  });

  if (type == CDVDMsg::DEMUXER_PACKET ||  type == CDVDMsg::NONE)
  {
    m_iDataSize = 0;
    m_TimeBack = DVD_NOPTS_VALUE;
    m_TimeFront = DVD_NOPTS_VALUE;
  }
}

void CDVDMessageQueue::Abort()
{
  std::unique_lock lock(m_section);

  m_bAbortRequest = true;

  // inform waiter for abort action
  m_hEvent.Set();
}

void CDVDMessageQueue::End()
{
  std::unique_lock lock(m_section);

  Flush(CDVDMsg::NONE);

  m_bInitialized = false;
  m_iDataSize = 0;
  m_bAbortRequest = false;
}

MsgQueueReturnCode CDVDMessageQueue::Put(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  return Put(pMsg, priority, true);
}

MsgQueueReturnCode CDVDMessageQueue::PutBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority)
{
  return Put(pMsg, priority, false);
}

MsgQueueReturnCode CDVDMessageQueue::Put(const std::shared_ptr<CDVDMsg>& pMsg,
                                         int priority,
                                         bool front)
{
  std::unique_lock lock(m_section);

  if (!m_bInitialized)
  {
    CLog::Log(LOGWARNING, "CDVDMessageQueue({})::Put MSGQ_NOT_INITIALIZED", m_owner);
    return MSGQ_NOT_INITIALIZED;
  }
  if (!pMsg)
  {
    CLog::Log(LOGFATAL, "CDVDMessageQueue({})::Put MSGQ_INVALID_MSG", m_owner);
    return MSGQ_INVALID_MSG;
  }

  if (priority > 0)
  {
    int prio = priority;
    if (!front)
      prio++;

    auto it = std::find_if(m_prioMessages.begin(), m_prioMessages.end(),
                           [prio](const DVDMessageListItem &item){
                             return prio <= item.priority;
                           });
    m_prioMessages.emplace(it, pMsg, priority);
  }
  else
  {
    if (m_messages.empty())
    {
      m_iDataSize = 0;
      m_TimeBack = DVD_NOPTS_VALUE;
      m_TimeFront = DVD_NOPTS_VALUE;
    }

    if (front)
      m_messages.emplace_front(pMsg, priority);
    else
      m_messages.emplace_back(pMsg, priority);
  }

  if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET) && priority == 0)
  {
    DemuxPacket* packet = static_cast<CDVDMsgDemuxerPacket*>(pMsg.get())->GetPacket();
    if (packet)
    {
      m_iDataSize += packet->iSize;
      if (front)
        UpdateTimeFront();
      else
        UpdateTimeBack();
    }
  }

  // inform waiter for new packet
  m_hEvent.Set();

  return MSGQ_OK;
}

MsgQueueReturnCode CDVDMessageQueue::Get(std::shared_ptr<CDVDMsg>& pMsg,
                                         std::chrono::milliseconds timeout,
                                         int& priority)
{
  std::unique_lock lock(m_section);

  int ret = 0;

  if (!m_bInitialized)
  {
    CLog::Log(LOGFATAL, "CDVDMessageQueue({})::Get MSGQ_NOT_INITIALIZED", m_owner);
    return MSGQ_NOT_INITIALIZED;
  }

  while (!m_bAbortRequest)
  {
    std::list<DVDMessageListItem> &msgs = (priority > 0 || !m_prioMessages.empty()) ? m_prioMessages : m_messages;

    if (!msgs.empty() && (msgs.back().priority >= priority || m_drain))
    {
      DVDMessageListItem& item(msgs.back());
      priority = item.priority;

      if (item.message->IsType(CDVDMsg::DEMUXER_PACKET) && item.priority == 0)
      {
        DemuxPacket* packet =
            std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
        if (packet)
        {
          m_iDataSize -= packet->iSize;
        }
      }

      pMsg = std::move(item.message);
      msgs.pop_back();
      UpdateTimeBack();
      ret = MSGQ_OK;
      break;
    }
    else if (timeout == 0ms)
    {
      ret = MSGQ_TIMEOUT;
      break;
    }
    else
    {
      m_hEvent.Reset();
      lock.unlock();

      // wait for a new message
      if (!m_hEvent.Wait(timeout))
        return MSGQ_TIMEOUT;

      lock.lock();
    }
  }

  if (m_bAbortRequest)
    return MSGQ_ABORT;

  return (MsgQueueReturnCode)ret;
}

void CDVDMessageQueue::UpdateTimeFront()
{
  if (!m_messages.empty())
  {
    auto &item = m_messages.front();
    if (item.message->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* packet =
          std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
      if (packet)
      {
        if (packet->dts != DVD_NOPTS_VALUE)
          m_TimeFront = packet->dts;
        else if (packet->pts != DVD_NOPTS_VALUE)
          m_TimeFront = packet->pts;

        if (m_TimeBack == DVD_NOPTS_VALUE)
          m_TimeBack = m_TimeFront;
      }
    }
  }
}

void CDVDMessageQueue::UpdateTimeBack()
{
  if (!m_messages.empty())
  {
    auto &item = m_messages.back();
    if (item.message->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* packet =
          std::static_pointer_cast<CDVDMsgDemuxerPacket>(item.message)->GetPacket();
      if (packet)
      {
        if (packet->dts != DVD_NOPTS_VALUE)
          m_TimeBack = packet->dts;
        else if (packet->pts != DVD_NOPTS_VALUE)
          m_TimeBack = packet->pts;

        if (m_TimeFront == DVD_NOPTS_VALUE)
          m_TimeFront = m_TimeBack;
      }
    }
  }
}

unsigned CDVDMessageQueue::GetPacketCount(CDVDMsg::Message type)
{
  std::unique_lock lock(m_section);

  if (!m_bInitialized)
    return 0;

  unsigned count = 0;
  for (const auto &item : m_messages)
  {
    if(item.message->IsType(type))
      count++;
  }
  for (const auto &item : m_prioMessages)
  {
    if(item.message->IsType(type))
      count++;
  }

  return count;
}

void CDVDMessageQueue::WaitUntilEmpty()
{
  {
    std::unique_lock lock(m_section);
    m_drain = true;
  }

  const size_t entryMsgs = [this] {
    std::unique_lock lock(m_section);
    return m_messages.size() + m_prioMessages.size();
  }();
  CLog::Log(LOGINFO, "CDVDMessageQueue({})::WaitUntilEmpty - {} msgs, {:.2f}s queued", m_owner,
            entryMsgs, GetTimeSize());
  // Drain for as long as the consumer keeps making progress and bail out once
  // it stalls. A fixed timeout cannot serve both drain cases: the outgoing
  // stream of a BD menu->title jump stops being consumed entirely (a long
  // fixed wait stalls the title start - formerly 2x40s of black screen),
  // while a BD menu-loop playlist transition is a legitimate realtime playout
  // of the remaining queued content, which a short fixed cap cuts off
  // mid-loop. The ceiling bounds consumers that trickle without real
  // progress (e.g. sync-waiting against a stopped clock during teardown), so
  // scale it to the content the queue actually holds: a realtime playout of
  // N seconds of queued content legitimately takes N seconds (TNG-style HDMV
  // title jumps arrive with 16s+ queued; a fixed 8s ceiling silenced the
  // last 8s of every long intro while the video played on). Data-based
  // queues report 0.0 and keep the old 8s floor.
  const auto ceiling = std::chrono::milliseconds(
      std::clamp(static_cast<int>(GetTimeSize() * 1000.0) + 3000, 8000, 30000));
  XbmcThreads::EndTime<> totalTimer(ceiling);
  size_t lastRemaining = std::numeric_limits<size_t>::max();
  XbmcThreads::EndTime<> stallTimer(1500ms);
  while (!m_bAbortRequest && !totalTimer.IsTimePast())
  {
    size_t remaining;
    {
      std::unique_lock lock(m_section);
      remaining = m_messages.size() + m_prioMessages.size();
    }

    if (remaining == 0)
      break;

    // Progress must look like a realtime playout, not a trickle: a consumer
    // sync-waiting against a stopped clock still pulls the odd message, which
    // reset the old any-progress check and let it ride the ceiling (TNG title
    // jump: 30s of silent "drain" with the audio thread never rendering).
    // Realtime consumption is ~30+ msgs per window (audio ~32ms frames,
    // video 24fps+); demand a modest fraction of that. Nearly-empty queues
    // exit via remaining==0 within a window anyway.
    constexpr size_t MIN_WINDOW_PROGRESS = 10;
    if (remaining + MIN_WINDOW_PROGRESS <= lastRemaining)
    {
      lastRemaining = remaining;
      stallTimer.Set(1500ms);
    }
    else if (stallTimer.IsTimePast())
    {
      CLog::Log(LOGINFO,
                "CDVDMessageQueue({})::WaitUntilEmpty - drain stalled/trickling, {} msgs left",
                m_owner, remaining);
      break;
    }

    std::this_thread::sleep_for(25ms);
  }

  if (totalTimer.IsTimePast())
    CLog::Log(LOGINFO, "CDVDMessageQueue({})::WaitUntilEmpty - ceiling hit after {}ms", m_owner,
              ceiling.count());

  {
    std::unique_lock lock(m_section);
    m_drain = false;
  }
}

int CDVDMessageQueue::GetLevel(bool dataLevel) const
{
  std::unique_lock lock(m_section);

  if (m_iDataSize > m_iMaxDataSize)
    return 100;
  if (m_iDataSize == 0)
    return 0;

  if (IsDataBased() || dataLevel)
  {
    return std::min((uint64_t)100, 100 * m_iDataSize / m_iMaxDataSize);
  }

  int level = std::min(100.0, ceil(100.0 * m_TimeSize * (m_TimeFront - m_TimeBack) / DVD_TIME_BASE ));

  // if we added lots of packets with NOPTS, make sure that the queue is not signalled empty
  if (level == 0 && m_iDataSize != 0)
  {
    CLog::Log(LOGDEBUG, "CDVDMessageQueue::GetLevel() - can't determine level");
    return 1;
  }

  return level;
}

double CDVDMessageQueue::GetTimeSize() const
{
  std::unique_lock lock(m_section);

  if (IsDataBased())
    return 0.0;
  else
    return (m_TimeFront - m_TimeBack) / DVD_TIME_BASE;
}

bool CDVDMessageQueue::IsDataBased() const
{
  std::unique_lock lock(m_section);

  return (m_TimeBack == DVD_NOPTS_VALUE  ||
          m_TimeFront == DVD_NOPTS_VALUE ||
          m_TimeFront <= m_TimeBack);
}
