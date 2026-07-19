/*
 *  Copyright (C) 2017-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "windowing/VideoSync.h"
#include "guilib/DispResource.h"

#include <cstdint>

class CVideoSyncAML : public CVideoSync, IDispResource
{
public:
  CVideoSyncAML(CVideoReferenceClock *clock);
  virtual ~CVideoSyncAML();
  virtual bool Setup()override;
  virtual void Run(CEvent& stopEvent)override;
  virtual void Cleanup()override;
  virtual float GetFps()override;
  virtual void OnResetDisplay()override;
private:
  // CE22 drives the display via DRM/KMS (mesondrmfb), so the reference clock is
  // the DRM CRTC vblank sequence (drmCrtcGetSequence) — NOT the legacy OSD-fb
  // FBIO_WAITFORVSYNC ioctl, which the DRM fbdev doesn't implement. The DRM fd
  // and CRTC id are borrowed from CWinSystemAmlogic's DRM device (not owned
  // here, so Cleanup must not close the fd).
  volatile bool m_abort;
  int m_fd{-1};
  uint32_t m_crtcId{0};
  uint64_t m_sequence{0};
  // Host-counter (MONOTONIC_RAW) minus DRM vblank ns (CLOCK_MONOTONIC).
  // Signed: either clock can lead, and Run() slews it continuously.
  int64_t m_offset{0};

  // Measured vblank rate from the DRM hardware timestamps. The kernel can
  // silently fail to apply the fractional rate (hdmitx20 SoCs lack
  // vout_set_vframe_rate_hint), leaving the panel at the integer rate while
  // the mode claims 23.976 — a 1000ppm clock-rate error that drifts passthrough
  // A/V sync ~1ms/s. When the measured rate deviates from the nominal one by
  // more than the threshold, the clock is restarted with the measured rate.
  double m_reportedFps{0.0};   // rate GetFps() last handed to the clock
  uint64_t m_measureAnchorSeq{0};
  uint64_t m_measureAnchorNs{0};
  // In-band candidate awaiting confirmation by the next window (see Run()).
  double m_pendingMeasuredFps{0.0};
};
