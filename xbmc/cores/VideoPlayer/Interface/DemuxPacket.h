/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "TimingConstants.h"
#include "addons/kodi-dev-kit/include/kodi/c-api/addon-instance/inputstream/demux_packet.h"

#define DMX_SPECIALID_STREAMINFO DEMUX_SPECIALID_STREAMINFO
#define DMX_SPECIALID_STREAMCHANGE DEMUX_SPECIALID_STREAMCHANGE

#ifdef __cplusplus
extern "C"
{
#endif /* __cplusplus */

  struct DemuxPacket : DEMUX_PACKET
  {
    DemuxPacket()
    {
      pData = nullptr;
      iSize = 0;
      iStreamId = -1;
      isDualStream = false;
      isELPackage = false;
      demuxerId = -1;
      iGroupId = -1;
      subtitlePlane = 0;

      pSideData = nullptr;
      iSideDataElems = 0;

      pts = DVD_NOPTS_VALUE;
      dts = DVD_NOPTS_VALUE;
      duration = 0;
      dispTime = 0;
      recoveryPoint = false;
      timelineRestartSeq = 0;
      demuxDts = DVD_NOPTS_VALUE;

      subtitlePlane = 0;

      cryptoInfo = nullptr;
    }

    //! @brief PTS offset correction applied to the PTS and DTS.
    double m_ptsOffsetCorrection{0};
    //! @brief The demuxer's own dts, captured once at read time and never
    //! rewritten afterwards. dts/pts are the PLAYER's timeline: CheckContinuity
    //! shifts them by m_offset_pts at a discontinuity and blanks them to
    //! DVD_NOPTS_VALUE while a jump is unconfirmed. Neither is safe for
    //! associating packets that belong to the same coded frame, because those
    //! rewrites are applied per-stream and a Dolby Vision enhancement layer
    //! does not pass through CheckContinuity at all. Consumers that must match
    //! two streams frame-for-frame compare this instead.
    double demuxDts;
    //! @brief Non-zero on the first packet of a timeline restart (e.g. a
    //! Blu-ray seamless playitem boundary), carrying a per-jump sequence
    //! number. Stamped by CheckContinuity, which detects the jump before it
    //! rewrites the timestamps that would otherwise reveal it. A sequence
    //! number rather than a flag because the transport re-delivers the same
    //! packet - on every AddData retry, and on the VC_FLUSHED/VC_REOPEN replay
    //! - and a consumer must act on each jump exactly once.
    uint32_t timelineRestartSeq;
    //! @brief Indicate package is from a Dolby Vision dual stream source.
    bool isDualStream;
    //! @brief Indicate package is from a Dolby Vision enhancement layer.
    bool isELPackage;
    /// @brief The 3D MVC subtitle plane
    int subtitlePlane;
  };

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */
