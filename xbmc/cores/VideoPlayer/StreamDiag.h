/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for complete license information.
 */

#pragma once

// TEMP STREAMDIAG (design §17, step S1): log-only timing of the stream
// threads' waits, the render locks' holds and the control messages' latency,
// on validated SoCs. Removed at the gate.

#include <chrono>
#include <cstdint>

namespace STREAMDIAG
{

enum class Ctx
{
  STEADY,
  FLUSH, // a FlushBuffers or a stream flush (every seek)
  CODECFLUSH, // VC_FLUSHED / VC_REOPEN
  FORMAT, // a renderer configuration change, up to its next picture
};

enum class Lock
{
  STATE,
  PRESENT,
};

bool Enabled();

// a named site, registered once (function-local static)
int Site(const char* name);

// a sample in microseconds (wall and, for holds, on-CPU)
void Record(int site, double wallUs, double cpuUs = -1.0);

// context for lock holds (counted, so nested flushes stay tagged)
void EnterCtx(Ctx ctx);
void LeaveCtx(Ctx ctx);
void FormatPicturePending(); // FORMAT until the next AddVideoPicture
void PictureAdded();

struct CtxScope
{
  explicit CtxScope(Ctx c) : ctx(c) { EnterCtx(ctx); }
  ~CtxScope() { LeaveCtx(ctx); }
  Ctx ctx;
};

double ThreadCpuUs();

// a render lock hold from the depth-0 acquire to the depth-0 release, minus
// the condition variable waits inside it; placed right after the lock
class Hold
{
public:
  Hold(Lock lock, int site);
  ~Hold() { End(); }
  void End(); // the lock was released before the scope ends
private:
  Lock m_lock;
  int m_site;
  bool m_outer = false;
  bool m_ended = false;
};

// a wait inside a hold that releases the lock
class CvWait
{
public:
  CvWait();
  ~CvWait();

private:
  std::chrono::steady_clock::time_point m_t0;
  double m_cpu0;
};

// a timed span recorded at scope end (stream thread waits)
class Span
{
public:
  explicit Span(int site) : m_site(site), m_t0(std::chrono::steady_clock::now()) {}
  ~Span();

private:
  int m_site;
  std::chrono::steady_clock::time_point m_t0;
};

// periodic cumulative summary (rate-limited; called from a stream thread)
void MaybeReport(bool force = false);

} // namespace STREAMDIAG

#define STREAMDIAG_SITE(name) \
  ([] { \
    static const int s = STREAMDIAG::Site(name); \
    return s; \
  }())

#define STREAMDIAG_HOLD(var, lockkind, name) \
  STREAMDIAG::Hold var(lockkind, STREAMDIAG_SITE(name))
