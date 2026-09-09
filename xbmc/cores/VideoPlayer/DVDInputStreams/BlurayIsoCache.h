/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A read-ahead page cache in front of an ISO opened for Blu-ray playback.
//
// CDVDInputStreamBluray opens the image with READ_NO_CACHE, so libbluray's
// read_blocks() calls land as bare Seek+Read pairs on the file handle with no
// buffering whatsoever. That is fine on local storage; on a network share it
// means every 2 KiB..64 KiB request pays the full link latency and any dip in
// throughput is felt immediately by the demuxer.
//
// This sits in that gap: reads are served from whole pages, and a worker
// thread pulls the next page(s) ahead of a sequential reader so a stall in the
// link is absorbed instead of starving playback. It is deliberately fail-open
// - any page that cannot be loaded makes ReadBlocks() return -1 and the caller
// falls back to reading the file directly.
class CBlurayIsoCache
{
public:
  static constexpr size_t BLOCK_SIZE = 2048;

  struct Config
  {
    size_t pageSize{256 * 1024};
    size_t maxBytes{64 * 1024 * 1024};
    size_t forwardPrefetchPages{1};
  };

  // Reads up to `size` bytes at `offset`; returns the byte count, or <= 0 on
  // failure. Called from both the caller's thread and the prefetch worker, so
  // the implementation must serialise access to the underlying handle.
  using ReadCallback = std::function<int64_t(int64_t offset, uint8_t* buffer, size_t size)>;

  CBlurayIsoCache(int64_t sourceLength, ReadCallback readCallback, Config config);
  ~CBlurayIsoCache();

  void Start();
  void Stop();
  // Forget the sequential-read history, so the next read does not look like a
  // continuation of the one before it. Called on seeks and title/playlist
  // changes: without it the jump's first read would be judged against the
  // pre-jump run and pull a whole window from the wrong place.
  void ResetAccessPattern();
  // Returns the number of 2 KiB blocks copied, or -1 if nothing could be
  // served (the caller must then read directly).
  int ReadBlocks(uint8_t* buffer, int lba, int numBlocks);

private:
  struct Page
  {
    std::vector<uint8_t> data;
    size_t validBytes{0};
  };

  using PagePtr = std::shared_ptr<Page>;

  struct CacheSlot
  {
    std::list<int64_t>::iterator lruIt;
    PagePtr page;
  };

  PagePtr GetPage(int64_t pageIndex);
  PagePtr LoadPage(int64_t pageIndex, bool prefetch);
  void InsertPage(int64_t pageIndex, PagePtr page);
  void TouchPageUnlocked(std::unordered_map<int64_t, CacheSlot>::iterator it);
  void TrimUnlocked();
  void LogStats(const char* reason);

  void QueueForwardPrefetch(int64_t firstPage, int64_t lastPage);
  void QueuePrefetchWindow(int64_t firstPage, size_t pageCount);
  void QueuePage(int64_t pageIndex);
  void Worker();

  bool IsValidPageIndex(int64_t pageIndex) const;
  int64_t GetMaxPageIndex() const;

  Config m_config;
  ReadCallback m_readCallback;
  int64_t m_sourceLength{0};
  size_t m_maxPages{1};

  std::mutex m_cacheMutex;
  std::list<int64_t> m_lruOrder;
  std::unordered_map<int64_t, CacheSlot> m_pages;

  std::mutex m_queueMutex;
  std::condition_variable m_queueChanged;
  std::deque<int64_t> m_prefetchQueue;
  std::unordered_set<int64_t> m_prefetchQueuedPages;
  std::thread m_worker;
  bool m_stopWorker{false};
  bool m_prefetchEnabled{false};
  bool m_started{false};

  std::mutex m_accessMutex;
  int64_t m_lastReadEndPage{-1};
  // Highest page already queued for read-ahead, so a steady reader queues one
  // page per page consumed instead of re-walking the whole window.
  int64_t m_prefetchHorizon{-1};

  std::atomic<uint64_t> m_readRequests{0};
  std::atomic<uint64_t> m_requestedBlocks{0};
  std::atomic<uint64_t> m_pageHits{0};
  std::atomic<uint64_t> m_pageMisses{0};
  std::atomic<uint64_t> m_syncPageLoads{0};
  std::atomic<uint64_t> m_prefetchPageLoads{0};
  std::atomic<uint64_t> m_prefetchFailures{0};
  std::atomic<uint64_t> m_evictions{0};
  std::atomic<uint64_t> m_forwardPrefetchRequests{0};
};
