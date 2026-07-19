/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDInputStreamBluray.h"

#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDCodecs/Overlay/DVDOverlayImage.h"
#include "DVDInputStreamFile.h"
#include "DVDDemuxers/DemuxMVC.h"
#include "IVideoPlayer.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "filesystem/BlurayCallback.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/Geometry.h"
#include "utils/LanguageTag.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "video/VideoFileItemClassify.h"
#include "video/VideoInfoTag.h"

#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <libbluray/bluray-version.h>
#include <libbluray/bluray.h>
#include <libbluray/clpi_data.h>
#include <libbluray/mpls_data.h>
#include <libbluray/log_control.h>

#define LIBBLURAY_BYTESEEK 0
#define EMPTY_QUEUE(x) { while(!x.empty()) x.pop(); }

using namespace KODI;
using namespace std::chrono_literals;

static int read_blocks(void* handle, void* buf, int lba, int num_blocks)
{
  CDVDInputStreamBluray* blurayStream = reinterpret_cast<CDVDInputStreamBluray*>(handle);
  if (!blurayStream)
    return -1;
  return blurayStream->ReadBlocks(reinterpret_cast<uint8_t*>(buf), lba, num_blocks);
}

static void bluray_overlay_cb(void *this_gen, const BD_OVERLAY * ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallback(ov);
}

#ifdef HAVE_LIBBLURAY_BDJ
void  bluray_overlay_argb_cb(void *this_gen, const struct bd_argb_overlay_s * const ov)
{
  static_cast<CDVDInputStreamBluray*>(this_gen)->OverlayCallbackARGB(ov);
}
#endif

CDVDInputStreamBluray::CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem) :
  CDVDInputStream(DVDSTREAM_TYPE_BLURAY, fileitem), m_player(player)
{
  m_content = "video/x-mpegts";
  memset(&m_event, 0, sizeof(m_event));
#ifdef HAVE_LIBBLURAY_BDJ
  memset(&m_argb,  0, sizeof(m_argb));
#endif
}

CDVDInputStreamBluray::~CDVDInputStreamBluray()
{
  Close();
}

void CDVDInputStreamBluray::Abort()
{
  m_hold = HOLD_EXIT;
}

bool CDVDInputStreamBluray::IsEOF()
{
  return false;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleFromState(const std::string& xmlstate)
{
  BlurayState blurayState;
  if (!m_blurayStateSerializer.XMLToBlurayState(blurayState, xmlstate))
  {
    CLog::LogF(LOGWARNING, "Failed to deserialize Bluray state");
    return nullptr;
  }
  return bd_get_playlist_info(m_bd, blurayState.playlistId, 0);
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleLongest()
{
  BLURAY_TITLE_INFO *s = nullptr;
  for(int i=0; i < m_nTitles; i++)
  {
    BLURAY_TITLE_INFO *t = bd_get_title_info(m_bd, i, 0);
    if(!t)
    {
      CLog::Log(LOGDEBUG, "get_main_title - unable to get title {}", i);
      continue;
    }
    if(!s || s->duration < t->duration)
      std::swap(s, t);

    if(t)
      bd_free_title_info(t);
  }
  return s;
}

BLURAY_TITLE_INFO* CDVDInputStreamBluray::GetTitleFile(const std::string& filename)
{
  unsigned int playlist;
  if(sscanf(filename.c_str(), "%05u.mpls", &playlist) != 1)
  {
    CLog::Log(LOGERROR, "get_playlist_title - unsupported playlist file selected {}",
              CURL::GetRedacted(filename));
    return nullptr;
  }

  return bd_get_playlist_info(m_bd, playlist, 0);
}


bool CDVDInputStreamBluray::Open()
{
  if(m_player == nullptr)
    return false;

  std::string strPath(m_item.GetDynPath());
  std::string filename;
  std::string root;

  bool openStream = false;
  bool openDisc = false;
  bool resumable = true;

  // The item was selected via the simple menu
  if (URIUtils::IsProtocol(strPath, "bluray"))
  {
    CURL url(strPath);
    root = url.GetHostName();
    filename = URIUtils::GetFileName(url.GetFileName());

    // Check whether disc is AACS protected
    CURL url2(root);
    CFileItem item(url2, false);
    openDisc = VIDEO::IsProtectedBlurayDisc(item);

    // check for a menu call for an image file
    if (StringUtils::EqualsNoCase(filename, "menu") &&
        !(m_item.GetStartOffset() == STARTOFFSET_RESUME && m_item.IsResumable()))
    {
      resumable = false;

      // Remove udf:// if present
      if (url2.IsProtocol("udf"))
      {
        item.SetPath(url2.GetHostName());
        openDisc = VIDEO::IsProtectedBlurayDisc(item);
      }

      if (item.IsDiscImage())
      {
        if (!OpenStream(item))
          return false;

        openStream = true;
      }
    }
  }
  else if (m_item.IsDiscImage())
  {
    CURL url2("udf://");

    url2.SetHostName(m_item.GetPath());
    root = url2.Get();

    if (!OpenStream(m_item))
      return false;

    openStream = true;
  }
  else if (VIDEO::IsProtectedBlurayDisc(m_item))
  {
    openDisc = true;
  }
  else
  {
    strPath = URIUtils::GetDirectory(strPath);
    URIUtils::RemoveSlashAtEnd(strPath);

    if(URIUtils::GetFileName(strPath) == "PLAYLIST")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }

    if(URIUtils::GetFileName(strPath) == "BDMV")
    {
      strPath = URIUtils::GetDirectory(strPath);
      URIUtils::RemoveSlashAtEnd(strPath);
    }
    root = strPath;
    // Use the resolved (dynamic) path so playlist selectors survive plugin
    // resolution and library .strm playback. m_item.GetPath() returns the
    // original library reference (e.g. .strm file) which has no .mpls
    // extension, causing the MPLS title selector to be lost and playback
    // to fall through to navigation/main-feature mode.
    filename = URIUtils::GetFileName(m_item.GetDynPath());
  }

  // root should not have trailing slash
  URIUtils::RemoveSlashAtEnd(root);

  bd_set_debug_handler(CBlurayCallback::bluray_logger);
  // DBG_HDMV traces the HDMV VM instruction stream (MovieObject + decrypted IG
  // button commands) - the register reads/branches disc capability cascades
  // run; essential for diagnosing capability-PSR behavior against real discs.
  // Only enabled while debug logging is live: with the mask bit set libbluray
  // formats every VM instruction (and the handler copies it) just for the
  // log-level filter to drop it, which is pure waste during menu VM execution
  // on production runs.
  uint32_t debugMask = DBG_CRIT | DBG_BLURAY | DBG_NAV;
  if (CServiceBroker::GetLogging().IsLogLevelLogged(LOGDEBUG))
    debugMask |= DBG_HDMV;
  bd_set_debug_mask(debugMask);

  m_bd = bd_init();

  if (!m_bd)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to initialize libbluray");
    return false;
  }

  SetupPlayerSettings();

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - opening {}", CURL::GetRedacted(root));

  if (openStream)
  {
    if (!bd_open_stream(m_bd, this, read_blocks))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open {} in stream mode",
                CURL::GetRedacted(root));
      return false;
    }
  }
  else if (openDisc)
  {
    // This special case is required for opening original AACS protected Blu-ray discs. Otherwise
    // things like Bus Encryption might not be handled properly and playback will fail.
    m_rootPath = root;
    if (!bd_open_disc(m_bd, root.c_str(), nullptr))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open {} in disc mode",
                CURL::GetRedacted(root));
      return false;
    }
  }
  else
  {
    m_rootPath = root;

#if defined(HAS_UDFREAD)
    // Only in files mode does libbluray reach the disc through Kodi's filesystem, opening a dozen
    // or so files and directories on it, and then the clips as they play. On a disc image each of
    // those opens would otherwise re-mount the image's UDF volume, so keep it mounted for as long
    // as the disc is open. (Stream mode reads the image itself, disc mode is a physical disc.)
    m_udfMount.emplace(root);
#endif

    if (!bd_open_files(m_bd, &m_rootPath, CBlurayCallback::dir_open, CBlurayCallback::file_open))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to open {} in files mode",
                CURL::GetRedacted(root));
      return false;
    }
  }

  bd_get_event(m_bd, nullptr);

  m_root = root;
  const BLURAY_DISC_INFO *disc_info = bd_get_disc_info(m_bd);

  if (!disc_info)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - bd_get_disc_info() failed");
    return false;
  }

  // libbluray clobbers the app-set UHD capability PSRs on UHD disc detection:
  // index version 0300+ triggers psr_init_UHD(regs, force=1), restoring its
  // /* TODO */ 0xffffffff placeholders over whatever SetupPlayerSettings wrote
  // (verified in the register trace: "PSR25 0x23 -> 0xffffffff" one second
  // after our write). Re-apply the real values now that detection has run.
  ApplyUHDCapabilities();

  if (disc_info->bluray_detected)
  {
#if (BLURAY_VERSION > BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Disc name           : {}",
              disc_info->disc_name ? disc_info->disc_name : "");
#endif
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - First Play supported: {}",
              disc_info->first_play_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - Top menu supported  : {}",
              disc_info->top_menu_supported);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - HDMV titles         : {}",
              disc_info->num_hdmv_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J titles         : {}",
              disc_info->num_bdj_titles);
    m_hasBdjTitles = disc_info->num_bdj_titles > 0;
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD-J handled        : {}",
              disc_info->bdj_handled);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - UNSUPPORTED titles  : {}",
              disc_info->num_unsupported_titles);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS detected       : {}",
              disc_info->aacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libaacs detected    : {}",
              disc_info->libaacs_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - AACS handled        : {}",
              disc_info->aacs_handled);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ detected        : {}",
              disc_info->bdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - libbdplus detected  : {}",
              disc_info->libbdplus_detected);
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - BD+ handled         : {}",
              disc_info->bdplus_handled);
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1,0,0))
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - no menus (libmmbd, or profile 6 bdj)  : {}",
              disc_info->no_menu_support);
#endif
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::Open - 3D content exist    : {}", disc_info->content_exist_3D);
  }
  else
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - BluRay not detected");

  if (disc_info->aacs_detected && !disc_info->aacs_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with AACS");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  if (disc_info->bdplus_detected && !disc_info->bdplus_handled)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Media stream scrambled/encrypted with BD+");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    return false;
  }

  m_nTitles = bd_get_titles(m_bd, TITLES_RELEVANT, 0);

  if (URIUtils::HasExtension(filename, ".mpls"))
  {
    m_navmode = false;
    m_titleInfo = GetTitleFile(filename);
  }
  else if (resumable && m_item.GetStartOffset() == STARTOFFSET_RESUME && m_item.IsResumable())
  {
    m_navmode = false;
    m_titleInfo = GetTitleFromState(m_item.GetVideoInfoTag()->GetResumePoint().playerState);
  }
  else
  {
    m_navmode = true;
    if (!disc_info->first_play_supported)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Can't play disc in HDMV navigation mode - First Play title not supported");
      m_navmode = false;
    }

    if (m_navmode && disc_info->num_unsupported_titles > 0) {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - Unsupported titles found - Some titles can't be played in navigation mode");
    }

    if(!m_navmode)
      m_titleInfo = GetTitleLongest();
  }

  if (m_navmode)
  {

    bd_register_overlay_proc (m_bd, this, bluray_overlay_cb);
#ifdef HAVE_LIBBLURAY_BDJ
    bd_register_argb_overlay_proc (m_bd, this, bluray_overlay_argb_cb, nullptr);
#endif

    if(bd_play(m_bd) <= 0)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed play disk {}",
                CURL::GetRedacted(strPath));
      return false;
    }
    m_hold = HOLD_DATA;
  }
  else
  {
    if(!m_titleInfo)
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to get title info");
      return false;
    }

    if(!bd_select_playlist(m_bd, m_titleInfo->playlist))
    {
      CLog::Log(LOGERROR, "CDVDInputStreamBluray::Open - failed to select playlist {}",
                m_titleInfo->idx);
      return false;
    }
  }

  // For playlist/chapter watch time
  m_startWatchTime = std::chrono::steady_clock::now();

  // Process any events that occurred during opening
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  OpenNextStream();

  return true;
}

// close file and reset everything
void CDVDInputStreamBluray::Close()
{
  CloseMVCDemux();
  FreeTitleInfo();

  if(m_bd)
  {
    bd_register_overlay_proc(m_bd, nullptr, nullptr);
    bd_close(m_bd);
  }

  m_bd = nullptr;
  m_pstream.reset();
  m_rootPath.clear();

#if defined(HAS_UDFREAD)
  // Released last, as the files opened from the volume are closed above
  m_udfMount.reset();
#endif
}

void CDVDInputStreamBluray::FreeTitleInfo()
{
  if (m_titleInfo)
    bd_free_title_info(m_titleInfo);

  m_titleInfo = nullptr;
  m_clip = nullptr;
  FreeClipInfo();
}

void CDVDInputStreamBluray::FreeClipInfo()
{
  if (m_clipInfo)
    bd_free_clpi(m_clipInfo);

  m_clipInfo = nullptr;
}

void CDVDInputStreamBluray::UpdateClipInfo(unsigned int playItem)
{
  FreeClipInfo();

  // A copy of the .clpi libbluray read when it opened the title, so this costs no disc access.
  // The play items of the playlist being played and the clips of the open title are both in the
  // order the .mpls lists them, so the index of one is the index of the other.
  m_clipInfo = bd_get_clpi(m_bd, playItem);
  if (!m_clipInfo)
    CLog::Log(LOGDEBUG,
              "CDVDInputStreamBluray::UpdateClipInfo - no clip information for play item {} - the "
              "streams the playlist does not present will have no language",
              playItem);
}

void CDVDInputStreamBluray::ProcessEvent() {

  int pid = -1, ret;

  // any playlist-scope change voids a pending seamless (playitem-only) hold:
  // the format may differ, so the player must do a full stream reopen.
  // Deliberately NOT the same list as the hold cases in Read(): PLAYITEM is
  // what ARMS the hold there, and PLAYLIST_STOP here only voids.
  switch (m_event.event) {
  case BD_EVENT_SEEK:
  case BD_EVENT_TITLE:
  case BD_EVENT_ANGLE:
  case BD_EVENT_PLAYLIST:
  case BD_EVENT_PLAYLIST_STOP:
    m_seamlessHold = false;
    break;
  default:
    break;
  }

  switch (m_event.event) {

   /* errors */

  case BD_EVENT_ERROR:
    switch (m_event.param)
    {
    case BD_ERROR_HDMV:
    case BD_ERROR_BDJ:
      m_player->OnDiscNavResult(nullptr, BD_EVENT_MENU_ERROR);
      break;
    default:
      break;
    }
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_ERROR: Fatal error. Playback can't be continued.");
    m_hold = HOLD_ERROR;
    break;

  case BD_EVENT_READ_ERROR:
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_READ_ERROR");
    break;

  case BD_EVENT_ENCRYPTED:
    CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_EVENT_ENCRYPTED");
    switch (m_event.param)
    {
    case BD_ERROR_AACS:
      CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_ERROR_AACS");
      break;
    case BD_ERROR_BDPLUS:
      CLog::Log(LOGERROR, "CDVDInputStreamBluray - BD_ERROR_BDPLUS");
      break;
    default:
      break;
    }
    m_hold = HOLD_ERROR;
    m_player->OnDiscNavResult(nullptr, BD_EVENT_ENC_ERROR);
    break;

  /* playback control */

  case BD_EVENT_SEEK:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SEEK");
    //m_player->OnDVDNavResult(nullptr, 1);
    //bd_read_skip_still(m_bd);
    //m_hold = HOLD_HELD;
    break;

  case BD_EVENT_STILL_TIME:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_STILL_TIME {}", m_event.param);
    pid = m_event.param;
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL_TIME);
    m_hold = HOLD_STILL;
    break;

  case BD_EVENT_STILL:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_STILL {}", m_event.param);

    pid = m_event.param;

    if (pid == 0)
      m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_STILL);
    break;

  case BD_EVENT_DISCONTINUITY:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_DISCONTINUITY");
    // during a seamless menu playitem continuation the demuxer must NOT be
    // reset: CDVDDemuxFFmpeg::Reset() is a full avformat re-probe which
    // intermittently breaks the dual-layer (BL+EL) DV packet routing (BL
    // delivery dies, playback coasts on the queued backlog, then starves).
    // ffmpeg's mpegts demuxer absorbs the in-band TS discontinuity (PAT/PMT
    // reappear, pts jump) natively; the pts jump is handled by the player's
    // CheckContinuity.
    if (!IsSeamlessStreamChange())
      m_player->OnDiscNavResult(&m_event.param, BD_EVENT_DISCONTINUITY);
    m_hold = HOLD_NONE;
    break;

    /* playback position */

  case BD_EVENT_ANGLE:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_ANGLE {}", m_event.param);
    m_angle = m_event.param;

    if (m_playlist <= MAX_PLAYLIST_ID)
    {
      FreeTitleInfo();
      m_titleInfo = bd_get_playlist_info(m_bd, m_playlist, m_angle);
    }
    break;

  case BD_EVENT_END_OF_TITLE:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_END_OF_TITLE {}", m_event.param);
    /* when a title ends, playlist WILL eventually change */
    FreeTitleInfo();
    break;

  case BD_EVENT_TITLE:
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_TITLE {}", m_event.param);
    const BLURAY_DISC_INFO* disc_info = bd_get_disc_info(m_bd);

    m_menu = false;
    m_isInMainMenu = false;

    if (m_event.param == BLURAY_TITLE_TOP_MENU)
    {
      m_title = disc_info->top_menu;
      m_menu = true;
      m_isInMainMenu = true;
    }
    else if (m_event.param == BLURAY_TITLE_FIRST_PLAY)
      m_title = disc_info->first_play;
    else if (m_event.param <= disc_info->num_titles)
      m_title = disc_info->titles[m_event.param];
    else
      m_title = nullptr;

    break;
  }
  case BD_EVENT_PLAYLIST:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST {}", m_event.param);
    m_playlist = m_event.param;
    ProcessItem(m_playlist);
    break;

  case BD_EVENT_PLAYITEM:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYITEM {}", m_event.param);
    if (m_titleInfo && m_event.param < m_titleInfo->clip_count)
    {
      m_clip = &m_titleInfo->clips[m_event.param];
      UpdateClipInfo(m_event.param);
    }
    uint64_t clip_start, clip_in, bytepos;
    ret = bd_get_clip_infos(m_bd, m_event.param, &clip_start, &clip_in, &bytepos, nullptr);
    if (ret)
      m_clipStartTime = clip_start / 90;
    break;

  case BD_EVENT_CHAPTER:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_CHAPTER {}", m_event.param);
    break;

    /* stream selection */

  case BD_EVENT_AUDIO_STREAM:
    pid = -1;
    if (m_titleInfo && m_clip && static_cast<uint32_t>(m_clip->audio_stream_count) > (m_event.param - 1))
      pid = m_clip->audio_streams[m_event.param - 1].pid;
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_AUDIO_STREAM {} {}", m_event.param, pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_AUDIO_STREAM);
    break;

  case BD_EVENT_PG_TEXTST:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST {}", m_event.param);
    pid = m_event.param;
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST);
    break;

  case BD_EVENT_PG_TEXTST_STREAM:
    pid = -1;
    if (m_titleInfo && m_clip && static_cast<uint32_t>(m_clip->pg_stream_count) > (m_event.param - 1))
      pid = m_clip->pg_streams[m_event.param - 1].pid;
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PG_TEXTST_STREAM {}, {}", m_event.param,
              pid);
    m_player->OnDiscNavResult(static_cast<void*>(&pid), BD_EVENT_PG_TEXTST_STREAM);
    break;

  case BD_EVENT_MENU:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_MENU {}", m_event.param);
    m_menu = (m_event.param != 0);
    if (!m_menu)
      m_isInMainMenu = false;
    m_player->OnDiscNavResult(&m_event.param, BD_EVENT_MENU);
    break;

  case BD_EVENT_IDLE:
    KODI::TIME::Sleep(100ms);
    break;

  case BD_EVENT_SOUND_EFFECT:
  {
    BLURAY_SOUND_EFFECT effect;
    if (bd_get_sound_effect(m_bd, m_event.param, &effect) <= 0)
    {
      CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT {} not valid",
                m_event.param);
    }
    else
    {
      CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_SOUND_EFFECT {}", m_event.param);
    }
  }

  case BD_EVENT_IG_STREAM:
  case BD_EVENT_SECONDARY_AUDIO:
  case BD_EVENT_SECONDARY_AUDIO_STREAM:
  case BD_EVENT_SECONDARY_VIDEO:
  case BD_EVENT_SECONDARY_VIDEO_SIZE:
  case BD_EVENT_SECONDARY_VIDEO_STREAM:
  case BD_EVENT_PLAYMARK:
  case BD_EVENT_KEY_INTEREST_TABLE:
  case BD_EVENT_UO_MASK_CHANGED:
    break;

  case BD_EVENT_PLAYLIST_STOP:
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - BD_EVENT_PLAYLIST_STOP: flush buffers");
    m_player->OnDiscNavResult(nullptr, BD_EVENT_PLAYLIST_STOP);
    break;
  case BD_EVENT_NONE:
    break;

  default:
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - unhandled libbluray event {} [param {}]",
              m_event.event, m_event.param);
    break;
  }

  /* event has been consumed */
  m_event.event = BD_EVENT_NONE;

  if ( m_bMVCPlayback && m_clip
    && m_titleInfo
    && m_clip < m_titleInfo->clips + m_titleInfo->clip_count
    && m_nMVCClip != m_clip
    && (m_clipQueue.empty()
      || m_clip != m_titleInfo->clips + m_clipQueue.front()))
  {
    m_clipQueue.push(m_clip - m_titleInfo->clips);
    if (m_pMVCDemux == NULL)
      OpenNextStream();
  }
}

void CDVDInputStreamBluray::DisableExtention()
{
  CloseMVCDemux();
  m_bMVCDisabled = true;
  m_bMVCPlayback = false;
}

int CDVDInputStreamBluray::Read(uint8_t* buf, int buf_size)
{
  int result = 0;
  m_dispTimeBeforeRead = static_cast<int>((bd_tell_time(m_bd) / 90));
  if(m_navmode)
  {
    do {

      if (m_hold == HOLD_HELD)
         return 0;

      if(  m_hold == HOLD_ERROR
        || m_hold == HOLD_EXIT)
        return -1;

      result = bd_read_ext (m_bd, buf, buf_size, &m_event);

      if(result < 0)
      {
        m_hold = HOLD_ERROR;
        return result;
      }

      /* Check for holding events */
      switch(m_event.event) {
        case BD_EVENT_SEEK:
        case BD_EVENT_TITLE:
        case BD_EVENT_ANGLE:
        case BD_EVENT_PLAYLIST:
        case BD_EVENT_PLAYITEM:
          if(m_hold != HOLD_DATA)
          {
            // menu state at the moment the stream change was signalled -
            // consulted by the player to decide whether the queued remainder
            // is dropped (user left the menu) or rendered out
            m_menuAtHold = m_menu;
            // a hold from a bare playitem advance (no intervening playlist/
            // title/seek/angle event - ProcessEvent's voiding switch clears
            // those) is a same-format continuation the player can serve
            // without teardown
            m_seamlessHold = (m_event.event == BD_EVENT_PLAYITEM);
            m_hold = HOLD_HELD;
            return result;
          }
          break;

        case BD_EVENT_STILL_TIME:
          if(m_hold == HOLD_STILL)
            m_event.event = 0; /* Consume duplicate still event */
          else
            m_hold = HOLD_HELD;
          return result;

        default:
          break;
      }

      if(result > 0)
        m_hold = HOLD_NONE;

      ProcessEvent();

    } while(result == 0);

  }
  else
  {
    result = bd_read(m_bd, buf, buf_size);
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
  return result;
}

int CDVDInputStreamBluray::ReadBlocks(uint8_t* buf, int lba, int num_blocks)
{
  CDVDInputStreamFile* lpstream = m_pstream.get();
  if (!lpstream)
    return -1;
  int result = -1;
  int64_t offset = static_cast<int64_t>(lba) * 2048;
  std::unique_lock lock(m_readBlocksLock);
  if (lpstream->Seek(offset, SEEK_SET) >= 0)
  {
    int64_t size = static_cast<int64_t>(num_blocks) * 2048;
    if (size <= std::numeric_limits<int>::max())
      result = lpstream->Read(buf, static_cast<int>(size)) / 2048;
  }
  return result;
}

static uint8_t  clamp(double v)
{
  return (v) > 255.0 ? 255 : ((v) < 0.0 ? 0 : static_cast<uint32_t>((v + 0.5)));
}

static uint32_t build_rgba(const BD_PG_PALETTE_ENTRY &e)
{
  double r = 1.164 * (e.Y - 16)                        + 1.596 * (e.Cr - 128);
  double g = 1.164 * (e.Y - 16) - 0.391 * (e.Cb - 128) - 0.813 * (e.Cr - 128);
  double b = 1.164 * (e.Y - 16) + 2.018 * (e.Cb - 128);
  return static_cast<uint32_t>(e.T)      << PIXEL_ASHIFT
       | static_cast<uint32_t>(clamp(r)) << PIXEL_RSHIFT
       | static_cast<uint32_t>(clamp(g)) << PIXEL_GSHIFT
       | static_cast<uint32_t>(clamp(b)) << PIXEL_BSHIFT;
}

void CDVDInputStreamBluray::OverlayClose()
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  for(SPlane& plane : m_planes)
    plane.o.clear();
  auto group = std::make_shared<CDVDOverlayGroup>();
  group->bForced = true;
  // menu overlays belong to disc navigation, not to a demux stream: they must
  // survive the overlay-container flushes done on stream close/switch
  group->SetOverlayContainerFlushable(false);
  m_player->OnDiscNavResult(static_cast<void*>(&group), BD_EVENT_MENU_OVERLAY);
  m_hasOverlay = false;
#endif
}

void CDVDInputStreamBluray::RedrawMenuOverlays()
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  // The player clears its overlay container when streams are reopened on a
  // menu playlist/playitem change, but libbluray only resends graphics when
  // they change. The current composition is retained in m_planes - repost it
  // so the menu image survives the reopen.
  if (m_hasOverlay)
    OverlayFlush(-1);
#endif
}

void CDVDInputStreamBluray::OverlayInit(SPlane& plane, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  plane.o.clear();
  plane.w = w;
  plane.h = h;
#endif
}

void CDVDInputStreamBluray::OverlayClear(SPlane& plane, int x, int y, int w, int h)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  CRectInt ovr(x
          , y
          , x + w
          , y + h);

  /* fixup existing overlays */
  for(SOverlays::iterator it = plane.o.begin(); it != plane.o.end();)
  {
    CRectInt old((*it)->x
            , (*it)->y
            , (*it)->x + (*it)->width
            , (*it)->y + (*it)->height);

    std::vector<CRectInt> rem = old.SubtractRect(ovr);

    /* if no overlap we are done */
    if(rem.size() == 1 && !(rem[0] != old))
    {
      ++it;
      continue;
    }

    SOverlays add;
    for(std::vector<CRectInt>::iterator itr = rem.begin(); itr != rem.end(); ++itr)
    {
      SOverlay overlay =
          std::make_shared<CDVDOverlayImage>(*(*it), itr->x1, itr->y1, itr->Width(), itr->Height());
      add.push_back(overlay);
    }

    it = plane.o.erase(it);
    plane.o.insert(it, add.begin(), add.end());
  }
#endif
}

void CDVDInputStreamBluray::OverlayFlush(int64_t pts)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  auto group = std::make_shared<CDVDOverlayGroup>();
  group->bForced       = true;
  // menu overlays belong to disc navigation, not to a demux stream: they must
  // survive the overlay-container flushes done on stream close/switch
  group->SetOverlayContainerFlushable(false);
  group->iPTSStartTime = static_cast<double>(pts);
  group->iPTSStopTime  = 0;

  for(SPlane& plane : m_planes)
  {
    for(SOverlays::iterator it = plane.o.begin(); it != plane.o.end(); ++it)
      group->m_overlays.push_back(*it);
  }

  m_player->OnDiscNavResult(static_cast<void*>(&group), BD_EVENT_MENU_OVERLAY);
  m_hasOverlay = true;
#endif
}

void CDVDInputStreamBluray::OverlayCallback(const BD_OVERLAY * const ov)
{
#if(BD_OVERLAY_INTERFACE_VERSION >= 2)
  if(ov == nullptr || ov->cmd == BD_OVERLAY_CLOSE)
  {
    OverlayClose();
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_OVERLAY_CLEAR)
  {
    plane.o.clear();
    return;
  }

  if (ov->cmd == BD_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    return;
  }

  if (ov->cmd == BD_OVERLAY_DRAW || ov->cmd == BD_OVERLAY_WIPE)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);

  /* uncompress and draw bitmap */
  if (ov->img && ov->cmd == BD_OVERLAY_DRAW)
  {
    SOverlay overlay = std::make_shared<CDVDOverlayImage>();

    if (ov->palette)
    {
      overlay->palette.resize(256);

      for(unsigned i = 0; i < 256; i++)
        overlay->palette[i] = build_rgba(ov->palette[i]);
    }
    else
      overlay->palette.clear();

    const BD_PG_RLE_ELEM *rlep = ov->img;
    size_t bytes = ov->w * ov->h;
    overlay->pixels.resize(bytes);

    for (size_t i = 0; i < bytes; i += rlep->len, rlep++)
      memset(overlay->pixels.data() + i, rlep->color, rlep->len);

    overlay->linesize = ov->w;
    overlay->x = ov->x;
    overlay->y = ov->y;
    overlay->height = ov->h;
    overlay->width = ov->w;
    overlay->source_height = plane.h;
    overlay->source_width = plane.w;
    plane.o.push_back(overlay);
  }

  if (ov->cmd == BD_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
#endif
}

#ifdef HAVE_LIBBLURAY_BDJ
void CDVDInputStreamBluray::OverlayCallbackARGB(const struct bd_argb_overlay_s * const ov)
{
  if(ov == nullptr || ov->cmd == BD_ARGB_OVERLAY_CLOSE)
  {
    OverlayClose();
    return;
  }

  if (ov->plane > 1)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray - Ignoring overlay with multiple planes");
    return;
  }

  SPlane& plane(m_planes[ov->plane]);

  if (ov->cmd == BD_ARGB_OVERLAY_INIT)
  {
    OverlayInit(plane, ov->w, ov->h);
    return;
  }

  if (ov->cmd == BD_ARGB_OVERLAY_DRAW)
    OverlayClear(plane, ov->x, ov->y, ov->w, ov->h);

  /* uncompress and draw bitmap */
  if (ov->argb && ov->cmd == BD_ARGB_OVERLAY_DRAW)
  {
    SOverlay overlay = std::make_shared<CDVDOverlayImage>();

    overlay->palette.clear();
    size_t bytes = static_cast<size_t>(ov->stride * ov->h * 4);
    overlay->pixels.resize(bytes);
    memcpy(overlay->pixels.data(), ov->argb, bytes);

    overlay->linesize = ov->stride * 4;
    overlay->x = ov->x;
    overlay->y = ov->y;
    overlay->height = ov->h;
    overlay->width = ov->w;
    overlay->source_height = plane.h;
    overlay->source_width = plane.w;
    plane.o.push_back(overlay);
  }

  if(ov->cmd == BD_ARGB_OVERLAY_FLUSH)
    OverlayFlush(ov->pts);
}
#endif


int CDVDInputStreamBluray::GetTotalTime()
{
  if(m_titleInfo)
    return static_cast<int>(m_titleInfo->duration / 90);
  else
    return 0;
}

int CDVDInputStreamBluray::GetTime()
{
  return m_dispTimeBeforeRead;
}

bool CDVDInputStreamBluray::PosTime(int ms)
{
  if(bd_seek_time(m_bd, ms * 90) < 0)
    return false;

  EMPTY_QUEUE(m_clipQueue);
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  if (m_bMVCPlayback)
  {
    OpenNextStream();
    SeekMVCDemux(ms - m_clipStartTime);
  }
  return true;
}

int CDVDInputStreamBluray::GetChapterCount()
{
  if(m_titleInfo)
    return static_cast<int>(m_titleInfo->chapter_count);
  else
    return 0;
}

int CDVDInputStreamBluray::GetChapter()
{
  if(m_titleInfo)
    return static_cast<int>(bd_get_current_chapter(m_bd) + 1);
  else
    return 0;
}

void CDVDInputStreamBluray::GetChapterName(std::string& name, int ch)
{
  name.clear();

#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0))
  if (ch == -1 || ch > GetChapterCount())
    ch = GetChapter();
  if (ch < 1 || ch > GetChapterCount())
    return;

  if (m_titleInfo && m_titleInfo->chapters && m_titleInfo->chapters[ch - 1].chapter_name)
    name = m_titleInfo->chapters[ch - 1].chapter_name;
#endif
}

bool CDVDInputStreamBluray::SeekChapter(int ch)
{
  if(m_titleInfo && bd_seek_chapter(m_bd, ch-1) < 0)
    return false;

  EMPTY_QUEUE(m_clipQueue);
  while (bd_get_event(m_bd, &m_event))
    ProcessEvent();

  if (m_bMVCPlayback)
  {
    OpenNextStream();
    SeekMVCDemux((GetChapterPos(ch) - std::chrono::milliseconds(m_clipStartTime)).count());
  }
  return true;
}

std::chrono::milliseconds CDVDInputStreamBluray::GetChapterPos(int ch)
{
  if (ch == -1 || ch > GetChapterCount())
    ch = GetChapter();

  if (m_titleInfo && m_titleInfo->chapters)
    return std::chrono::milliseconds{m_titleInfo->chapters[ch - 1].start / 90};
  else
    return std::chrono::milliseconds{0};
}

int64_t CDVDInputStreamBluray::Seek(int64_t offset, int whence)
{
#if LIBBLURAY_BYTESEEK
  if(whence == SEEK_POSSIBLE)
    return 1;
  else if(whence == SEEK_CUR)
  {
    if(offset == 0)
      return bd_tell(m_bd);
    else
      offset += bd_tell(m_bd);
  }
  else if(whence == SEEK_END)
    offset += bd_get_title_size(m_bd);
  else if(whence != SEEK_SET)
    return -1;

  int64_t pos = bd_seek(m_bd, offset);
  if(pos < 0)
  {
    CLog::Log(LOGERROR, "CDVDInputStreamBluray::Seek - seek to {}, failed with {}", offset, pos);
    return -1;
  }

  if(pos != offset)
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Seek - seek to {}, ended at {}", offset, pos);

  return offset;
#else
  if (whence == DVDSTREAM_SEEK_POSSIBLE)
    return 0;
  return -1;
#endif
}

int64_t CDVDInputStreamBluray::GetLength()
{
  return static_cast<int64_t>(bd_get_title_size(m_bd));
}

static bool find_stream(int pid, BLURAY_STREAM_INFO *info, int count, std::string &language)
{
  int i=0;
  for(;i<count;i++,info++)
  {
    if(info->pid == static_cast<uint16_t>(pid))
      break;
  }
  if(i==count)
    return false;
  language = reinterpret_cast<char*>(info->lang);
  return true;
}

static bool is_first_stream(int pid, const BLURAY_STREAM_INFO* info, int count)
{
  return count > 0 && info[0].pid == static_cast<uint16_t>(pid);
}

bool CDVDInputStreamBluray::GetPlaylistStreamLanguage(int pid, std::string& language) const
{
  if (pid == HDMV_PID_VIDEO || pid == HDMV_PID_VIDEO_EL)
    return find_stream(pid, m_clip->video_streams, m_clip->video_stream_count, language);
  if (HDMV_PID_AUDIO_FIRST <= pid && pid <= HDMV_PID_AUDIO_LAST)
    return find_stream(pid, m_clip->audio_streams, m_clip->audio_stream_count, language);
  if ((HDMV_PID_PG_FIRST <= pid && pid <= HDMV_PID_PG_LAST) ||
      (HDMV_PID_PG_HDR_FIRST <= pid && pid <= HDMV_PID_PG_HDR_LAST))
    return find_stream(pid, m_clip->pg_streams, m_clip->pg_stream_count, language);
  if (HDMV_PID_IG_FIRST <= pid && pid <= HDMV_PID_IG_LAST)
    return find_stream(pid, m_clip->ig_streams, m_clip->ig_stream_count, language);

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::GetStreamInfo - unhandled pid {}", pid);
  return false;
}

bool CDVDInputStreamBluray::GetClipStreamLanguage(int pid, std::string& language) const
{
  if (!m_clipInfo)
    return false;

  const CLPI_PROG_INFO& programs{m_clipInfo->program};
  for (unsigned int i = 0; i < programs.num_prog; ++i)
  {
    const CLPI_PROG& program{programs.progs[i]};
    for (unsigned int j = 0; j < program.num_streams; ++j)
    {
      const CLPI_PROG_STREAM& stream{program.streams[j]};
      if (stream.pid != static_cast<uint16_t>(pid))
        continue;

      // The language is three characters, and absent for a stream that has none (video)
      if (stream.lang[0] == 0)
        return false;

      language = std::string(stream.lang, strnlen(stream.lang, sizeof(stream.lang)));
      return true;
    }
  }

  return false;
}

void CDVDInputStreamBluray::GetStreamInfo(int pid, std::string &language)
{
  if(!m_titleInfo || !m_clip)
    return;

  if (GetPlaylistStreamLanguage(pid, language))
    return;

  // The playlist's stream number table lists only the streams it presents, whereas the m2ts of its
  // clip commonly carries more - two playlists sharing a clip each present their own selection of
  // them. The demuxer exposes every stream of the transport stream, so the language of one the
  // playlist does not present comes from the clip information, leaving it named rather than
  // unknown. Which stream the playlist starts on is unaffected (see IsDefaultStream).
  if (!GetClipStreamLanguage(pid, language))
    CLog::Log(LOGDEBUG,
              "CDVDInputStreamBluray::GetStreamInfo - no language for pid {} in the playlist or "
              "its clip",
              pid);
}

bool CDVDInputStreamBluray::IsDefaultStream(int pid) const
{
  if (!m_titleInfo || !m_clip)
    return false;

  // The clip's stream number table lists the primary streams in stream number order, and a player
  // starts with audio stream number 1 (PSR1) and presentation graphic stream number 1 (PSR2), so
  // the first entry of each is the disc's default.
  if (HDMV_PID_AUDIO_FIRST <= pid && pid <= HDMV_PID_AUDIO_LAST)
    return is_first_stream(pid, m_clip->audio_streams, m_clip->audio_stream_count);
  if ((HDMV_PID_PG_FIRST <= pid && pid <= HDMV_PID_PG_LAST) ||
      (HDMV_PID_PG_HDR_FIRST <= pid && pid <= HDMV_PID_PG_HDR_LAST))
    return is_first_stream(pid, m_clip->pg_streams, m_clip->pg_stream_count);

  return false;
}

bool CDVDInputStreamBluray::GetDiscStreamHdrMetadata(int pid, bool& isDolbyVision, bool& isHdrPlus)
{
  isDolbyVision = false;
  isHdrPlus = false;
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0))
  if (!m_titleInfo || m_titleInfo->clip_count == 0)
    return false;

  // STN tables are per-clip but the PID layout is constant across a playlist's
  // clips; fall back to the first clip when no PLAYITEM event has fired yet.
  const BLURAY_CLIP_INFO* clip = m_clip ? m_clip : &m_titleInfo->clips[0];

  bool known = false;
  // A pid listed in the playlist's DV extension stream table IS a Dolby Vision
  // (RPU/enhancement-carrying) stream, even when the DOVI side data ffmpeg
  // looks for is absent from the bitstream it has parsed so far.
  for (uint8_t i = 0; i < clip->dv_stream_count; i++)
  {
    if (clip->dv_streams[i].pid == pid)
    {
      isDolbyVision = true;
      known = true;
    }
  }
  for (uint8_t i = 0; i < clip->video_stream_count; i++)
  {
    if (clip->video_streams[i].pid == pid)
    {
      known = true;
      if (clip->video_streams[i].dynamic_range_type == BLURAY_DYNAMIC_RANGE_DOLBY_VISION)
        isDolbyVision = true;
      if (clip->video_streams[i].hdr_plus_flag)
        isHdrPlus = true;
    }
  }
  return known;
#else
  return false;
#endif
}

CDVDInputStream::ENextStream CDVDInputStreamBluray::NextStream()
{
  if(!m_navmode || m_hold == HOLD_EXIT || m_hold == HOLD_ERROR)
    return NEXTSTREAM_NONE;

  /* process any current event */
  ProcessEvent();

  /* process all queued up events */
  while(bd_get_event(m_bd, &m_event))
    ProcessEvent();

  if(m_hold == HOLD_STILL)
    return NEXTSTREAM_RETRY;

  m_hold = HOLD_DATA;
  return NEXTSTREAM_OPEN;
}

void CDVDInputStreamBluray::UserInput(bd_vk_key_e vk)
{
  if(m_bd == nullptr || !m_navmode)
    return;

  int ret = bd_user_input(m_bd, -1, vk);
  if (ret < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::UserInput - user input failed");
  }
  else
  {
    /* process all queued up events */
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
}

bool CDVDInputStreamBluray::MouseMove(const CPoint &point)
{
  if (m_bd == nullptr || !m_navmode)
    return false;

  // Disable mouse selection for BD-J menus, since it's not implemented in libbluray as of version 1.0.2
  if (m_title && m_title->bdj == 1)
    return false;

  if (bd_mouse_select(m_bd, -1, static_cast<uint16_t>(point.x), static_cast<uint16_t>(point.y)) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseMove - mouse select failed");
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::MouseClick(const CPoint &point)
{
  if (m_bd == nullptr || !m_navmode)
    return false;

  // Disable mouse selection for BD-J menus, since it's not implemented in libbluray as of version 1.0.2
  if (m_title && m_title->bdj == 1)
    return false;

  if (bd_mouse_select(m_bd, -1, static_cast<uint16_t>(point.x), static_cast<uint16_t>(point.y)) < 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse select failed");
    return false;
  }

  if (bd_user_input(m_bd, -1, BD_VK_MOUSE_ACTIVATE) >= 0)
    return true;

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::MouseClick - mouse click (user input) failed");
  return false;
}

bool CDVDInputStreamBluray::OnMenu()
{
  if(m_bd == nullptr || !m_navmode)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - navigation mode not enabled");
    return false;
  }

  // we can not use this event to track a possible popup menu state since bd-j blu-rays can
  // toggle the popup menu on their own without firing this event, and if they do this, our
  // internal tracking state would be wrong. So just process and return.
  if(bd_user_input(m_bd, -1, BD_VK_POPUP) >= 0)
  {
    return true;
  }

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - popup failed, trying root");

  if (bd_user_input(m_bd, -1, BD_VK_ROOT_MENU) >= 0)
  {
    return true;
  }

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - root failed, trying explicit");
  if (bd_menu_call(m_bd, -1) <= 0)
  {
    CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OnMenu - root failed");
    return false;
  }
  return true;
}

bool CDVDInputStreamBluray::IsInMenu()
{
  if(m_bd == nullptr || !m_navmode)
    return false;

  // since there is no way to tell in a BD-J blu-ray when a popup menu actually is visible,
  // we have to assume that the blu-ray is in menu/navigation mode when there is an overlay
  // on screen, even if it might be invisible (which is impossible to detect)
  if(m_menu || m_hasOverlay)
    return true;
  return false;
}

void CDVDInputStreamBluray::SkipStill()
{
  if(m_bd == nullptr || !m_navmode)
    return;

  if ( m_hold == HOLD_STILL)
  {
    m_hold = HOLD_HELD;
    bd_read_skip_still(m_bd);

    /* process all queued up events */
    while (bd_get_event(m_bd, &m_event))
      ProcessEvent();
  }
}

bool CDVDInputStreamBluray::CanSeek()
{
  return !IsInMenu() || !m_isInMainMenu;
}

MenuType CDVDInputStreamBluray::GetSupportedMenuType()
{
  if (m_navmode)
  {
    return MenuType::NATIVE;
  }
  return MenuType::NONE;
}

bool CDVDInputStreamBluray::ProcessItem(int playitem)
{
  FreeTitleInfo();

  m_titleInfo = bd_get_playlist_info(m_bd, playitem, m_angle);

  if (!m_bMVCDisabled)
  {
    MPLS_PL * mpls = bd_get_title_mpls(m_bd);
    if (mpls)
    {
      for (int i = 0; i < mpls->ext_sub_count; i++)
      {
        if (mpls->ext_sub_path[i].type == 8
          && mpls->ext_sub_path[i].sub_playitem_count == mpls->list_count)
        {
          CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - Enabling BD3D MVC demuxing");
          CLog::Log(LOGDEBUG, "CDVDInputStreamBluray - MVC_Base_view_R_flag: {}", m_titleInfo->mvc_base_view_r_flag);
          m_bMVCPlayback = true;
          m_nMVCSubPathIndex = i;
          m_bFlipEyes = m_titleInfo->mvc_base_view_r_flag != 0;
          break;
        }
      }
    }
  }
  CloseMVCDemux();
  return true;
}

int CDVDInputStreamBluray::Get3dSubtitlePlane(uint16_t pid)
{
  if (!m_bMVCDisabled)
  {
    MPLS_PL *mpls = bd_get_title_mpls(m_bd);
    if (mpls)
    {
      for (int i = 0; i < mpls->list_count; i++)
      {
        for (int s = 0; s < mpls->play_item[i].stn.num_pg; s++)
        {
          if (mpls->play_item[i].stn.pg[s].pid == pid && mpls->play_item[i].stn.pg[s].ss_offset_sequence_id != 0xff)
            return mpls->play_item[i].stn.pg[s].ss_offset_sequence_id;
        }
      }
    }
  }

  return 0;
}

bool CDVDInputStreamBluray::OpenNextStream()
{
  if (m_clipQueue.empty())
    return false;

  int clip = m_clipQueue.front();
  m_clipQueue.pop();

  CDemuxMVC *pMVCDemux = dynamic_cast<CDemuxMVC*>(m_pMVCDemux);
  if (!pMVCDemux) {
    // either it's not a CDemuxMVC or it's 2D playback
    CloseMVCDemux();
    return OpenMVCDemux(clip);
  }

  // save start time for the next clip
  int64_t start_time = pMVCDemux->GetStartTime();

  CloseMVCDemux();

  bool res = OpenMVCDemux(clip);
  if (res) {
    CDemuxMVC *nextDemux = dynamic_cast<CDemuxMVC*>(m_pMVCDemux);
    if (nextDemux) {
      // set start time for next clip
      CDVDInputStream::IMenus *menu = dynamic_cast<CDVDInputStream::IMenus*>(this);
      nextDemux->SetStartTime(start_time, menu->GetSupportedMenuType());
    }
  }

  return res;
}

bool CDVDInputStreamBluray::OpenMVCDemux(int playItem)
{
  MPLS_PL *pl = bd_get_title_mpls(m_bd);
  if (!pl)
    return false;

  std::string strFileName;
  strFileName.append(m_root);
  strFileName.append("/BDMV/STREAM/");
  strFileName.append(pl->ext_sub_path[m_nMVCSubPathIndex].sub_play_item[playItem].clip->clip_id);
  strFileName.append(".m2ts");

  CLog::Log(LOGDEBUG, "CDVDInputStreamBluray::OpenMVCDemuxer(): Opening MVC extension stream at {}", strFileName);

  CFileItem fileitem(CURL(strFileName), false);
  m_pMVCInput = new CDVDInputStreamFile(fileitem, 0);

  // Try to open the MVC stream
  if (!m_pMVCInput->Open())
  {
    CloseMVCDemux();
    m_bMVCPlayback = false;
    return false;
  }

  if (m_pMVCDemux)
    delete m_pMVCDemux;

  CDemuxMVC* pMVCDemux = new CDemuxMVC;
  m_pMVCDemux = pMVCDemux;

  if (!pMVCDemux->Open(m_pMVCInput))
  {
    CloseMVCDemux();
    m_bMVCPlayback = false;
    return false;
  }

  m_nMVCClip = m_titleInfo->clips + playItem;
  return true;
}

bool CDVDInputStreamBluray::CloseMVCDemux()
{
  if (m_pMVCDemux)
  {
    delete m_pMVCDemux;
    m_pMVCDemux = nullptr;
  }

  delete m_pMVCInput;
  m_pMVCInput = nullptr;
  m_nMVCClip = nullptr;
  return true;
}

void CDVDInputStreamBluray::SeekMVCDemux(int64_t time)
{
  if (m_bMVCPlayback && m_pMVCDemux)
    m_pMVCDemux->SeekTime(time, time < GetTime());
}

void CDVDInputStreamBluray::SetupPlayerSettings()
{
  int region = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_BLURAY_PLAYERREGION);
  if ( region != BLURAY_REGION_A
    && region != BLURAY_REGION_B
    && region != BLURAY_REGION_C)
  {
    CLog::Log(LOGWARNING, "CDVDInputStreamBluray::Open - Blu-ray region must be set in setting, assuming region A");
    region = BLURAY_REGION_A;
  }
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_REGION_CODE, static_cast<uint32_t>(region));
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PARENTAL, 99);
  // Report the player's REAL 3D capability (PSR24) instead of libbluray's
  // /* TODO */ 0xffffffff "every 3D mode" placeholder. 3D output is bounded by
  // the connected display, which the box exposes via sysfs
  // (aml_display_support_3d() -> amhdmitx0/support_3d). On a non-3D display the
  // truthful value is 0 (no 3D capability), so a disc that branches on PSR24 is
  // no longer told the player can do 3D it cannot. The exact non-zero 3D-cap
  // bit layout is in the paywalled BD 3D spec and cannot be validated here (no
  // 3D display available), so the 3D-capable branch is left at 0xffffffff.
  // NOTE: libbluray's psr_init_3D() would rewrite PSR24 on 3D-disc detection,
  // but it is invoked with force=0 (bluray.c) and register.c refuses the
  // non-forced init once PSR_PROFILE_VERSION >= 0x0300 - and this player
  // declares profile 6 v3.1 (0x0310) below - so the value written here
  // survives 3D-disc detection too. Only psr_init_UHD is called forced.
  // Corollary for the open 3D/MVC work: 3D discs never receive libbluray's
  // coordinated 3D-profile PSR setup on this build.
  const bool display3d = aml_display_support_3d();
  const uint32_t threeDCap = display3d ? 0xffffffff : 0;
  CLog::Log(LOGINFO,
            "CDVDInputStreamBluray: 3D capability PSR24 0x{:08x} (display 3D: {})",
            threeDCap, display3d);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_3D_CAP, threeDCap);
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 0, 2))
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_6_v3_1);
  ApplyUHDCapabilities();
#else
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_PLAYER_PROFILE, BLURAY_PLAYER_PROFILE_5_v2_4);
#endif

  ApplyAudioCapability();

#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 5, 0))
  // Pin the UO (User Operation) restriction enforcement level explicitly.
  // RELAXED is libbluray's default; stating it here makes the persona baseline
  // deliberate rather than inherited. Do NOT raise to SAFE/COMPLIANT while
  // BD_EVENT_UO_MASK_CHANGED is still ignored in HandleEvent(): libbluray
  // would refuse a disc-masked seek/angle change (bd_seek_time returns -1 /
  // BDJ_EVENT_UO_MASKED) with no UI feedback, so the user would see a silent
  // no-op instead of a reference-player "operation prohibited" response.
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_UO_RESTRICTION_LEVEL,
                        BLURAY_PLAYER_SETTING_UO_RESTRICTION_RELAXED);
#endif

  const std::string audioLang{g_langInfo.GetDVDAudioLanguage().AsIso6392T()};
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_AUDIO_LANG, audioLang.c_str());

  const std::string subtitleLang{g_langInfo.GetDVDSubtitleLanguage().AsIso6392T()};
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_PG_LANG, subtitleLang.c_str());

  const std::string menuLang{g_langInfo.GetDVDMenuLanguage().AsIso6392T()};
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_MENU_LANG, menuLang.c_str());

  const std::string countryCode{g_langInfo.GetRegionCodeAlpha2()};
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_SETTING_COUNTRY_CODE, countryCode.c_str());

#ifdef HAVE_LIBBLURAY_BDJ
  std::string cacheDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/cache");
  std::string persistentDir = CSpecialProtocol::TranslatePath("special://userdata/cache/bluray/persistent");
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_PERSISTENT_ROOT, persistentDir.c_str());
  bd_set_player_setting_str(m_bd, BLURAY_PLAYER_CACHE_ROOT, cacheDir.c_str());
#endif
}

void CDVDInputStreamBluray::ApplyUHDCapabilities()
{
#if (BLURAY_VERSION >= BLURAY_VERSION_CODE(1, 0, 2))
  // Report REAL UHD capability registers instead of libbluray's 0xffffffff
  // placeholder. Decoded from the S&M UHD Benchmark's unencrypted
  // MovieObject.bdmv (re/sm_uhd/, object 0) with libbluray's HDMV BC subset
  // semantics (execute-next iff PSR & ~mask == 0):
  //   BC PSR25,0x02 -> GPR35=1     BC PSR26,0x04 -> GPR37=1   sum==2 -> DV
  //   BC PSR25,0x20 -> GPR36=1     BC PSR26,0x10 -> GPR38=1   sum==2 -> HDR10+
  //   BC PSR25,0x04 -> GPR53=1     BC PSR26,0x08 -> GPR52=1   (third format)
  //   neither pair complete -> HDR10 baseline path
  // The subset test means these registers carry the player's CURRENTLY
  // SELECTED output format EXCLUSIVELY - one format bit, no others, or every
  // branch fails (which is exactly what libbluray's all-ones placeholder does,
  // and why the demo menu claimed "display does not support Dolby Vision").
  // A UB820 outputting DV reports the 0x02/0x04 pair and the disc offers DV.
  {
    uint32_t uhdCap, uhdDisplayCap, hdrPreference;
    // aml_dolby_vision_enabled() = SoC support && display support && user
    // toggle, so it is the whole chain.
    if (aml_dolby_vision_enabled())
    {
      uhdCap = 0x02;         // player outputs Dolby Vision
      uhdDisplayCap = 0x04;  // display accepts Dolby Vision
      hdrPreference = 0x02;
    }
    else if (aml_display_support_hdr10plus())
    {
      uhdCap = 0x20;         // player outputs HDR10+
      uhdDisplayCap = 0x10;  // display accepts HDR10+
      hdrPreference = 0x20;
    }
    else
    {
      uhdCap = 0x01;         // HDR10 baseline
      uhdDisplayCap = 0x02;
      hdrPreference = 0x01;
    }

    // Empirical probe hook: override the computed values from a file (three
    // whitespace-separated hex/dec values: psr25 psr26 psr27) for on-box
    // probing against real discs without a rebuild.
    const std::string overridePath =
        CSpecialProtocol::TranslatePath("special://profile/psr_uhd_override.txt");
    XFILE::CFile overrideFile;
    std::vector<uint8_t> buf;
    if (overrideFile.LoadFile(overridePath, buf) > 0)
    {
      const std::string content(buf.begin(), buf.end());
      int v25, v26, v27;
      if (sscanf(content.c_str(), "%i %i %i", &v25, &v26, &v27) == 3)
      {
        uhdCap = static_cast<uint32_t>(v25);
        uhdDisplayCap = static_cast<uint32_t>(v26);
        hdrPreference = static_cast<uint32_t>(v27);
        CLog::Log(LOGWARNING,
                  "CDVDInputStreamBluray: UHD capability PSRs OVERRIDDEN from {}",
                  overridePath);
      }
    }

    CLog::Log(LOGINFO,
              "CDVDInputStreamBluray: UHD output-format PSRs: "
              "UHD_CAP 0x{:02x} UHD_DISPLAY_CAP 0x{:02x} HDR_PREFERENCE 0x{:02x}",
              uhdCap, uhdDisplayCap, hdrPreference);
    bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_UHD_CAP, uhdCap);
    bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_UHD_DISPLAY_CAP, uhdDisplayCap);
    bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_HDR_PREFERENCE, hdrPreference);
  }
#endif
}

void CDVDInputStreamBluray::ApplyAudioCapability()
{
  // Report the player's REAL audio capability (PSR15) derived from the connected
  // HDMI sink, instead of libbluray's static 0xAAAA "every surround format"
  // default. A disc's HDMV/BD-J startup logic reads PSR15 to decide which audio
  // experience to offer; feeding the true chain lets a reduced sink (a TV with
  // no HD-audio AVR, or a stereo-only output) branch the way a reference player
  // would, rather than the player always claiming every format.
  //
  // PSR15 is a genuine capability OR-mask, NOT the single-format subset-select
  // idiom of the UHD PSRs. Bits are per libbluray player_settings.h; each
  // codec's max-channel count (parsed from aud_cap by AMLUtils) selects its
  // surround (>2 ch) vs stereo-only bit.
  //
  // Clobber note: psr_init_UHD()/psr_init_3D() do NOT touch PSR15 (verified in
  // libbluray register.c), so this single write in SetupPlayerSettings survives
  // disc-type detection - no re-apply after bd_get_disc_info() is required.
  const AMLHdmiAudioCaps caps = aml_get_hdmi_audio_caps();

  // No readable sink descriptor -> keep libbluray's full default rather than
  // under-report (matches "if hardware truth is unavailable, don't regress").
  if (!caps.valid)
  {
    CLog::Log(LOGINFO,
              "CDVDInputStreamBluray: audio capability PSR15 - no aud_cap node, "
              "leaving libbluray default");
    return;
  }

  // Surround bit when >2 ch (or channel count unknown), stereo-only bit for
  // 1-2 ch, nothing when the codec is absent. Each ACAP format is a 2-bit
  // field - exactly one state bit is set, never both.
  auto pick = [](int ch, uint32_t surroundBit, uint32_t stereoBit) -> uint32_t {
    if (ch == 0)
      return 0;
    return (ch > 2 || ch < 0) ? surroundBit : stereoBit;
  };
  // A capability advertised on two aud_cap lines must still collapse to ONE
  // state of its 2-bit field: unknown (-1, assume surround) dominates, else
  // the larger channel count.
  auto combine = [](int a, int b) -> int {
    if (a == 0)
      return b;
    if (b == 0)
      return a;
    if (a < 0 || b < 0)
      return -1;
    return a > b ? a : b;
  };

  uint32_t acap = 0;

  // LPCM 48/96kHz is spec-mandatory, so it is always reported; its surround vs
  // stereo-only state follows the PCM line's channel count (defaulting to
  // surround if the mandatory PCM line is somehow unreadable). 192kHz LPCM is
  // optional and listed among the PCM line's sample rates.
  acap |= pick(caps.pcm_ch != 0 ? caps.pcm_ch : -1, BLURAY_ACAP_LPCM_48_96_SURROUND,
               BLURAY_ACAP_LPCM_48_96_STEREO_ONLY);
  if (caps.pcm_192k)
    acap |= pick(caps.pcm_ch, BLURAY_ACAP_LPCM_192_SURROUND, BLURAY_ACAP_LPCM_192_STEREO_ONLY);

  // Dolby TrueHD / Atmos-MAT (MLP lossless).
  acap |= pick(caps.truehd_ch, BLURAY_ACAP_MLP_SURROUND, BLURAY_ACAP_MLP_STEREO_ONLY);

  // Dolby Digital Plus; Atmos = dependent (JOC) substream.
  acap |= pick(caps.ddp_ch, BLURAY_ACAP_DDPLUS_SURROUND, BLURAY_ACAP_DDPLUS_STEREO_ONLY);
  if (caps.ddp_atmos)
    acap |= pick(caps.ddp_ch, BLURAY_ACAP_DDPLUS_DEP_SURROUND,
                 BLURAY_ACAP_DDPLUS_DEP_STEREO_ONLY);

  // Dolby Digital (AC-3).
  acap |= pick(caps.ac3_ch, BLURAY_ACAP_DD_SURROUND, BLURAY_ACAP_DD_STEREO_ONLY);

  // DTS-HD (Master Audio / High-Res) = DTS core + extension substream; a plain
  // DTS line advertises the core too, so the core state combines both lines.
  acap |= pick(combine(caps.dtshd_ch, caps.dts_ch), BLURAY_ACAP_DTSHD_CORE_SURROUND,
               BLURAY_ACAP_DTSHD_CORE_STEREO_ONLY);
  acap |= pick(caps.dtshd_ch, BLURAY_ACAP_DTSHD_EXT_SURROUND,
               BLURAY_ACAP_DTSHD_EXT_STEREO_ONLY);

  // Empirical probe hook: override the computed mask from a file (single hex/dec
  // PSR15 value) for on-box A/B against real discs / the reference player with
  // no rebuild - mirrors psr_uhd_override.txt.
  const std::string overridePath =
      CSpecialProtocol::TranslatePath("special://profile/psr15_override.txt");
  XFILE::CFile overrideFile;
  std::vector<uint8_t> buf;
  if (overrideFile.LoadFile(overridePath, buf) > 0)
  {
    const std::string content(buf.begin(), buf.end());
    int v15;
    if (sscanf(content.c_str(), "%i", &v15) == 1)
    {
      acap = static_cast<uint32_t>(v15);
      CLog::Log(LOGWARNING,
                "CDVDInputStreamBluray: audio capability PSR15 OVERRIDDEN from {}",
                overridePath);
    }
  }

  CLog::Log(LOGINFO,
            "CDVDInputStreamBluray: audio capability PSR15 0x{:04x} from sink "
            "(ch: PCM {} MAT {} DD+ {} AC-3 {} DTS-HD {} DTS {})",
            acap, caps.pcm_ch, caps.truehd_ch, caps.ddp_ch, caps.ac3_ch, caps.dtshd_ch,
            caps.dts_ch);
  bd_set_player_setting(m_bd, BLURAY_PLAYER_SETTING_AUDIO_CAP, acap);
}

bool CDVDInputStreamBluray::OpenStream(CFileItem &item)
{
  m_pstream = std::make_unique<CDVDInputStreamFile>(
      item, XFILE::READ_TRUNCATED | XFILE::READ_BITRATE | XFILE::READ_NO_CACHE);

  if (!m_pstream->Open())
  {
    CLog::Log(LOGERROR, "Error opening image file {}", CURL::GetRedacted(item.GetPath()));
    Close();
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::GetState(std::string& xmlstate)
{
  if (!m_bd || !m_titleInfo)
  {
    return false;
  }

  BlurayState blurayState;
  blurayState.playlistId = m_titleInfo->playlist;

  if (!m_blurayStateSerializer.BlurayStateToXML(xmlstate, blurayState))
  {
    CLog::LogF(LOGWARNING, "Failed to serialize Bluray state");
    return false;
  }

  return true;
}

bool CDVDInputStreamBluray::SetState(const std::string& xmlstate)
{
  if (!m_bd)
    return false;

  BlurayState blurayState;
  if (!m_blurayStateSerializer.XMLToBlurayState(blurayState, xmlstate))
  {
    CLog::LogF(LOGWARNING, "Failed to deserialize Bluray state");
    return false;
  }

  m_titleInfo = bd_get_playlist_info(m_bd, blurayState.playlistId, 0);
  if (!m_titleInfo)
  {
    CLog::LogF(LOGERROR, "Open - failed to get title info");
    return false;
  }

  if (!bd_select_playlist(m_bd, m_titleInfo->playlist))
  {
    CLog::LogF(LOGERROR, "Open - failed to select playlist {}", m_titleInfo->idx);
    return false;
  }

  return true;
}

void CDVDInputStreamBluray::SaveCurrentState(const CStreamDetails& details)
{
  std::unique_lock lock(m_statesLock);

  if (!m_titleInfo)
    return;

  // Details for this playlist
  SavePlaylistDetails(m_playedPlaylists, m_startWatchTime,
                      {.playlist = static_cast<int>(m_titleInfo->playlist),
                       .inMenu = m_isInMainMenu,
                       .duration = std::chrono::milliseconds(GetTotalTime()),
                       .details = details});

  // Reset watch timer for next playlist
  m_startWatchTime = std::chrono::steady_clock::now();
}

CDVDInputStream::UpdateState CDVDInputStreamBluray::UpdateItemFromSavedStates(CFileItem& item,
                                                                              double time,
                                                                              bool& closed)
{
  std::unique_lock lock(m_statesLock);

  // First add current state to the list of playlist states
  if (item.HasVideoInfoTag())
    SaveCurrentState(item.GetVideoInfoTag()->m_streamDetails);

  return UpdateItemFromPlaylistDetails(DVDSTREAM_TYPE_BLURAY, m_playedPlaylists, item, time,
                                       closed);
}

void CDVDInputStreamBluray::UpdateStack(CFileItem& item)
{
  return UpdateStackItem(item,
                         m_titleInfo ? std::chrono::milliseconds(m_titleInfo->duration / 90) : 0ms);
}
