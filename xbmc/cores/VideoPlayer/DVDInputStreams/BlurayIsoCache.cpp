/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BlurayIsoCache.h"

#include "utils/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

CBlurayIsoCache::CBlurayIsoCache(int64_t sourceLength, ReadCallback readCallback, Config config)
  : m_config(config), m_readCallback(std::move(readCallback)), m_sourceLength(sourceLength)
{
  // Pages are whole numbers of 2 KiB blocks, so a block never straddles two.
  if (m_config.pageSize < BLOCK_SIZE)
    m_config.pageSize = BLOCK_SIZE;

  const size_t remainder = m_config.pageSize % BLOCK_SIZE;
  if (remainder != 0)
    m_config.pageSize += BLOCK_SIZE - remainder;

  if (m_config.maxBytes < m_config.pageSize)
    m_config.maxBytes = m_config.pageSize;

  m_maxPages = std::max<size_t>(1, m_config.maxBytes / m_config.pageSize);

  // The read-ahead window has to fit inside the cache, or it evicts the very
  // pages it just fetched and every one of them is read twice.
  m_config.forwardPrefetchPages = std::min(m_config.forwardPrefetchPages, m_maxPages - 1);
}

CBlurayIsoCache::~CBlurayIsoCache()
{
  Stop();
}

void CBlurayIsoCache::Start()
{
  Stop();

  if (m_sourceLength <= 0 || !m_readCallback)
  {
    CLog::Log(LOGDEBUG, "CBlurayIsoCache::Start - cache disabled, invalid source length {}",
              m_sourceLength);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_stopWorker = false;
    m_prefetchEnabled = true;
  }

  m_worker = std::thread(&CBlurayIsoCache::Worker, this);
  m_started = true;

  CLog::Log(LOGINFO,
            "CBlurayIsoCache::Start - started for a {} byte source (pageSize {} maxBytes {} "
            "maxPages {} forwardPages {})",
            m_sourceLength, m_config.pageSize, m_config.maxBytes, m_maxPages,
            m_config.forwardPrefetchPages);
}

void CBlurayIsoCache::Stop()
{
  if (m_started)
    LogStats("stop");

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_stopWorker = true;
    m_prefetchEnabled = false;
    m_prefetchQueue.clear();
    m_prefetchQueuedPages.clear();
  }
  m_queueChanged.notify_all();

  if (m_worker.joinable())
    m_worker.join();

  {
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    m_pages.clear();
    m_lruOrder.clear();
  }

  ResetAccessPattern();
  m_started = false;
}

void CBlurayIsoCache::ResetAccessPattern()
{
  std::lock_guard<std::mutex> lock(m_accessMutex);
  m_lastReadEndPage = -1;
  m_prefetchHorizon = -1;
}

int CBlurayIsoCache::ReadBlocks(uint8_t* buffer, int lba, int numBlocks)
{
  if (!buffer || lba < 0 || numBlocks <= 0)
    return -1;

  ++m_readRequests;
  m_requestedBlocks += static_cast<uint64_t>(numBlocks);

  const int64_t offset = static_cast<int64_t>(lba) * BLOCK_SIZE;
  const int64_t requestedBytes = static_cast<int64_t>(numBlocks) * BLOCK_SIZE;
  if (offset < 0 || requestedBytes <= 0 || offset >= m_sourceLength)
    return -1;

  // A read that runs past the end of the image is served short rather than
  // failed - the direct path behaves the same way.
  const int64_t availableBytes = std::min<int64_t>(requestedBytes, m_sourceLength - offset);
  const int64_t firstPage = offset / static_cast<int64_t>(m_config.pageSize);
  const int64_t lastPage = (offset + availableBytes - 1) / static_cast<int64_t>(m_config.pageSize);

  int64_t copied = 0;
  for (int64_t pageIndex = firstPage; pageIndex <= lastPage; ++pageIndex)
  {
    PagePtr page = GetPage(pageIndex);
    if (!page)
    {
      ++m_pageMisses;
      page = LoadPage(pageIndex, false);
    }
    else
      ++m_pageHits;

    if (!page)
      return copied >= static_cast<int64_t>(BLOCK_SIZE) ? static_cast<int>(copied / BLOCK_SIZE) : -1;

    const int64_t pageStart = pageIndex * static_cast<int64_t>(m_config.pageSize);
    const int64_t copyStart = std::max<int64_t>(offset, pageStart);
    const int64_t copyEnd = std::min<int64_t>(offset + availableBytes,
                                              pageStart + static_cast<int64_t>(page->validBytes));
    if (copyEnd <= copyStart)
      break;

    const size_t pageOffset = static_cast<size_t>(copyStart - pageStart);
    const size_t chunk = static_cast<size_t>(copyEnd - copyStart);
    // Indexed by where the byte belongs in the request, never by how much has
    // been copied so far - those differ the moment a page is short, and the
    // difference would be a silent hole in the middle of the stream.
    std::memcpy(buffer + (copyStart - offset), page->data.data() + pageOffset, chunk);
    copied += static_cast<int64_t>(chunk);

    // A page that ended early cannot be followed by a contiguous one.
    if (copyEnd < std::min<int64_t>(offset + availableBytes,
                                    pageStart + static_cast<int64_t>(m_config.pageSize)))
      break;
  }

  // Never report a partial block as success: the caller reads whole 2 KiB
  // blocks, and a 0 here would look like "nothing to do" rather than a miss.
  if (copied < static_cast<int64_t>(BLOCK_SIZE))
    return -1;

  QueueForwardPrefetch(firstPage, lastPage);
  {
    std::lock_guard<std::mutex> lock(m_accessMutex);
    m_lastReadEndPage = lastPage;
  }

  return static_cast<int>(copied / BLOCK_SIZE);
}

CBlurayIsoCache::PagePtr CBlurayIsoCache::GetPage(int64_t pageIndex)
{
  std::lock_guard<std::mutex> lock(m_cacheMutex);
  auto it = m_pages.find(pageIndex);
  if (it == m_pages.end())
    return nullptr;

  TouchPageUnlocked(it);
  return it->second.page;
}

CBlurayIsoCache::PagePtr CBlurayIsoCache::LoadPage(int64_t pageIndex, bool prefetch)
{
  if (!IsValidPageIndex(pageIndex) || !m_readCallback)
    return nullptr;

  auto page = std::make_shared<Page>();
  page->data.resize(m_config.pageSize);

  const int64_t offset = pageIndex * static_cast<int64_t>(m_config.pageSize);
  const size_t bytesToRead = static_cast<size_t>(
      std::min<int64_t>(static_cast<int64_t>(m_config.pageSize), m_sourceLength - offset));
  const int64_t bytesRead = m_readCallback(offset, page->data.data(), bytesToRead);
  if (bytesRead <= 0)
    return nullptr;

  // A page is cached only if it was filled exactly. A short page would other-
  // wise be retained for the whole session and silently drop bytes out of the
  // middle of every later read that spans it; bytesToRead is already clamped
  // to the end of the image, so the final page is a full page by this test.
  if (static_cast<size_t>(bytesRead) != bytesToRead)
  {
    CLog::Log(LOGERROR,
              "CBlurayIsoCache::LoadPage - short read on page {}: {} bytes of {} expected",
              pageIndex, bytesRead, bytesToRead);
    return nullptr;
  }

  page->validBytes = static_cast<size_t>(bytesRead);
  InsertPage(pageIndex, page);

  if (prefetch)
    ++m_prefetchPageLoads;
  else
    ++m_syncPageLoads;

  return page;
}

void CBlurayIsoCache::InsertPage(int64_t pageIndex, PagePtr page)
{
  if (!page)
    return;

  std::lock_guard<std::mutex> lock(m_cacheMutex);
  auto it = m_pages.find(pageIndex);
  if (it != m_pages.end())
  {
    // Two threads can miss the same page at once; last writer wins, both get
    // a valid page back.
    it->second.page = std::move(page);
    TouchPageUnlocked(it);
    return;
  }

  m_lruOrder.push_front(pageIndex);
  m_pages.emplace(pageIndex, CacheSlot{m_lruOrder.begin(), std::move(page)});
  TrimUnlocked();
}

void CBlurayIsoCache::TouchPageUnlocked(std::unordered_map<int64_t, CacheSlot>::iterator it)
{
  if (it->second.lruIt != m_lruOrder.begin())
    m_lruOrder.splice(m_lruOrder.begin(), m_lruOrder, it->second.lruIt);
  it->second.lruIt = m_lruOrder.begin();
}

void CBlurayIsoCache::TrimUnlocked()
{
  while (m_pages.size() > m_maxPages && !m_lruOrder.empty())
  {
    const int64_t pageIndex = m_lruOrder.back();
    m_lruOrder.pop_back();
    m_pages.erase(pageIndex);
    ++m_evictions;
  }
}

void CBlurayIsoCache::LogStats(const char* reason)
{
  size_t cachedPages = 0;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    cachedPages = m_pages.size();
  }

  CLog::Log(LOGINFO,
            "CBlurayIsoCache::LogStats - {}: {} reads ({} blocks), {} page hits / {} misses, {} "
            "sync loads, {} prefetch loads ({} failed), {} evictions, {} forward prefetches, {} "
            "pages resident",
            reason, m_readRequests.load(), m_requestedBlocks.load(), m_pageHits.load(),
            m_pageMisses.load(), m_syncPageLoads.load(), m_prefetchPageLoads.load(),
            m_prefetchFailures.load(), m_evictions.load(), m_forwardPrefetchRequests.load(),
            cachedPages);
}

void CBlurayIsoCache::QueueForwardPrefetch(int64_t firstPage, int64_t lastPage)
{
  if (m_config.forwardPrefetchPages == 0)
    return;

  const int64_t windowEnd = lastPage + static_cast<int64_t>(m_config.forwardPrefetchPages);
  int64_t queueFrom = 0;
  {
    std::lock_guard<std::mutex> lock(m_accessMutex);

    // Only read ahead for a reader that is actually walking forward. libbluray
    // interleaves index/clip/BD-J lookups with the m2ts stream, and reading
    // ahead of one of those would pull a window of pages nobody wants. An
    // unknown position (start of play, or just after a jump) is NOT treated as
    // sequential: that read establishes the anchor, the next one prefetches.
    if (m_lastReadEndPage < 0 || firstPage < m_lastReadEndPage ||
        firstPage > (m_lastReadEndPage + 1))
    {
      m_prefetchHorizon = -1;
      return;
    }

    // Queue only what is not already queued. Without this the whole window
    // would be re-walked on every read - and reads arrive every few KiB, so a
    // deep window would cost hundreds of thousands of map lookups a second.
    queueFrom = std::max<int64_t>(lastPage + 1, m_prefetchHorizon + 1);
    if (queueFrom > windowEnd)
      return;
    m_prefetchHorizon = windowEnd;
  }

  ++m_forwardPrefetchRequests;
  QueuePrefetchWindow(queueFrom, static_cast<size_t>(windowEnd - queueFrom + 1), true);
}

void CBlurayIsoCache::QueuePrefetchWindow(int64_t firstPage, size_t pageCount, bool highPriority)
{
  if (pageCount == 0)
    return;

  for (size_t i = 0; i < pageCount; ++i)
    QueuePage(firstPage + static_cast<int64_t>(i), highPriority);
}

void CBlurayIsoCache::QueuePage(int64_t pageIndex, bool highPriority)
{
  if (!IsValidPageIndex(pageIndex))
    return;

  {
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    if (m_pages.find(pageIndex) != m_pages.end())
      return;
  }

  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (!m_prefetchEnabled || m_stopWorker)
      return;
    if (!m_prefetchQueuedPages.insert(pageIndex).second)
      return;
    if (highPriority)
      m_prefetchQueue.push_front(pageIndex);
    else
      m_prefetchQueue.push_back(pageIndex);
  }
  m_queueChanged.notify_one();
}

void CBlurayIsoCache::Worker()
{
  while (true)
  {
    int64_t pageIndex = -1;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueChanged.wait(lock, [this] { return m_stopWorker || !m_prefetchQueue.empty(); });
      if (m_stopWorker)
        break;

      pageIndex = m_prefetchQueue.front();
      m_prefetchQueue.pop_front();
      m_prefetchQueuedPages.erase(pageIndex);
    }

    {
      std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
      if (m_pages.find(pageIndex) != m_pages.end())
        continue;
    }

    if (!LoadPage(pageIndex, true))
    {
      ++m_prefetchFailures;
      CLog::Log(LOGDEBUG, "CBlurayIsoCache::Worker - failed to prefetch page {}", pageIndex);
    }
  }
}

bool CBlurayIsoCache::IsValidPageIndex(int64_t pageIndex) const
{
  return pageIndex >= 0 && pageIndex <= GetMaxPageIndex();
}

int64_t CBlurayIsoCache::GetMaxPageIndex() const
{
  if (m_sourceLength <= 0)
    return -1;
  return (m_sourceLength - 1) / static_cast<int64_t>(m_config.pageSize);
}
