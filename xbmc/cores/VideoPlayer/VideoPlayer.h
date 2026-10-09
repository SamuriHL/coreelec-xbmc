/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDClock.h"
#include "DVDMessageQueue.h"
#include "Edl.h"
#include "FileItem.h"
#include "IVideoPlayer.h"
#include "VideoPlayerAudioID3.h"
#include "VideoPlayerRadioRDS.h"
#include "VideoPlayerSubtitle.h"
#include "VideoPlayerTeletext.h"
#include "cores/IPlayer.h"
#include "cores/MenuType.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "guilib/DispResource.h"
#include "threads/SystemClock.h"
#include "threads/Thread.h"
#include "utils/LanguageTag.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <map>
#include <utility>
#include <vector>

struct SPlayerState
{
  SPlayerState() { Clear(); }
  void Clear()
  {
    timestamp = 0;
    time = 0;
    startTime = 0;
    timeMin = 0;
    timeMax = 0;
    time_offset = 0;
    dts = DVD_NOPTS_VALUE;
    player_state  = "";
    isInMenu = false;
    menuType = MenuType::NONE;
    chapter = 0;
    chapters.clear();
    rawChapters.clear();
    m_bookmarks.clear();
    canpause = false;
    canseek = false;
    cantempo = false;
    caching = false;
    cache_bytes = 0;
    cache_level = 0.0;
    cache_offset = 0.0;
    cache_time = 0.0;
    lastSeek = 0;
    streamsReady = false;
  }

  double timestamp;         // last time of update
  double lastSeek;          // time of last seek
  double time_offset;       // difference between time and pts

  double time;              // current playback time
  double timeMax;
  double timeMin;
  time_t startTime;
  double dts;               // last known dts

  std::string player_state; // full player state
  bool isInMenu;
  MenuType menuType;
  bool streamsReady;

  // 1-based current chapter, numbered among the chapters visible to the user (i.e. excluding
  // chapters fully hidden by an EDL cut). <=0 means no chapter / unknown
  int chapter;
  // name and start timestamp of chapters visible to the user (chapters fully contained within
  // an EDL cut are omitted).
  std::vector<std::pair<std::string, std::chrono::milliseconds>> chapters;
  // for each entry in `chapters`, the corresponding 1-based chapter index reported by the
  // demuxer/inputstream, used to translate a visible chapter number back to a seekable one.
  std::vector<int> rawChapters;
  // position of the bookmarks
  std::vector<std::chrono::milliseconds> m_bookmarks;

  bool canpause;            // pvr: can pause the current playing item
  bool canseek;             // pvr: can seek in the current playing item
  bool cantempo;
  bool caching;

  int64_t cache_bytes; // number of bytes current's cached
  double cache_level; // current cache level
  double cache_offset; // percentage of file ahead of current position
  double cache_time; // estimated playback time of current cached bytes
};

class CDVDInputStream;
struct BlurayTitleUiSnapshot;
class CDVDInputStreamBluray;

class CDVDDemux;
class CDemuxStreamVideo;
class CDemuxStreamAudio;
class CStreamInfo;
class CDVDDemuxCC;
class CVideoPlayer;

#define DVDSTATE_NORMAL           0x00000001 // normal dvd state
#define DVDSTATE_STILL            0x00000002 // currently displaying a still frame
#define DVDSTATE_WAIT             0x00000003 // waiting for demuxer read error
#define DVDSTATE_SEEK             0x00000004 // we are finishing a seek request

class CCurrentStream
{
public:
  int64_t demuxerId; // demuxer's id of current playing stream
  int id;     // id of current playing stream
  int source;
  double dts;    // last dts from demuxer, used to find discontinuities
  double dur;    // last frame expected duration
  int dispTime; // display time from input stream
  CDVDStreamInfo hint;   // stream hints, used to notice stream changes
  void* stream; // pointer or integer, identifying stream playing. if it changes stream changed
  int changes; // remembered counter from stream to track codec changes
  bool inited;
  unsigned int packets;
  IDVDStreamPlayer::ESyncState syncState;
  double starttime;
  double firststarttime; // audio: the first frame queued for the start
  bool starttimePending;
  double cachetime;
  double cachetotal;
  const StreamType type;
  const int player;
  // stuff to handle starting after seek
  double startpts;
  double lastdts;
  // video: the last packet a BD-J mark's own time converts with (design
  // 15.50) - its player dts, raw 90 kHz dts and title byte with generation
  double markRefDts;
  int64_t markRefRawDts;
  int64_t markRefPos;
  uint32_t markRefGen;

  enum
  {
    AV_SYNC_NONE,
    AV_SYNC_CHECK,
    AV_SYNC_CONT,
    AV_SYNC_FORCE
  } avsync;

  CCurrentStream(StreamType t, int i)
    : type(t)
    , player(i)
  {
    Clear();
  }

  void Clear()
  {
    id = -1;
    demuxerId = -1;
    source = STREAM_SOURCE_NONE;
    dts = DVD_NOPTS_VALUE;
    dur = DVD_NOPTS_VALUE;
    hint.Clear();
    stream = NULL;
    changes = 0;
    inited = false;
    packets = 0;
    syncState = IDVDStreamPlayer::SYNC_STARTING;
    starttime = DVD_NOPTS_VALUE;
    firststarttime = DVD_NOPTS_VALUE;
    starttimePending = false;
    startpts = DVD_NOPTS_VALUE;
    lastdts = DVD_NOPTS_VALUE;
    markRefDts = DVD_NOPTS_VALUE;
    markRefRawDts = 0;
    markRefPos = -1;
    markRefGen = 0;
    avsync = AV_SYNC_FORCE;
  }

  double dts_end()
  {
    if(dts == DVD_NOPTS_VALUE)
      return DVD_NOPTS_VALUE;
    if(dur == DVD_NOPTS_VALUE)
      return dts;
    return dts + dur;
  }
};

//------------------------------------------------------------------------------
// selection streams
//------------------------------------------------------------------------------
struct SelectionStream
{
  StreamType type = StreamType::NONE;
  int type_index = 0;
  std::string filename;
  std::string filename2;  // for vobsub subtitles, 2 files are necessary (idx/sub)
  KODI::UTILS::CLanguageTag language;
  std::string name;
  StreamFlags flags = StreamFlags::FLAG_NONE;
  int source = 0;
  int id = 0;
  int64_t demuxerId = -1;
  std::string codec;
  std::string codecDesc;
  AVCodecID codecId = AV_CODEC_ID_NONE;
  int profile = AV_PROFILE_UNKNOWN;
  int channels = 0;
  int bitrate = 0;
  int width = 0;
  int height = 0;
  CRect SrcRect;
  CRect DestRect;
  CRect VideoRect;
  std::string stereo_mode;
  float aspect_ratio = 0.0f;
  StreamHdrType hdrType = StreamHdrType::HDR_TYPE_NONE;
  AVDOVIDecoderConfigurationRecord dovi{};
  uint32_t fpsScale{0};
  uint32_t fpsRate{0};
};

class CSelectionStreams
{
public:
  CSelectionStreams() = default;

  int TypeIndexOf(StreamType type, int source, int64_t demuxerId, int id) const;
  int CountTypeOfSource(StreamType type, StreamSource source) const;
  int CountType(StreamType type) const;
  SelectionStream& Get(StreamType type, int index);
  const SelectionStream& Get(StreamType type, int index) const;
  bool Get(StreamType type, StreamFlags flag, SelectionStream& out);
  bool Contains(StreamType type, int source, int64_t demuxerId, int id) const;
  void Clear(StreamType type, StreamSource source);
  int Source(StreamSource source, const std::string& filename);
  void Update(SelectionStream& s);
  void Update(const std::shared_ptr<CDVDInputStream>& input, CDVDDemux* demuxer);
  void Update(const std::shared_ptr<CDVDInputStream>& input,
              CDVDDemux* demuxer,
              const std::string& filename2);

  std::vector<SelectionStream> Get(StreamType type);
  template<typename Compare> std::vector<SelectionStream> Get(StreamType type, Compare compare)
  {
    std::vector<SelectionStream> streams = Get(type);
    std::stable_sort(streams.begin(), streams.end(), std::move(compare));
    return streams;
  }

  std::vector<SelectionStream> m_Streams;

protected:
  SelectionStream m_invalid;
};

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

struct CacheInfo
{
  double level; // current cache level
  double offset; // percentage of file ahead of current position
  double time; // estimated playback time of current cached bytes
  bool valid;
};

class CProcessInfo;
class CJobQueue;

class CVideoPlayer : public IPlayer, public CThread, public IVideoPlayer,
                     public IDispResource, public IRenderLoop, public IRenderMsg
{
  enum class UpdateStreamDetails : bool
  {
    UPDATE_IF_FLAGGED,
    ALWAYS_UPDATE
  };

public:
  explicit CVideoPlayer(IPlayerCallback& callback);
  ~CVideoPlayer() override;
  bool OpenFile(const CFileItem& file, const CPlayerOptions &options) override;
  bool CloseFile(bool reopen = false) override;
  bool IsPlaying() const override;
  void Pause() override;
  bool HasVideo() const override;
  bool HasAudio() const override;
  bool HasRDS() const override;
  bool HasID3() const override;
  bool IsPassthrough() const override;
  bool CanSeek() const override;
  void Seek(bool bPlus, bool bLargeStep, bool bChapterOverride) override;
  bool SeekScene(Direction seekDirection) override;
  void SeekPercentage(float iPercent) override;
  float GetCachePercentage() const override;

  void SetDynamicRangeCompression(long drc) override;
  bool CanPause() const override;
  void SetAVDelay(float fValue = 0.0f) override;
  float GetAVDelay() override;
  bool IsInMenu() const override;

  /*!
   * \brief Get the supported menu type
   * \return The supported menu type
  */
  MenuType GetSupportedMenuType() const override;

  void SetSubTitleDelay(float fValue = 0.0f) override;
  float GetSubTitleDelay() override;
  int GetSubtitleCount() const override;
  int GetSubtitle() override;
  void GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const override;
  void SetSubtitle(int iStream) override;
  bool GetSubtitleVisible() const override;
  void SetSubtitleVisible(bool bVisible) override;

  /*!
   * \brief Set the subtitle vertical position,
   * it depends on current screen resolution
   * \param value The subtitle position in pixels
   * \param save If true, the value will be saved to resolution info
   */
  void SetSubtitleVerticalPosition(const int value, bool save) override;

  void AddSubtitle(const std::string& strSubPath) override;

  int GetAudioStreamCount() const override;
  int GetAudioStream() override;
  void SetAudioStream(int iStream) override;

  int GetVideoStream() const override;
  int GetVideoStreamCount() const override;
  void GetVideoStreamInfo(int streamId, VideoStreamInfo& info) const override;
  void SetVideoStream(int iStream) override;

  int GetPrograms(std::vector<ProgramInfo>& programs) override;
  void SetProgram(int progId) override;
  int GetProgramsCount() const override;

  std::shared_ptr<TextCacheStruct_t> GetTeletextCache() override;
  bool HasTeletextCache() const override;
  void LoadPage(int p, int sp, unsigned char* buffer) override;

  int GetChapterCount() const override;
  int GetChapter() const override;
  void GetChapterName(std::string& strChapterName, int chapterIdx = -1) const override;
  int64_t GetChapterPos(int chapterIdx = -1) const override; // chapter start ts in seconds
  int  SeekChapter(int iChapter) override;
  std::vector<std::chrono::milliseconds> GetBookmarks() const override;
  bool HasBookmarks() const;
  void SetBookmarks(const std::vector<std::chrono::milliseconds>& bookmarks) override;

  void SeekTime(int64_t iTime) override;
  bool SeekTimeRelative(int64_t iTime) override;
  void SetSpeed(float speed) override;
  void SetTempo(float tempo) override;
  bool SupportsTempo() const override;
  void FrameAdvance(int frames) override;
  bool OnAction(const CAction &action) override;

  void GetAudioStreamInfo(int index, AudioStreamInfo& info) const override;

  std::string GetPlayerState() override;
  bool SetPlayerState(const std::string& state) override;

  void FrameMove() override;
  void Render(bool clear, uint32_t alpha = 255, bool gui = true) override;
  void FlushRenderer() override;
  void SetRenderViewMode(int mode, float zoom, float par, float shift, bool stretch) override;
  float GetRenderAspectRatio() const override;
  void GetRects(CRect& source, CRect& dest, CRect& view) const override;
  unsigned int GetOrientation() const override;
  void TriggerUpdateResolution() override;
  bool IsRenderingVideo() const override;
  bool HasVisibleOverlay() const override;
  bool IsLiveStream() const override;
  bool IsStreaming() const override;
  bool Supports(EINTERLACEMETHOD method) const override;
  EINTERLACEMETHOD GetDeinterlacingMethodDefault() const override;
  bool Supports(ESCALINGMETHOD method) const override;
  bool Supports(ERENDERFEATURE feature) const override;

  // IDispResource interface
  void OnLostDisplay() override;
  void OnResetDisplay() override;

  bool IsCaching() const override;
  int GetCacheLevel() const override;

  int OnDiscNavResult(void* pData, int iMessage) override;
  void GetVideoResolution(unsigned int &width, unsigned int &height) override;

  CVideoSettings GetVideoSettings() const override;
  void SetVideoSettings(CVideoSettings& settings) override;

  void SetUpdateStreamDetails();

protected:
  friend class CSelectionStreams;

  void OnStartup() override;
  void OnExit() override;
  void Process() override;
  void VideoParamsChange() override;
  void GetDebugInfo(std::string &audio, std::string &video, std::string &general) override;
  void UpdateClockSync(bool enabled) override;
  void UpdateRenderInfo(CRenderInfo &info) override;
  void UpdateRenderBuffers(int queued, int discard, int free) override;
  void UpdateGuiRender(bool gui) override;
  void UpdateVideoRender(bool video) override;

  virtual void CreatePlayers();
  void DestroyPlayers();

  void Prepare();
  bool ShouldDeferSync(bool ready, std::chrono::steady_clock::time_point now);
  bool OpenStream(CCurrentStream& current, int64_t demuxerId, int iStream, int source, bool reset = true);
  bool OpenAudioStream(CDVDStreamInfo& hint, bool reset = true);
  bool OpenVideoStream(CDVDStreamInfo& hint, bool reset = true);
  bool OpenSubtitleStream(const CDVDStreamInfo& hint);
  bool OpenTeletextStream(CDVDStreamInfo& hint);
  bool OpenRadioRDSStream(CDVDStreamInfo& hint);
  bool OpenAudioID3Stream(CDVDStreamInfo& hint);

  /** \brief Switches forced subtitles to forced subtitles matching the language of the current audio track.
  *          If these are not available, subtitles are disabled.
  */
  void AdaptForcedSubtitles();
  bool CloseStream(CCurrentStream& current, bool bWaitForBuffers);

  /*!
   * \brief Reopen a stream whose player thread has exited, or drop it.
   *
   * A dead player still reports IsInited(), so the demuxer keeps filling a
   * queue nothing drains until the read gate stops playback entirely.
   */
  void CheckStreamPlayerAlive(CCurrentStream& current,
                              IDVDStreamPlayer* player,
                              int& restarts,
                              int& deadPolls,
                              const char* name);

  bool CheckIsCurrent(const CCurrentStream& current, CDemuxStream* stream, DemuxPacket* pkg);
  void ProcessPacket(CDemuxStream* pStream, DemuxPacket* pPacket);
  void ProcessAudioData(CDemuxStream* pStream, DemuxPacket* pPacket);
  void ProcessVideoData(CDemuxStream* pStream, DemuxPacket* pPacket);
  //! EL packets that arrive before their clip's video stream is open (13.u3)
  void SendPendingElPackets(const DemuxPacket* firstBl);
  void ClearPendingElPackets();
  void ProcessSubData(CDemuxStream* pStream, DemuxPacket* pPacket);
  void ProcessTeletextData(CDemuxStream* pStream, DemuxPacket* pPacket);
  void ProcessRadioRDSData(CDemuxStream* pStream, DemuxPacket* pPacket);
  void ProcessAudioID3Data(CDemuxStream* pStream, DemuxPacket* pPacket);

  int  AddSubtitleFile(const std::string& filename, const std::string& subfilename = "");

  /*!
   * \brief Propagate enable stream callbacks to demuxers.
   * \param current The current stream
   * \param isEnabled Set to true to enable the stream, otherwise false
   */
  void SetEnableStream(CCurrentStream& current, bool isEnabled);

  void SetSubtitleVisibleInternal(bool bVisible);

  enum SubtitleChange
  {
    FLAG_STATUS_CHANGE = 0x0001,
    FLAG_STREAMINFO_CHANGE = 0x0002,
  };
  void NotifySubtitleUpdate(int flags);
  void NotifyAudioUpdate();
  void NotifyVideoUpdate();

  /**
   * one of the DVD_PLAYSPEED defines
   */
  void SetPlaySpeed(int iSpeed);

  enum ECacheState
  {
    CACHESTATE_DONE = 0,
    CACHESTATE_FULL,     // player is filling up the demux queue
    CACHESTATE_INIT,     // player is waiting for first packet of each stream
    CACHESTATE_PLAY,     // player is waiting for players to not be stalled
    CACHESTATE_FLUSH,    // temporary state player will choose startup between init or full
  };

  void SetCaching(ECacheState state);

  double GetQueueTime();
  CacheInfo GetCachingTimes();

  void FlushBuffers(double pts, bool accurate, bool sync);

  void HandleMessages();
  void HandlePlaySpeed();
  bool IsInMenuInternal() const;
  void SynchronizeDemuxer();
  void QueueAutoSceneSkip(std::chrono::milliseconds seekTime);
  void CheckAutoSceneSkip();
  bool CheckContinuity(CCurrentStream& current, DemuxPacket* pPacket);
  bool CheckSceneSkip(const CCurrentStream& current);
  std::chrono::milliseconds GetEdlTime(const CCurrentStream& current) const;
  std::chrono::milliseconds GetSourceStreamLength() const;
  bool CheckPlayerInit(CCurrentStream& current);
  void UpdateCorrection(DemuxPacket* pkt, double correction);
  void UpdateTimestamps(CCurrentStream& current, DemuxPacket* pPacket);
  IDVDStreamPlayer* GetStreamPlayer(unsigned int player);
  void SendPlayerMessage(std::shared_ptr<CDVDMsg> pMsg, unsigned int target);

  bool ReadPacket(DemuxPacket*& packet, CDemuxStream*& stream);
  void HandleDynamicBufferLevel();
  void UpdateMenuDomainQueueDepth(bool segmentOpen);

  /* BD segment transition: every NEXTSTREAM_OPEN boundary is executed by one
   * routine against an explicit survival contract - which pipeline components
   * (demuxer, stream players/decoders, queued packets) live across the
   * boundary and which are rebuilt. Classification is read-only; execution is
   * the only place allowed to tear pipeline components down at a boundary.
   * See BdSegmentTransition() and docs/bd_menu_architecture.md §5. */
  enum class EBdTransition
  {
    SEAMLESS, // same-playlist playitem advance from a stable pipeline
    DISCARD_KEEPALIVE, // menu->title jump (HDMV): drop queues, keep decoders
    DISCARD_CLOSE, // menu->title jump (BD-J): drop queues, full close
    DRAIN, // default: render out the old streams, then close
  };
  EBdTransition ClassifyBdTransition() const;
  /* @param glided true when the seam was crossed without holding the read
   *        (CDVDInputStreamBluray's seamless glide), so the demuxer is part
   *        way through the incoming clip and must not be flushed. */
  void BdSegmentTransition(bool glided = false);
  //! real_player E1 (docs/real_player_disc_session_design.md §3): a start is
  //! committed with the clock paused until the output mode is final
  void HoldStart();
  void ReleaseAudioSessionHold();
  void CheckHeldStart();
  //! a user resume landing on the timeline like a start (design §16.21)
  bool ScheduleUserResume();
  void ReleaseHeldStart(const char* why);
  //! true while a held disc boundary waits for the old streams to finish starting
  bool WaitStartAtBoundary();

  bool IsValidStream(const CCurrentStream& stream);
  bool IsBetterStream(const CCurrentStream& current, CDemuxStream* stream);
  void CheckBetterStream(CCurrentStream& current, CDemuxStream* stream);
  void CheckStreamChanges(CCurrentStream& current, CDemuxStream* stream);

  bool OpenInputStream();
  bool OpenDemuxStream();
  // A disc (BD-J) waiting for playback: nothing to demux yet.
  // 0 = no, 1 = a screen with no playlist, 2 = a playlist selected, not started
  int DiscWaitState(bool drain);
  // present a screen with no playlist: leave the busy dialog, go fullscreen,
  // route keys (after a grace, so a normal start is not announced early)
  void CheckMenuOnlyStart(bool noPlaylist);
  std::chrono::steady_clock::time_point m_menuOnlyCandidateSince{};
  void SignalStreamsReady();
  void CloseDemuxer();
  void OpenDefaultStreams(bool reset = true);
  void UpdateHasVideoAudio();

  void UpdatePlayState(double timeout);
  void GetGeneralInfo(std::string& strVideoInfo);
  int64_t GetUpdatedTime();
  int64_t GetTime() const;
  float GetPercentage();

  virtual bool CanTempo();

  virtual void UpdateContent();
  void UpdateContentState();

  void UpdateFileItemStreamDetails(CFileItem& item, UpdateStreamDetails update);
  int GetPreviousChapter();
  std::optional<std::chrono::milliseconds> GetChapterPosMs(int chapterIdx = -1) const;
  // Translate a 1-based visible chapter number (as seen by GetChapter()/GetChapterCount())
  // to the 1-based chapter index expected by the demuxer/inputstream. Falls back to
  // returning visibleChapter unchanged if no mapping is available yet.
  int ToRawChapter(int visibleChapter) const;
  int GetPreviousBookmark(std::chrono::milliseconds ts);
  int GetNextBookmark(std::chrono::milliseconds ts);
  std::optional<std::chrono::milliseconds> GetBookmarkPos(int idx);

  struct SeekCandidate
  {
    int64_t targetTime;
    std::function<void()> action;
  };

  enum class SeekStep
  {
    NORMAL,
    LARGE,
  };

  static int64_t CalcTimeOrPercentSeekTarget(int64_t time,
                                             int64_t maxTime,
                                             Direction direction,
                                             SeekStep step);
  std::optional<SeekCandidate> GetTimeOrPercentSeekCandidate(int64_t time,
                                                             Direction direction,
                                                             SeekStep step);
  std::optional<SeekCandidate> GetChapterSeekCandidate(int64_t time, Direction direction);
  std::optional<SeekCandidate> GetBookmarkSeekCandidate(int64_t time, Direction direction);
  void ExecuteTimeSeek(int64_t target, Direction direction, bool accurate);
  bool EvaluateIsStreaming() const;

  bool m_players_created;

  CFileItem m_item;
  CPlayerOptions m_playerOptions;
  bool m_bAbortRequest;
  bool m_error;
  bool m_bCloseRequest;

  ECacheState  m_caching;
  XbmcThreads::EndTime<> m_cachingTimer;

  std::unique_ptr<CProcessInfo> m_processInfo;

  CCurrentStream m_CurrentAudio;
  CCurrentStream m_CurrentVideo;
  //! SPLICE survey: a disc stream's format at its close, compared at the next open
  CDVDStreamInfo m_closedVideoHint;
  CDVDStreamInfo m_closedAudioHint;
  bool m_closedVideoValid = false;
  bool m_closedAudioValid = false;
  CCurrentStream m_CurrentSubtitle;
  CCurrentStream m_CurrentTeletext;
  CCurrentStream m_CurrentRadioRDS;
  CCurrentStream m_CurrentAudioID3;

  CSelectionStreams m_SelectionStreams;
  std::vector<ProgramInfo> m_programs;

  struct SContent
  {
    mutable CCriticalSection m_section;
    CSelectionStreams m_selectionStreams;
    std::vector<ProgramInfo> m_programs;
    int m_videoIndex{-1};
    int m_audioIndex{-1};
    int m_subtitleIndex{-1};
  } m_content;

  int m_playSpeed;
  int m_streamPlayerSpeed;
  int m_demuxerSpeed = DVD_PLAYSPEED_NORMAL;
  struct SSpeedState
  {
    double lastpts{0.0}; // holds last display pts during ff/rw operations
    int64_t lasttime{0};
    double lastseekpts{0.0};
    double lastabstime{0.0};

    void Reset(double pts)
    {
      *this = {};
      if (pts != DVD_NOPTS_VALUE)
      {
        lastseekpts = pts;
      }
    }
  } m_SpeedState;

  double m_offset_pts;
  // the part of m_offset_pts applied at glided seams (DemuxPacket::m_seamOffsetCorrection)
  double m_seamOffsetPts = 0.0;

  // Glided Blu-ray seams, crossed by each stream at its own byte position: a
  // stream takes a seam's playlist step from its first packet past the seam.
  struct SeamMark
  {
    int64_t pos;
    double step;
    uint32_t gen; // the title-byte generation of pos
  };
  struct SeamStream
  {
    int64_t lastPos = -1;
    uint32_t lastGen = 0;
    double offset = 0.0;
  };
  bool m_seamByPos = false;
  std::vector<SeamMark> m_seamMarks;
  std::map<std::pair<int64_t, int>, SeamStream> m_seamStreams;
  void TakeSeamMarks();
  double CrossSeams(const DemuxPacket* packet, double& crossed);
  void ClearSeams();
  double VideoOffsetPts() const;

  // An audio track switch re-cued for the new track only, the picture running
  // on (design 15.34): the title is read again from just before the clock and
  // what the other streams were already given is skipped by byte position.
  struct DeliveredMark
  {
    int64_t pos = -1; // the byte the packet's PES began at, inherited when it has none
    double dts = DVD_NOPTS_VALUE; // the demuxer's own dts (DemuxPacket::demuxDts)
    int64_t readPos = -1; // the re-read's position in this stream
    bool el = false; // a Dolby Vision enhancement layer, fed to the video decoder
    bool unique = false; // one packet per dts (video, enhancement layer)
  };
  bool m_recueEnabled = false;
  std::map<std::pair<int64_t, int>, DeliveredMark> m_delivered;
  std::deque<std::pair<int64_t, double>> m_videoPosHistory; // current video: byte, corrected dts
  std::map<std::pair<int64_t, int>, DeliveredMark> m_recueGate;
  std::chrono::steady_clock::time_point m_recueSince;
  unsigned int m_recueDropped = 0;
  bool m_recueJoinPending = false;
  bool PrepareRecue(int64_t& pos);
  bool StartRecue(int64_t pos);
  bool RecueGateDrops(const DemuxPacket* packet);
  void NoteDelivered(const DemuxPacket* packet);
  void ClearRecue();

  CDVDMessageQueue m_messenger;
  std::unique_ptr<CJobQueue> m_outboundEvents;

  std::unique_ptr<IDVDStreamPlayerVideo> m_VideoPlayerVideo;
  std::unique_ptr<IDVDStreamPlayerAudio> m_VideoPlayerAudio;
  std::unique_ptr<CVideoPlayerSubtitle> m_VideoPlayerSubtitle;
  std::unique_ptr<CDVDTeletextData> m_VideoPlayerTeletext;
  std::unique_ptr<CDVDRadioRDSData> m_VideoPlayerRadioRDS;
  std::unique_ptr<CVideoPlayerAudioID3> m_VideoPlayerAudioID3;

  CDVDClock m_clock;
  CDVDOverlayContainer m_overlayContainer;

  std::shared_ptr<CDVDInputStream> m_pInputStream;
  std::unique_ptr<CDVDDemux> m_pDemuxer;
  std::shared_ptr<CDVDDemux> m_pSubtitleDemuxer;
  std::unordered_map<int64_t, std::shared_ptr<CDVDDemux>> m_subtitleDemuxerMap;
  std::unique_ptr<CDVDDemuxCC> m_pCCDemuxer;

  CRenderManager m_renderManager;

  struct SDVDInfo
  {
    void Clear()
    {
      state                =  DVDSTATE_NORMAL;
      iSelectedSPUStream   = -1;
      iSelectedAudioStream = -1;
      iSelectedVideoStream = -1;
      iDVDStillTime = std::chrono::milliseconds::zero();
      iDVDStillStartTime = {};
      syncClock = false;
    }

    int state;                // current dvdstate
    bool syncClock;
    std::chrono::milliseconds
        iDVDStillTime; // total time in ticks we should display the still before continuing
    std::chrono::time_point<std::chrono::steady_clock>
        iDVDStillStartTime; // time in ticks when we started the still
    int iSelectedSPUStream;   // mpeg stream id, or -1 if disabled
    int iSelectedAudioStream; // mpeg stream id, or -1 if disabled
    int iSelectedVideoStream; // mpeg stream id or angle, -1 if disabled
  } m_dvd;

  SPlayerState m_State;
  mutable CCriticalSection m_StateSection;
  XbmcThreads::EndTime<> m_syncTimer;
  // bounded deferral of the stream start-sync while no stream has a start pts
  // (post-flush demux gap on disc transitions) - see HandlePlaySpeed
  bool m_syncStartDeferred = false;
  XbmcThreads::EndTime<> m_syncStartDeferTimer;
  // Watchdog for a stream player that never reaches SYNC_WAITSYNC while its
  // own queue is full - the shape a dead player thread leaves behind.
  bool m_syncStuckArmed = false;
  XbmcThreads::EndTime<> m_syncStuckTimer;
  int m_audioPlayerRestarts = 0;
  int m_videoPlayerRestarts = 0;
  // consecutive polls that saw the player dead - a deliberate self-stop is
  // withdrawn within one pass, a real death is not
  int m_audioPlayerDeadPolls = 0;
  int m_videoPlayerDeadPolls = 0;
  // set only while CheckStreamPlayerAlive reopens, so OpenStream does not read
  // the cleared stream id as a first open and re-arm the display mode switch
  bool m_restartingStreamPlayer = false;

  std::optional<std::chrono::steady_clock::time_point> m_syncStartPtsWait;

  CEdl m_Edl;
  bool m_SkipCommercials;

  bool m_HasVideo;
  bool m_HasAudio;

  // Broken-source detection (coreelec.detectbrokenfiles). Player thread only.
  std::chrono::steady_clock::time_point m_brokenFileStallStart{};
  int64_t m_brokenFileStallBytes{-1};
  bool m_brokenFileNotified{false};
  bool m_brokenFileStallStarveLogged{false};
  // BD menu->title jump: the discard path keeps the stream players and their
  // decoders alive (flush instead of close); these one-shot flags let
  // OpenAudio/VideoStream reattach the running decoder when the new stream's
  // format matches ignoring the demuxer/stream id - avoiding the DV tunnel
  // drop/re-latch (toast + display-change black) when jumping from a menu
  // into a same-format title. Per-stream because the streams open in either
  // order after the reopen (OpenDefaultStreams is video-first; the disc-
  // dictated lazy path opens per-packet) - each open decision consumes only
  // its own flag. Player thread only.
  bool m_bdStreamReuseVideo = false;
  bool m_bdStreamReuseAudio = false;
  /* Armed at a Blu-ray seamless playitem boundary, consumed by the first
   * sub-second FORWARD timestamp step CheckContinuity sees afterwards - or, on a
   * glided (cc 5/6) seam (m_seamStepOverlapOk), by the first sub-second
   * BACKWARD step, which is an overlap closed the same way. The two
   * clips either side of a boundary are timed independently; a forward step is
   * a cut, not elapsed content, and waiting it out is a visible freeze plus an
   * audio dropout long enough to unlock a passthrough sink. The generic
   * forward resync cannot cover it - its 1000ms threshold exists to tell a
   * seek from jitter, and these steps are a third of a second. */
  bool m_seamStepPending = false;
  bool m_seamStepOverlapOk = false;
  /* The video dts the arm above was taken at; the arm is voided once video has
   * run a second past it. Video, not audio: m_CurrentAudio.dts is the last dts
   * audio was handed rather than where audio is, and across a menu boundary it
   * can be tens of minutes stale on another timeline. */
  double m_seamStepArmedDts = DVD_NOPTS_VALUE;
  // the playlist's own step at the armed boundary (IN - OUT), DVD time
  std::optional<double> m_seamStepPlaylistStep;

  // BD menu loop wrap: video's own measured timestamp gap, recorded when video
  // first flags the backward jump (0.0 = unset). With a single global offset
  // only one stream can be made exactly continuous across the wrap; preferring
  // the video gap makes the wrap visually gapless and leaves audio a small
  // residual its sync skew absorbs. Player thread only.
  double m_menuWrapVideoGap = 0.0;
  //! the one correction of a timeline jump joined ahead of the clock after a still
  double m_stillJoinCorrection = DVD_NOPTS_VALUE;
  //! a jump's join decided onto the old end (every stream of the jump joins its own)
  bool m_stillJoinOldEnd = false;
  //! the clock is the timeline of the streams it times: the stream it was
  //! started on is in sync and no start is held or scheduled (design 15.59)
  bool ClockOnStreams();
  //! the clock was placed on these streams' timeline (the sync commit), even if
  //! a start still holds it at its first picture (design 15.60)
  bool TimelineCommitted() const;
  //! why disc timeline entries with a due time wait: the clock is not (yet)
  //! their streams' (design 15.60). Worked out once per timeline pass, which
  //! also keeps its one 15 s bound.
  enum class DiscClockHold
  {
    NONE,
    UNCOMMITTED, //!< the clock is still the previous timeline's: every entry
    START, //!< the clock waits at a start's first picture: BD-J entries
  };
  DiscClockHold UpdateDiscClockHold(bool readerAtEnd);
  void ResetDiscClockHold();
  std::optional<std::chrono::steady_clock::time_point> m_discClockWaitSince;
  bool m_discClockGateExpired = false;
  std::chrono::steady_clock::time_point m_boundaryStartWaitSince{};
  //! E1: a full audio queue waiting on the video's first picture (bounded)
  std::chrono::steady_clock::time_point m_firstPictureWaitSince{};
  bool m_firstPictureWaitExpired = false;

  // Per-jump sequence stamped onto the packet that opens a timeline restart,
  // and a latch so only the first packet of a jump is stamped - the unconfirmed
  // branch of CheckContinuity re-enters for every packet until another stream
  // confirms. Both are cleared wherever the timeline dies.
  uint32_t m_timelineRestartSeq = 0;
  bool m_timelineRestartStamped = false;

  bool m_updateStreamDetails{false};

  std::atomic<bool> m_displayLost;
  bool m_heldStartEnabled = false; //!< E1 flag file, read at open
  bool m_scheduledStart = false; //!< design §15 step 2.2 flag file, read at open
  //! debug (flag file): BD-J discs take the keep-alive transition, to reproduce
  //! the crash that excludes them (disc session design 4.2)
  bool m_bdjKeepAliveDebug = false;
  bool m_startHeld = false;
  //! the caching pause is a stall while playing: it keeps the output's audio
  //! and resumes like a user resume (design §16.25)
  bool m_cachingStall = false;
  std::chrono::steady_clock::time_point m_stallSince;
  bool m_keepFrameEnabled = false; //!< design 4.3 debug flag (special://profile/keepframe)
  std::atomic_bool m_audioSessionHold{false}; //!< design 5 debug flag (special://profile/audiohold)
  bool m_startHeldSawLost = false;
  unsigned int m_startHeldDecisions = 0;
  double m_startReleasedClock = DVD_NOPTS_VALUE; //!< clock at the last release
  //! disc segment generation (6.1): advances at every BdSegmentTransition
  unsigned int m_segmentGen = 0;
  unsigned int m_segmentGenPublished = 0;
  void PublishSegmentGen();
  bool m_menuPageWaiting = false;
  std::chrono::steady_clock::time_point m_menuPageWaitSince;
  void CheckMenuPageWait();
  std::chrono::steady_clock::time_point m_startHeldSince;
  // playback started on a disc screen with no stream behind it (BD-J screen
  // with no playlist); HasVideo() reports it so the fullscreen video window
  // can own the screen and the remote. Read from the GUI thread.
  std::atomic<bool> m_discMenuOnly{false};
  std::atomic<bool> m_repostDiscOverlays{false};
  //! a display reset resumes the clock on the vblank grid (not for live input)
  std::atomic<bool> m_displayResumeOnGrid{false};
  std::atomic<bool> m_displayResumeScheduled{false};
  //! the user's audio offset moves the sound, as a player's audio delay
  std::atomic<bool> m_audioOffsetToAudio{false};
  std::atomic<double> m_requestedAudioOffset{0.0};
  std::atomic<int64_t> m_audioOffsetChangedMs{0};
  double m_landedAudioOffset = 0.0;
  void ApplyAudioOffset();

  double m_messageQueueTimeSize{0.0};

  // Cached downcast of m_pInputStream for the per-tick consumers
  // (UpdateMenuDomainQueueDepth / ApplyDiscTimelineEvents run every process
  // loop iteration; a dynamic_pointer_cast per tick was measured waste).
  // Set with m_pInputStream in OpenInputStream, cleared wherever
  // m_pInputStream is reset. Player thread only.
  std::shared_ptr<CDVDInputStreamBluray> m_pInputBluray;
  // menu-domain queue clamp (advancedsettings menudomainqueuetimesize),
  // read once per item in Prepare
  double m_menuDomainClampSeconds{0.0};

  // Menu-domain low-latency mode: true while the A/V message queues are
  // clamped to the short menu-domain read-ahead. Shrunk only when a video
  // segment opens, restored (grow-only) from the process loop. Player thread
  // only.
  bool m_menuDomainLowLatency{false};

  // Timeline-stamped disc events (docs/bd_timeline_events_design.md):
  // presentation-affecting disc state captured at demux time, stamped with
  // the demux position (last delivered video dts), applied from the process
  // loop when the render clock reaches the stamp. Phase 1: the presented
  // menu state (BD_EVENT_MENU). Phase 2: the presented title-UI snapshot
  // (BD_EVENT_PLAYLIST - chapters/total time; titleUi non-null selects it).
  // Player thread only.
  struct SDiscTimelineEvent
  {
    double stampPts;
    uint32_t menuState;
    std::shared_ptr<const BlurayTitleUiSnapshot> titleUi;
    // non-zero: release BD-J presentation-timing items up to this sequence
    // (libbluray patch 13); menuState/titleUi unused
    uint32_t bdjReleaseSeq = 0;
    // a held BD-J start read after everything before it was presented: its
    // first picture is the next segment's, so it waits for that segment
    bool awaitSegment = false;
    // a held mark's own picture on the player's timeline (libbluray patch 18,
    // design 15.50); it is due then, not at the stamp
    double presentPts = DVD_NOPTS_VALUE;
    // the playlist's END_OF_PLAYLIST: due at its last picture, once all of it
    // was delivered (design 15.54)
    bool endOfPlaylist = false;
    // a held mark whose clip's packets had not arrived when it was read (a
    // clip's first frame, a playlist read before its first picture): it
    // converts with the first record in its clip (design 15.55)
    bool markPending = false;
    uint32_t markPts45 = 0;
    int64_t markClipStart = 0;
    int64_t markClipEnd = 0;
    uint32_t markGen = 0;
    double markPendingClock = DVD_NOPTS_VALUE;
    std::chrono::steady_clock::time_point markPendingSince{};
  };
  std::deque<SDiscTimelineEvent> m_discTimelineEvents;
  std::deque<DemuxPacket*> m_pendingElPackets;
  //! readerAtEnd: the input waits at the end of a BD-J playlist
  void ApplyDiscTimelineEvents(bool flushAll, bool readerAtEnd = false);
  double BdjMarkPresentPts(uint32_t pts45, int64_t clipStart, int64_t clipEnd, double stamp,
                           uint32_t readGen) const;
  bool m_videoKeptUnconfirmed = false; //!< CheckContinuity kept an unconfirmed jump's keyframe
  // design 15.54: the segment's last picture (video, else audio; player timeline)
  // and whether the demuxer has delivered everything up to a BD-J playlist's end
  double m_segmentVideoPtsEnd = DVD_NOPTS_VALUE;
  double m_segmentAudioPtsEnd = DVD_NOPTS_VALUE;
  bool m_bdjAllDelivered = false;
  void ResetSegmentEnd();
  bool m_bdjMarksPending = false;
  void ResolvePendingMarks(int64_t packetPos, uint32_t packetGen);
  //! the instant of a held BD-J start's first picture, once known (design §15.48)
  bool BdjStartInstant(double stampPts, double clock, int64_t& startNs);
  void ArmBdjStartForSegment();
  std::optional<std::chrono::steady_clock::time_point> m_bdjStartWaitSince;
};
