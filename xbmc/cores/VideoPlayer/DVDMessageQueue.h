/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDMessage.h"
#include "threads/CriticalSection.h"
#include "threads/Event.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <list>
#include <string>

struct DVDMessageListItem
{
  DVDMessageListItem(std::shared_ptr<CDVDMsg> msg, int prio) : message(std::move(msg))
  {
    priority = prio;
  }
  DVDMessageListItem() { priority = 0; }
  DVDMessageListItem(const DVDMessageListItem&) = delete;
  ~DVDMessageListItem() = default;

  DVDMessageListItem& operator=(const DVDMessageListItem&) = delete;

  std::shared_ptr<CDVDMsg> message;
  int priority;
};

enum MsgQueueReturnCode
{
  MSGQ_OK = 1,
  MSGQ_TIMEOUT = 0,
  MSGQ_ABORT = -1, // negative for legacy, not an error actually
  MSGQ_NOT_INITIALIZED = -2,
  MSGQ_INVALID_MSG = -3,
  MSGQ_OUT_OF_MEMORY = -4
};

#define MSGQ_IS_ERROR(c)    (c < 0)

//! 5.E2-7 (R5): a draining close's held predicate with a third answer: END leaves the wait at once
enum class EDrainHeld
{
  NOT_HELD,
  HELD,
  END,
};

class CDVDMessageQueue
{
public:
  explicit CDVDMessageQueue(const std::string &owner);
  virtual ~CDVDMessageQueue();

  void Init();
  void Flush(CDVDMsg::Message message = CDVDMsg::DEMUXER_PACKET);
  void Abort();
  void End();

  MsgQueueReturnCode Put(const std::shared_ptr<CDVDMsg>& pMsg, int priority = 0);
  MsgQueueReturnCode PutBack(const std::shared_ptr<CDVDMsg>& pMsg, int priority = 0);

  /**
   * msg,       message type from DVDMessage.h
   * timeout,   timeout in msec
   * priority,  minimum priority to get, outputs returned packets priority
   */
  MsgQueueReturnCode Get(std::shared_ptr<CDVDMsg>& pMsg,
                         std::chrono::milliseconds timeout,
                         int& priority);
  MsgQueueReturnCode Get(std::shared_ptr<CDVDMsg>& pMsg, std::chrono::milliseconds timeout)
  {
    int priority = 0;
    return Get(pMsg, timeout, priority);
  }
  /**
   * A stream worker that takes no data now: the highest priority message that
   * is not a demuxer packet, ahead of queued replays; no wait
   */
  MsgQueueReturnCode GetControl(std::shared_ptr<CDVDMsg>& pMsg, int& priority);
  /**
   * 5.E2-7: the oldest message (priority messages first) the predicate accepts, leaving every
   * other message in place and in its order; waits up to timeout for one (every put wakes it)
   */
  MsgQueueReturnCode GetFirstOf(std::shared_ptr<CDVDMsg>& pMsg,
                                const std::function<bool(CDVDMsg&)>& accept,
                                std::chrono::milliseconds timeout);
  /**
   * 5.E2-7b: the frozen inbox's take (docs/phase5_e2_7b_impl.md D2): in Get's order, passes over
   * the messages `pass` matches (left in place) and takes the first other message only if `accept`
   * holds, so a take never overtakes a message left queued. Otherwise takes nothing and reports
   * that message's type in frontType (CDVDMsg::NONE when nothing but passed messages is queued).
   * No wait
   */
  MsgQueueReturnCode GetFrontIf(std::shared_ptr<CDVDMsg>& pMsg,
                                const std::function<bool(CDVDMsg&)>& accept,
                                const std::function<bool(CDVDMsg&)>& pass,
                                CDVDMsg::Message& frontType);

  int GetDataSize() const { return m_iDataSize; }
  double GetTimeSize() const;
  unsigned GetPacketCount(CDVDMsg::Message type);
  bool ReceivedAbortRequest() { return m_bAbortRequest; }
  void WaitUntilEmpty(const std::function<bool()>& consumerHeld = {});
  //! 5.E2-7 (R5): as WaitUntilEmpty, with a tri-state held predicate (P's drain hook)
  void WaitUntilEmptyHeld(const std::function<EDrainHeld()>& held);

  // non messagequeue related functions
  // Full when EITHER dimension is exhausted. The data (MB) cap alone lets
  // low-bitrate content queue for minutes (TNG HDMV: 120s audio / 90s video
  // observed), running the disc VM minutes ahead of presentation; the time
  // cap alone would under-use the buffer on high-bitrate streams. GetLevel's
  // time path falls back to data levels for queues without usable pts.
  bool IsFull() const { return GetLevel(true) == 100 || GetLevel(false) == 100; }

  /*!
   * \brief Get the current queue level.
   *
   * \param dataLevel If true, return the data-based level.
   * \return Queue level as a percentage.
   */
  int GetLevel(bool dataLevel = false) const;

  void SetMaxDataSize(int iMaxDataSize) { m_iMaxDataSize = iMaxDataSize; }
  // Locked: re-targeted at runtime (menu-domain low-latency mode) while the
  // consumer thread reads m_TimeSize in GetLevel().
  void SetMaxTimeSize(double sec)
  {
    std::unique_lock lock(m_section);
    m_TimeSize = 1.0 / sec;
  }
  int GetMaxDataSize() const { return m_iMaxDataSize; }
  double GetMaxTimeSize() const
  {
    std::unique_lock lock(m_section);
    return m_TimeSize;
  }
  bool IsInited() const { return m_bInitialized; }
  bool IsDataBased() const;

  // a stream worker's eventfd, written at every put the worker can take, at
  // Abort and when a draining wait starts (-1 clears; the worker clears it
  // before closing)
  void SetWakeFd(int fd);
  // the priority the worker takes from now on (set before its take): a put
  // below it does not wake the worker
  void SetWakePriority(int priority);

private:
  void Wake();
  MsgQueueReturnCode Put(const std::shared_ptr<CDVDMsg>& pMsg, int priority, bool front);
  void UpdateTimeFront();
  void UpdateTimeBack();

  CEvent m_hEvent;
  mutable CCriticalSection m_section;

  std::atomic<bool> m_bAbortRequest = false;
  bool m_bInitialized;
  bool m_drain = false;
  int m_wakeFd = -1;
  int m_wakePriority = 0;

  uint64_t m_iDataSize;
  double m_TimeFront;
  double m_TimeBack;
  double m_TimeSize;

  uint64_t m_iMaxDataSize;
  std::string m_owner;

  std::list<DVDMessageListItem> m_messages;
  std::list<DVDMessageListItem> m_prioMessages;
};

