/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OUTPUT
{

//! One writer, any readers; a reader never blocks the writer (Boehm, "Can seqlocks get along
//! with programming language memory models?", 2012). Header-only for the aarch64 stress test.
template<std::size_t N>
class CSeqlock
{
public:
  void Write(const std::array<uint64_t, N>& words)
  {
    const uint64_t seq = m_seq.load(std::memory_order_relaxed);
    m_seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < N; i++)
      m_words[i].store(words[i], std::memory_order_relaxed);
    m_seq.store(seq + 2, std::memory_order_release);
  }

  //! false: the copy is torn (a write overlapped), words is unusable
  bool TryRead(std::array<uint64_t, N>& words, uint64_t& seq) const
  {
    const uint64_t s1 = m_seq.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < N; i++)
      words[i] = m_words[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = m_seq.load(std::memory_order_relaxed);
    seq = s1;
    return s1 == s2 && (s1 & 1) == 0;
  }

  //! 0 before the first write
  uint64_t Sequence() const { return m_seq.load(std::memory_order_acquire); }

private:
  std::atomic<uint64_t> m_seq{0};
  std::array<std::atomic<uint64_t>, N> m_words{};
};

} // namespace OUTPUT
