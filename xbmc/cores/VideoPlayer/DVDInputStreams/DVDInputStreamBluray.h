/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "BlurayStateSerializer.h"
#include "DVDInputStream.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "threads/CriticalSection.h"
#if defined(HAS_UDFREAD)
#include "filesystem/UDFContext.h"
#endif

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <queue>
#include <vector>

extern "C"
{
#include <libbluray/bluray.h>
#include <libbluray/bluray-version.h>
#include <libbluray/keys.h>
#include <libbluray/overlay.h>
#include <libbluray/player_settings.h>
#include "DVDInputStreamFile.h"
}

#define MAX_PLAYLIST_ID 99999
#define MAX_CLIP_ID 99999
#define BD_EVENT_MENU_OVERLAY -1
#define BD_EVENT_MENU_ERROR   -2
#define BD_EVENT_ENC_ERROR    -3

#define HDMV_PID_VIDEO            0x1011
#define HDMV_PID_VIDEO_EL         0x1015
#define HDMV_PID_AUDIO_FIRST      0x1100
#define HDMV_PID_AUDIO_LAST       0x111f
#define HDMV_PID_PG_FIRST         0x1200
#define HDMV_PID_PG_LAST          0x121f
#define HDMV_PID_PG_HDR_FIRST     0x12a0
#define HDMV_PID_PG_HDR_LAST      0x12bf
#define HDMV_PID_IG_FIRST         0x1400
#define HDMV_PID_IG_LAST          0x141f

class CDVDOverlayImage;
class IVideoPlayer;
class CDVDDemux;

/* Presentation-side playlist identity for the OSD
 * (docs/bd_timeline_events_design.md Phase 2): chapter list + total
 * duration as the VIEWER should see them. The demux-side m_titleInfo swaps
 * the moment the VM changes playlist; with deep buffers the outgoing
 * playlist keeps presenting for seconds, so the IChapter/IDisplayTime UI
 * accessors read a snapshot of this shape which the player applies on the
 * render clock (same timeline queue as the presented menu state). At
 * namespace scope so VideoPlayer.h can forward-declare it. */
struct BlurayTitleUiSnapshot
{
  uint32_t playlist = MAX_PLAYLIST_ID + 1;
  int totalTimeMs = 0;
  struct SChapter
  {
    int64_t startMs = 0;
    std::string name;
  };
  std::vector<SChapter> chapters;
};

class CDVDInputStreamBluray
  : public CDVDInputStream
  , public CDVDInputStream::IDisplayTime
  , public CDVDInputStream::IChapter
  , public CDVDInputStream::IPosTime
  , public CDVDInputStream::IMenus
  , public CDVDInputStream::IExtentionStream
{
public:
  CDVDInputStreamBluray() = delete;
  CDVDInputStreamBluray(IVideoPlayer* player, const CFileItem& fileitem);
  ~CDVDInputStreamBluray() override;
  bool Open() override;
  void Close() override;
  int Read(uint8_t* buf, int buf_size) override;
  int ReadBlocks(uint8_t* buf, int lba, int num_blocks);
  int64_t Seek(int64_t offset, int whence) override;
  void Abort() override;
  bool IsEOF() override;
  int64_t GetLength() override;
  int GetBlockSize() override { return 6144; }
  ENextStream NextStream() override;


  /* IMenus */
  void ActivateButton() override { UserInput(BD_VK_ENTER); }
  void SelectButton(int iButton) override
  {
    if(iButton < 10)
      UserInput((bd_vk_key_e)(BD_VK_0 + iButton));
  }
  int  GetCurrentButton() override { return 0; }
  int  GetTotalButtons() override { return 0; }
  void OnUp() override  { UserInput(BD_VK_UP); }
  void OnDown() override  { UserInput(BD_VK_DOWN); }
  void OnLeft() override { UserInput(BD_VK_LEFT); }
  void OnRight() override { UserInput(BD_VK_RIGHT); }

  /*! \brief Open the Menu
  * \return true if the menu is successfully opened, false otherwise
  */
  bool OnMenu(MenuCall type = MenuCall::Auto) override;
  void OnBack() override
  {
    if(IsInMenu())
      OnMenu();
  }
  void OnNext() override {}
  void OnPrevious() override {}

  /*!
   * \brief Get the supported menu type
   * \return The supported menu type
  */
  MenuType GetSupportedMenuType() override;

  bool IsInMenu() override;
  // DEMUX-side menu truth (m_menu/overlay as the VM decided it, NOT the
  // clock-deferred presented state): for demux-side machinery operating on
  // packets at demux time (CheckContinuity menu-wrap correction) - the
  // presented state lags by the deferral window and would miss wraps there
  bool IsInMenuDemux() const { return m_menu || m_hasOverlay; }
  // True while playing menu-incidental video (FirstPlay bumper, top menu,
  // or any segment with a menu/overlay up) - used by the disc-session DV
  // latch to VS10-map such segments into the DV output.
  bool IsMenuDomainVideo();
  /*! \brief True when the current playlist declares PQ-authored graphics, i.e.
   * its video stream is HDR10 or Dolby Vision in the MPLS STN table.
   *
   * Both the BD-J overlay path and the PG (subtitle) palette path need the
   * pre-invert in that case. The PG decoder is a separate codec with no view
   * of the input stream, so CVideoPlayer stamps this onto the subtitle
   * CDVDStreamInfo - the same way it stamps hint.stills from
   * IsMenuDomainVideo(). Atomic: written on the player thread, read when a
   * subtitle stream is opened. */
  bool HasPqAuthoredGraphics() const { return m_pqAuthoredGraphics; }
  bool OnMouseMove(const CPoint &point) override  { return MouseMove(point); }
  bool OnMouseClick(const CPoint &point) override { return MouseClick(point); }
  void SkipStill() override;
  bool GetState(std::string& xmlstate) override;
  bool SetState(const std::string& xmlstate) override;
  bool CanSeek() override;


  void UserInput(bd_vk_key_e vk);
  bool MouseMove(const CPoint &point);
  bool MouseClick(const CPoint &point);

  int GetChapter() override;
  int GetChapterCount() override;
  void GetChapterName(std::string& name, int ch = -1) override;
  std::chrono::milliseconds GetChapterPos(int ch) override;
  bool SeekChapter(int ch) override;

  CDVDInputStream::IDisplayTime* GetIDisplayTime() override { return this; }
  int GetTotalTime() override;
  int GetTime() override;

  CDVDInputStream::IPosTime* GetIPosTime() override { return this; }
  bool PosTime(int ms) override;

  void GetStreamInfo(int pid, std::string &language);

  // Disc-authoritative (playlist STN) HDR metadata for a mpeg-ts PID.
  // Returns true if the pid is listed in the current clip's stream tables;
  // flags report the DV extension table / per-stream HDR10+ attribute.
  bool GetDiscStreamHdrMetadata(int pid, bool& isDolbyVision, bool& isHdrPlus);

  int Get3dSubtitlePlane(uint16_t pid);

  /*!
   * \brief Check whether a stream is the default of the playlist being played, ie. audio stream
   *        number 1 or presentation graphic stream number 1 of the current clip.
   * \param pid The packet identifier of the stream
   * \return True if the stream is the default audio or subtitle stream, false otherwise
   */
  bool IsDefaultStream(int pid) const;

  void OverlayCallback(const BD_OVERLAY * const);
#ifdef HAVE_LIBBLURAY_BDJ
  void OverlayCallbackARGB(const struct bd_argb_overlay_s * const);
#endif
  void RedrawMenuOverlays();

  /* the pending NEXTSTREAM_OPEN crossed the menu boundary:
   * - menu->title (user started a title): always drop the queued menu-loop
   *   remainder.
   * - title->menu: drop the queued feature remainder ONLY when the user
   *   recently called the menu (they abandoned it - draining up to ~19s of
   *   movie before the menu appears was review finding A12). A NATURAL
   *   end-of-title return to menu must NOT discard: the queued tail is the
   *   movie's ending, still unpresented (dropping it would cut the last
   *   queue-depth seconds of every film).
   * Same-state transitions keep their existing classes. */
  /* The decision and its reason come from ONE place, so a log line can never
   * drift from the behaviour it describes.
   *
   * This is spelled out because the inputs are otherwise invisible in a log:
   * m_menu is set silently by BD_EVENT_TITLE (see ProcessEvent) as well as by
   * the logged BD_EVENT_MENU, so a discard cannot be attributed to a branch
   * from the event lines alone - a menu->title discard and a title->menu
   * discard look identical unless the reason is recorded here. */
  enum class QueueDecision : uint8_t
  {
    KEEP_TITLE_TO_TITLE,
    KEEP_MENU_TO_MENU,
    KEEP_TITLE_TO_MENU_NATURAL,
    DISCARD_MENU_TO_TITLE,
    DISCARD_TITLE_TO_MENU_USER,
  };

  QueueDecision ClassifyStreamQueue() const
  {
    if (m_menuAtHold && !m_menu)
      return QueueDecision::DISCARD_MENU_TO_TITLE;
    if (!m_menuAtHold && m_menu)
      return m_lastUserMenuCall.has_value() &&
                     std::chrono::steady_clock::now() - *m_lastUserMenuCall <
                         std::chrono::seconds(20)
                 ? QueueDecision::DISCARD_TITLE_TO_MENU_USER
                 : QueueDecision::KEEP_TITLE_TO_MENU_NATURAL;
    return m_menuAtHold ? QueueDecision::KEEP_MENU_TO_MENU
                        : QueueDecision::KEEP_TITLE_TO_TITLE;
  }

  static const char* DescribeQueueDecision(QueueDecision decision)
  {
    switch (decision)
    {
      case QueueDecision::KEEP_TITLE_TO_TITLE:
        return "title->title, nothing to discard";
      case QueueDecision::KEEP_MENU_TO_MENU:
        return "menu->menu, nothing to discard";
      case QueueDecision::KEEP_TITLE_TO_MENU_NATURAL:
        return "title->menu with no recent user menu call - draining the feature tail";
      case QueueDecision::DISCARD_MENU_TO_TITLE:
        return "menu->title - dropping the queued menu remainder";
      case QueueDecision::DISCARD_TITLE_TO_MENU_USER:
        return "title->menu after a user menu call - dropping the queued feature remainder";
    }
    return "unclassified";
  }

  bool ShouldDiscardStreamQueue() const
  {
    const QueueDecision decision = ClassifyStreamQueue();
    return decision == QueueDecision::DISCARD_MENU_TO_TITLE ||
           decision == QueueDecision::DISCARD_TITLE_TO_MENU_USER;
  }

  /* the pending NEXTSTREAM_OPEN is a playitem advance within the same playlist:
   * the stream format is unchanged, so the whole pipeline (demuxer, stream
   * players, decoders) can keep running across the boundary (no close/reopen ->
   * no DV re-latch/toast/black, no per-segment blank). Applies to menu loop
   * segments AND multi-playitem titles (seamless-branch playlists: concert
   * per-song playitems, the S&M sync-test loop) - a reference player (UB820)
   * plays these gaplessly, and the close/reopen path is what blanked every
   * ~24s segment. Cleared by any playlist-scope event (playlist/title/seek/
   * angle), so playlist changes and seeks still take the full reopen. */
  bool IsSeamlessStreamChange() const { return m_seamlessHold; }

  /* disc carries BD-J titles: the menu->title decoder keep-alive is scoped to
   * HDMV-only discs until the BD-J interaction (avformat teardown crash under
   * the JVM's signal handlers) is understood */
  bool HasBDJTitles() const { return m_hasBdjTitles; }

  /* presentation-side menu state: the player applies the demux-side menu
   * flip (m_menu) when the render clock reaches the demux position where
   * the VM flipped it (docs/bd_timeline_events_design.md). Consumed by
   * IsInMenu(); demux-side machinery (ShouldDiscardStreamQueue,
   * IsMenuDomainVideo) keeps reading m_menu directly. */
  void SetPresentedMenuState(bool menu) { m_menuPresented = menu; }

  /* presentation-side playlist identity (see BlurayTitleUiSnapshot): applied
   * by the player's timeline queue when the render clock reaches the demux
   * position of the playlist change. Read by the IChapter/IDisplayTime UI
   * accessors; demux machinery (stream tables, dictation, MVC, state
   * save/restore) keeps reading m_titleInfo directly. */
  void SetPresentedTitleUi(const std::shared_ptr<const BlurayTitleUiSnapshot>& ui)
  {
    if (ui)
      m_titleUiPresented = ui;
  }

  /* TS PID of the disc-dictated audio/PG stream, resolved LIVE against the
   * CURRENT playitem's stream table. The BD stream-selection events are
   * edge-triggered (they fire only when the stream NUMBER changes) and any
   * PID cached at event time goes stale the moment a playitem with a
   * different stream layout starts; resolving number->PID on demand keeps
   * the dictation valid across every segment transition. -1 when the
   * current clip carries no such stream (e.g. silent still patterns). */
  int GetDictatedAudioPid() const
  {
    if (!m_titleInfo || !m_clip || m_audioStreamNum < 1 ||
        m_clip->audio_stream_count < m_audioStreamNum)
      return -1;
    return m_clip->audio_streams[m_audioStreamNum - 1].pid;
  }
  int GetDictatedPgPid() const
  {
    if (!m_titleInfo || !m_clip || m_pgStreamNum < 1 ||
        m_clip->pg_stream_count < m_pgStreamNum)
      return -1;
    return m_clip->pg_streams[m_pgStreamNum - 1].pid;
  }

  /* the disc explicitly deselected primary audio (BD_EVENT_AUDIO_STREAM
   * 0xff): dictated silence - the player must NOT fall back to opening the
   * first arriving audio pid (that turns authored silence into sound) */
  bool IsAudioDictatedNone() const { return m_audioStreamNum == BD_STREAM_NONE; }

  /* User stream selection routed through the HDMV VM (bd_select_stream):
   * updates PSR1/PSR2 so the VM, the dictation getters above and the player
   * all agree - without this the per-packet dictation reverts any manual
   * track choice within one packet. Respects the disc's UO masks; returns
   * false when the disc prohibits the change (caller keeps the dictated
   * stream). pid is the mpeg-ts pid of the wanted stream in the CURRENT
   * clip. */
  bool SelectAudioStream(int pid);
  bool SelectSubtitleStream(int pid, bool enable);

  /* current BLURAY_UO_* mask (BD_EVENT_UO_MASK_CHANGED); 0 = everything
   * permitted. Consulted by seek/skip gating - the UO restriction level
   * stays RELAXED in libbluray, enforcement is app-side so the user gets
   * feedback instead of a silent no-op. */
  uint32_t GetUserOperationMask() const { return m_uoMask.load(); }

  /* BD-J key interest table (BLURAY_KIT_*): which transport UOs the running
   * Xlet asked to handle itself. libbluray's bd_user_input has no transport
   * key codes, so full delegation is not possible - exposed for logging and
   * future routing decisions. */
  uint32_t GetBdjKeyInterest() const { return m_bdjKeyInterest.load(); }

  BLURAY_TITLE_INFO* GetTitleFromState(const std::string& xmlstate);
  BLURAY_TITLE_INFO* GetTitleLongest();
  BLURAY_TITLE_INFO* GetTitleFile(const std::string& name);
  bool DiscHasDolbyVision();

  /*! \brief Refresh m_pqAuthoredGraphics from the current playitem's STN table.
   * Player thread only; call wherever m_clip changes. */
  void UpdatePqAuthoredGraphics();

  void ProcessEvent();
  CDVDDemux* GetExtentionDemux() override { return m_pMVCDemux; };
  bool HasExtention() override { return m_bMVCPlayback; }
  bool AreEyesFlipped() override { return m_bFlipEyes; }
  void DisableExtention() override;
  bool OpenNextStream() override;

  void SaveCurrentState(const CStreamDetails& details) override;
  UpdateState UpdateItemFromSavedStates(CFileItem& item, double time, bool& closed) override;
  void UpdateStack(CFileItem& item) override;

protected:
  struct SPlane;

  void OverlayFlush(int64_t pts, bool keepAliveEligible = false);
  void OverlayClose();
  static void OverlayClear(SPlane& plane, int x, int y, int w, int h);
  static void OverlayInit (SPlane& plane, int w, int h);
  bool ProcessItem(int playitem);

  bool OpenMVCDemux(int playItem);
  bool CloseMVCDemux();
  void SeekMVCDemux(int64_t time);

  IVideoPlayer* m_player = nullptr;
  BLURAY* m_bd = nullptr;
  const BLURAY_TITLE* m_title = nullptr;
  BLURAY_TITLE_INFO* m_titleInfo = nullptr;
  uint32_t m_playlist = MAX_PLAYLIST_ID + 1;
  BLURAY_CLIP_INFO* m_clip = nullptr;

  //! The clip information of the play item being played, ie. every stream its m2ts carries (see
  //! GetStreamInfo). Owned, and only valid while m_clip refers to the same play item.
  struct clpi_cl* m_clipInfo = nullptr;
  uint32_t m_angle = 0;
  /* atomics: m_menu is written on the player thread but read by GUI/app
   * threads (IsInMenu chain); m_menuPresented likewise; m_hasOverlay is
   * additionally WRITTEN from the BD-J JVM graphics thread (ARGB overlay
   * callback) - see m_overlayLock for the container itself */
  std::atomic<bool> m_menu{false};
  std::atomic<bool> m_menuPresented{false};
  /* never null - starts as an empty snapshot (playlist unset), bootstrapped
   * at open / first ProcessItem, then swapped by SetPresentedTitleUi */
  std::shared_ptr<const BlurayTitleUiSnapshot> m_titleUiPresented =
      std::make_shared<BlurayTitleUiSnapshot>();
  std::shared_ptr<const BlurayTitleUiSnapshot> BuildTitleUiSnapshot() const;
  /* demux-side chapter position (m_titleInfo) - for seek mechanics that must
   * act on the playlist the demuxer is actually in (MVC sub-demux seek) */
  std::chrono::milliseconds ChapterPosDemux(int ch) const;
  bool m_menuAtHold = false;
  bool m_seamlessHold = false;
  /* last explicit user menu call (OnMenu) - discriminates "user abandoned
   * the feature for the menu" (discard queued tail) from "the feature
   * ended and the disc returned to menu" (drain it). Player thread only.
   *
   * std::optional, deliberately NOT a sentinel time_point: steady_clock's
   * duration is 64-bit nanoseconds, so now() - time_point::min() overflows
   * and wraps NEGATIVE, which compares as "called just now". That made every
   * natural end-of-title return to the menu discard the film's queued ending
   * until the viewer happened to press Menu once. A value-initialised {} is
   * no better - steady_clock's epoch is boot, so it misreads for the first
   * 20s of uptime. */
  std::optional<std::chrono::steady_clock::time_point> m_lastUserMenuCall;
  /* "no stream selected" sentinel (PSR semantics: PSR1=0xff / PSR2=0x0fff
   * both exceed any clip stream count, so the dictation getters resolve
   * them to -1) */
  static constexpr uint32_t BD_STREAM_NONE = 0xff;
  /* current disc-dictated stream NUMBERS (1-based, PSR semantics). Audio
   * defaults to stream 1 (PSR1 init 0xff = "player decides"; stream 1 is
   * the sane player decision). PG defaults to NONE: PSR2 init is 0x0fff
   * "no stream selected" (libbluray register.c) - defaulting PG to 1 made
   * a subtitle track the disc never selected auto-open. Updated by the
   * stream-selection events; resolved to PIDs on demand via
   * GetDictatedAudioPid/GetDictatedPgPid. */
  uint32_t m_audioStreamNum = 1;
  uint32_t m_pgStreamNum = BD_STREAM_NONE;
  bool m_hasBdjTitles = false;
  bool m_isInMainMenu = false;
  std::atomic<bool> m_hasOverlay{false};
  /* BD-J ARGB flush-cadence tracker (guarded by m_overlayLock, written on the
   * JVM graphics thread): a composition that has been re-posted at a sustained
   * high cadence is one whose visibility is maintained by continuous re-posts -
   * when they stop, the composition was abandoned (BD-J sends no clear event)
   * and must expire instead of freezing on screen. Draw-once compositions
   * (static popups, all HDMV menus) never reach the streak threshold and keep
   * full persist-forever semantics. */
  int64_t m_argbFlushLastTick = 0;
  int m_argbFlushStreak = 0;
  /* diagnostics only: tracks whether the keep-alive streak is currently
   * earned, so OverlayFlush logs the transitions instead of every flush */
  bool m_argbKeepAliveActive = false;
  /* keep-alive stamp of the most recent LIVE ARGB flush (0 = that composition
   * was not keep-alive). Non-eligible flushes (RedrawMenuOverlays reposting
   * retained plane content after a stream reopen) inherit this UN-renewed, so
   * an abandoned composition stays expired after the repost instead of being
   * resurrected as persist-forever, while a live one keeps rendering (its
   * ongoing re-posts renew it immediately anyway). */
  int64_t m_argbLastKeepAliveTick = 0;
  /* popup-menu availability announced by the disc (BD_EVENT_POPUP) - lets
   * OnMenu() try the right key first instead of firing BD_VK_POPUP blind */
  std::atomic<bool> m_popupAvailable{false};
  std::atomic<uint32_t> m_uoMask{0};
  std::atomic<uint32_t> m_bdjKeyInterest{0};
  bool m_navmode = false;
  // Latched true at Open when this disc's DV session is engaged (DV disc + DV
  // display); held for the whole disc session. Gates the BD-J ARGB overlay's
  // PQ->sRGB pre-inversion: a STABLE signal that correctly predicts the DV/PQ
  // output plane, unlike the live aml_dv_get_output_mode() which flips across
  // the repeated OpenDecoder calls of a movie-load transition (that flicker
  // baked half of the resume menu washed and half correct).
  bool m_dvDiscSession = false;

  /*! \brief True while the CURRENT playitem's video is HDR, i.e. its graphics are
   * authored in BT.2020 ST.2084 (PQ).
   *
   * BD-ROM 3.x keys the graphics regime on the playlist's dynamic range, not on
   * whether the disc carries Dolby Vision: on any HDR playlist, BD-J and IG
   * graphics are authored directly in BT.2020 PQ (they never pass through the DV
   * composer). m_dvDiscSession only covers DV discs, so a plain HDR10 UHD had its
   * PQ graphics treated as sRGB and encoded a second time - the washed grey-blue
   * this pre-inversion exists to prevent.
   *
   * Taken from the clip's own STN table (bd_stream_info::dynamic_range_type), so
   * it is disc-authored STATIC metadata that is stable for the whole playitem -
   * deliberately not the volatile aml_dv_get_output_mode(), which flips across a
   * movie-load transition and once baked half the resume menu washed.
   *
   * Resolved per PLAYLIST (with a first-clip fallback), refreshed wherever
   * m_titleInfo is rebuilt as well as at BD_EVENT_PLAYITEM, and never reset to a
   * "regime unknown" false while a playlist is being swapped - see
   * UpdatePqAuthoredGraphics and FreeTitleInfo for why that stability matters.
   *
   * Atomic: written on the player thread, read per draw on the BD-J thread in
   * OverlayCallbackARGB. */
  std::atomic<bool> m_pqAuthoredGraphics{false};
  bool m_pqRegimeLogged = false;
  int m_dispTimeBeforeRead = 0;
  int                 m_nTitles = -1;
  std::string         m_root;

  // MVC related members
  CDVDDemux*          m_pMVCDemux = nullptr;
  CDVDInputStream    *m_pMVCInput = nullptr;
  bool                m_bMVCPlayback = false;
  int                 m_nMVCSubPathIndex = 0;
  BLURAY_CLIP_INFO*   m_nMVCClip = nullptr;
  bool                m_bFlipEyes = false;
  bool                m_bMVCDisabled = false;
  uint64_t            m_clipStartTime = 0;
  std::queue<int>     m_clipQueue;

  typedef std::shared_ptr<CDVDOverlayImage> SOverlay;
  typedef std::list<SOverlay> SOverlays;

  struct SPlane
  {
    SOverlays o;
    int w = 0;
    int h = 0;
  };

  /* index = bd_overlay_plane_e: 0 PG, 1 IG (above PG), 2 BG (behind video,
   * libbluray 1.5.0). Composited bottom-up as BG, PG, IG in OverlayFlush.
   *
   * m_overlayLock: the planes are mutated by libbluray overlay callbacks -
   * HDMV inside bd_read_ext on the player thread, but BD-J ARGB callbacks
   * arrive on the JVM graphics thread - while RedrawMenuOverlays iterates
   * them on the player thread (repost after demuxer reopen / display
   * reset). Every reader/writer of m_planes takes this lock (it is a
   * recursive CCriticalSection: OverlayFlush runs both standalone and
   * inside a locked callback). */
  mutable CCriticalSection m_overlayLock;
  SPlane m_planes[3];
  enum EHoldState {
    HOLD_NONE = 0,
    HOLD_HELD,
    HOLD_DATA,
    HOLD_STILL,
    HOLD_ERROR,
    HOLD_EXIT
  } m_hold = HOLD_NONE;
  BD_EVENT m_event;
#ifdef HAVE_LIBBLURAY_BDJ
  struct bd_argb_buffer_s m_argb;
#endif

  private:
    bool OpenStream(CFileItem &item);
    void SetupPlayerSettings();
    void ApplyUHDCapabilities();
    void ApplyAudioCapability();
    void FreeTitleInfo();
    void FreeClipInfo();

    /*!
     * \brief Read the clip information of a play item of the playlist being played.
     * \param playItem The index of the play item, as BD_EVENT_PLAYITEM reports it
     */
    void UpdateClipInfo(unsigned int playItem);

    /*!
     * \brief Find the language of a stream in the stream number table of the current play item.
     * \param pid The packet identifier of the stream
     * \param language Filled in with the language of the stream, if it is found
     * \return True if the playlist presents the stream, false otherwise
     */
    bool GetPlaylistStreamLanguage(int pid, std::string& language) const;

    /*!
     * \brief Find the language of a stream in the clip information of the current play item.
     * \param pid The packet identifier of the stream
     * \param language Filled in with the language of the stream, if it is found
     * \return True if the clip carries the stream, false otherwise
     */
    bool GetClipStreamLanguage(int pid, std::string& language) const;
    void LogTitleAppInfo();
    /* re-evaluate the libbluray debug mask (DBG_HDMV rides the Kodi debug
     * loglevel; called at segment boundaries so a mid-session ToggleDebug
     * takes effect without a disc reopen) */
    void UpdateLibblurayDebugMask();
    /* stream-attribute compare across a glued playitem seam: cc=1 permits
     * attribute/STN changes, and a format change must never be glued into
     * live decoders (falls back to the full-reopen path instead) */
    static bool ClipFormatsMatch(const BLURAY_CLIP_INFO* a, const BLURAY_CLIP_INFO* b);
    /* IG button sound effects (sound.bdmv): decoded LPCM from libbluray,
     * cached as AE sounds at open, fired by BD_EVENT_SOUND_EFFECT */
    void LoadMenuSounds();
    void FreeMenuSounds();
    void PlayMenuSound(uint32_t id);
    std::vector<IAE::SoundPtr> m_menuSounds;
    /* menu-domain classification change log guard (-1 = not yet logged) */
    int m_menuDomainLogged = -1;
    std::unique_ptr<CDVDInputStreamFile> m_pstream;
    std::string m_rootPath;

#if defined(HAS_UDFREAD)
    // Keeps a disc image's UDF volume mounted for as long as the disc is open
    std::optional<XFILE::CUDFMount> m_udfMount;
#endif

    /*! Bluray state serializer handler */
    CBlurayStateSerializer m_blurayStateSerializer;

    /* used during bd_open_stream read block*/
    CCriticalSection m_readBlocksLock;

    std::chrono::steady_clock::time_point m_startWatchTime{};
    std::vector<PlaylistInformation> m_playedPlaylists;
    CCriticalSection m_statesLock;
};
