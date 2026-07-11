/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "ServiceBroker.h"
#include "cores/VideoPlayer/VideoReferenceClock.h"
#include "threads/Thread.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"
#include "windowing/amlogic/WinSystemAmlogic.h"

#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

CVideoSyncAML::CVideoSyncAML(CVideoReferenceClock *clock)
: CVideoSync(clock)
, m_abort(false)
{
}

CVideoSyncAML::~CVideoSyncAML()
{
}

bool CVideoSyncAML::Setup()
{
  m_abort = false;

  CServiceBroker::GetWinSystem()->Register(this);
  CLog::Log(LOGDEBUG, "CVideoSyncAML: setting up (DRM vblank)");

  // CE22 runs the display on DRM/KMS (mesondrmfb). The reference clock is the
  // DRM CRTC vblank sequence, read non-blocking via drmCrtcGetSequence — the
  // same mechanism CVideoSyncGbm uses. The legacy OSD-fb FBIO_WAITFORVSYNC
  // ioctl is not implemented on the DRM fbdev, so it can't be used here.
  auto winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem)
  {
    CLog::Log(LOGWARNING, "CVideoSyncAML: no Amlogic win system, falling back to system clock");
    return false;
  }

  m_fd = winSystem->GetDRMDeviceFd();
  m_crtcId = winSystem->GetDRMCrtcId();
  if (m_fd < 0 || m_crtcId == 0)
  {
    CLog::Log(LOGWARNING,
              "CVideoSyncAML: no DRM device/crtc (fd:{} crtc:{}), falling back to system clock",
              m_fd, m_crtcId);
    return false;
  }

  uint64_t ns = 0;
  int s = drmCrtcGetSequence(m_fd, m_crtcId, &m_sequence, &ns);
  if (s != 0)
  {
    CLog::Log(LOGWARNING,
              "CVideoSyncAML: drmCrtcGetSequence failed ({}), falling back to system clock", s);
    m_fd = -1;
    return false;
  }
  // ns is CLOCK_MONOTONIC; CurrentHostCounter() is the same domain on Linux, so
  // this offset re-bases the vblank timestamps into the reference clock's units.
  m_offset = CurrentHostCounter() - ns;

  CLog::Log(LOGINFO, "CVideoSyncAML: using DRM vblank (fd:{} crtc:{} seq:{})",
            m_fd, m_crtcId, m_sequence);
  return true;
}

void CVideoSyncAML::Run(CEvent& stopEvent)
{
  /* This shouldn't be very busy and timing is important so increase priority */
  CThread::GetCurrentThread()->SetPriority(ThreadPriority::ABOVE_NORMAL);

  while (!stopEvent.Signaled() && !m_abort)
  {
    uint64_t sequence = 0, ns = 0;
    usleep(1000);
    int s = drmCrtcGetSequence(m_fd, m_crtcId, &sequence, &ns);
    if (s != 0)
    {
      CLog::Log(LOGWARNING,
                "CVideoSyncAML: drmCrtcGetSequence failed ({}), stopping vblank clock", s);
      break;
    }

    if (sequence == m_sequence)
      continue;

    m_refClock->UpdateClock(static_cast<int>(sequence - m_sequence), m_offset + ns);
    m_sequence = sequence;
  }
}

void CVideoSyncAML::Cleanup()
{
  CLog::Log(LOGDEBUG, "CVideoSyncAML: cleaning up");
  // m_fd is borrowed from the win system's DRM device — do not close it.
  m_fd = -1;
  CServiceBroker::GetWinSystem()->Unregister(this);
}

float CVideoSyncAML::GetFps()
{
  m_fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  CLog::Log(LOGDEBUG, "CVideoSyncAML: fps: {:.3f}", m_fps);
  return m_fps;
}

void CVideoSyncAML::RefreshChanged()
{
  // A refresh-rate change moves the CRTC timing; abort so the reference clock
  // re-runs Setup and re-anchors the vblank sequence to the new rate.
  if (m_fps != CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS())
    m_abort = true;
}

void CVideoSyncAML::OnResetDisplay()
{
  m_abort = true;
}
