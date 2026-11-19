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
#include "DiscTimeOwner.h"
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
#include <climits>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <map>
#include <utility>
#include <vector>

namespace DISPLAYSTATE
{
struct SStartJob;
struct ModeFacts;
enum class StartSettleKind;
enum class StartAdoptForm;
} // namespace DISPLAYSTATE
//! 5.E2-8b: P's receiver of a Start job's replies (VideoPlayer.cpp)
class CTxStartSink;

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
struct RenderLoopHandle;

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
  void FlushRenderer(bool overlaysOnly = false) override;
  void SetRenderViewMode(int mode, float zoom, float par, float shift, bool stretch) override;
  float GetRenderAspectRatio() const override;
  void GetRects(CRect& source, CRect& dest, CRect& view) const override;
  unsigned int GetOrientation() const override;
  void TriggerUpdateResolution() override;
  //! a live VS10 action's trigger (E2-8b: notice kind vs10 while main is shut out)
  void TriggerVs10Resolution();
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

  //! nextFile: a PLAYER_OPENFILE on the running player (5.V3-1's landing population)
  void Prepare(bool nextFile = false);
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
  /* @param glided true for a seam the input stream glided (cc 5/6 playitem
   *        advance, see BdSegmentTransition) */
  EBdTransition ClassifyBdTransition(bool glided = false) const;
  /* @param glided true when the seam was crossed without holding the read
   *        (CDVDInputStreamBluray's seamless glide), so the demuxer is part
   *        way through the incoming clip and must not be flushed. */
  void BdSegmentTransition(bool glided = false);
  //! real_player E1 (docs/real_player_disc_session_design.md §3): a start is
  //! committed with the clock paused until the output mode is final
  void HoldStart(std::optional<unsigned int> decisions);
  void ReleaseAudioSessionHold();
  void CheckHeldStart();
  //! a user resume landing on the timeline like a start (design §16.21)
  bool ScheduleUserResume();
  void ReleaseHeldStart(const char* why);
  //! 5.E2-6 (r2 M2): V's speed after a picture gate's hold ends outside ReleaseHeldStart
  void TxRestoreDecoderSpeed(const char* why);
  //! true while a held disc boundary waits for the old streams to finish starting
  bool WaitStartAtBoundary();
  //! start order design §3: a stream set's audio and start wait for its output mode
  void ArmStartGate(const char* where);
  void CheckStartGate();
  void OpenStartGate(const char* why, bool bound);
  bool StartGateShut() const { return m_startGateShut; }

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
    uint64_t clipPos; // the clip's start in title bytes: the seam itself
  };
  struct SeamStream
  {
    int64_t lastPos = -1;
    uint32_t lastGen = 0;
    double offset = 0.0;
  };
  bool m_seamByPos = false;
  //! a seamless (cc 5/6) playitem seam glides while the video stream is still
  //! starting, not only once it is in sync (JW3 menu, P5-13; A/B flag)
  bool m_earlyGlide = false;
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
  //! 5.V2-5: this Process run's posted render-loop registration (vconfigure on)
  std::shared_ptr<RenderLoopHandle> m_renderLoopHandle;
  unsigned int m_renderLoopGen = 0;
  std::atomic<bool> m_renderLoopDead{false};

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
  bool m_testReopenDone = false; //!< geometrytick_reopenpending: once per file
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
  //! design §18.24 (P5-8): a lost display as this player sees it; every
  //! transition is a compare-exchange (the return races the 15 s conversion)
  enum class LossState
  {
    NONE,
    LOST,
    CONVERTING,
    PAUSED,
    RETURNING,
  };
  std::atomic<LossState> m_lossState{LossState::NONE};
  //! video, a disc menu, or audio on HDMI: this player stops for a lost display
  std::atomic_bool m_holdOnDisplayLoss{true};
  void PublishHoldOnDisplayLoss();
  //! a display gone for 15 s becomes an ordinary pause (user ruling Q6)
  void ConvertLostDisplayToPause();
  //! 5.E2-7 (C2): the conversion's halves (speeds; callback and PVR), the reset's body and the
  //! PLAYER_OPENFILE branch, each moved out unchanged
  void ConvertToPauseSpeeds();
  void ConvertToPauseCallbacks();
  void ClearPlayerFreeze(bool heldAsPause);
  void HandleOpenFile(const std::shared_ptr<CDVDMsg>& pMsg);
  //! 5.E2-7b (C1): HandleMessages' PLAYER_SETSPEED branch, moved out unchanged
  void HandleSetSpeed(const std::shared_ptr<CDVDMsg>& pMsg);
  //! 5.E2-9b (docs/phase5_e2_9b_impl.md D1.4): P, a pending speed applied by the caching state
  //! (INIT/FULL: m_playSpeed only; PLAY/DONE, and at the commit in any state: the clock, the
  //! players, the hold); never a callback, never ScheduleUserResume; clears the pending value
  void ApplySpeed(int speed, bool atCommit = false);
  //! D1.2: the user's speed, P only: the pending one if set, else the applied m_playSpeed
  int UserSpeed() const { return m_txPendingSpeed != INT_MIN ? m_txPendingSpeed : m_playSpeed; }
  // 5.E2-7: the real loss path (docs/phase5_e2_7_impl.md D1-D4, D7). m_txFreezeReal: this
  // freeze is real (stored before the mark, cleared at every clear); m_txBodyDone: its FREEZE
  // body (or re-freeze) has finished under the guard; m_txVsyncRecord: the vsync resets taken
  // when the freeze began or last re-froze; m_txHeldAsPause: the reset found it converted
  std::atomic_bool m_txFreezeReal{false};
  std::atomic_bool m_txBodyDone{false};
  std::atomic<uint64_t> m_txVsyncRecord{0};
  //! code review M3, L7: the record's other halves and its generation (bumped after each record,
  //! W under the guard); RESUME's wait restarts whenever the generation moves
  std::atomic<uint64_t> m_txSetupRecord{0};
  std::atomic_bool m_txVsyncWasRegistered{false};
  std::atomic<uint64_t> m_txVsyncGen{0};
  uint64_t m_txResumeGen = 0; //!< P: the record generation RESUME's wait started under
  bool m_txResumeClockErrLogged = false;
  void TxRecordVsync();
  std::atomic_bool m_txHeldAsPause{false};
  //! 5.E2-7b (docs/phase5_e2_7b_impl.md D3 (a)): the take-back value VPA writes for this real
  //! freeze (0 where VPA writes 0 or nothing), recorded by the FREEZE body under the guard,
  //! before m_txBodyDone; the frozen inbox writes it into an empty hold (r3 N-L1)
  std::atomic<double> m_txLossHoldFrom{0.0};
  //! the audio offset in force at the same moment (D6's log)
  std::atomic<double> m_txLossOffset{0.0};
  //! the clock's hold value found at the same moment, before VPA's write for this freeze
  std::atomic<double> m_txLossHoldBefore{0.0};
  //! the picture's cut at the loss whatever the speed (0 without audio or a picture)
  std::atomic<double> m_txLossPicture{0.0};
  //! P only, the frozen inbox (D1-D6): a take in progress (the SETSPEED deviations key on it);
  //! its hold write (kept: a value was already there); per-freeze counts and logged-once marks
  bool m_txInboxTaking = false;
  bool m_txInboxHoldKept = false;
  double m_txInboxHoldValue = 0.0;
  bool m_txInboxPauseTaken = false;
  //! gate E29B-1 item 4: the loss (m_txVsyncGen, one per freeze and re-freeze) in which the
  //! inbox took a speed change; that loss's 15 s conversion leaves it standing. 0 = none
  uint64_t m_txInboxSpeedLoss = 0;
  bool m_txInboxFrontLogged = false;
  int m_txInboxTook = 0;
  std::vector<std::string> m_txInboxHeldLogged;
  bool TxInbox() const;
  //! P: the inbox's takes for one frozen pass
  void TxInboxPass();
  //! D10 (E27B-2): a front PLAYER_SETSPEED other than pause or play waits while the clock's
  //! scheduled start is open (it would cancel it); HEAD's order, nothing passes it
  bool TxSpeedWaits();
  std::chrono::steady_clock::time_point m_txSpeedWaitSince{};
  bool m_txSpeedWaiting = false;
  //! nullptr when a GUI action may be taken while frozen (D5), else why it is held
  const char* TxInboxGuiHold(const CAction& action);
  //! P only: conversion step 1 done; RESUME's wait; the other-word timer; logged-once marks
  int m_txConvertStep = 0;
  bool m_txResumeWaiting = false;
  bool m_txResumeWaitLogged = false;
  std::chrono::steady_clock::time_point m_txResumeWaitSince;
  bool m_txOtherWordTiming = false;
  std::chrono::steady_clock::time_point m_txOtherWordSince;
  bool m_txClockErrLogged = false;
  bool m_txSpanRefusedLogged = false;
  uint64_t m_txSkipLogged = 0;
  int m_txStateLogged = -1;
  //! P: HandleOpenFile clears the freeze before Prepare (a next file taken in a span)
  bool m_txClearBeforePrepare = false;
  //! W, under the guard: a walk meeting a player with a real freeze (D2b); true when re-frozen
  bool TxRefreeze();
  //! W, under the guard: a real freeze's reset, the word's CAS and P's wake (D2d)
  void TxResetReal();
  //! P: one pass of the frozen states (D3)
  void TxFrozenPass();
  //! P: conversion step 2, its effects on whatever word it reads (r3 N1)
  void TxConvertStep2();
  //! P: RESUME (D4); tryGuard from the drain hook; true when done
  bool TxResume(const char* site, bool tryGuard);
  //! P: the stop's or the next file's clear (D7), or the fallback under another word
  void TxClearAtEnd(const char* why);
  void TxFreezeEnded();
  //! P: the P-only pass fields back to rest (a legacy freeze, every end of a real one)
  void TxResetPassState();
  //! 5.E2-7 (R5, D6): the caller of the draining close in progress (P only); OnExit's line once
  const char* m_txDrainSite = nullptr;
  bool m_txExitHookLogged = false;
  //! P: installs (site) or clears (nullptr) the held predicate of a stream player's close
  void TxSetDrainHook(StreamType type, const char* site);
  //! P (D8): after a stream open's GENERAL_PAUSE(mark), resend the pause if the mark appeared
  void TxResendPause(IDVDStreamPlayer* player, bool lostAtSend, const char* stream);
  bool m_heldStartEnabled = false; //!< E1 flag file, read at open
  bool m_scheduledStart = false; //!< design §15 step 2.2 flag file, read at open
  bool m_startGateEnabled = false; //!< start order flag files, read at open
  bool m_startGateShut = false;
  unsigned int m_startGateGen = 0; //!< the stream set the gate belongs to
  // 5.E2-1's per-open shadow line (log only, P's own): a set of this file was armed already,
  // and the number of opens the shadow would have engaged (T)
  bool m_txShadowSetSeen = false;
  //! 5.E2-6: a set of this file armed CN's gate (the picture gate's candidate is the first)
  bool m_txGateSetSeen = false;
  // 5.E2-8a (docs/phase5_e2_8a_impl.md): this file's start transaction, reset in Prepare
  bool m_txEngaged = false; //!< D1: P's verdict at the first set's arm
  bool m_txWordStarting = false; //!< D2: the word took STARTING at CN's opening
  bool m_txDiscHeld = false; //!< D3: disc time held from CN's "output mode decided" opening
  bool m_txStarted = false; //!< D4: a START point ran
  bool m_txExited = false; //!< D5: ended to legacy
  bool m_txSyncPassed = false; //!< D4: the sync commit passed without sending onAVStarted
  unsigned int m_txEngagedGen = 0;
  bool m_txCapForce = false; //!< transaction_capforce, read at the engage
  bool m_txAudioFullForce = false; //!< transaction_audiofullforce, read at the engage
  //! 5.E2-8b transaction_noticeat <trigger|changed>@<prime|posted|settle>, read at the engage
  std::string m_txNoticeKind;
  std::string m_txNoticePoint;
  bool m_txLostSinceArm = false; //!< a loss inside the open start: its bounds re-armed after it
  std::chrono::steady_clock::time_point m_txLossAtUntil{}; //!< transaction_lossat's window
  const char* m_txFlushCaller = nullptr; //!< D5: the next FlushBuffers' caller, for the exit line
  const char* TxEngageRefusal(const char* where);
  //! 5.E2-10 (D1 item 2): the engage, at the file's first set or a reopen
  void TxEngage(const char* where, bool startFullScreen);
  //! 5.E2-10 (D1 item 4): an open's start state reset (Prepare), or a reopen's explicit list
  void TxResetStart(bool reopen);
  // 5.E2-10 (docs/phase5_e2_10_impl.md): a disc reopen
  enum class TxSegClass
  {
    NONE,
    DRAIN,
    DISCARD_CLOSE,
    KEEPALIVE,
  };
  static const char* TxSegClassName(TxSegClass cls);
  //! D1 item 1: BdSegmentTransition's class, taken by the next ArmStartGate (one-shot)
  TxSegClass m_txSegClass = TxSegClass::NONE;
  bool m_txNextFile = false; //!< §5: this open is Prepare(true)'s
  //! D1 item 7: a KEEPALIVE set whose verdict waits for the reuse compare (0: none)
  unsigned int m_txReopenCandidateGen = 0;
  bool m_txReopenOpen = false; //!< D1 item 6: this start is a reopen: DECIDE waits for a publish
  bool m_txReopenInStart = false; //!< the reopen superseded an open start
  bool m_txReopenWaitLogged = false;
  uint64_t m_txReopenSeq = 0; //!< D1 item 6: the inputs' seq at the reopen's OPEN
  std::chrono::steady_clock::time_point m_txReopenOpenAt{};
  //! D1 items 3, 5: a reopen's refusals and its word step; false: the set is legacy (logged)
  bool TxReopenAllowed(const char* where, TxSegClass cls);
  void TxKeepaliveVerdict(bool reuse);
  //! §3 (row 3): a reopen inside an open start drops its job before the commit; main stays shut
  //! and the start stays open for the reopen's OPEN
  bool m_txSuperseded = false;
  std::chrono::steady_clock::time_point m_txSupersededAt{};
  void TxSupersede(TxSegClass cls);
  //! D1 item 8: a superseded start ended at a refusal, main back in with the owed decision
  void TxEndSuperseded(const char* refusal);
  //! §3: no new OPEN 12 s after the step
  void TxSupersededBound();
  //! test hooks: transaction_reopendelay's end ({} = none); boundarynohold / the lead's line,
  //! once per boundary
  std::chrono::steady_clock::time_point m_txReopenDelayUntil{};
  bool m_txBoundaryLogged = false;
  // 5.E2-10 9d: the 3D target P decides on (StereoscopicsManager::OnStreamChange's)
  struct STxStereo
  {
    enum class Kind
    {
      TARGET, //!< no question
      ASK,
      UNAVAILABLE, //!< an input P cannot read: legacy
    } kind = Kind::TARGET;
    RenderStereoMode target = RenderStereoMode::OFF;
    RenderStereoMode gui = RenderStereoMode::OFF;
    const char* rule = "";
    bool stereoscopic = false;
  };
  STxStereo TxStereoTarget(const std::string& stream, const DISPLAYSTATE::DecideInputs& latest);
  //! SetStereoMode's rule for m on the GUI mode gui
  static RenderStereoMode TxStereoApply(RenderStereoMode m,
                                        RenderStereoMode gui,
                                        RenderStereoMode playing,
                                        bool support3d);
  //! the ASK step's pass: the dialog's pause and unpause, then DECIDE on the answer
  void TxAskPass();
  //! 5.E2-11 D4: the job's askOpen from TxAskPass's dialog predicate
  void TxAskOpenUpdate();
  //! the target the ASK step decided (DECIDE re-runs on it)
  std::optional<RenderStereoMode> m_txAskTarget;
  std::string m_txAskStream;
  RenderStereoMode m_txAskPlaying = RenderStereoMode::OFF;
  bool m_txAskSupport3d = false;
  //! r1 M-5: a pending pause since this set's OPEN counts as the dialog open
  bool m_txAskPauseSeen = false;
  bool m_txAskOpen = false;
  bool m_txAskBoundLogged = false;
  uint64_t m_txAskInputsSeq = 0; //!< the publish after the dialog closed (0: not closed yet)
  std::chrono::steady_clock::time_point m_txAskSince{};
  std::chrono::steady_clock::time_point m_txAskClosedAt{};
  //! the set's first pass with a configured picture (the 2 s no-dialog bound counts from it)
  std::chrono::steady_clock::time_point m_txConfiguredAt{};
  //! D2/D3 at CN's decided or unchanged opening; false while transaction_lossat holds the opening
  bool TxDecided(bool modeDecided);
  //! guarded: 5.E2-11 D1.5's START took the word and m_txStarted under the player guard
  void TxStartPoint(const char* point, bool guarded = false);
  void TxStartRelease(int64_t discRunNs);
  void TxCheckVerdict(const char* where);
  //! 5.E2-8b D8: how / need are the site's two columns (a job not past its drop CAS, or never
  //! posted / a job past it); unused on an open main decides (no Start job)
  //! 5.E2-9b D1.4: applySpeed false only at stop, next file and TxErrorExit
  void EndTransactionToLegacy(const char* site,
                              CRenderManager::TxHow how,
                              CRenderManager::TxNeed need,
                              bool applySpeed = true);
  bool TxOpen() const { return m_txEngaged && !m_txStarted && !m_txExited; }
  // 5.E2-8b (docs/phase5_e2_8b_impl.md): the engaged start decided on P, main shut out. Reset in
  // Prepare; every read behind m_txJob (an engaged open not under transaction_skip decide)
  enum class TxStep
  {
    NONE, //!< not decided yet
    WAIT_GUI, //!< D4 step 7: decided, the post waits while a GUI set is in flight
    POSTED, //!< the Start job is on W
    ASK, //!< 5.E2-10 9d: DECIDE waits for the 3D question
  };
  //! D2: the open's Start job (null: main decides this open)
  std::shared_ptr<DISPLAYSTATE::SStartJob> m_txJob;
  // 5.E2-12b (docs/phase5_e2_12b_impl.md D1-D3): a no-video screen inside a start (row 3b)
  struct STxNoVideoCarry
  {
    std::shared_ptr<DISPLAYSTATE::SStartJob> job; //!< the start's committed job, ended at a transition
    uint64_t T = 0;
    bool settled = false; //!< its SettleDone was read
  };
  std::optional<STxNoVideoCarry> m_txNoVideoCarry;
  bool m_txNoVideoHold = false; //!< CN's gate kept shut for row 3b's step
  std::chrono::steady_clock::time_point m_txNoVideoSince; //!< the wait's grace (Q3)
  const char* m_txNoVideoStep = nullptr; //!< row 3b's step pending: "wait" or "novideo"
  int m_txNoVideoWaitState = 0;
  bool m_txNoVideoHeld = false;
  bool m_txNoVideoOwedPosted = false;
  bool m_txNoVideoWaitLogged = false;
  bool m_txNoVideoClosed = false; //!< code review H-2: the start closed, its replays still running
  bool m_txNoVideoCapped = false; //!< code review L-3: the cap opened this set's gate
  std::shared_ptr<DISPLAYSTATE::ModeFacts> m_txNoVideoWire; //!< the wire mode when the hold began
  //! gate hook transaction_novideo: the first later Blu-ray set of the file is treated as having no
  //! video (its video stream not opened), once
  bool m_txNoVideoForced = false;
  bool m_txNoVideoHookDone = false;
  //! an engaged start of this file is open, or carried committed, and CN's gate is shut
  bool TxNoVideoInStart() const;
  void TxNoVideoStepPass();
  void TxNoVideoReset(const char* why);
  //! the wire mode when the hold begins (connected only)
  void TxNoVideoNoteWire();
  // 5.E2-12a (docs/phase5_e2_12_impl.md D1, D2, D4): W's deferral window for this player's
  // file, its close, the pick folded into DECIDE and the amends
  uint32_t m_txDeferF = 0; //!< the window's file (0: none open)
  int64_t m_txLeadEndNs = 0; //!< the lead's end (CLOCK_MONOTONIC ns); 0: no lead
  std::chrono::steady_clock::time_point m_txCarriedAt; //!< a carried window's start
  std::optional<RESOLUTION_INFO> m_txStickyTarget; //!< a deferred pick's target, by value
  uint64_t m_txStickyReqId = 0;
  unsigned int m_txAmends = 0;
  bool m_txAmendPending = false; //!< a pick after the post waits for its amend (TxDeferPass)
  void TxDeferPass();
  //! the StartDone step (D4): outcome is an OUTPUT::COutputWorker::DeferOutcome
  void TxStartDone(int outcome, const char* why);
  //! D1.3: the sticky target replaces the resolved row when the table has it
  bool TxApplySticky(CRenderManager::SStartResolve& r,
                     const DISPLAYSTATE::DecideInputs& latest,
                     bool markFolded = true);
  //! D2: a different decision after the post: a new Start job of the same T, only over an
  //! uncommitted job with CN's gate shut. True when posted or the decision is the same
  bool TxPostAmend(const char* origin);
  //! the gate's transaction_requestat: armed at OPEN, fired at its point, repeated by TxDeferPass
  void TxRequestAt(const char* point);
  void TxRequestFire();
  bool m_txRequestAtArmed = false;
  int m_txRequestAtLeft = 0;
  int m_txRequestAtIndex = 0;
  std::chrono::steady_clock::time_point m_txRequestAtNext;
  std::unique_ptr<CTxStartSink> m_txSink;
  TxStep m_txStep = TxStep::NONE;
  bool m_txOpenPending = false; //!< D2: engaged before Prepare's WordOpen: OPEN after it
  bool m_txWordOpened = false; //!< Prepare's WordOpen ran for this file
  bool m_txFullScreen = false; //!< D2: the arm's computed fullscreen term
  bool m_txRootAtOpen = false;
  uint64_t m_txExitsAtOpen = 0; //!< D2: GC's fullscreen-exit counter at OpenFile
  uint64_t m_txOpenInputsSeq = 0; //!< D2: OpenFile's DECIDE inputs publish (0: none)
  //! D2: the open message's snapshot (OpenFile, on main), taken by the open
  uint64_t m_txMsgInputsSeq = 0;
  uint64_t m_txMsgExits = 0;
  uint64_t m_txGuiSetSnap = 0; //!< D4 step 7: the guiSet notice counter before P's CAS
  CRenderManager::STxNotice m_txJobFacts; //!< D3: RM's facts DECIDE read (the newest notice)
  bool m_txJobFactsSet = false;
  bool m_txGuiWaitLogged = false;
  bool m_txWindowWaitLogged = false;
  bool m_txBoundMetCommit = false; //!< D8: CN's cap or audio-full bound met a committed job
  // 5.E2-9a (docs/phase5_e2_9a_impl.md D2-D4)
  bool m_txReprimed = false; //!< D2: a flush kept this open: its re-sync schedules, never holds
  bool m_txExc1Done = false; //!< transaction_exc1: the injected flush ran for this open
  bool m_txGateLostSeen = false; //!< D4 (E28A-8): CN's bounds re-armed when the display is back
  // 5.E2-9b D1 (P only, reset in Prepare): a speed requested inside the open start, applied at
  // START, an exit or the first pass after a display return (INT_MIN = none); every m_playSpeed
  // write clears it
  int m_txPendingSpeed = INT_MIN;
  bool m_txPendingTempo = false;
  //! P only: a non-pause, non-1x pending speed Prepare dropped, read once by the next speed's
  //! OnPlayBackSpeedChanged (gate E29B-1 item 5b); INT_MIN = none
  int m_txDroppedSpeed = INT_MIN;
  bool m_txSkipPausestart = false; //!< D1.6: transaction_skip pausestart, read at the engage
  // 5.E2-9c (docs/phase5_e2_9c_impl.md; P only, reset in Prepare)
  //! skip words read at the engage: `readon` (D1 off), `primebound` (D2 off; implies readon)
  bool m_txSkipReadon = false;
  bool m_txSkipPrimebound = false;
  //! D1/D2: a disc menu domain at the engage, or a menu page waiting since: CN's 12 s and its
  //! 1 s bound for the open
  bool m_txMenuDomain = false;
  //! D1: the read past a full audio queue for a starving video decoder
  enum class TxReadOn
  {
    IDLE, //!< not entered (or reset by a re-PRIME)
    ACTIVE,
    ENDED, //!< ended until a re-PRIME resets it
    FORCED, //!< transaction_audiofullforce: off for the open
  };
  TxReadOn m_txReadOn = TxReadOn::IDLE;
  int m_txReadOnMax = 0; //!< the audio data maximum at the entry
  double m_txReadOnCap = 0.0; //!< the audio time cap at the entry (s)
  int m_txReadOnPeakData = 0;
  double m_txReadOnPeakTime = 0.0;
  bool m_txReadOnBypassed = false; //!< this pass's read gate was passed by D1
  //! D1: an engaged open P decides with a video stream, outside menu domains and
  //! transaction_skip readon
  bool TxReadOnOpen() const
  {
    return TxOpen() && m_txJob && !m_txSuperseded && m_CurrentVideo.id >= 0 && !m_txMenuDomain &&
           !m_txSkipReadon;
  }
  //! D1: the read gate stops only for the audio queue: true when the read goes on
  bool TxReadOnBypass();
  //! D1: an active read-on ends at START, an exit, a menu page or the first picture
  void TxReadOnPass();
  void TxReadOnEnd(const char* why);
  //! D2: the 5 s PRIME bound's open: P decides with a video stream, outside menu domains and
  //! transaction_skip primebound
  bool TxPrimeBoundOpen() const
  {
    return TxOpen() && m_txJob && m_CurrentVideo.id >= 0 && !m_txMenuDomain &&
           !m_txSkipPrimebound;
  }
  //! D2/D3: latched at the bound until the hint job is posted
  bool m_txHintDecide = false;
  //! D3: a hint job was posted; kept until START or an exit (a re-PRIME keeps it)
  bool m_txHintJob = false;
  std::chrono::steady_clock::time_point m_txHintBoundAt{}; //!< D3: the window wait counts from it
  CRenderManager::SStartFacts m_txHintFacts; //!< D3: the facts the hint job was decided on
  // D3-D5: this file's counts, logged and reset by TxNoPictureSummary
  unsigned int m_txNoPictureDecides = 0; //!< D3: posted hint jobs
  unsigned int m_txNoPictureContradicted = 0; //!< D4: contradiction exits before START
  unsigned int m_txSettleExits = 0; //!< D5: the 12 s exits after settle
  unsigned int m_txLateContradictedSeen = 0; //!< RM's late count at the last summary
  //! D3: the stream's facts from m_CurrentVideo.hint, as V's first Configure would report them
  CRenderManager::SStartFacts TxDecideFromHints();
  //! D4: a picture against a hint decision before START: the exit "input changed"
  void TxHintContradicted(const CRenderManager::STxNotice& picture, const char* where);
  //! D3: both start flags cleared (open, exits, START)
  void TxHintClear();
  //! D4: before START, the hint job's picture once its gen is configured (compared once)
  void TxHintPictureCheck(const char* where);
  bool m_txHintChecked = false;
  bool m_txHintNoticeSeen = false; //!< D4: the gen's first picture notice was compared
  mutable bool m_txHint2xLogged = false; //!< D4: "fps 2x accepted" once per hint job
  //! D5: the hint-decided start's settle timer (armed at SettleDone, {} = unarmed)
  std::chrono::steady_clock::time_point m_txSettleSince{};
  bool m_txSettleLostSeen = false; //!< D5: re-armed on the first pass after a display loss
  bool m_txSettleAudioLogged = false;
  //! D5: at the top of the sync pass: true once START on audio is due
  bool TxSettleTimerPass();
  //! D6 (transaction_nopicture settle+<ms>): V withholds until settle done + this (-1: none)
  int m_txNoPictureSettleMs = -1;
  //! D6 (transaction_audiotimecap): the audio queue's time cap before the hook (0: none)
  double m_txAudioTimeCapSaved = 0.0;
  void TxTestHooksAtEngage();
  void TxRestoreAudioTimeCap(const char* why);
  //! D3: the counters, logged at the player's stop and at a next file
  void TxNoPictureSummary(const char* why);
  //! D2: a flush inside a P-decided open keeps it (same T) for the callers re-PRIME covers
  bool TxKeepsFlush(const char* caller) const;
  //! D2: the flush's re-PRIME: an E1 hold dropped without START, CN's bounds re-armed
  void TxReprime(const char* caller);
  //! D6: the job's replies, taken from P's messenger
  bool m_txOutputDone = false;
  std::optional<DISPLAYSTATE::StartSettleKind> m_txSettle;
  std::optional<DISPLAYSTATE::StartAdoptForm> m_txAdopt;
  //! D6's backstop: from the post, paused while P is lost
  std::chrono::steady_clock::time_point m_txPostedAt{};
  std::chrono::steady_clock::time_point m_txLostAt{};
  std::chrono::steady_clock::duration m_txLostPaused{};
  //! D5b (r13 L3): the gen of an exit whose outcome may be unresolved, for RM::TxFileEnded
  unsigned int m_txEndedGen = 0;
  bool m_txEndedLeft = false; //!< code review A-M1: that exit was `left` past the drop CAS
  //! D2 at the engage, or after WordOpen (the open's T)
  void TxOpenStart();
  //! D4: DECIDE at the set's first configure
  void TxDecide();
  //! D4 step 7: the Dekker step and the post
  void TxTryPost();
  //! D3, D4, D6, D9: one CheckStartGate pass of an engaged start decided on P
  void TxStartGatePass(CRenderManager::ModeFirstState state);
  //! D6: the replies, CN's opener and the backstop
  void TxRepliesPass();
  //! D8: the backstop row (a reply missing)
  void TxBackstopExit(const char* site);
  // 5.E2-9a (D7): the OUTPUT bound, the 30 s rule with no answer from W, the error exit
  void TxOutputBoundExit();
  void TxNoAnswerExit(const char* site);
  void TxErrorExit(const char* why);
  bool m_txBoundInCommitLogged = false;
  //! D9: while lost, a failed Start job's reply or D6's 30 s backstop ends the start
  void TxLostFailedPass();
  // 5.E2-11 (docs/phase5_e2_11_impl.md; P only, reset in Prepare)
  //! D1.3: a row-9 span over the open start (the word's span, NONE or STARTING of this T, P not
  //! frozen), derived at the top of each pass
  bool m_txInSpan = false;
  bool m_txSpanSeen = false; //!< P's own edge of it
  bool m_txSpanDiscLost = false; //!< the open edge set disc time lost
  uint32_t m_txSpanE = 0; //!< the word's e at the open edge (the span's e)
  std::chrono::steady_clock::time_point m_txSpanOpenedAt{};
  //! D1.3: DECIDE's render timeout counts from the last span's close
  std::chrono::steady_clock::time_point m_txSpanClosedAt{};
  //! D1.9: the time the word's span was open since the post (any span, a freeze inside it too)
  std::chrono::steady_clock::duration m_txSpanPaused{};
  std::chrono::steady_clock::time_point m_txSpanPausedFrom{};
  std::chrono::steady_clock::duration TxSpanPausedNow() const;
  void TxSpanPass();
  //! D1.3: P's edge dropped; disc time back unless a freeze owns it
  void TxSpanEdgeClear(const char* why);
  const char* TxSpanStateName(bool starting) const;
  //! D1.6, D1.7: the span's own 15 s conversion, once per span; a speed taken in it stands
  void TxSpanConvertPass();
  //! D2.6: the open job cleared (START, an exit, the stop): W re-queues an owed Restore
  bool TxClearOpenJob(const char* why);
  //! transaction_decidehold / posthold's ends ({} = not started)
  std::chrono::steady_clock::time_point m_txDecideHoldUntil{};
  std::chrono::steady_clock::time_point m_txPostHoldUntil{};
  bool m_txSpanConverted = false;
  bool m_txSpanSpeedTaken = false;
  //! D1.5: START as the word CAS STARTING{T, e, no span} -> NONE under the player guard, with
  //! only START's irreversible steps; on a P-decided engaged open with TxLossInStartOn()
  bool TxGuardedStartOn() const;
  enum class TxStartPath
  {
    SCHEDULED, //!< the sync commit's held and released start
    UNHELD, //!< the sync commit's unheld start (SetCaching's DONE)
    RELEASE, //!< an E1-held start's release
  };
  //! the release's results, for the steps and logs after the guard
  struct STxStartDone
  {
    double held = 0.0;
    double lead = 0.0;
    int64_t startNs = 0;
    int64_t discRunNs = 0;
    double startClock = 0.0;
  };
  //! false: no START this pass (the guard busy or the CAS refused; nothing ran)
  bool TxStartGuarded(TxStartPath path,
                      STxStartDone& done,
                      const std::function<void()>& commitSteps);
  void TxReleaseClock(STxStartDone& done);
  void TxStartAfterGuard(TxStartPath path, const STxStartDone& done, const char* why);
  uint32_t m_txStartE = 0; //!< the e START's CAS expects (STARTING's, or a Restore's settle)
  int64_t m_txRestoredE = -1; //!< D3.2: the last RESTORED's e (-1: none)
  uint64_t m_txReShowSeen = 0; //!< D3.3: the job's restoreCommit value acted on
  bool m_txPreRollReShown = false;
  std::chrono::steady_clock::time_point m_txRestoreMissingSince{}; //!< D3.4's bound
  bool m_txGuardBusyLogged = false;
  uint64_t m_txStartHeldWord = 0; //!< the word a refused CAS last logged
  uint32_t m_txStartHeldE = 0; //!< and the e it expected
  bool m_txStartCasDelayDone = false; //!< transaction_startcasdelay before: once per start
  //! D5: the scheduled start's T0 (CLOCK_MONOTONIC ns), read by W's lost walk
  std::atomic<int64_t> m_txLeadT0Ns{0};
  //! test (transaction_noticeat): the notice raised once its point is reached
  void TxTestNoticeAt();
  //! HandleMessages: PLAYER_TX_REPLY
  void TxOnReply(CDVDMsg& msg);
  //! D3: a notice after the post against the job's facts (5.E2-9c D4: a hint job's first
  //! picture notice takes the first-configure rules)
  bool TxNoticeSame(const CRenderManager::STxNotice& notice, bool firstConfigure = false) const;
  //! D8: the cap / audio-full row; false when the committed job is not ended
  bool TxBoundEnds(const char* site);
  //! D5b: the job's sink cleared and TxOpenJob released (stop, next file, destruction)
  void TxReleaseJob(const char* why);
  unsigned int m_txShadowSerial = 0;
  void LogTransactionShadow(
      bool arm, bool hasVideo, bool startFullScreen, const char* where, TxSegClass segClass);
  //! 5.E2-3: this player's T in the shadow display word (0: none); read by the announcing thread
  std::atomic<uint32_t> m_txWordT{0};
  //! 5.E2-5 (shadow): the CN set ResolveOutput last ran for
  unsigned int m_decideShadowGen = 0;
  void DecideShadow();
  void LogWordBoundary(bool arm, const char* why);
  //! the word state mirroring a loss state (a COutputWorker::WordState), and its name
  static uint8_t WordMirror(LossState state);
  static const char* LossStateName(LossState state);
  //! a P hook's HEAD: m_lossState's name now, and whether it left the state the hook mirrors
  std::pair<const char*, bool> WordHeadNow(LossState expected) const;
  //! P (gate: WORD and m_lossState agree at every P sample): logged once per change of the pair;
  //! a disagreement only once it has lasted 200 ms (a hook trails HEAD's store by a statement)
  void SampleWord();
  int m_wordSamplePair = -1;
  int m_wordSampleLogged = -1;
  std::chrono::steady_clock::time_point m_wordSampleSince;
  std::chrono::steady_clock::time_point m_startGateSince;
  std::chrono::steady_clock::time_point m_startGateStarveSince;
  bool m_startGateFullScreenPosted = false;
  //! the gate's opening, until the set's audio reports or 1 s has passed
  std::chrono::steady_clock::time_point m_startGateOpenedAt;
  bool m_gatedAudioWaitLogged = false;
  //! debug (flag file): BD-J discs take the keep-alive transition, to reproduce
  //! the crash that excludes them (disc session design 4.2)
  bool m_bdjKeepAliveDebug = false;
  bool m_startHeld = false;
  CDiscTimeOwner m_discTime; //!< design §18.6-18.8: libbluray's disc clock
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
  //! 5.B2-5c: the step limit at the loss, the frames out having been released on that mode's grid
  std::atomic<double> m_lostMaxStep{0.0};
  std::atomic<double> m_lostFront{DVD_NOPTS_VALUE}; //!< the next frame at the loss
  //! the user's audio offset moves the sound, as a player's audio delay
  std::atomic<bool> m_audioOffsetToAudio{false};
  bool m_startAtFrame = false; // the start's clock is a picture's pts
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
