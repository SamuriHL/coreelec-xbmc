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
  virtual void RefreshChanged()override;
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
  uint64_t m_offset{0};
};
