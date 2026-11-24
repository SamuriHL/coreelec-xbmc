/*
 *  Copyright (C) 2005-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayer.h"

#include "cores/VideoPlayer/BDStageTrace.h"

#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDDemuxers/DVDDemuxCC.h"
#include "DVDDemuxers/DVDDemuxFFmpeg.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "DVDDemuxers/DVDDemuxVobsub.h"
#include "DVDDemuxers/DVDFactoryDemuxer.h"
#include "DVDInputStreams/DVDFactoryInputStream.h"
#include "DVDInputStreams/DVDInputStream.h"
#include "network/NetworkFileItemClassify.h"
#if defined(HAVE_LIBBLURAY)
#include "DVDInputStreams/DVDInputStreamBluray.h"
#endif
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "DVDInputStreams/InputStreamPVRBase.h"
#include "DVDMessage.h"
#include "FileItem.h"
#include "LangInfo.h"
#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "VideoPlayerAudio.h"
#include "VideoPlayerRadioRDS.h"
#include "VideoPlayerVideo.h"
#include "application/Application.h"
#include "cores/DataCacheCore.h"
#include "cores/EdlEdit.h"
#include "cores/FFmpeg.h"
#include "cores/VideoPlayer/Interface/InputStreamConstants.h"
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "filesystem/File.h"
#include "guilib/GUIComponent.h"
#include "guilib/StereoscopicsManager.h"
#include "input/actions/Action.h"
#include "input/actions/ActionIDs.h"
#include "interfaces/AnnouncementManager.h"
#include "jobs/JobQueue.h"
#include "messaging/ApplicationMessenger.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "utils/AMLUtils.h"
#include "utils/FontUtils.h"
#include "utils/MathUtils.h"
#include "utils/PresentationTimeline.h"
#include "windowing/amlogic/DisplayState.h"
#include "windowing/amlogic/GraphicsRegime.h"
#include "windowing/amlogic/OutputWorker.h"
#include "windowing/amlogic/SettingsMirror.h"
#include "windowing/amlogic/ThreadRole.h"
#include "windowing/amlogic/VideoSyncAML.h"
#include "windowing/amlogic/WinSystemAmlogic.h"
#include "utils/StreamDetails.h"
#include "utils/StreamUtils.h"
#include "utils/StringUtils.h"
#include "utils/TimeUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "utils/log.h"
#include "video/Bookmark.h"
#include "video/VideoInfoTag.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>

#if defined(HAS_LIBAMCODEC)
#include "windowing/amlogic/GraphicsTick.h" // GT-D15: the video path's log queue
#endif

using namespace KODI;
using namespace KODI::UTILS;
using namespace std::chrono_literals;

//------------------------------------------------------------------------------
// selection streams
//------------------------------------------------------------------------------

#define PREDICATE_RETURN(lh, rh) \
  do { \
    if((lh) != (rh)) \
      return (lh) > (rh); \
  } while(0)

namespace
{
//! 5.E2-7 (R5): names the caller of a draining close for P's drain hook, for its scope
struct CTxDrainSite
{
  CTxDrainSite(const char*& slot, const char* site) : m_slot(slot), m_prev(slot) { slot = site; }
  ~CTxDrainSite() { m_slot = m_prev; }
  CTxDrainSite(const CTxDrainSite&) = delete;
  CTxDrainSite& operator=(const CTxDrainSite&) = delete;
  const char*& m_slot;
  const char* m_prev;
};

// 5.E2-3: a P hook's HEAD for the shadow word's skip lines
OUTPUT::COutputWorker::SWordHead ToWordHead(std::pair<const char*, bool> head)
{
  return {head.first, head.second};
}

bool IsKnownLanguage(const CLanguageTag& language)
{
  return !language.IsUndetermined();
}

#if defined(HAS_LIBAMCODEC)
// test (vconfigure_regdelay "<ms>"): main applies the player's posted register <ms> late
std::chrono::milliseconds TestRegisterDelay()
{
  static const int ms = []
  {
    const auto line = OUTPUT::ReadTestSwitch("vconfigure_regdelay");
    const int value = line ? std::atoi(line->c_str()) : 0;
    if (value > 0)
      CLog::Log(LOGWARNING, "VCONFIGURE test: the render-loop register applied {} ms late", value);
    return value;
  }();
  return std::chrono::milliseconds(ms);
}

// test (vconfigure_legacyclient): a legacy render-loop client unregistered off main while main
// is inside its FrameMove must return only after that call (RetroPlayer's contract)
void TestLegacyRenderLoopClient()
{
  static std::once_flag once;
  std::call_once(once,
                 []
                 {
                   if (!OUTPUT::ReadTestSwitch("vconfigure_legacyclient"))
                     return;
                   struct CClient : IRenderLoop
                   {
                     std::atomic<int> state{0}; // 1 inside its first FrameMove, 2 returned
                     void FrameMove() override
                     {
                       if (state != 0)
                         return;
                       state = 1;
                       std::this_thread::sleep_for(500ms);
                       state = 2;
                     }
                   };
                   auto client = std::make_shared<CClient>();
                   CServiceBroker::GetWinSystem()->RegisterRenderLoop(client.get());
                   std::thread(
                       [client]
                       {
                         for (int i = 0; i < 2000 && client->state == 0; ++i)
                           std::this_thread::sleep_for(5ms);
                         const bool inside = client->state == 1;
                         CServiceBroker::GetWinSystem()->UnregisterRenderLoop(client.get());
                         CLog::Log(LOGWARNING,
                                   "VCONFIGURE test: legacy client unregistered off main {}",
                                   !inside             ? "without a FrameMove in progress"
                                   : client->state == 2 ? "after its FrameMove returned"
                                                        : "BEFORE its FrameMove returned");
                       })
                       .detach();
                 });
}
#endif

// How long one stream player may sit at the sync handshake with a full queue
// while its partner never arrives, before the wait is treated as a deadlock
// rather than slowness. Comfortably longer than any legitimate handshake, and
// short enough that a viewer sees a hiccup instead of a freeze.
constexpr auto SYNC_STUCK_TIMEOUT = std::chrono::milliseconds(3000);

// Reopen attempts allowed per playback for a stream player whose thread has
// exited. Past this the stream is dropped: something is repeatably wrong, and
// playing on without it beats an endless open/die cycle.
constexpr int MAX_PLAYER_RESTARTS = 3;
} // unnamed namespace

class PredicateSubtitleFilter
{
private:
  // Played audio language,
  // may differ from the language setting if the movie does not provide it the desired language
  CLanguageTag m_playedAudioLang;
  CLanguageTag m_subLang;
  bool m_isPrefOriginal;
  bool m_isPrefForced;
  bool m_isPrefHearingImp;
  bool m_isSubNone;
  bool m_hideSameAudioLang;
  int m_subStream;

public:
  /*
   * \brief The class' operator() decides if the given (subtitle) SelectionStream can match user settings, so relevant.
   *        If the subtitle is relevant "false" is returned.
   * \param[in] playedAudioLang The language actually playing, which may differ from the language
   *            setting where the movie does not carry the desired one.
   */
  explicit PredicateSubtitleFilter(const CLanguageTag& playedAudioLang, int subStream)
    : m_playedAudioLang(playedAudioLang),
      m_subStream(subStream)
  {
    auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
    const std::string subLangSetting =
        settings->GetString(CSettings::SETTING_LOCALE_SUBTITLELANGUAGE);

    m_isSubNone = StringUtils::EqualsNoCase(subLangSetting, LANGINFO::subLanguageNone);
    m_isPrefOriginal = StringUtils::EqualsNoCase(subLangSetting, LANGINFO::subLanguageOriginal);
    m_isPrefForced = StringUtils::EqualsNoCase(subLangSetting, LANGINFO::subLanguageForcedOnly);
    m_isPrefHearingImp = settings->GetBool(CSettings::SETTING_ACCESSIBILITY_SUBHEARING);
    // The setting keeps its value while disabled for none and forced_only
    m_hideSameAudioLang = !m_isSubNone && !m_isPrefForced &&
                          settings->GetBool(CSettings::SETTING_SUBTITLES_HIDESAMEAUDIOLANGUAGE);

    // Prefer the subtitle language setting; none, original and forced_only name no language, so
    // fall back to the audio setting, and default, original and mediadefault name none either,
    // so fall back to the language actually playing
    m_subLang = g_langInfo.GetSubtitleLanguage(false);
    if (m_subLang.IsEmpty())
      m_subLang = g_langInfo.GetAudioLanguage(false);
    if (m_subLang.IsEmpty())
      m_subLang = m_playedAudioLang;

    // Dont allow "forced" setting to be combined with "impaired" setting
    if (m_isPrefHearingImp && m_isPrefForced)
      m_isPrefForced = false;
  };

  // \brief Whether a stream is in the language the settings ask for
  bool MatchesSubtitleLanguage(const CLanguageTag& language) const
  {
    return language.Matches(m_subLang);
  }

  // \brief Whether a stream is in the language of the audio being played. Both languages must be
  //        declared, a stream that states none is never assumed to match.
  bool MatchesPlayedAudioLanguage(const CLanguageTag& language) const
  {
    return IsKnownLanguage(m_playedAudioLang) && IsKnownLanguage(language) &&
           language.Matches(m_playedAudioLang);
  }

  // \brief Whether a stream is a forced one that takes the place of the subtitles hidden for being
  //        in the audio language, which is also the language the settings ask for
  bool IsForcedForHiddenAudioLanguage(const SelectionStream& ss) const
  {
    return m_hideSameAudioLang && (ss.flags & FLAG_FORCED) &&
           MatchesPlayedAudioLanguage(ss.language) && MatchesSubtitleLanguage(ss.language);
  }

  // \brief Whether subtitles in the audio language are hidden
  bool HidesSameAudioLanguage() const { return m_hideSameAudioLang; }
  // \brief Whether the subtitle language setting is "original"
  bool IsPreferredOriginal() const { return m_isPrefOriginal; }
  // \brief Whether the subtitle language setting is "forced_only"
  bool IsPreferredForced() const { return m_isPrefForced; }
  // \brief Whether subtitles for the hearing impaired are preferred
  bool IsPreferredHearingImpaired() const { return m_isPrefHearingImp; }

  bool operator()(const SelectionStream& ss) const
  {
    if (ss.type_index == m_subStream)
      return false;

    if (m_isSubNone)
      return true;

    const bool isExternal = STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_DEMUX_SUB ||
                            STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_TEXT;
    const bool isCC = STREAM_SOURCE_MASK(ss.source) == STREAM_SOURCE_VIDEOMUX;

    // External subtitles with unknown language always allow it
    if (isExternal && !IsKnownLanguage(ss.language))
    {
      return false;
    }

    const bool isSameSubLang = MatchesSubtitleLanguage(ss.language);

    // The user does not want to read subtitles in a language they are already listening to.
    // Forced subtitles take their place, as they usually only translate foreign language parts.
    if (IsForcedForHiddenAudioLanguage(ss))
      return false;

    if (m_hideSameAudioLang && (ss.flags & FLAG_FORCED) == 0 &&
        MatchesPlayedAudioLanguage(ss.language))
    {
      return true;
    }

    if (m_isPrefHearingImp)
    {
      const int checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED;
      if ((ss.flags & checkFlags) == checkFlags)
        return false;

      // to consider CC streams may not have the language code
      if ((ss.flags & StreamFlags::FLAG_HEARING_IMPAIRED) &&
          (isSameSubLang || (isCC && !IsKnownLanguage(ss.language))))
      {
        return false;
      }
      // fallback to regular subtitles
      if (isSameSubLang && (ss.flags & FLAG_FORCED) == 0)
        return false;

      return true;
    }

    if (m_isPrefOriginal)
    {
      if ((ss.flags & FLAG_ORIGINAL))
        return false;
    }
    else if (m_isPrefForced)
    {
      if ((ss.flags & StreamFlags::FLAG_FORCED) && isSameSubLang)
        return false;
      else
        return true;
    }

    // can fall here only when "forced" and "impaired" are disabled,
    // it always enable subs if language is unknown for external and CC
    if ((isSameSubLang || (isCC && !IsKnownLanguage(ss.language))) &&
        (ss.flags & FLAG_FORCED) == 0 && (ss.flags & FLAG_HEARING_IMPAIRED) == 0)
    {
      return false;
    }

    return true;
  }
};

class PredicateAudioFilter
{
private:
  int currentAudioStream;
  StreamUtils::AudioPreferences preferences;

public:
  explicit PredicateAudioFilter(int audioStream, bool preferStereo)
    : currentAudioStream(audioStream),
      preferences(StreamUtils::AudioPreferences::Current())
  {
    // Follows from the audio output layout rather than a setting
    preferences.preferStereo = preferStereo;
  };
  bool operator()(const SelectionStream& lh, const SelectionStream& rh)
  {
    // A stream remembered from a previous watch outranks everything
    PREDICATE_RETURN(lh.type_index == currentAudioStream
                     , rh.type_index == currentAudioStream);

    // Everything below this is shared with the library
    return StreamUtils::CompareAudioPreference({.language = lh.language,
                                                .codec = lh.codec,
                                                .channels = lh.channels,
                                                .flags = lh.flags},
                                               {.language = rh.language,
                                                .codec = rh.codec,
                                                .channels = rh.channels,
                                                .flags = rh.flags},
                                               preferences) > 0;
  };
};

/*
 * \brief The class' operator() decides if the given (subtitle) SelectionStream lh is 'better than'
 *        the given (subtitle) SelectionStream rh.
 *        If lh is 'better than' rh the return value is true, false otherwise.
 *        The priority sequence is exactly as shown by the code sequence of the operator() method.
 *
 *        NOTE: Dont exists a "default" setting for subtitles, as there is for audio (media default),
 *              so the default flag will give a priority over another only when two streams
 *              have same properties (e.g. same language with a same flag, but a different codec, author, etc...).
 */
class PredicateSubtitlePriority
{
private:
  // Holds the resolved settings this predicate ranks by, and decides whether a stream is
  // relevant at all
  PredicateSubtitleFilter m_filter;
  int m_subStream;

public:
  explicit PredicateSubtitlePriority(const CLanguageTag& playedAudioLang, int stream)
    : m_filter(playedAudioLang, stream),
      m_subStream(stream)
  {
  }

  // \brief Check if a stream can match the user settings
  bool relevant(const SelectionStream& ss) const { return !m_filter(ss); }

  bool operator()(const SelectionStream& lh, const SelectionStream& rh) const
  {
    PREDICATE_RETURN(lh.type_index == m_subStream, rh.type_index == m_subStream);

    const bool isLexternal = STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_DEMUX_SUB ||
                             STREAM_SOURCE_MASK(lh.source) == STREAM_SOURCE_TEXT;
    const bool isRexternal = STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_DEMUX_SUB ||
                             STREAM_SOURCE_MASK(rh.source) == STREAM_SOURCE_TEXT;

    if (m_filter.HidesSameAudioLanguage())
    {
      // External subtitles stay ahead of the forced ones only when they are shown, as those in
      // the audio language are hidden as well
      PREDICATE_RETURN(isLexternal && relevant(lh), isRexternal && relevant(rh));
      PREDICATE_RETURN(m_filter.IsForcedForHiddenAudioLanguage(lh),
                       m_filter.IsForcedForHiddenAudioLanguage(rh));
    }

    // prefer external subs (note that this prevents any fallback to internal subs)
    PREDICATE_RETURN(isLexternal, isRexternal);

    const bool isLSameSubLang = m_filter.MatchesSubtitleLanguage(lh.language);
    const bool isRSameSubLang = m_filter.MatchesSubtitleLanguage(rh.language);

    // "is included" is used to not consider forced and impaired
    const bool isLincluded =
        (lh.flags & FLAG_FORCED) == 0 && (lh.flags & FLAG_HEARING_IMPAIRED) == 0;
    const bool isRincluded =
        (rh.flags & FLAG_FORCED) == 0 && (rh.flags & FLAG_HEARING_IMPAIRED) == 0;

    if (m_filter.IsPreferredHearingImpaired())
    {
      if (m_filter.IsPreferredOriginal())
      {
        int checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED | FLAG_DEFAULT;
        PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags,
                         (rh.flags & checkFlags) == checkFlags);

        checkFlags = FLAG_ORIGINAL | FLAG_HEARING_IMPAIRED;
        PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags,
                         (rh.flags & checkFlags) == checkFlags);
      }

      int checkFlags = FLAG_HEARING_IMPAIRED | FLAG_DEFAULT;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      checkFlags = FLAG_HEARING_IMPAIRED;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);
    }

    if (m_filter.IsPreferredOriginal())
    {
      // try find original (default) in audio language
      const int checkFlags = FLAG_ORIGINAL | FLAG_DEFAULT;
      PREDICATE_RETURN(isLincluded && (lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       isRincluded && (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      // try find original in audio language
      PREDICATE_RETURN(isLincluded && (lh.flags & FLAG_ORIGINAL) && isLSameSubLang,
                       isRincluded && (rh.flags & FLAG_ORIGINAL) && isRSameSubLang);

      // try find original (default) with any language
      PREDICATE_RETURN(isLincluded && (lh.flags & checkFlags) == checkFlags,
                       isRincluded && (rh.flags & checkFlags) == checkFlags);

      // try find original with any language
      PREDICATE_RETURN(isLincluded && (lh.flags & FLAG_ORIGINAL),
                       isRincluded && (rh.flags & FLAG_ORIGINAL));
    }
    else if (m_filter.IsPreferredForced())
    {
      const int checkFlags = FLAG_FORCED | FLAG_DEFAULT;
      PREDICATE_RETURN((lh.flags & checkFlags) == checkFlags && isLSameSubLang,
                       (rh.flags & checkFlags) == checkFlags && isRSameSubLang);

      PREDICATE_RETURN((lh.flags & FLAG_FORCED) && isLSameSubLang,
                       (rh.flags & FLAG_FORCED) && isRSameSubLang);

      // dont return false here, allow you to fallback on regular sub
      // its just listitem pre-selection courtesy, in any case the sub will be not enabled
    }

    // try find regular (default)
    PREDICATE_RETURN(isLincluded && lh.flags & FLAG_DEFAULT && isLSameSubLang,
                     isRincluded && rh.flags & FLAG_DEFAULT && isRSameSubLang);
    // try find regular
    PREDICATE_RETURN(isLincluded && isLSameSubLang, isRincluded && isRSameSubLang);

    // if all previous conditions do not match, allow fallback to "unknown" language
    if (!m_filter.IsPreferredForced())
    {
      PREDICATE_RETURN(isLincluded && !IsKnownLanguage(lh.language),
                       isRincluded && !IsKnownLanguage(rh.language));
    }

    return false;
  }
};

class PredicateVideoFilter
{
private:
  int currentVideoStream;
public:
  explicit PredicateVideoFilter(int videoStream) : currentVideoStream(videoStream)
  {
  };
  bool operator()(const SelectionStream& lh, const SelectionStream& rh)
  {
    PREDICATE_RETURN(lh.type_index == currentVideoStream,
                     rh.type_index == currentVideoStream);

    PREDICATE_RETURN(lh.flags & StreamFlags::FLAG_DEFAULT,
                     rh.flags & StreamFlags::FLAG_DEFAULT);
    return false;
  }
};

void CSelectionStreams::Clear(StreamType type, StreamSource source)
{
  auto new_end = std::remove_if(m_Streams.begin(), m_Streams.end(),
                                [type, source](const SelectionStream& stream)
                                {
                                  return (type == StreamType::NONE || stream.type == type) &&
                                         (source == 0 || stream.source == source);
                                });
  m_Streams.erase(new_end, m_Streams.end());
}

SelectionStream& CSelectionStreams::Get(StreamType type, int index)
{
  return const_cast<SelectionStream&>(std::as_const(*this).Get(type, index));
}

const SelectionStream& CSelectionStreams::Get(StreamType type, int index) const
{
  int count = -1;
  for (size_t i = 0; i < m_Streams.size(); ++i)
  {
    if (m_Streams[i].type != type)
      continue;
    count++;
    if (count == index)
      return m_Streams[i];
  }
  return m_invalid;
}

std::vector<SelectionStream> CSelectionStreams::Get(StreamType type)
{
  std::vector<SelectionStream> streams;
  std::ranges::copy_if(m_Streams, std::back_inserter(streams),
                       [type](const SelectionStream& stream) { return stream.type == type; });
  return streams;
}

bool CSelectionStreams::Get(StreamType type, StreamFlags flag, SelectionStream& out)
{
  for(size_t i=0;i<m_Streams.size();i++)
  {
    if(m_Streams[i].type != type)
      continue;
    if((m_Streams[i].flags & flag) != flag)
      continue;
    out = m_Streams[i];
    return true;
  }
  return false;
}

bool CSelectionStreams::Contains(StreamType type, int source, int64_t demuxerId, int id) const
{
  if (id < 0)
    return false;

  return std::any_of(m_Streams.cbegin(), m_Streams.cend(),
                     [type, source, demuxerId, id](const SelectionStream& stream)
                     {
                       return stream.type == type && stream.source == source &&
                              stream.demuxerId == demuxerId && stream.id == id;
                     });
}

int CSelectionStreams::TypeIndexOf(StreamType type, int source, int64_t demuxerId, int id) const
{
  if (id < 0)
    return -1;

  auto it = std::find_if(m_Streams.begin(), m_Streams.end(),
    [&](const SelectionStream& stream) {return stream.type == type
    && stream.source == source && stream.id == id
    && stream.demuxerId == demuxerId;});

  if (it != m_Streams.end())
    return it->type_index;
  else
    return -1;
}

int CSelectionStreams::Source(StreamSource source, const std::string& filename)
{
  int index = source - 1;
  for (size_t i=0; i<m_Streams.size(); i++)
  {
    SelectionStream &s = m_Streams[i];
    if (STREAM_SOURCE_MASK(s.source) != source)
      continue;
    // if it already exists, return same
    if (s.filename == filename)
      return s.source;
    if (index < s.source)
      index = s.source;
  }
  // return next index
  return index + 1;
}

void CSelectionStreams::Update(SelectionStream& s)
{
  int index = TypeIndexOf(s.type, s.source, s.demuxerId, s.id);
  if(index >= 0)
  {
    SelectionStream& o = Get(s.type, index);
    s.type_index = o.type_index;
    o = s;
  }
  else
  {
    s.type_index = CountType(s.type);
    m_Streams.push_back(s);
  }
}

void CSelectionStreams::Update(const std::shared_ptr<CDVDInputStream>& input,
                               CDVDDemux* demuxer,
                               const std::string& filename2)
{
  if(input && input->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> nav = std::static_pointer_cast<CDVDInputStreamNavigator>(input);
    std::string filename = nav->GetFileName();
    int source = Source(STREAM_SOURCE_NAV, filename);

    std::vector<CDemuxStream*> demuxStreams;
    if (demuxer)
      demuxStreams = demuxer->GetStreams();

    int count;
    count = nav->GetAudioStreamCount();
    for(int i=0;i<count;i++)
    {
      const auto stream =
          std::find_if(demuxStreams.begin(), demuxStreams.end(), [i](const auto& strm)
                       { return strm->type == StreamType::AUDIO && strm->dvdNavId == i; });
      const CDemuxStreamAudio* aStream =
          (stream != demuxStreams.end()) ? static_cast<CDemuxStreamAudio*>(*stream) : nullptr;

      SelectionStream s;
      s.source   = source;
      s.type = StreamType::AUDIO;
      s.id       = i;
      s.flags    = StreamFlags::FLAG_NONE;
      s.filename = filename;

      AudioStreamInfo info = nav->GetAudioStreamInfo(i);
      s.name     = info.name;
      s.codec    = info.codecName;
      // additional/more reliable info from ffmpeg than IFO nav data
      if (aStream)
      {
        s.codecDesc = aStream->GetStreamType();
        s.channels = aStream->iChannels;
        s.bitrate = aStream->iBitRate;
      }
      else
      {
        s.codecDesc = info.codecDesc;
        s.channels = info.channels;
      }
      s.language = info.language;
      s.flags = info.flags;
      Update(s);
    }

    count = nav->GetSubTitleStreamCount();
    for(int i=0;i<count;i++)
    {
      SelectionStream s;
      s.source   = source;
      s.type = StreamType::SUBTITLE;
      s.id       = i;
      s.filename = filename;
      s.channels = 0;

      SubtitleStreamInfo info = nav->GetSubtitleStreamInfo(i);
      s.name     = info.name;
      s.codec = info.codecName;
      s.codecDesc = info.codecDesc;
      s.flags = info.flags;
      s.language = info.language;
      Update(s);
    }

    VideoStreamInfo info = nav->GetVideoStreamInfo();
    for (int i = 1; i <= info.angles; i++)
    {
      SelectionStream s;
      s.source = source;
      s.type = StreamType::VIDEO;
      s.id = i;
      s.flags = StreamFlags::FLAG_NONE;
      s.filename = filename;
      s.channels = 0;
      s.aspect_ratio = info.videoAspectRatio;
      s.width = info.width;
      s.height = info.height;
      s.codec = info.codecName;
      s.name = StringUtils::Format(
          "{} {}", CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(38032), i);
      Update(s);
    }
  }
  else if(demuxer)
  {
    std::string filename = demuxer->GetFileName();
    int source;
    if(input) /* hack to know this is sub decoder */
      source = Source(STREAM_SOURCE_DEMUX, filename);
    else if (!filename2.empty())
      source = Source(STREAM_SOURCE_DEMUX_SUB, filename);
    else
      source = Source(STREAM_SOURCE_VIDEOMUX, filename);

    for (auto stream : demuxer->GetStreams())
    {
      /* skip streams with no type */
      if (stream->type == StreamType::NONE)
        continue;
      /* make sure stream is marked with right source */
      stream->source = source;

      SelectionStream s;
      s.source   = source;
      s.type     = stream->type;
      s.id       = stream->uniqueId;
      s.demuxerId = stream->demuxerId;
      s.language = stream->language;
      s.flags    = stream->flags;
      s.filename = demuxer->GetFileName();
      s.filename2 = filename2;
      s.name = stream->GetStreamName();
      s.codec    = demuxer->GetStreamCodecName(stream->demuxerId, stream->uniqueId);
      s.codecId = stream->codec;
      s.profile = stream->profile;
      s.channels = 0; // Default to 0. Overwrite if STREAM_AUDIO below.
      if (stream->type == StreamType::VIDEO)
      {
        CDemuxStreamVideo* vstream = static_cast<CDemuxStreamVideo*>(stream);
        s.width = vstream->iWidth;
        s.height = vstream->iHeight;
        s.aspect_ratio = vstream->fAspect;
        s.stereo_mode = vstream->stereo_mode;
        s.bitrate = vstream->iBitRate;
        s.hdrType = vstream->hdr_type;
        s.dovi = vstream->dovi;
        s.fpsRate = static_cast<uint32_t>(vstream->iFpsRate);
        s.fpsScale = static_cast<uint32_t>(vstream->iFpsScale);
      }
      if (stream->type == StreamType::AUDIO)
      {
        s.codecDesc = static_cast<CDemuxStreamAudio*>(stream)->GetStreamType();
        s.channels = static_cast<CDemuxStreamAudio*>(stream)->iChannels;
        s.bitrate = static_cast<CDemuxStreamAudio*>(stream)->iBitRate;
      }
      if (stream->type == StreamType::SUBTITLE)
      {
        s.codecDesc = static_cast<CDemuxStreamSubtitle*>(stream)->GetStreamType();
      }
      Update(s);
    }
  }
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

void CSelectionStreams::Update(const std::shared_ptr<CDVDInputStream>& input, CDVDDemux* demuxer)
{
  Update(input, demuxer, "");
}

int CSelectionStreams::CountTypeOfSource(StreamType type, StreamSource source) const
{
  return std::count_if(m_Streams.begin(), m_Streams.end(),
    [&](const SelectionStream& stream) {return (stream.type == type) && (stream.source == source);});
}

int CSelectionStreams::CountType(StreamType type) const
{
  return std::count_if(m_Streams.begin(), m_Streams.end(),
                       [&](const SelectionStream& stream) { return stream.type == type; });
}

//------------------------------------------------------------------------------
// main class
//------------------------------------------------------------------------------

void CVideoPlayer::CreatePlayers()
{
  if (m_players_created)
    return;

  m_VideoPlayerVideo =
      std::make_unique<CVideoPlayerVideo>(&m_clock, &m_overlayContainer, m_messenger,
                                          m_renderManager, *m_processInfo, m_messageQueueTimeSize);
  m_VideoPlayerAudio = std::make_unique<CVideoPlayerAudio>(&m_clock, m_messenger, *m_processInfo,
                                                           m_messageQueueTimeSize);
  m_VideoPlayerSubtitle =
      std::make_unique<CVideoPlayerSubtitle>(&m_overlayContainer, *m_processInfo);
  m_VideoPlayerTeletext = std::make_unique<CDVDTeletextData>(*m_processInfo);
  m_VideoPlayerRadioRDS = std::make_unique<CDVDRadioRDSData>(*m_processInfo);
  m_VideoPlayerAudioID3 = std::make_unique<CVideoPlayerAudioID3>(*m_processInfo);
  m_players_created = true;
}

void CVideoPlayer::DestroyPlayers()
{
  if (!m_players_created)
    return;

  m_VideoPlayerVideo.reset();
  m_VideoPlayerAudio.reset();
  m_VideoPlayerSubtitle.reset();
  m_VideoPlayerTeletext.reset();
  m_VideoPlayerRadioRDS.reset();
  m_VideoPlayerAudioID3.reset();

  m_players_created = false;
}

namespace
{
// 5.E2-8b D5, D6: one Start job reply, carried to P on its messenger (PLAYER_TX_REPLY)
struct STxReply
{
  enum class Kind
  {
    OUTPUT_DONE,
    SETTLE_DONE,
    ADOPTED,
    RESTORED, //!< 5.E2-11 D3.2: a Restore inside STARTING settled at e
  } kind = Kind::OUTPUT_DONE;
  uint64_t T = 0;
  uint32_t e = 0;
  bool committed = false;
  uint64_t seq = 0;
  unsigned int jobSeq = 0; //!< 5.E2-12a D2.1 (RESTORED: none, keyed on T only)
  DISPLAYSTATE::StartSettleKind settle = DISPLAYSTATE::StartSettleKind::DONE;
  DISPLAYSTATE::StartAdoptForm form = DISPLAYSTATE::StartAdoptForm::FAILED;
};

CRenderManager::TxAnswer ToTxAnswer(DISPLAYSTATE::StartAnswer answer)
{
  using A = DISPLAYSTATE::StartAnswer;
  using R = CRenderManager::TxAnswer;
  switch (answer)
  {
    case A::ADOPTED:
      return R::ADOPTED;
    case A::BEHIND:
      return R::BEHIND;
    case A::BEHIND_GUI:
      return R::BEHIND_GUI;
    case A::FAILED:
      return R::FAILED;
    case A::FAILED_SET:
      return R::FAILED_SET;
    case A::RECOVERING:
      return R::RECOVERING;
    case A::RECOVERED:
      return R::RECOVERED;
    case A::NO_CHANGE: // 5.G-3: a mid-play job's only, never a Start job's
      break;
  }
  return R::FAILED;
}

// 5.E2-9b D1: the caching state's name for the pending speed's lines
const char* TxCachingName(int state)
{
  switch (state)
  {
    case 0:
      return "DONE";
    case 1:
      return "FULL";
    case 2:
      return "INIT";
    case 3:
      return "PLAY";
    case 4:
      return "FLUSH";
  }
  return "?";
}
} // namespace

// 5.E2-8b D5 (lifetime): P's receiver of its Start jobs' replies. Each call runs under the job's
// sink mutex, on W (sometimes inside m_resourceSection) or on main: it only queues on P's
// messenger, never waits; the adopt answer goes to RM on main's stack, as D5b says
class CTxStartSink : public DISPLAYSTATE::IStartJobSink
{
public:
  CTxStartSink(CDVDMessageQueue& messenger, CRenderManager& renderManager)
    : m_messenger(messenger), m_renderManager(renderManager)
  {
  }
  void OnOutputDone(const DISPLAYSTATE::SOutputDone& reply) override
  {
    STxReply r;
    r.kind = STxReply::Kind::OUTPUT_DONE;
    r.T = reply.T;
    r.committed = reply.committed;
    r.seq = reply.seq;
    r.jobSeq = reply.jobSeq;
    Put(r);
  }
  void OnSettleDone(const DISPLAYSTATE::SSettleDone& reply) override
  {
    STxReply r;
    r.kind = STxReply::Kind::SETTLE_DONE;
    r.T = reply.T;
    r.settle = reply.kind;
    r.jobSeq = reply.jobSeq;
    Put(r);
  }
  bool OnAdoptAnswer(unsigned int gen, DISPLAYSTATE::StartAnswer answer) override
  {
    return m_renderManager.TxAdoptAnswer(gen, ToTxAnswer(answer));
  }
  void OnTxAdopted(const DISPLAYSTATE::STxAdopted& reply) override
  {
    // r2 N-L6: queued behind the answer's PLAYER_AVCHANGE (both at priority 0, in order)
    STxReply r;
    r.kind = STxReply::Kind::ADOPTED;
    r.T = reply.T;
    r.form = reply.form;
    r.jobSeq = reply.jobSeq;
    Put(r);
  }
  void OnRestoreSettled(uint64_t T, uint32_t e) override
  {
    STxReply r;
    r.kind = STxReply::Kind::RESTORED;
    r.T = T;
    r.e = e;
    Put(r);
  }

private:
  void Put(const STxReply& r)
  {
    m_messenger.Put(std::make_shared<CDVDMsgType<STxReply>>(CDVDMsg::PLAYER_TX_REPLY, r));
  }
  CDVDMessageQueue& m_messenger;
  CRenderManager& m_renderManager;
};

// 5.G-3 (code review r1 S3): process-wide, so a stopped player's late verdict or reply never
// matches a later player's job
static std::atomic<unsigned int> s_midJobSeq{0};

// 5.G-3 (docs/phase5_g0_design.md §4.3 c): one mid-play job's receiver. Its replies go to P's
// inbox (TxDeferPass takes it every pass, while frozen too); the adopt answer goes to RM on
// main's stack
class CTxMidSink : public DISPLAYSTATE::IStartJobSink
{
public:
  //! av false: a pick's job (5.G-5), whose adopt posts no AVCHANGE (today's set posts none)
  CTxMidSink(CRenderManager& renderManager, unsigned int seq, bool av)
    : m_renderManager(renderManager), m_seq(seq), m_av(av)
  {
  }
  void OnOutputDone(const DISPLAYSTATE::SOutputDone& reply) override
  {
    DISPLAYSTATE::STxInboxEntry e = Entry(Mid::OUTPUT_DONE);
    e.committed = reply.committed;
    DISPLAYSTATE::PostTxInbox(std::move(e));
  }
  void OnSettleDone(const DISPLAYSTATE::SSettleDone& reply) override
  {
    DISPLAYSTATE::STxInboxEntry e = Entry(Mid::SETTLE_DONE);
    e.settle = reply.kind;
    DISPLAYSTATE::PostTxInbox(std::move(e));
  }
  bool OnAdoptAnswer(unsigned int, DISPLAYSTATE::StartAnswer answer) override
  {
    using A = DISPLAYSTATE::StartAnswer;
    // P posts a no-change's and a failed set's at W's SettleDone; RECOVERED follows RECOVERING
    const char* name = answer == A::NO_CHANGE    ? "DONE_NO_CHANGE"
                       : answer == A::FAILED_SET ? "FAILED"
                       : answer == A::RECOVERED  ? "DONE (recovered)"
                       : answer == A::ADOPTED    ? "DONE"
                                                 : "DONE (not adopted)";
    return m_renderManager.TxMidAdoptAnswer(
        m_seq,
        m_av && answer != A::NO_CHANGE && answer != A::FAILED_SET && answer != A::RECOVERED,
        name);
  }
  void OnTxAdopted(const DISPLAYSTATE::STxAdopted& reply) override
  {
    DISPLAYSTATE::STxInboxEntry e = Entry(Mid::ADOPTED);
    e.form = reply.form;
    DISPLAYSTATE::PostTxInbox(std::move(e));
  }
  void OnRestoreSettled(uint64_t, uint32_t) override {}

private:
  using Mid = DISPLAYSTATE::STxInboxEntry::Mid;
  DISPLAYSTATE::STxInboxEntry Entry(Mid mid) const
  {
    DISPLAYSTATE::STxInboxEntry e;
    e.kind = DISPLAYSTATE::STxInboxEntry::Kind::MIDPLAY;
    e.mid = mid;
    e.jobSeq = m_seq;
    return e;
  }
  CRenderManager& m_renderManager;
  const unsigned int m_seq;
  const bool m_av;
};

CVideoPlayer::CVideoPlayer(IPlayerCallback& callback)
  : IPlayer(callback),
    CThread("VideoPlayer"),
    m_CurrentAudio(StreamType::AUDIO, VideoPlayer_AUDIO),
    m_CurrentVideo(StreamType::VIDEO, VideoPlayer_VIDEO),
    m_CurrentSubtitle(StreamType::SUBTITLE, VideoPlayer_SUBTITLE),
    m_CurrentTeletext(StreamType::TELETEXT, VideoPlayer_TELETEXT),
    m_CurrentRadioRDS(StreamType::RADIO_RDS, VideoPlayer_RDS),
    m_CurrentAudioID3(StreamType::AUDIO_ID3, VideoPlayer_ID3),
    m_messenger("player"),
    m_outboundEvents(std::make_unique<CJobQueue>(false, 1, CJob::PRIORITY_NORMAL)),
    m_pInputStream(nullptr),
    m_pDemuxer(nullptr),
    m_pSubtitleDemuxer(nullptr),
    m_pCCDemuxer(nullptr),
    m_renderManager(m_clock, this)
{
  m_players_created = false;

  m_dvd.Clear();
  m_State.Clear();

  // Let the overlay renderer see the container so it can clear a stale overlay
  // plane when the container empties during a stall (menu->playback / stop).
  // Both are members of this player, so the pointer is valid for its lifetime.
  m_renderManager.SetOverlayContainer(&m_overlayContainer);

  m_bAbortRequest = false;
  m_offset_pts = 0.0;
  m_seamOffsetPts = 0.0;
  ClearSeams();
  m_playSpeed = DVD_PLAYSPEED_NORMAL;
  m_streamPlayerSpeed = DVD_PLAYSPEED_NORMAL;
  m_caching = CACHESTATE_DONE;
  m_HasVideo = false;
  m_HasAudio = false;

  int tenthsSeconds = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      CSettings::SETTING_VIDEOPLAYER_QUEUETIMESIZE);
  // The queue-size settings were dropped from settings.xml with the dynamic
  // data sizer, so GetInt returns 0 here -> SetMaxTimeSize(0) -> 1/0 -> an
  // INFINITE queue time cap. Data-bounded-only queues hold minutes of
  // low-bitrate content (TNG HDMV menus: 120s audio / 90s video observed
  // queued), running the disc VM minutes ahead of presentation and turning
  // every title jump into a monster drain; it also feeds 0 into the caching
  // math below. Floor to the largest offered option (16s) - high-bitrate
  // streams stay data-bound well below it.
  if (tenthsSeconds <= 0)
    tenthsSeconds = 160;

  m_messageQueueTimeSize = static_cast<double>(tenthsSeconds) / 10.0;

  m_SkipCommercials = true;

  m_processInfo.reset(CProcessInfo::CreateInstance());
  // if we have a gui, register the cache
  m_processInfo->SetDataCache(&CServiceBroker::GetDataCacheCore());
  m_processInfo->SetSpeed(1.0);
  m_processInfo->SetTempo(1.0);
  m_processInfo->SetFrameAdvance(false);

  CreatePlayers();

  m_displayLost = false;
  m_renderManager.SetDisplayLost(false);
  m_error = false;
  m_bCloseRequest = false;
  if (auto system = CServiceBroker::GetWinSystem(); system != nullptr)
    system->Register(this);
}

CVideoPlayer::~CVideoPlayer()
{
  if (auto system = CServiceBroker::GetWinSystem(); system != nullptr)
    system->Unregister(this);

  CloseFile();
  // 5.E2-8b D5 (lifetime): the sink cleared (OnExit did, unless the thread never ran)
  TxReleaseJob("player destroyed");
  if (m_renderLoopHandle)
  {
    // waits at most for main's call of this player's own FrameMove
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(m_renderLoopHandle->section);
    m_renderLoopHandle->dead = true;
    m_renderLoopDead = true;
    CLog::Log(LOGINFO, "RENDERLOOP handle dead gen={} (waited {} ms)", m_renderLoopHandle->gen,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
  }
  DestroyPlayers();
  ReleaseAudioSessionHold();

  while (m_outboundEvents->IsProcessing())
  {
    CThread::Sleep(10ms);
  }
}

bool CVideoPlayer::OpenFile(const CFileItem& file, const CPlayerOptions &options)
{
  CLog::Log(LOGINFO, "VideoPlayer::OpenFile: {}", CURL::GetRedacted(file.GetPath()));
  OUTPUT::DisarmDemuxBlock();

  BDSTAGE::Play();

  // Stash the path for the Amlogic DV L5 active-area detector, which runs on the
  // codec thread where g_application.CurrentFile() is not yet set.
  aml_dv_detect_set_file(file.GetPath());
  m_testReopenDone = false;
  // 5.E2-8b D2 (r4 L4): GC's fullscreen-exit counter at the open, so a Back between here and the
  // engage counts
  const uint64_t txExits = DISPLAYSTATE::FullScreenExits();
  uint64_t txInputsSeq = 0;
  // 5.E2-5 (shadow): DECIDE's GUI-side inputs as they stand at the open; main's pump re-publishes
  // changes until P reads them at the set's first configure (off main the last publish stands)
  if (DISPLAYSTATE::DecideShadowOn() && CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    DISPLAYSTATE::PublishDecideInputs("open", true, true);
    // 5.E2-8b D2 (r4 M1, r5 L3): the forced publish's seq travels with the open to P
    txInputsSeq = DISPLAYSTATE::GetDecideInputs().seq;
  }

  // real_player: a start is held until the output mode is final (disc session
  // design 3); the debug flag turns it off for A/B runs
  m_heldStartEnabled = options.fullscreen;
  const bool heldStartOff = XFILE::CFile::Exists("special://profile/heldstart_off");
  if (heldStartOff)
  {
    m_heldStartEnabled = false;
    CLog::Log(LOGWARNING, "VideoPlayer: DEBUG held start disabled");
  }
  // start order design §3: a start that needs a mode change opens no audio output
  // until the mode is set (A/B flag)
  m_startGateEnabled = aml_presentation_validated() && !heldStartOff;
  if (m_startGateEnabled && XFILE::CFile::Exists("special://profile/startgate_off"))
  {
    m_startGateEnabled = false;
    CLog::Log(LOGWARNING, "VideoPlayer: DEBUG start gate disabled");
  }
  // design §15 step 2.2: a held start resumes at a scheduled instant on the
  // timeline (A/B flag)
  m_scheduledStart = (aml_presentation_validated() && PRESENTATION::TimelineClockActive() &&
                      !XFILE::CFile::Exists("special://profile/scheduled_start_off")) ||
                     XFILE::CFile::Exists("special://profile/scheduled_start");
  // design §15.33: glided seams crossed per stream by byte position (A/B flag)
  m_seamByPos =
      aml_presentation_validated() && !XFILE::CFile::Exists("special://profile/seamstep_off");
  // P5-13: a seamless seam read before the start completes glides too (A/B flag)
  m_earlyGlide = m_seamByPos && !XFILE::CFile::Exists("special://profile/earlyglide_off");
  // design 15.34: an audio track switch re-cues the audio alone (A/B flag)
  m_recueEnabled = m_seamByPos && !XFILE::CFile::Exists("special://profile/recue_off");
  // design 5 (4b): the audio output is held for the session; the debug flag
  // turns it off for A/B runs
  if (XFILE::CFile::Exists("special://profile/audiohold_off"))
    CLog::Log(LOGWARNING, "VideoPlayer: DEBUG audio session hold disabled");
  else if (IAE* ae = CServiceBroker::GetActiveAE())
  {
    ae->SetSessionHold(true);
    m_audioSessionHold = true;
    CLog::Log(LOGINFO, "VideoPlayer: audio session hold on");
  }
  // design 4.3: a disc join keeps the last picture; the debug flag turns it off
  m_keepFrameEnabled = !XFILE::CFile::Exists("special://profile/keepframe_off");
  if (!m_keepFrameEnabled)
    CLog::Log(LOGWARNING, "VideoPlayer: DEBUG kept picture across disc reinits disabled");
  m_bdjKeepAliveDebug = XFILE::CFile::Exists("special://profile/bdj_keepalive");
  if (m_bdjKeepAliveDebug)
    CLog::Log(LOGWARNING, "VideoPlayer: DEBUG BD-J keep-alive transitions enabled");

  if (IsRunning())
  {
    CDVDMsgOpenFile::FileParams params;
    params.m_item = file;
    params.m_options = options;
    params.m_item.SetMimeTypeForInternetFile();
    params.m_txInputsSeq = txInputsSeq;
    params.m_txExits = txExits;
    // 5.E2-8b test (displaystate_mainstall straddle, code review A-L2): main's pass straddles OPEN
    m_renderManager.TxTestStraddleRaise();
    m_messenger.Put(std::make_shared<CDVDMsgOpenFile>(params), 1);

    return true;
  }

  m_item = file;
  m_playerOptions = options;
  // 5.E2-8b D2: the first open's snapshot goes with its options, before P's thread exists
  // E2-8b NOTE: no open message on a first open: written as m_playerOptions is, before Create()
  m_txMsgInputsSeq = txInputsSeq;
  m_txMsgExits = txExits;

  m_processInfo->SetPlayTimes(0,0,0,0);
  m_bAbortRequest = false;
  m_error = false;
  m_bCloseRequest = false;
  m_renderManager.PreInit();
  // 5.E2-8b test (displaystate_mainstall straddle, code review A-L2), after PreInit's reset
  m_renderManager.TxTestStraddleRaise();

  Create();
  m_messenger.Init();

  m_callback.OnPlayBackStarted(m_item);

  return true;
}

bool CVideoPlayer::CloseFile(bool reopen)
{
  CLog::Log(LOGINFO, "CVideoPlayer::CloseFile()");

  // set the abort request so that other threads can finish up
  m_bAbortRequest = true;
  m_bCloseRequest = true;
  m_discMenuOnly = false;

  // tell demuxer to abort
  if(m_pDemuxer)
    m_pDemuxer->Abort();

  if(m_pSubtitleDemuxer)
    m_pSubtitleDemuxer->Abort();

  if(m_pInputStream)
    m_pInputStream->Abort();

  // If a BD menu / subtitle overlay is still presented, clear the overlay plane
  // before tearing the renderer down. At this point nothing else will overdraw
  // those pixels (the overlay renderer is about to be destroyed, the
  // fullscreen-video window's ClearBackground is a no-op, and the composite
  // clear is switched off during teardown), so the plane keeps scanning out the
  // stale menu until an unrelated full-screen redraw. Gated on HasVisibleOverlay
  // so seamless/overlay-free stops pay no cost; IsProcessThread ensures we are
  // on the rendering thread with a current GL context (the destructor path may
  // not be).
  if (CServiceBroker::GetAppMessenger()->IsProcessThread() && m_renderManager.HasVisibleOverlay())
  {
    if (auto system = CServiceBroker::GetWinSystem(); system != nullptr)
      system->ClearOverlayPlane();
  }

  m_renderManager.UnInit();

  CLog::Log(LOGINFO, "VideoPlayer: waiting for threads to exit");

  // a draining close in progress must not hold up the stop
  m_VideoPlayerVideo->AbortDrain();

  // wait for the main thread to finish up
  // since this main thread cleans up all other resources and threads
  // we are done after the StopThread call
  if (auto system = CServiceBroker::GetWinSystem(); system != nullptr)
  {
    CSingleExit exitlock(system->GetGfxContext());
    StopThread();
  }
  else
  {
    StopThread();
  }

  m_Edl.Clear();
  CServiceBroker::GetDataCacheCore().Reset();
  m_processInfo->SetDataCache(&CServiceBroker::GetDataCacheCore());

  m_HasVideo = false;
  m_HasAudio = false;

  CLog::Log(LOGINFO, "VideoPlayer: finished waiting");
  return true;
}

bool CVideoPlayer::IsPlaying() const
{
  return !m_bStop;
}

void CVideoPlayer::OnStartup()
{
  aml_hdr_conversion_player_open(true);
  m_syncStartPtsWait.reset();
  m_syncStartDeferred = false;
  m_CurrentVideo.Clear();
  m_CurrentAudio.Clear();
  m_CurrentSubtitle.Clear();
  m_CurrentTeletext.Clear();
  m_CurrentRadioRDS.Clear();
  m_CurrentAudioID3.Clear();
  m_audioPlayerRestarts = 0;
  m_videoPlayerRestarts = 0;

  UTILS::FONT::ClearTemporaryFonts();
}

bool CVideoPlayer::OpenInputStream()
{
  m_pInputBluray.reset();
  if (m_pInputStream.use_count() > 1)
    throw std::runtime_error("m_pInputStream reference count is greater than 1");
  m_pInputStream.reset();

  CLog::Log(LOGINFO, "Creating InputStream");

  m_pInputStream = CDVDFactoryInputStream::CreateInputStream(this, m_item, true);
  if (m_pInputStream == nullptr)
  {
    CLog::Log(LOGERROR, "CVideoPlayer::OpenInputStream - unable to create input stream for [{}]",
              CURL::GetRedacted(m_item.GetPath()));
    return false;
  }

  if (!m_pInputStream->Open())
  {
    CLog::Log(LOGERROR, "CVideoPlayer::OpenInputStream - error opening [{}]",
              CURL::GetRedacted(m_item.GetPath()));
    return false;
  }
  m_renderManager.SetDvdInput(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD));
  m_preadFileStartNs = 0;
  m_preadStallDone = false;
  m_starveResumeArmed = false;
  // issue 27: a stall of a plain input is a starvation, never a still (V holds its picture)
  PRESENTATION::PlayerReads().plainInput.store(
      !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
      !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) &&
      !std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream));

#if defined(HAVE_LIBBLURAY)
  m_pInputBluray = std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
  if (m_pInputBluray)
    m_discTime.Begin(m_pInputBluray->DiscTimeOn());
#endif

  // The dual-dimension queue bound (16s time floor) exists for the disc
  // VM-runahead class; applying it to non-disc playback capped low-bitrate
  // network streams that upstream let buffer minutes deep (review §C).
  // Non-disc restores the upstream data-bound-only behavior; disc keeps
  // the time bound.
  {
    const bool discInput = m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) ||
                           m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD);
    const double timeCap = discInput ? m_messageQueueTimeSize : 3600.0;
    m_VideoPlayerAudio->SetMaxTimeSize(timeCap);
    m_VideoPlayerVideo->SetMaxTimeSize(timeCap);
    CLog::Log(LOGDEBUG, "CVideoPlayer::OpenInputStream - queue time cap {:.0f}s ({} input)",
              timeCap, discInput ? "disc" : "non-disc");
  }

  // find any available external subtitles for non dvd files
  if (!m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) &&
      !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
  {
    // find any available external subtitles
    std::vector<std::string> filenames;

    if (!URIUtils::IsUPnP(m_item.GetPath()) &&
        !m_item.GetProperty("no-ext-subs-scan").asBoolean(false))
      CUtil::ScanForExternalSubtitles(m_item.GetDynPath(), filenames);

    // load any subtitles from file item
    std::string key("subtitle:1");
    for (unsigned s = 1; m_item.HasProperty(key); key = StringUtils::Format("subtitle:{}", ++s))
      filenames.push_back(m_item.GetProperty(key).asString());

    for (unsigned int i=0;i<filenames.size();i++)
    {
      // if vobsub subtitle:
      if (URIUtils::HasExtension(filenames[i], ".idx"))
      {
        std::string strSubFile;
        if (CUtil::FindVobSubPair( filenames, filenames[i], strSubFile))
          AddSubtitleFile(filenames[i], strSubFile);
      }
      else
      {
        if (!CUtil::IsVobSub(filenames, filenames[i] ))
        {
          AddSubtitleFile(filenames[i]);
        }
      }
    } // end loop over all subtitle files
  }

  m_displayResumeOnGrid = m_scheduledStart && !m_pInputStream->IsRealtime();
#if defined(HAS_LIBAMCODEC)
  // test (vconfigure_tweakvideo): the tweak and the offset on the video, as for a realtime input
  static const bool tweakVideo = XFILE::CFile::Exists("special://profile/vconfigure_tweakvideo");
  if (tweakVideo && m_displayResumeOnGrid)
  {
    m_displayResumeOnGrid = false;
    CLog::Log(LOGWARNING, "VCONFIGURE test: the tweak on the video, as for a realtime input");
  }
#endif
  // the audio offset moves the sound, landed with every start (P4-20)
  m_audioOffsetToAudio = m_displayResumeOnGrid.load();
  {
    const double offset =
        m_audioOffsetToAudio
            ? static_cast<double>(m_processInfo->GetVideoSettings().m_AudioDelay) * DVD_TIME_BASE
            : 0.0;
    m_clock.SetAudioOffset(offset);
    m_requestedAudioOffset = offset;
    m_landedAudioOffset = offset;
    m_renderManager.SetDelay(
        m_audioOffsetToAudio
            ? 0
            : static_cast<int>(m_processInfo->GetVideoSettings().m_AudioDelay * 1000.0f));
    // the display latency tweak too: the picture would move by whole frames (P4-24)
    m_renderManager.SetLatencyTweakToAudio(m_audioOffsetToAudio);
  }
  // with the tweak and offset on the audio, a grid landing's phase is 0
  m_clock.SetGridPhase(m_audioOffsetToAudio);
  m_clock.Reset();
  m_dvd.Clear();

  return true;
}

bool CVideoPlayer::OpenDemuxStream()
{
  CloseDemuxer();

  CLog::Log(LOGINFO, "Creating Demuxer");

  int attempts = 10;
  while (!m_bStop && attempts-- > 0)
  {
    m_pDemuxer.reset(CDVDFactoryDemuxer::CreateDemuxer(m_pInputStream));
    if(!m_pDemuxer && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
    {
      continue;
    }
    else if(!m_pDemuxer && m_pInputStream->NextStream() != CDVDInputStream::NEXTSTREAM_NONE)
    {
      CLog::Log(LOGDEBUG, "{} - New stream available from input, retry open", __FUNCTION__);
      continue;
    }
    break;
  }

  if (!m_pDemuxer)
  {
    CLog::Log(LOGERROR, "{} - Error creating demuxer", __FUNCTION__);
    return false;
  }

  m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX);
  m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_NAV);
  m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
  m_pDemuxer->GetPrograms(m_programs);
  UpdateContent();
  m_demuxerSpeed = DVD_PLAYSPEED_NORMAL;
  m_processInfo->SetStateRealtime(false);

  int64_t len = m_pInputStream->GetLength();
  int64_t tim = m_pDemuxer->GetStreamLength();
  if (len > 0 && tim > 0)
    m_pInputStream->SetReadRate(static_cast<uint32_t>(len * 1000 / tim));

  m_offset_pts = 0;
  m_seamOffsetPts = 0.0;
  ClearSeams();
#if defined(HAVE_LIBBLURAY)
  ResetSegmentEnd();
  if (m_pInputBluray)
    m_pInputBluray->SetBdjDemuxerOpen(true);
#endif

  if (m_updateStreamDetails)
  {
    CFileItem item;
    UpdateFileItemStreamDetails(item, UpdateStreamDetails::ALWAYS_UPDATE);
    if (item.HasVideoInfoTag())
      m_pInputStream->SaveCurrentState(item.GetVideoInfoTag()->m_streamDetails);
  }
  else
    m_pInputStream->SaveCurrentState({});

  return true;
}

// A pending mark converts with the first record of its own clip within 2.5 s
// before it (a record further ahead waits for a nearer one: a short playlist
// read whole has its marks seconds into the clip); it gives up at any packet
// of a later title-byte generation or past its clip, at that clip's last
// record if there is one (design 15.55).
void CVideoPlayer::ResolvePendingMarks(int64_t packetPos, uint32_t packetGen)
{
  const CCurrentStream& ref = m_CurrentVideo.id >= 0 ? m_CurrentVideo : m_CurrentAudio;
  bool anyPending = false;
  for (SDiscTimelineEvent& ev : m_discTimelineEvents)
  {
    if (!ev.markPending)
      continue;
    const bool refInClip = ref.markRefDts != DVD_NOPTS_VALUE && ref.markRefGen == ev.markGen &&
                           ref.markRefPos >= ev.markClipStart && ref.markRefPos < ev.markClipEnd;
    if (refInClip)
    {
      constexpr int64_t wrap = int64_t{1} << 33;
      int64_t delta = (static_cast<int64_t>(ev.markPts45) * 2 - ref.markRefRawDts) % wrap;
      if (delta < 0)
        delta += wrap;
      if (delta >= wrap / 2)
        delta -= wrap;
      const double offset = static_cast<double>(delta) * DVD_TIME_BASE / 90000.0;
      if (offset > DVD_SEC_TO_TIME(2.5))
      {
        anyPending = true;
        continue;
      }
      ev.presentPts = ref.markRefDts + std::max(offset, 0.0 - DVD_SEC_TO_TIME(1.0));
      ev.markPending = false;
      CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J mark (seq {}) at its picture {:.3f}, with its clip's packets",
                ev.bdjReleaseSeq, ev.presentPts / DVD_TIME_BASE);
      continue;
    }
    const bool passed =
        packetPos >= 0 &&
        (static_cast<int32_t>(packetGen - ev.markGen) > 0 ||
         (packetGen == ev.markGen && packetPos >= ev.markClipEnd));
    if (passed)
    {
      // the first record past its clip; else (another stream passed it first) the stamp
      ev.presentPts = ref.markRefDts != DVD_NOPTS_VALUE && ref.markRefGen == ev.markGen &&
                              ref.markRefPos >= ev.markClipEnd
                          ? ref.markRefDts
                          : DVD_NOPTS_VALUE;
      ev.markPending = false;
      CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J mark (seq {}) has no packet in its clip: at {}",
                ev.bdjReleaseSeq,
                ev.presentPts != DVD_NOPTS_VALUE ? StringUtils::Format("{:.3f}", ev.presentPts / DVD_TIME_BASE)
                                                 : std::string("its stamp"));
      continue;
    }
    anyPending = true;
  }
  m_bdjMarksPending = anyPending;
}

void CVideoPlayer::ResetSegmentEnd()
{
  m_segmentVideoPtsEnd = DVD_NOPTS_VALUE;
  m_segmentAudioPtsEnd = DVD_NOPTS_VALUE;
  m_bdjAllDelivered = false;
}

void CVideoPlayer::CloseDemuxer()
{
  // held ELs belong to this demuxer's clip; a clip after it can reuse its timestamps
  ClearPendingElPackets();
  m_pDemuxer.reset();
  m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX);
#if defined(HAVE_LIBBLURAY)
  if (m_pInputBluray)
    m_pInputBluray->SetBdjDemuxerOpen(false);
  ResetSegmentEnd();
#endif

  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

void CVideoPlayer::UpdateHasVideoAudio()
{
  // a playlist item transition (e.g. music video to audio-only track) reuses this
  // running player instance without a CloseFile(), so a stale true from the previous
  // item must be cleared here if no stream was opened for the current item
  if (m_CurrentVideo.id < 0)
    m_HasVideo = false;
  if (m_CurrentAudio.id < 0)
    m_HasAudio = false;
}

void CVideoPlayer::OpenDefaultStreams(bool reset)
{
  // if input stream dictate, we will open later
  // unless we are loading a bluray playlist directly in which case set now
  const bool noBlurayMenu{m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
                          m_State.menuType == MenuType::NONE};
  if (!noBlurayMenu && (m_dvd.iSelectedAudioStream >= 0 || m_dvd.iSelectedSPUStream >= 0))
    return;

  bool valid;

  // open video stream
  valid   = false;

  // A file offering two full video streams of different HDR formats gets the
  // one the user's preference selects, unless they have already picked a stream
  // for this item by hand - that choice outranks a format preference.
  int videoStreamPref = m_processInfo->GetVideoSettings().m_VideoStream;
  const int preferredVideoId = m_pDemuxer ? m_pDemuxer->GetPreferredVideoStream() : -1;
  if (videoStreamPref < 0 && preferredVideoId >= 0)
  {
    for (const auto& s : m_SelectionStreams.Get(StreamType::VIDEO))
    {
      // Qualified by source: a DVD's nav streams are numbered 1..angles and can
      // land on the same value as a demuxer uniqueId.
      if (s.id == preferredVideoId && STREAM_SOURCE_MASK(s.source) == STREAM_SOURCE_DEMUX &&
          s.demuxerId == m_pDemuxer->GetDemuxerId())
      {
        videoStreamPref = s.type_index;
        break;
      }
    }
  }

  PredicateVideoFilter vf(videoStreamPref);
  for (const auto& stream : m_SelectionStreams.Get(StreamType::VIDEO, vf))
  {
    // choose video base layer as default if dual layer stream
    // only demuxer streams can be dual layer, nav and external streams are not
    // known to the demuxer and must not be probed for it
    if (m_pDemuxer && STREAM_SOURCE_MASK(stream.source) == STREAM_SOURCE_DEMUX)
    {
      CDemuxStream* st = m_pDemuxer->GetStream(stream.demuxerId, stream.id);
      if (st && st->type == StreamType::VIDEO)
      {
        CDemuxStreamVideo* vstream = static_cast<CDemuxStreamVideo*>(st);
        if (vstream->isDualStream && vstream->isELStream)
          continue;
      }
    }

    if (OpenStream(m_CurrentVideo, stream.demuxerId, stream.id, stream.source, reset))
    {
      valid = true;
      break;
    }
  }
  if (!valid)
  {
    {
      CTxDrainSite txSite(m_txDrainSite, "OpenDefaultStreams");
      CloseStream(m_CurrentVideo, true);
    }
    m_processInfo->ResetVideoCodecInfo();
  }

  // open audio stream
  valid = false;
  if (!m_playerOptions.videoOnly)
  {
    PredicateAudioFilter af(m_processInfo->GetVideoSettings().m_AudioStream, m_playerOptions.preferStereo);
    for (const auto& stream : m_SelectionStreams.Get(StreamType::AUDIO, af))
    {
      if(OpenStream(m_CurrentAudio, stream.demuxerId, stream.id, stream.source, reset))
      {
        valid = true;
        break;
      }
    }
  }

  if(!valid)
  {
    {
      CTxDrainSite txSite(m_txDrainSite, "OpenDefaultStreams");
      CloseStream(m_CurrentAudio, true);
    }
    m_processInfo->ResetAudioCodecInfo();
  }

  UpdateHasVideoAudio();

  // enable or disable subtitles
  bool visible = m_processInfo->GetVideoSettings().m_SubtitleOn;
  const bool isDefaultVideosettings =
      m_processInfo->GetVideoSettings().m_isDefaultVideoSettings.value_or(true);

  // open subtitle stream
  SelectionStream as = m_SelectionStreams.Get(StreamType::AUDIO, GetAudioStream());
  PredicateSubtitlePriority psp(as.language, m_processInfo->GetVideoSettings().m_SubtitleStream);
  valid = false;
  // We need to close CC subtitles to avoid conflicts with external sub stream
  if (m_CurrentSubtitle.source == STREAM_SOURCE_VIDEOMUX)
    CloseStream(m_CurrentSubtitle, false);

  for (const auto& stream : m_SelectionStreams.Get(StreamType::SUBTITLE, psp))
  {
    if (OpenStream(m_CurrentSubtitle, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      // default settings: let the predicates control sub visibility
      // video specific settings: respect the user's choice
      if (isDefaultVideosettings)
      {
        if (!psp.relevant(stream))
          visible = false;

        // Image type subtitles (e.g. VOBSUB) can support "forced" flag on overlays (images)
        // so you need to keep the stream open to parse "forced" flag on each image
        // since we leave the stream open by default, it is necessary to close it
        // if the language does not match the preferences.
        if (!visible && StreamUtils::IsCodecSupportForcedOverlay(stream.codecId) &&
            !stream.language.Matches(as.language))
        {
          valid = false;
        }
      }
      break;
    }
  }

  if (!valid)
  {
    CloseStream(m_CurrentSubtitle, false);
    m_processInfo->ResetSubtitleCodecInfo();
  }

  // only set subtitle visibility if state not stored by dvd navigator, because navigator will restore it (if visible)
  if (!std::dynamic_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream) ||
      m_playerOptions.state.empty())
  {
    // SetEnableStream only if not visible, when visible OpenStream already implied that stream is enabled
    if (valid && !visible)
      SetEnableStream(m_CurrentSubtitle, false);

    SetSubtitleVisibleInternal(visible);
  }

  // open teletext stream
  valid   = false;
  for (const auto& stream : m_SelectionStreams.Get(StreamType::TELETEXT))
  {
    if (OpenStream(m_CurrentTeletext, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if(!valid)
    CloseStream(m_CurrentTeletext, false);

  // open RDS stream
  valid   = false;
  for (const auto& stream : m_SelectionStreams.Get(StreamType::RADIO_RDS))
  {
    if (OpenStream(m_CurrentRadioRDS, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if(!valid)
    CloseStream(m_CurrentRadioRDS, false);

  // open ID3 stream
  valid = false;
  for (const auto& stream : m_SelectionStreams.Get(StreamType::AUDIO_ID3))
  {
    if (OpenStream(m_CurrentAudioID3, stream.demuxerId, stream.id, stream.source))
    {
      valid = true;
      break;
    }
  }
  if (!valid)
    CloseStream(m_CurrentAudioID3, false);

  // disable demux streams
  if (NETWORK::IsRemote(m_item) && m_pDemuxer)
  {
    for (auto &stream : m_SelectionStreams.m_Streams)
    {
      if (STREAM_SOURCE_MASK(stream.source) == STREAM_SOURCE_DEMUX)
      {
        if (stream.id != m_CurrentVideo.id && stream.id != m_CurrentAudio.id &&
            stream.id != m_CurrentSubtitle.id && stream.id != m_CurrentTeletext.id &&
            stream.id != m_CurrentRadioRDS.id && stream.id != m_CurrentAudioID3.id)
        {
          m_pDemuxer->EnableStream(stream.demuxerId, stream.id, false);
        }
      }
    }
  }

  if (noBlurayMenu)
    SynchronizeDemuxer();
}

void CVideoPlayer::TestReadStall(int64_t nowNs)
{
  // test (issue 27): special://profile/pread_stall holds "<ms> <at_s>": the first demuxer read
  // after <at_s> s of reading a file sleeps <ms>, inside the read P's stamps cover. A negative
  // <ms> sleeps until the coordinator judged the outputs starved, then -<ms> more (bounded at
  // 600 s): the read then outlasts the queues, however much they hold
  static const std::pair<int, int> stall = []
  {
    const auto line = OUTPUT::ReadTestSwitch("pread_stall");
    const int ms = line ? std::atoi(line->c_str()) : 0;
    const auto space = line ? line->find(' ') : std::string::npos;
    const int at = space == std::string::npos ? 0 : std::max(0, std::atoi(line->c_str() + space));
    if (ms > 0)
      CLog::Log(LOGWARNING, "PREAD test: the first read {} s into each file sleeps {} ms", at, ms);
    else if (ms < 0)
      CLog::Log(LOGWARNING,
                "PREAD test: the first read {} s into each file sleeps until a starvation, then "
                "{} ms",
                at, -ms);
    return std::pair<int, int>{ms, at};
  }();
  if (stall.first == 0 || m_preadStallDone)
    return;
  if (!m_preadFileStartNs)
    m_preadFileStartNs = nowNs;
  if (nowNs - m_preadFileStartNs < static_cast<int64_t>(stall.second) * 1000000000)
    return;
  m_preadStallDone = true;
  // the queues the outputs play from while P sleeps: a stall shorter than them starves nothing.
  // Seconds queued: the last demuxed dts of the stream ahead of the clock (-1: none)
  const double clock = m_clock.GetClock();
  const auto ahead = [clock](double dts)
  { return dts == DVD_NOPTS_VALUE ? -1.0 : (dts - clock) / DVD_TIME_BASE; };
  CLog::Log(LOGWARNING,
            "PREAD test: read stall {} ms now (clock {:.3f}, queued video {:.1f} s audio {:.1f} s, "
            "levels video {}% audio {}%)",
            stall.first, clock / DVD_TIME_BASE, ahead(m_CurrentVideo.dts),
            ahead(m_CurrentAudio.dts), m_processInfo->GetLevelVQ(), m_VideoPlayerAudio->GetLevel());
  if (stall.first > 0)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(stall.first));
    return;
  }
  const auto steadyNs = []
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  const int64_t limitNs = nowNs + 600000000000LL;
  int64_t starvedAt = 0;
  while (steadyNs() < limitNs)
  {
    const int64_t starved = PRESENTATION::PlayerReads().starvedNs.load(std::memory_order_relaxed);
    if (!starvedAt && starved >= nowNs)
    {
      starvedAt = starved;
      CLog::Log(LOGWARNING, "PREAD test: starvation judged after {} ms, {} ms more",
                (starved - nowNs) / 1000000, -stall.first);
    }
    if (starvedAt && steadyNs() - starvedAt >= static_cast<int64_t>(-stall.first) * 1000000)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CLog::Log(LOGWARNING, "PREAD test: no starvation in 600 s, the read returns");
}

bool CVideoPlayer::ReadPacket(DemuxPacket*& packet, CDemuxStream*& stream)
{
  OUTPUT::TestDemuxBlock();

  // check if we should read from subtitle demuxer
  if (m_pSubtitleDemuxer && m_VideoPlayerSubtitle->AcceptsData())
  {
    packet = m_pSubtitleDemuxer->Read();

    if(packet)
    {
      packet->demuxDts = packet->dts;
      UpdateCorrection(packet, VideoOffsetPts());
      packet->m_seamOffsetCorrection += m_seamOffsetPts;
      if(packet->iStreamId < 0)
        return true;

      stream = m_pSubtitleDemuxer->GetStream(packet->demuxerId, packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "{} - Error demux packet doesn't belong to a valid stream",
                  __FUNCTION__);
        return false;
      }
      if (stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX_SUB);
        m_SelectionStreams.Update(NULL, m_pSubtitleDemuxer.get());
        UpdateContent();
      }
      return true;
    }
  }

  // read a data frame from stream.
  if (m_pDemuxer)
  {
    // issue 27 (log only): the read in progress, for the coordinator's starvation measurement
    PRESENTATION::SPlayerReads& reads = PRESENTATION::PlayerReads();
    const int64_t readStart = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
    reads.readStartNs.store(readStart, std::memory_order_release);
    TestReadStall(readStart);
    packet = m_pDemuxer->Read();
    // an audio track switch reads again what the other streams already have
    while (packet && !m_recueGate.empty() && RecueGateDrops(packet))
    {
      CDVDDemuxUtils::FreeDemuxPacket(packet);
      packet = m_pDemuxer->Read();
    }
    const int64_t readNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count() -
                           readStart;
    // seq_cst: the coordinator sets a freeze, then re-reads this stamp (issue 27)
    reads.readStartNs.store(0);
    double frozenAt = 0.0;
    double ranOn = 0.0;
    if (m_clock.EndStarveFreeze(frozenAt, ranOn))
    {
      // the outputs ran dry during this read: the stall path from the clock where it stopped,
      // so the resume lands at the frame on screen (nothing skipped)
      CLog::Log(LOGINFO,
                "STARVE freeze ended: clock paused at {:.3f} (the read took {} ms, the clock "
                "would have run on {:.0f} ms)",
                frozenAt / DVD_TIME_BASE, readNs / 1000000, ranOn / 1000.0);
      m_starveResumeArmed = true;
      m_starveAudioNext = DVD_NOPTS_VALUE;
      SetCaching(CACHESTATE_FULL);
    }
    reads.reads.fetch_add(1, std::memory_order_relaxed);
    int64_t was = reads.maxNs.load(std::memory_order_relaxed);
    while (readNs > was && !reads.maxNs.compare_exchange_weak(was, readNs))
    {
    }
    if (readNs > 100000000)
      reads.over100ms.fetch_add(1, std::memory_order_relaxed);
    if (readNs > 1000000000)
      reads.over1s.fetch_add(1, std::memory_order_relaxed);
  }

  if (packet)
  {
    // stream changed, update and open defaults
    if (packet->iStreamId == DMX_SPECIALID_STREAMCHANGE)
    {
      m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX);
      m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
      m_pDemuxer->GetPrograms(m_programs);
      UpdateContent();
      OpenDefaultStreams(false);

      // OpenDefaultStreams() returns early while DVD/BD navigation controls stream
      // selection, so reevaluate here as well (we may have switched from/to a radio
      // channel)
      UpdateHasVideoAudio();

      return true;
    }

    // Capture the demuxer's own dts before any correction touches it. The BL/EL
    // pairing in the Amlogic codec needs a timeline that is identical for both
    // layers of a frame; dts is not, because the enhancement layer bypasses
    // CheckContinuity and so misses the correction the base layer receives at a
    // seamless playitem boundary.
    packet->demuxDts = packet->dts;
    double seamOffset = 0.0;
    double crossed = 0.0;
    if (m_seamByPos)
    {
      // FFmpeg can return the incoming clip's packets before the glide is committed
      TakeSeamMarks();
      if (packet->iStreamId >= 0)
        seamOffset = CrossSeams(packet, crossed);
    }
    UpdateCorrection(packet, m_offset_pts + seamOffset);
    packet->m_seamOffsetCorrection += m_seamOffsetPts;
    if (m_seamByPos && packet->iStreamId >= 0)
      NoteDelivered(packet);
    if (crossed != 0.0)
    {
      CLog::Log(LOGDEBUG,
                "CVideoPlayer - stream {} crossed a seam at byte {}: step {:.3f} ms, offset {:.3f} ms",
                packet->iStreamId, packet->streamPos, crossed / 1000.0,
                (m_offset_pts + seamOffset) / 1000.0);
      // the base layer's restart, judged against its own end before the step
      // as CheckContinuity's backward restart is
      if (packet->iStreamId == m_CurrentVideo.id && packet->demuxerId == m_CurrentVideo.demuxerId &&
          m_playSpeed == DVD_PLAYSPEED_NORMAL && packet->dts != DVD_NOPTS_VALUE &&
          m_CurrentVideo.dts_end() != DVD_NOPTS_VALUE &&
          packet->dts + crossed + DVD_MSEC_TO_TIME(1000) < m_CurrentVideo.dts_end())
      {
        packet->timelineRestartSeq = ++m_timelineRestartSeq;
        CLog::Log(LOGDEBUG, "CVideoPlayer - timeline restart #{} stamped at the seam crossing",
                  m_timelineRestartSeq);
      }
    }

    if(packet->iStreamId < 0)
      return true;

    if(m_pDemuxer)
    {
      stream = m_pDemuxer->GetStream(packet->demuxerId, packet->iStreamId);
      if (!stream)
      {
        CLog::Log(LOGERROR, "{} - Error demux packet doesn't belong to a valid stream",
                  __FUNCTION__);
        return false;
      }
      if(stream->source == STREAM_SOURCE_NONE)
      {
        m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
        UpdateContent();
      }
    }
    return true;
  }
  return false;
}

void CVideoPlayer::HandleDynamicBufferLevel()
{
  // Run adjustment only every 250ms to avoid overreacting
  constexpr int64_t INTERVAL_NS = 250'000'000; // 250ms
  static int64_t lastAdjust = 0;
  const int64_t now = CurrentHostCounter();

  if (now - lastAdjust < INTERVAL_NS)
    return;

  lastAdjust = now;

  // Current buffer fill levels (0–100%)
  const int lvlv = m_VideoPlayerVideo->GetDataLevel();
  const int lvla = m_VideoPlayerAudio->GetDataLevel();

  // Current max buffer sizes
  const int vcOld = m_VideoPlayerVideo->GetMaxDataSize();
  const int acOld = m_VideoPlayerAudio->GetMaxDataSize();

  int vcNew = vcOld;
  int acNew = acOld;

  // Absolute difference between audio and video levels
  const int diff = std::abs(lvlv - lvla);

  // Fixed step sizes
  constexpr int vcStep = 4 * SIZE_1M;      // 4 MB
  constexpr int acStep = 512 * SIZE_1K;    // 0.5 MB

  auto inc = [](int cur, int step, int max) {
    const int v = cur + step;
    return (v <= max) ? v : cur;
  };

  auto dec = [](int cur, int step, int min) {
    const int v = cur - step;
    return (v >= min) ? v : cur;
  };

  // ---------------------------------------------------------
  // AUDIO BURST DETECTION (TrueHD, DTS-HD, EAC3 JOC)
  // ---------------------------------------------------------
  static int lastLvla = 0;
  const int deltaA = lvla - lastLvla;
  lastLvla = lvla;

  const bool audioBurst =
  (
      lvla > 85 &&          // audio buffer very full
      lvlv < 50 &&          // video buffer relatively low
      (lvla - lvlv) > 25 && // audio significantly fuller than video
      deltaA > 10           // rapid increase → real burst
  );

  // ---------------------------------------------------------
  // VIDEO BURST DETECTION (I-frames, HEVC/H.266 peaks)
  // ---------------------------------------------------------
  static int lastLvlv = 0;
  const int deltaV = lvlv - lastLvlv;
  lastLvlv = lvlv;

  const bool videoBurst =
  (
      lvlv > 85 &&          // video buffer very full
      lvla < 50 &&          // audio buffer relatively low
      (lvlv - lvla) > 25 && // video significantly fuller than audio
      deltaV > 10           // rapid increase → real burst
  );

  // -------------------------------------------------
  // INCREASE LOGIC
  // -------------------------------------------------
  if (!audioBurst && !videoBurst && diff > 20)
  {
    if (lvlv > lvla)
      vcNew = inc(vcOld, vcStep, LvLVideoMAX);
    else
      acNew = inc(acOld, acStep, LvLAudioMAX);
  }

  // -------------------------------------------------
  // DECREASE LOGIC
  //
  // Only decrease when:
  // - both buffers are reasonably filled (>20%)
  // - difference is small (<15)
  // -------------------------------------------------
  if (!audioBurst && !videoBurst &&
      diff < 15 &&
      lvlv > 20 &&
      lvla > 20)
  {
    if (lvlv < lvla)
      vcNew = dec(vcOld, vcStep, LvLVideoMIN);
    else
      acNew = dec(acOld, acStep, LvLAudioMIN);
  }

  // 5.E2-9c D1: the read-on's entry maximum stays fixed
  if (m_txReadOn == TxReadOn::ACTIVE)
    acNew = acOld;

  // ---------------------------------------------------------
  // APPLY CHANGES
  // ---------------------------------------------------------
  if (vcNew != vcOld)
  {
    CLog::Log(LOGDEBUG, LOGAVTIMING,
      "CVideoPlayer::{} {} video buffer to: {:d}MB, level video/audio: {:d}%/{:d}%",
      __FUNCTION__, (vcNew > vcOld) ? "increased" : "decreased",
      vcNew / SIZE_1M, lvlv, lvla);

    m_VideoPlayerVideo->SetMaxDataSize(vcNew);
  }

  if (acNew != acOld)
  {
    CLog::Log(LOGDEBUG, LOGAVTIMING,
      "CVideoPlayer::{} {} audio buffer to: {:.01f}MB, level video/audio: {:d}%/{:d}%",
      __FUNCTION__, (acNew > acOld) ? "increased" : "decreased",
      static_cast<double>(acNew) / SIZE_1M, lvlv, lvla);

    m_VideoPlayerAudio->SetMaxDataSize(acNew);
  }
}

// Timeline-stamped disc events (docs/bd_timeline_events_design.md): apply
// presentation-affecting disc state when the render clock reaches the demux
// position where the VM decided it. Phase 1 carries the presented menu
// state - IsInMenu() consumers (input routing, seek gating, GUI) then see
// the menu when the viewer does, not when the VM (queue-depth early) did.
// flushAll: a seek/jump/flush invalidates the stamps' timeline - snap the
// presented state to the latest demux truth instead of waiting on a clock
// position that may never arrive.
void CVideoPlayer::ApplyDiscTimelineEvents(bool flushAll, bool readerAtEnd)
{
#if defined(HAVE_LIBBLURAY)
  if (flushAll && m_pInputBluray)
    m_pInputBluray->ReleaseAllBdjEvents(true);
#endif
  // every pass, the queue empty or not, so the bound runs from the first entry
  // held to the next pass that holds nothing
  const DiscClockHold clockHold =
      flushAll ? (ResetDiscClockHold(), DiscClockHold::NONE) : UpdateDiscClockHold(readerAtEnd);
  if (m_discTimelineEvents.empty())
    return;

#if defined(HAVE_LIBBLURAY)
  const std::shared_ptr<CDVDInputStreamBluray>& bluray = m_pInputBluray;
  if (!bluray)
#endif
  {
    m_discTimelineEvents.clear();
    return;
  }

#if defined(HAVE_LIBBLURAY)
  const double clock = m_clock.GetClock();
  // a BD-J playlist player's start waiting for its first picture (patch 17)
  uint32_t startSeq = 0;
  bool startPending = bluray->BdjStartPending(startSeq);
  if (!startPending || flushAll)
    m_bdjStartWaitSince.reset();
  std::deque<SDiscTimelineEvent> keptForStart;
  while (!m_discTimelineEvents.empty())
  {
    SDiscTimelineEvent& ev = m_discTimelineEvents.front();
    if (ev.bdjReleaseSeq != 0 && startPending &&
        static_cast<int32_t>(ev.bdjReleaseSeq - startSeq) >= 0)
    {
      if (flushAll)
      {
        // the start's own items survive a flush (libbluray keeps them): its
        // first picture is the restart's, so the stamp is void
        bluray->ReleaseBdjEvents(ev.bdjReleaseSeq, 0, true);
        ev.stampPts = DVD_NOPTS_VALUE;
        ev.presentPts = DVD_NOPTS_VALUE;
        ev.markPending = false;
        ev.awaitSegment = false;
        keptForStart.push_back(ev);
        m_discTimelineEvents.pop_front();
        continue;
      }
      int64_t startNs = 0;
      if (ev.awaitSegment || !BdjStartInstant(ev.stampPts, clock, startNs))
      {
        // bounded like any held start (BdjStartInstant's own 15 s limit)
        if (ev.awaitSegment && m_bdjStartWaitSince &&
            std::chrono::steady_clock::now() - *m_bdjStartWaitSince > 15s)
          ev.awaitSegment = false;
        else if (ev.awaitSegment && !m_bdjStartWaitSince)
          m_bdjStartWaitSince = std::chrono::steady_clock::now();
        break;
      }
      bluray->ReleaseBdjEvents(ev.bdjReleaseSeq, startNs);
      m_bdjStartWaitSince.reset();
      m_discTimelineEvents.pop_front();
      // the start is presented: what was read after it releases by its stamp
      startPending = bluray->BdjStartPending(startSeq);
      continue;
    }
    // a held mark is due when its own picture is presented (design 15.50);
    // while the reader waits at the playlist's end, the last frame read may
    // never be delivered (ffmpeg emits a PES when the next starts), so not
    // after the last one that was
    if (ev.markPending && !flushAll)
    {
      // waiting for its clip's packets (design 15.55); bounded by the
      // playlist's end, by a reader waiting inside its read (a probe: no
      // packet can come until the read returns, which may wait on this mark),
      // and by 15 s of running clock (and of wall time: a clock jump forward)
      if (ev.markPendingClock == DVD_NOPTS_VALUE || clock < ev.markPendingClock)
        ev.markPendingClock = clock;
      const bool timedOut =
          clock - ev.markPendingClock > DVD_SEC_TO_TIME(15.0) &&
          std::chrono::steady_clock::now() - ev.markPendingSince > std::chrono::seconds(15);
      if (m_bdjAllDelivered || readerAtEnd || timedOut)
      {
        CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J mark (seq {}) released without its clip's packets",
                  ev.bdjReleaseSeq);
        ev.markPending = false;
      }
      else
        break;
    }
    // an entry read before a pending start's marker: the application holds
    // what it hears until its start, so it never waits ahead of the marker
    // (it would cost the start its lead, design 15.60)
    const bool beforeStart = ev.bdjReleaseSeq != 0 && startPending;
    // a mark at its picture is not due while a start's picture is still to
    // come (the scheduled lead): the clock sits at that picture meanwhile
    if (!flushAll && ev.presentPts != DVD_NOPTS_VALUE && !beforeStart)
    {
      int64_t pendingNs = 0;
      double pendingClock = 0.0;
      if (m_clock.GetPendingStart(pendingNs, pendingClock) && ev.presentPts >= pendingClock)
        break;
    }
    double due = ev.stampPts;
    if (ev.endOfPlaylist && !flushAll)
    {
      // the playlist's end: when its last picture (or sound) is presented,
      // once the demuxer delivered all of it (design 15.54); no stream: now
      if (!m_bdjAllDelivered)
        break;
      due = m_CurrentVideo.id >= 0 ? m_segmentVideoPtsEnd
            : m_CurrentAudio.id >= 0 ? m_segmentAudioPtsEnd
                                     : DVD_NOPTS_VALUE;
    }
    else if (ev.presentPts != DVD_NOPTS_VALUE)
    {
      due = ev.presentPts;
      const double end = m_CurrentVideo.dts_end();
      if (readerAtEnd && end != DVD_NOPTS_VALUE && end < due)
        due = std::max(end, ev.stampPts);
    }
    // a due time is on its streams' timeline: not judged by a clock that is
    // not theirs yet (design 15.60)
    if (!flushAll && due != DVD_NOPTS_VALUE && !beforeStart &&
        (clockHold == DiscClockHold::UNCOMMITTED ||
         (clockHold == DiscClockHold::START && ev.bdjReleaseSeq != 0)))
    {
      if (!m_discClockWaitSince)
        m_discClockWaitSince = std::chrono::steady_clock::now();
      break;
    }
    if (!flushAll && due != DVD_NOPTS_VALUE && clock < due)
    {
      // A stamp further ahead than the maximum queue depth (plus margin)
      // cannot be a real future position - the clock was corrected
      // backwards across a wrap/discontinuity after the stamp was taken.
      // Apply rather than stall the queue on a timeline that no longer
      // exists. Derived from the configured queue depth, not a constant:
      // raising queuetimesize via advancedsettings must not silently turn
      // legitimate deferred events into "stale" early-applies (review).
      if (due - clock < DVD_SEC_TO_TIME(m_messageQueueTimeSize + 4.0))
        break;
    }
    if (ev.bdjReleaseSeq != 0)
    {
      // the picture reached where the BD-J application's held notifications
      // were read (per read batch: logged only when a flush forces it)
      if (flushAll)
        CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J presentation timing - flush releases seq {}",
                  ev.bdjReleaseSeq);
      bluray->ReleaseBdjEvents(ev.bdjReleaseSeq);
      m_discTimelineEvents.pop_front();
      continue;
    }
    const std::string stamp =
        ev.stampPts == DVD_NOPTS_VALUE ? std::string("NOPTS")
                                       : StringUtils::Format("{:.3f}", ev.stampPts / DVD_TIME_BASE);
    if (ev.titleUi)
    {
      CLog::Log(LOGDEBUG,
                "CVideoPlayer: presented title UI -> playlist {} ({} chapters, {:.1f}s) "
                "(stamp {} clock {:.3f}{})",
                ev.titleUi->playlist, ev.titleUi->chapters.size(), ev.titleUi->totalTimeMs / 1000.0,
                stamp, clock / DVD_TIME_BASE, flushAll ? ", flush" : "");
      bluray->SetPresentedTitleUi(ev.titleUi);
    }
    else
    {
      CLog::Log(LOGDEBUG, "CVideoPlayer: presented menu state -> {} (stamp {} clock {:.3f}{})",
                ev.menuState, stamp, clock / DVD_TIME_BASE, flushAll ? ", flush" : "");
      bluray->SetPresentedMenuState(ev.menuState != 0);
    }
    m_discTimelineEvents.pop_front();
  }
  // a flush also covers what was held but not stamped yet
  if (flushAll)
  {
    bluray->ReleaseAllBdjEvents(true);
    m_discTimelineEvents.insert(m_discTimelineEvents.begin(), keptForStart.begin(),
                                keptForStart.end());
  }
#endif
}

#if defined(HAVE_LIBBLURAY)
// A held mark is reached when the reader passes the entry point before it, up
// to a GOP before its picture (design 15.50). Its own time converts onto the
// player's timeline with the last video packet processed, the one the read
// was stamped with: same clip, same correction. Only when that packet is
// provably in the mark's clip (same title-byte generation, inside its byte
// range, still the stamp) and the result lies after the stamp by less than a
// GOP plus the decode lead; otherwise the stamp stands (NOPTS).
double CVideoPlayer::BdjMarkPresentPts(uint32_t pts45, int64_t clipStart, int64_t clipEnd,
                                       double stamp, uint32_t readGen) const
{
  const CCurrentStream& video = m_CurrentVideo;
  if (stamp == DVD_NOPTS_VALUE || video.markRefDts == DVD_NOPTS_VALUE ||
      video.markRefDts != video.dts || video.markRefGen != readGen ||
      video.markRefPos < clipStart || video.markRefPos >= clipEnd)
    return DVD_NOPTS_VALUE;
  // ffmpeg unwraps the 33-bit timestamps: compare modulo 2^33
  constexpr int64_t wrap = int64_t{1} << 33;
  int64_t delta = (static_cast<int64_t>(pts45) * 2 - video.markRefRawDts) % wrap;
  if (delta < 0)
    delta += wrap;
  if (delta >= wrap / 2)
    delta -= wrap;
  const double present = video.markRefDts + static_cast<double>(delta) * DVD_TIME_BASE / 90000.0;
  if (present < stamp || present > stamp + DVD_SEC_TO_TIME(2.5))
    return DVD_NOPTS_VALUE;
  return present;
}

bool CVideoPlayer::TimelineCommitted() const
{
  const CCurrentStream& master = m_CurrentVideo.id >= 0 ? m_CurrentVideo : m_CurrentAudio;
  return master.id >= 0 && master.syncState == IDVDStreamPlayer::SYNC_INSYNC;
}

void CVideoPlayer::ResetDiscClockHold()
{
  m_discClockWaitSince.reset();
  m_discClockGateExpired = false;
}

CVideoPlayer::DiscClockHold CVideoPlayer::UpdateDiscClockHold(bool readerAtEnd)
{
  // not while the reader waits inside a read: the sync commit runs only once
  // it returns. Its wait doesn't count against the bound: a hold armed before
  // it is re-armed (the gate is skipped meanwhile, so nothing waits on it)
  if (readerAtEnd)
  {
    if (m_discClockWaitSince && !m_discClockGateExpired)
      m_discClockWaitSince = std::chrono::steady_clock::now();
    return DiscClockHold::NONE;
  }
  DiscClockHold hold = DiscClockHold::NONE;
  if (m_CurrentVideo.id >= 0 || m_CurrentAudio.id >= 0)
  {
    // the clock is still the previous timeline's; or it waits at a start's
    // first picture (held, or its lead): what the application hears after its
    // start, it hears from that picture on
    if (!TimelineCommitted())
      hold = DiscClockHold::UNCOMMITTED;
    else if (!ClockOnStreams())
      hold = DiscClockHold::START;
  }
  if (hold == DiscClockHold::NONE)
  {
    ResetDiscClockHold();
    return hold;
  }
  // armed by the first entry it holds (ApplyDiscTimelineEvents)
  if (m_discClockWaitSince && std::chrono::steady_clock::now() - *m_discClockWaitSince > 15s)
  {
    if (!m_discClockGateExpired)
      CLog::Log(LOGWARNING, "CVideoPlayer: disc timeline waited 15 s for its streams' clock, "
                            "released");
    m_discClockGateExpired = true;
  }
  return m_discClockGateExpired ? DiscClockHold::NONE : hold;
}

bool CVideoPlayer::ClockOnStreams()
{
  if (!TimelineCommitted() || m_startHeld)
    return false;
  int64_t ns = 0;
  double startClock = 0.0;
  return !m_clock.GetPendingStart(ns, startClock);
}

bool CVideoPlayer::BdjStartInstant(double stampPts, double clock, int64_t& startNs)
{
  // a start the application cannot be left waiting on (no sync ever comes:
  // a stream that never starts); libbluray's own limit is the same
  if (!m_bdjStartWaitSince)
    m_bdjStartWaitSince = std::chrono::steady_clock::now();
  else if (std::chrono::steady_clock::now() - *m_bdjStartWaitSince > 15s)
  {
    CLog::Log(LOGWARNING, "CVideoPlayer: BD-J start not presented in 15 s, released");
    startNs = 0;
    return true;
  }

  int64_t ns = 0;
  double startClock = 0.0;
  unsigned int epoch = 0;
  if (m_clock.GetPendingStart(ns, startClock))
  {
    // scheduled: released now with its instant, so the application draws
    // during the lead and its clock starts with the picture; held without a
    // schedule yet: wait for the release
    if (ns <= 0)
      return false;
    // a stamp past the scheduled start is a later segment's
    if (stampPts != DVD_NOPTS_VALUE && stampPts > startClock + DVD_MSEC_TO_TIME(50))
      return false;
    startNs = ns;
    CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J start released with the scheduled start");
    return true;
  }
  if (m_startHeld)
    return false;
  // no stream open: the segment the start belongs to is still to open (a
  // drain between playlists closes the old streams first) - unless its
  // demuxer is open and has delivered all of it with no stream to play (a
  // playlist of graphics only): no picture will come, so now (design 15.54)
  if (m_CurrentVideo.id < 0 && m_CurrentAudio.id < 0)
  {
    if (!m_pDemuxer || !m_bdjAllDelivered)
      return false;
    startNs = 0;
    CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J start released: its playlist has no stream to play");
    return true;
  }
  // a start still to come: a stream has not synced yet
  if ((m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
      (m_CurrentAudio.id >= 0 && m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_INSYNC))
    return false;
  if (stampPts == DVD_NOPTS_VALUE)
  {
    // its first picture came with a start that has landed (or no stream at
    // all to time it by): that start's instant, else now
    if (m_clock.GetScheduledStart(ns, startClock, epoch))
      startNs = ns;
    else
      startNs = 0;
    CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J start released after the start ({})",
              startNs ? "scheduled instant" : "now");
    return true;
  }
  // a start inside a running timeline (a natural-end loop, a seamless
  // carry): when the picture reaches the stamp
  // a stamp further ahead than the queue depth is on a timeline that no
  // longer exists (as for any disc timeline event): now
  if (clock < stampPts && stampPts - clock < DVD_SEC_TO_TIME(m_messageQueueTimeSize + 4.0))
    return false;
  if (clock < stampPts)
    stampPts = clock;
  struct timespec mono = {};
  clock_gettime(CLOCK_MONOTONIC, &mono);
  startNs = static_cast<int64_t>(mono.tv_sec) * 1000000000 + mono.tv_nsec -
            static_cast<int64_t>((clock - stampPts) * (1000000000.0 / DVD_TIME_BASE));
  CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J start released at its stamp {:.3f} (clock {:.3f})",
            stampPts / DVD_TIME_BASE, clock / DVD_TIME_BASE);
  return true;
}
#endif

// Menu-domain low-latency mode: while a Blu-ray is in menu domain, clamp the
// A/V queue read-ahead to ~1s so the disc VM (which executes at demux
// position) runs at roughly presentation time - menu input, overlay timing,
// title jumps and wrap decisions then act when the viewer sees them instead
// of up to the full queue depth early. Shrinking is only done when a video
// segment opens (segmentOpen), so a popup menu over a running feature never
// steals the feature's buffer depth; growing back to full read-ahead is safe
// at any time and happens from the process loop as soon as menu domain ends.
void CVideoPlayer::UpdateMenuDomainQueueDepth(bool segmentOpen)
{
  const double clamp = m_menuDomainClampSeconds;
  if (clamp <= 0.0 || clamp >= m_messageQueueTimeSize)
    return;

  bool menuDomain = false;
#if defined(HAVE_LIBBLURAY)
  if (m_pInputBluray)
    menuDomain = m_pInputBluray->IsMenuDomainVideo();
#endif

  if (menuDomain == m_menuDomainLowLatency)
    return;
  if (menuDomain && !segmentOpen)
    return;

  m_menuDomainLowLatency = menuDomain;
  const double target = menuDomain ? clamp : m_messageQueueTimeSize;
  m_VideoPlayerAudio->SetMaxTimeSize(target);
  m_VideoPlayerVideo->SetMaxTimeSize(target);
  CLog::Log(LOGINFO,
            "CVideoPlayer::UpdateMenuDomainQueueDepth - {} menu-domain low-latency mode, "
            "queue read-ahead {:.1f}s",
            menuDomain ? "entering" : "leaving", target);
}

/* BD segment transition (docs/bd_menu_architecture.md §5).
 *
 * Every NEXTSTREAM_OPEN boundary - a bluray playitem/playlist change, or any
 * other chaining input stream - runs through ClassifyBdTransition() +
 * BdSegmentTransition(): one read-only classification, one executor. The
 * executor honours an explicit survival contract per class:
 *
 *                     demuxer   players/decoders  queued packets  decoder reuse
 *  SEAMLESS           survives  survive           render out      (never closed)
 *  DISCARD_KEEPALIVE  rebuilt   survive           dropped         reattach
 *  DISCARD_CLOSE      rebuilt   closed            dropped         none
 *  DRAIN              rebuilt   closed            rendered out    none
 *
 * Stream-reopen obligation: whatever the class, the NEW segment's streams are
 * guaranteed to (re)open with a per-packet deadline - CheckBetterStream()
 * resolves the disc-dictated stream NUMBER against the new playitem's stream
 * table on every delivered packet (GetDictatedAudioPid/GetDictatedPgPid), so
 * the first packet of a dictated-but-unopened stream forces the open. No
 * event ordering or event loss can strand a stream closed ("the disc will
 * dictate later" is not part of any contract). */

/* A segment can be read out before its streams have finished starting: the
 * 4 s intro of Halo S2 is read in a quarter of a second, and the disc jumps to
 * its menu at once. CloseStream() only drains a stream that is in sync, so the
 * transition would tear the intro down unplayed. While the boundary is held,
 * nothing more is read; keep it held until the start completes (bounded) so
 * a draining transition plays the queued segment out. */
bool CVideoPlayer::WaitStartAtBoundary()
{
  // a stream still starting, with queued data the start can complete on (a
  // flush in the window leaves nothing, and nothing more is read while held)
  const auto starting = [](const CCurrentStream& current, bool hasData)
  { return current.id >= 0 && current.syncState != IDVDStreamPlayer::SYNC_INSYNC && hasData; };

#if defined(HAVE_LIBBLURAY)
  const bool held = m_pInputBluray && m_pInputBluray->IsHoldingBoundary();
#else
  const bool held = false;
#endif
  // E1: a start held for the output mode has its players paused, so a draining
  // close could not play the segment out; the held start has its own bound
  // ...and until the players have really resumed: the release reaches them as
  // speed messages, and a close that reads a player's speed before its thread
  // has taken that message skips the drain
  if (m_startReleasedClock != DVD_NOPTS_VALUE)
  {
    const double clock = m_clock.GetClock();
    // resumed, or a new timeline (the clock moved back)
    if (clock >= m_startReleasedClock + DVD_MSEC_TO_TIME(200) ||
        clock < m_startReleasedClock - DVD_MSEC_TO_TIME(1000))
      m_startReleasedClock = DVD_NOPTS_VALUE;
  }
  const bool resuming = m_startReleasedClock != DVD_NOPTS_VALUE;
  if (!held)
    m_txBoundaryLogged = false;
  // 5.E2-10 transaction_boundarynohold (r1 M-4): both holds skipped inside an open start, so the
  // boundary reaches the transition before the job's commit
  if (held && !m_bAbortRequest && TxOpen() && aml_transaction_hooks().boundaryNoHold)
  {
    if (!std::exchange(m_txBoundaryLogged, true))
      CLog::Log(LOGWARNING, "TXTEST boundarynohold: released at {}",
                m_txJob ? DISPLAYSTATE::StartJobStateName(m_txJob->state.load()) : "no job");
    m_boundaryStartWaitSince = {};
    return false;
  }
  // a shut start gate holds it under the gate's own bound
  if (held && !m_bAbortRequest && (m_startHeld || resuming || StartGateShut()))
  {
    // 5.E2-10 §4 (row 7): the boundary held in a transaction's lead
    if (resuming && !m_startHeld && m_txEngaged && !std::exchange(m_txBoundaryLogged, true))
      CLog::Log(LOGINFO,
                "TXSTART boundary held in the lead (clock {:.3f} < release {:.3f} + 200 ms)",
                m_clock.GetClock() / DVD_TIME_BASE, m_startReleasedClock / DVD_TIME_BASE);
    m_boundaryStartWaitSince = {};
    return true;
  }
  if (!held || m_bAbortRequest || !(starting(m_CurrentAudio, m_VideoPlayerAudio->HasData()) ||
        starting(m_CurrentVideo, m_VideoPlayerVideo->HasData())))
  {
    m_boundaryStartWaitSince = {};
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_boundaryStartWaitSince == std::chrono::steady_clock::time_point{})
  {
    m_boundaryStartWaitSince = now;
    CLog::Log(LOGINFO, "VideoPlayer: stream boundary before the streams started, holding it");
  }
  else if (now - m_boundaryStartWaitSince >= 3s)
  {
    CLog::Log(LOGWARNING, "VideoPlayer: streams still not started 3s after a stream boundary, "
                          "releasing it");
    return false;
  }
  return true;
}

CVideoPlayer::EBdTransition CVideoPlayer::ClassifyBdTransition(bool glided) const
{
#if defined(HAVE_LIBBLURAY)
  if (std::shared_ptr<CDVDInputStreamBluray> bluray =
          std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
  {
    // Record WHY, not just what: m_menu is set silently by BD_EVENT_TITLE as
    // well as by the logged BD_EVENT_MENU, so the branch taken here cannot be
    // reconstructed from the event lines alone.
    // Events for a stream without video were stamped NOPTS and reached the
    // BD-J application at read time, so its jump says nothing about what was
    // presented: classify as if it had not been flagged.
    const bool honourBdjJump =
        m_CurrentVideo.id >= 0 && m_CurrentVideo.dts != DVD_NOPTS_VALUE;
    CLog::Log(LOGINFO, "VideoPlayer: BD queue decision - {}{}",
              CDVDInputStreamBluray::DescribeQueueDecision(
                  bluray->ClassifyStreamQueue(honourBdjJump)),
              !honourBdjJump && bluray->ClassifyStreamQueue() ==
                                    CDVDInputStreamBluray::QueueDecision::DISCARD_BDJ_APP_JUMP
                  ? " (BD-J jump ignored: no video timeline)"
                  : "");

    if (bluray->ShouldDiscardStreamQueue(honourBdjJump))
    {
      // the transition crosses the menu boundary (menu->title OR
      // title->menu): the queued remainder of the abandoned segment must be
      // dropped, not rendered out. HDMV discs keep the stream
      // players and their decoders alive across the jump - the title often
      // has the same video format as the menu (e.g. both 4K DV) and
      // reattaching the running decoder (m_bdStreamReuse*, consulted in
      // OpenAudio/VideoStream) avoids the DV tunnel drop/re-latch; a format
      // change simply fails the reuse compare and takes the normal reopen.
      // BD-J discs take the full close: the keep-alive jump crashes in
      // avformat teardown under the JVM's signal handlers (SIGSEGV in
      // avio_close -> JVM abort) and keeps stock behavior until understood.
      if (m_bdjKeepAliveDebug && bluray->HasBDJTitles())
        CLog::Log(LOGWARNING, "VideoPlayer: DEBUG BD-J keep-alive transition");
      return bluray->HasBDJTitles() && !m_bdjKeepAliveDebug ? EBdTransition::DISCARD_CLOSE
                                                            : EBdTransition::DISCARD_KEEPALIVE;
    }

    // Only take the seamless path from a stable pipeline. Menu entry bursts
    // through playitems rapidly; continuing across a boundary before the
    // streams are in sync feeds from a torn position.
    // Direct playlist playback has no menu bursts, and no other path that
    // applies a seam's step: a seam read during a start or just after a seek
    // still glides.
    // A glided seam is a cc 5/6 playitem advance, one continuous transport
    // stream: holding it before the start completes cuts the incoming clip's
    // first access unit (JW3's menu background, P5-13).
    if (bluray->IsSeamlessStreamChange() && m_CurrentVideo.id >= 0 &&
        (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC ||
         (m_seamByPos && !bluray->IsNavigationMode()) || (glided && m_earlyGlide)))
      return EBdTransition::SEAMLESS;
  }
#endif
  return EBdTransition::DRAIN;
}

void CVideoPlayer::BdSegmentTransition(bool glided)
{
  // A Blu-ray stays inside one Process() for the whole disc, so a budget only
  // reset in OnStartup() is a per-DISC quota: three player deaths spread over a
  // two-hour title would silently drop the stream for the rest of it. A segment
  // swap is a clean boundary - a player that survived one is not the failure
  // this bound exists to stop.
  m_audioPlayerRestarts = 0;
  m_videoPlayerRestarts = 0;
  // pictures after this boundary are a new segment (6.1); published to the
  // renderer from the process loop, in order with the packets that follow
  ++m_segmentGen;
  // the segment a held BD-J start was waiting for: its first picture now
  // comes with this segment's own start
  ArmBdjStartForSegment();
  const EBdTransition transition = ClassifyBdTransition(glided);

  // Not on the seamless path: the pipeline survives the boundary, so a refill
  // in progress (a network stall at the seam) must be allowed to finish rather
  // than resume on a nearly empty buffer. Every other path rebuilds it.
  if (transition != EBdTransition::SEAMLESS)
    SetCaching(CACHESTATE_DONE);

  if (transition == EBdTransition::SEAMLESS)
  {
    // Same-playlist playitem advance (menu loop segment or a multi-playitem
    // title's seamless branch): keep EVERYTHING running - the demuxer, the
    // stream players, and their decoders. Closing/reopening the demuxer
    // re-probes the dual-layer (BL+EL) DV program and breaks BL/EL packet
    // routing (BL delivery dies, decoder starves); closing the streams drops
    // the DV tunnel (re-latch toast / display-change black every segment).
    // libbluray feeds a continuous transport stream across the boundary; the
    // input stream does NOT post the DEMUXER_RESET for BD_EVENT_DISCONTINUITY
    // here (a re-probe breaks dual-layer routing) and ffmpeg's mpegts demuxer
    // absorbs the TS discontinuity itself. The segment's timeline restart reaches
    // CheckContinuity as a plain backward jump, which its cross-stream
    // confirmation resolves into exactly one global offset correction - same
    // path a CE21 player takes for these boundaries, proven stable there.
    CLog::Log(LOGINFO, "VideoPlayer: next stream, seamless playitem continuation");

#if defined(HAVE_LIBBLURAY)
    // A held boundary leaves the input stream in HOLD_DATA until the next
    // segment's first byte; a glide never held. See SetSeamlessCarry().
    if (!glided)
    {
      if (std::shared_ptr<CDVDInputStreamBluray> bluray =
              std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
        bluray->SetSeamlessCarry(true);
    }
#endif

    // Arm the seam-step correction. The two clips either side of a playitem
    // boundary are timed independently, so the incoming clip's timestamps
    // rarely continue the outgoing clip's exactly - M3GAN 2.0's 00801.mpls
    // steps -0.159s, +0.332s, -0.430s, +0.291s ... across its 28 playitems. A
    // negative step is an overlap and harmless; a positive one is dead time
    // the renderer sits through, and it is a cut, not elapsed content. See
    // CheckContinuity.
    // a glided seam is crossed per stream by byte position instead
    m_seamStepPending = !(glided && m_seamByPos);
    // Only a glided (cc 5/6) seam may close a backward step; held cc=1 menu
    // loop joins keep their wrapback/resync handling.
    m_seamStepOverlapOk = glided;
    // Bound the arm. Roughly half of these boundaries step BACKWARD (an
    // overlap, which needs no correction), so an unbounded flag would stay
    // latched from one boundary to the next - measured 2m33s of continuous
    // playback in one capture - standing ready to consume the next unrelated
    // forward step, such as two dropped frames. The incoming clip's step
    // arrives within a packet or two of here, so a second is generous.
    //
    // Off VIDEO alone. m_CurrentAudio.dts is not audio's position: it is the
    // last dts audio was ever handed, and it neither advances nor clears while
    // audio is silent. Measured at three boundaries of one capture it sat at
    // 4225.056 - a MENU timeline, 21 seconds stale and 3610s ahead of video -
    // so taking the later of the two armed the deadline in the future and the
    // bound never expired. Without m_seamByPos, ClassifyBdTransition only
    // returns SEAMLESS with video open and SYNC_INSYNC, so video's dts is the
    // one value here that is guaranteed current; if it somehow is not, do not
    // arm at all rather than arm something unboundable.
    m_seamStepPlaylistStep.reset();
#if defined(HAVE_LIBBLURAY)
    if (m_pInputBluray)
    {
      if (const std::optional<int64_t> step = m_pInputBluray->GetSeamPlaylistStep())
        m_seamStepPlaylistStep = static_cast<double>(*step) * DVD_TIME_BASE / 90000.0;
    }
#endif
    if (m_CurrentVideo.dts != DVD_NOPTS_VALUE)
    {
      m_seamStepArmedDts = m_CurrentVideo.dts;
    }
    else
    {
      m_seamStepPending = false;
      m_seamStepArmedDts = DVD_NOPTS_VALUE;
    }

    // Clean the byte seam - but ONLY when we held it. Non-seamless-authored
    // playitem chains (connection_condition 1 - TNG stubs, menu loops) may
    // truncate the outgoing clip's last PES mid-body; feeding that tail into
    // the TS parser's reassembly emits "[mpegts] Packet corrupt" and decoder
    // reference errors at every glued boundary (defect C). When delivery was
    // held at the boundary the flush is synchronous with the read position
    // and drops exactly that partial tail.
    //
    // On the glide path there is no such moment. Reading never stopped, so by
    // the time this runs the demuxer is part way through the INCOMING clip's
    // first access unit, and Flush() would destroy it: avformat_flush closes
    // the codec parser and avio_flush moves the read position, which makes
    // mpegts reset every PES buffer to MPEGTS_SKIP. That is the same
    // amputation the glide exists to prevent, arriving by a different route.
    //
    // A truncated outgoing tail is still possible on the glide path - mpegts
    // terminates the old PES at the incoming clip's first payload-unit-start
    // whether or not it was ever handed an error - so the glide is gated to
    // connection_condition 5/6 (see ClipConnectionIsSeamless), where the
    // authoring forbids one. cc=1 chains keep the hold and keep the flush.
    // The AV_PKT_FLAG_CORRUPT backstop in CDVDDemuxFFmpeg::ReadInternal
    // covers what slips past either way; it is what actually caught all 13
    // truncated tails in the counter-test capture, the flush having run only
    // after the packet was already delivered.
    if (!glided)
      m_pDemuxer->Flush();

    // Dolby Vision FEL content (real enhancement layer, e.g. Spears & Munsil
    // demos) carries per-segment BL/EL pairing and DV metadata state across the
    // clip boundary and glitches until it resyncs. Drop that state, and only
    // that state: GENERAL_RESET would additionally codec_reset the decoder,
    // which discards the boundary IRAP the demuxer has already fed, and force
    // SYNC_STARTING + ShowVideo(false) on a stream that never stopped. Gated to
    // FEL so MEL menu loops (HALO), which cross boundaries cleanly, keep their
    // untouched seamless smoothness.
    if (m_processInfo && m_processInfo->GetDoviIsFEL())
    {
      CLog::Log(LOGINFO, "VideoPlayer: seamless boundary - FEL segment state reset");
      m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_SEGMENT_RESET));
    }
    return;
  }

  // 5.E2-10 D1: the next set's reopen class
  m_txSegClass = transition == EBdTransition::DRAIN           ? TxSegClass::DRAIN
                 : transition == EBdTransition::DISCARD_CLOSE ? TxSegClass::DISCARD_CLOSE
                                                              : TxSegClass::KEEPALIVE;
  // 5.E2-8a (D5): a non-seamless segment transition before START; 5.E2-10 §3: a P-decided
  // start's job is superseded instead, before the decoder is touched
  if (TxOpen())
  {
    if (m_txJob && !aml_transaction_skip("segment"))
      TxSupersede(m_txSegClass);
    else
      EndTransactionToLegacy("segment transition", CRenderManager::TxHow::OWED,
                             CRenderManager::TxNeed::LATER);
  }
  CloseDemuxer();

  m_syncStartPtsWait.reset();
  m_syncStartDeferred = false;

  const bool reuse = transition == EBdTransition::DISCARD_KEEPALIVE;
  m_bdStreamReuseVideo = reuse;
  m_bdStreamReuseAudio = reuse;

  if (transition == EBdTransition::DISCARD_KEEPALIVE)
  {
    // drop the queued abandoned remainder (menu loop on menu->title, movie
    // tail on title->menu); flush (don't close) the stream players so the
    // running decoders survive for reattach
    CLog::Log(LOGINFO, "VideoPlayer: next stream, discarding abandoned stream remainder");
    m_txFlushCaller = "keepalive";
    FlushBuffers(DVD_NOPTS_VALUE, false, true);
    return;
  }

  const bool drain = transition == EBdTransition::DRAIN;
  CLog::Log(LOGINFO, "VideoPlayer: next stream, {}",
            drain ? "wait for old streams to be finished"
                  : "discarding menu stream remainder (full close)");
#if defined(HAVE_LIBBLURAY)
  // The dropped remainder carried timeline events stamped on it - the BD-J
  // notifications held for it, and the menu/playlist/title-UI state changes
  // queued behind them - and none of that data will ever be presented. Apply
  // them now, as a flush does, for every discard: left in place they block the
  // FIFO until the new segment's clock happens to reach an old-timeline stamp
  // (up to the queue depth, or never if the new timeline sits just behind).
  if (!drain)
    ApplyDiscTimelineEvents(true);
  else if (m_pInputBluray)
  {
    // a mark's own picture may be in the tail the drain loses (the last PES):
    // past the drain the clock runs on the new timeline, so it goes at its
    // stamp, the last picture delivered
    for (SDiscTimelineEvent& ev : m_discTimelineEvents)
    {
      ev.presentPts = DVD_NOPTS_VALUE;
      ev.markPending = false;
    }
    // a held BD-J start read here is the next segment's: its stamp is on the
    // old timeline, and its first picture comes with the new segment's start
    uint32_t startSeq = 0;
    if (m_pInputBluray->BdjStartPending(startSeq))
      for (SDiscTimelineEvent& ev : m_discTimelineEvents)
        if (ev.bdjReleaseSeq != 0 && static_cast<int32_t>(ev.bdjReleaseSeq - startSeq) >= 0)
        {
          ev.stampPts = DVD_NOPTS_VALUE;
          ev.presentPts = DVD_NOPTS_VALUE;
        }
  }
#endif
  // the video tail may still be inside the decoder: start playing it out now,
  // alongside the audio drain, not after it
  if (drain && m_CurrentVideo.id >= 0 &&
      m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC && !m_bAbortRequest)
    m_VideoPlayerVideo->BeginDrain();
  {
    CTxDrainSite txSite(m_txDrainSite, "BdSegmentTransition");
    CloseStream(m_CurrentAudio, drain);
  }
  // design 4.3 (4a, debug flag): the last picture stays on screen across the
  // decoder reinit, as a reference player's non-seamless join. Not for DV
  // profile 5, whose IPT frame shows wrong without its core path.
  const bool keepFrame = m_keepFrameEnabled && m_CurrentVideo.id >= 0 && !m_bAbortRequest &&
                         m_CurrentVideo.hint.dovi.dv_profile != 5;
  aml_keep_frame_arm(keepFrame);
  {
    CTxDrainSite txSite(m_txDrainSite, "BdSegmentTransition");
    CloseStream(m_CurrentVideo, drain);
  }
  // 5.E2-12b-2 D4 point (1): the video close returned
  if (transition == EBdTransition::DRAIN || transition == EBdTransition::DISCARD_CLOSE)
    TxRow3cPoint(1);
  // the close is synchronous: an arm the decoder did not take must not reach
  // a later close (a stop blanks)
  aml_keep_frame_arm(false);
#if defined(HAVE_LIBBLURAY)
  // The drained tail is presented now, give or take the render queue (the
  // closes wait for it, up to 3 s, and only for a stream in sync), and the
  // timeline could not run meanwhile (this thread was here). What is still
  // queued is stamped on the old timeline: left in place it waits until the
  // new segment's clock reaches an old stamp (John Wick 3's feature start:
  // 8.3 s behind its picture). Apply it, as for a discard; a held start keeps
  // its own items for the new segment's first picture.
  if (drain && m_pInputBluray)
    ApplyDiscTimelineEvents(true);
#endif

  m_CurrentAudio.Clear();
  m_CurrentVideo.Clear();
  m_CurrentSubtitle.Clear();

  // This path tears the segment down without a FlushBuffers, so nothing else
  // clears an arm left over from an earlier seamless boundary that never
  // fired. It would otherwise cross a full demuxer and stream teardown onto a
  // brand new timeline - menu 4225s to title 0s - where its deadline can never
  // expire either. Same leak Prepare() closes at the file boundary.
  m_seamStepPending = false;
  m_seamStepArmedDts = DVD_NOPTS_VALUE;
  m_stillJoinCorrection = DVD_NOPTS_VALUE;
  m_stillJoinOldEnd = false;
}

void CVideoPlayer::PublishSegmentGen()
{
  if (m_segmentGenPublished == m_segmentGen)
    return;
  m_segmentGenPublished = m_segmentGen;
  // a page waiting for this segment waits from its opening, not its posting
  if (m_menuPageWaiting)
    m_menuPageWaitSince = std::chrono::steady_clock::now();
  // a running video player takes it in order with the packets that follow;
  // a closed one's next pictures come from a player opened after now
  if (m_CurrentVideo.id >= 0 && m_VideoPlayerVideo->IsInited())
    m_VideoPlayerVideo->SendMessage(
        std::make_shared<CDVDMsgInt>(CDVDMsg::GENERAL_SEGMENT_GEN, static_cast<int>(m_segmentGen)), 0);
  else
    m_renderManager.SetIncomingSegmentGen(m_segmentGen);
}

void CVideoPlayer::CheckMenuPageWait()
{
  if (!m_menuPageWaiting)
    return;
  if (!m_overlayContainer.HasPendingMenu())
  {
    m_menuPageWaiting = false;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // a held start's first picture is on its way
  if (m_startHeld)
    m_menuPageWaitSince = now;
  else if (now - m_menuPageWaitSince > 3s)
  {
    CLog::Log(LOGDEBUG, "CVideoPlayer: menu page shown without its picture (3s)");
    m_overlayContainer.ReleasePendingMenu();
    m_menuPageWaiting = false;
  }
}

void CVideoPlayer::ReleaseAudioSessionHold()
{
  if (!m_audioSessionHold.exchange(false))
    return;
  if (IAE* ae = CServiceBroker::GetActiveAE())
    ae->SetSessionHold(false);
}

void CVideoPlayer::ArmBdjStartForSegment()
{
  // A held BD-J start stamped behind the picture waits for the next segment:
  // a segment transition after the stamp shows it. Not a held start: that
  // can be the segment before it, still to be shown.
  for (SDiscTimelineEvent& ev : m_discTimelineEvents)
    if (ev.awaitSegment)
    {
      ev.awaitSegment = false;
      ev.stampPts = DVD_NOPTS_VALUE;
      ev.presentPts = DVD_NOPTS_VALUE;
      ev.markPending = false;
    }
}

void CVideoPlayer::HoldStart(std::optional<unsigned int> decisions,
                             std::optional<uint64_t> midSeq)
{
  // only a user pause keeps the output's audio (design §16.22)
  m_clock.SetHoldFrom(0.0);
  m_cachingStall = false;
  m_startHeld = true;
  m_startReleasedClock = DVD_NOPTS_VALUE;
  // 5.B2-2: the count read with the pending test; without it today's snapshot.
  // 5.E3: none for a P-decided open (main shut out): only the 12 s cap applies
  const bool txStart = TxOpen() && m_txJob;
  m_startHeldMidSeq = midSeq;
  if (txStart || midSeq)
    m_startHeldDecisions.reset();
  else
    m_startHeldDecisions = decisions.value_or(m_renderManager.GetResolutionDecisions());
  m_startHeldSince = std::chrono::steady_clock::now();
  m_clock.SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_VideoPlayerAudio->SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_VideoPlayerVideo->SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_streamPlayerSpeed = DVD_PLAYSPEED_PAUSE;
  // the held start frame is chosen by the clock on validated SoCs only
  m_clock.SetStartHeld(aml_presentation_validated());
  m_VideoPlayerVideo->SetStartHeld(true);
  m_renderManager.SetStartHeld(true);
  // 5.E2-8a (D3, r2 M1): the transaction's bounds count from E1's hold too
  if (TxOpen())
  {
    if (m_txDiscHeld)
      m_discTime.SetTxHeld(true);
    m_renderManager.ArmTxStartHeldBound();
  }
  if (txStart)
    CLog::Log(LOGINFO, "VideoPlayer: E1 start held at clock {:.3f} (transaction start)",
              m_clock.GetClock() / DVD_TIME_BASE);
  else if (midSeq)
    CLog::Log(LOGINFO, "MIDPLAY hold at sync commit seq={} clock {:.3f}", *midSeq,
              m_clock.GetClock() / DVD_TIME_BASE);
  else
    CLog::Log(LOGINFO, "VideoPlayer: E1 start held at clock {:.3f} (mode decisions so far {}{})",
              m_clock.GetClock() / DVD_TIME_BASE, *m_startHeldDecisions,
              decisions ? ", read with the pending test" : "");
}

void CVideoPlayer::CheckHeldStart()
{
  if (!m_startHeld || m_displayLost || m_txInSpan)
    return;
  const auto held = std::chrono::steady_clock::now() - m_startHeldSince;
  if (m_startHeldMidSeq && m_midTx.counted != *m_startHeldMidSeq)
  {
    CLog::Log(LOGINFO, "MIDPLAY hold released seq={}", m_midTx.counted);
    m_startHeldMidSeq.reset();
    ReleaseHeldStart("output mode decided");
  }
  else if (m_startHeldDecisions &&
           m_renderManager.GetResolutionDecisions() != *m_startHeldDecisions)
    ReleaseHeldStart("output mode decided");
  else if (held > 12s)
    ReleaseHeldStart("no mode decision in 12s");
}

void CVideoPlayer::ArmStartGate(const char* where)
{
  TxMidPlayEnd("later set");
  const TxSegClass segClass = std::exchange(m_txSegClass, TxSegClass::NONE);
  m_txReopenCandidateGen = 0;
  bool hasVideo = m_pDemuxer && m_SelectionStreams.CountType(StreamType::VIDEO) > 0;
#if defined(HAVE_LIBBLURAY)
  // a menu disc dictates its streams: none may be open yet
  if (m_pDemuxer && m_pInputBluray)
    hasVideo = m_pInputBluray->FirstClipHasVideo();
#endif
  // the open's fullscreen request holds until the session's first commit; later sets
  // follow the GUI (a user who left fullscreen gets no mode decision, as ungated)
  const bool startFullScreen = m_playerOptions.fullscreen && !m_State.streamsReady;
  const bool arm =
      hasVideo && m_startGateEnabled &&
      (startFullScreen || CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo());
  // 5.E2-8a (D5): a later set while the engaged start is open ends it first; 5.E2-10 §3: a
  // superseded start waits for the reopen's verdict (a refusal ends it, D1 item 8)
  const bool txFirstSet = !m_txGateSetSeen && !m_State.streamsReady;
  const bool reopenCandidate =
      arm && !txFirstSet && segClass != TxSegClass::NONE && aml_transaction_loss_base();
  // 5.E2-12b D1 (R1, errata E1; r1 H-1, H-2): a no-video set of an open or carried committed start
  // keeps the start and CN's gate: no later-set exit, no gate end; row 3b's step opens it
  // gate hook transaction_novideo (E2-12b rows 3, 4): the first later Blu-ray set has no video
  if (m_pInputBluray && hasVideo && m_txGateSetSeen && aml_transaction_hooks().noVideo &&
      !std::exchange(m_txNoVideoHookDone, true))
  {
    hasVideo = false;
    m_txNoVideoForced = true;
    CLog::Log(LOGWARNING, "TXTEST novideo: this set treated as having no video ({})", where);
  }
  if (aml_disc_screen_hold() && !hasVideo && m_pInputBluray && TxNoVideoInStart() &&
      !m_txNoVideoCapped)
  {
    m_txNoVideoHold = true;
    TxNoVideoNoteWire();
    CLog::Log(LOGINFO, "TRANSACTION no-video set, gate kept for T={} ({})", m_txWordT.load(),
              TxOpen() ? "open" : "carried");
    return;
  }
  if (TxOpen() && !(m_txSuperseded && reopenCandidate))
    EndTransactionToLegacy("later set", CRenderManager::TxHow::OWED, CRenderManager::TxNeed::LATER);
  // 5.E2-8b D8 (r2 N-L8): a KEEPALIVE reuse later set never configures: the owed mark of the
  // start this file ended raises at its arm
  if (m_txEndedGen && m_bdStreamReuseVideo)
    m_renderManager.TxRaiseOwed("keepalive later set");
  // code review A-M1 (backstop): an earlier exit's outcome still unresolved ends as `later`
  if (m_txEndedGen)
    m_renderManager.TxLaterSet(m_txEndedGen);
  LogTransactionShadow(arm, hasVideo, startFullScreen, where, segClass);
  if (!arm)
  {
    // the last set's gate ends with it; its audio is closed or flushed by now
    m_startGateOpenedAt = {};
    if (m_startGateShut)
    {
      m_startGateShut = false;
      m_VideoPlayerAudio->SetStartGated(false);
      m_renderManager.SetModeFirstGate(false, m_startGateGen);
      if (IAE* ae = CServiceBroker::GetActiveAE())
        ae->SetStartGate(false);
      CLog::Log(LOGINFO, "VideoPlayer: start gate ended (set {}): {} not gated", m_startGateGen,
                where);
      // 5.E2-6 (r1 H4, r2 M2): the picture gate of the set ends with it
      if (m_renderManager.ArmTxPictureGate(0, false, "gate ended"))
        TxRestoreDecoderSpeed("gate ended");
      // 5.E2-4: the window this gate opened ends with it (E2-4 review M1)
      if (aml_transaction_shadow_on())
        OUTPUT::COutputWorker::Get().ShadowStartPhase(
            this, OUTPUT::COutputWorker::StartPhase::NONE, 0, "gate ended",
            static_cast<int>(OUTPUT::COutputWorker::StartPhase::STARTING));
    }
    return;
  }
  ++m_startGateGen;
  m_startGateShut = true;
  // 5.E2-6 (D1, r1 H4): an older picture gate is cleared; the file's first set arms a candidate
  // that RM engages (or not) at the set's first picture
  // the file's first set (plan §2.3 item 4, E2-1's rule): the first armed set while the streams
  // are not ready, whatever armed it ("first set", or a disc's first "Blu-ray set" after a
  // no-video first-play)
  m_txGateSetSeen = true;
  // 5.E2-10 D1 item 3: a later set's reopen verdict, before its picture gate; a KEEPALIVE set's
  // waits for the reuse compare (its gate is no candidate: a MATCH never holds pictures)
  bool txReopen = false;
  if (reopenCandidate)
  {
    if (segClass == TxSegClass::KEEPALIVE)
      m_txReopenCandidateGen = m_startGateGen;
    else
      txReopen = TxReopenAllowed(where, segClass);
  }
  if (m_renderManager.ArmTxPictureGate(m_startGateGen, txFirstSet || txReopen, "new set"))
    TxRestoreDecoderSpeed("new set");
  if (txReopen)
  {
    const bool inStart = m_txSuperseded;
    TxResetStart(true);
    m_txReopenOpen = true;
    m_txReopenInStart = inStart;
    TxEngage(segClass == TxSegClass::DRAIN ? "reopen drain" : "reopen discard-close", false);
  }
  // 5.E2-8a (D1): P's verdict for the file's first set; graphics are held from here
  else if (txFirstSet && aml_transaction_loss_base())
  {
    const char* refused = TxEngageRefusal(where);
    // 5.E2-8b D1/D2: P decides the start unless transaction_skip decide (E2-8a's start); its
    // DECIDE reads the inputs main publishes at OpenFile, so without the publish no engage
    // E2-8b NOTE: D2's "TRANSACTION legacy-open (no decide inputs)" is logged in E2-8a's refusal
    // form, "TXSTART set=N legacy-open (no decide inputs; <where>)"
    if (!refused && !aml_transaction_skip("decide") && !DISPLAYSTATE::DecideShadowOn())
      refused = "no decide inputs";
    if (refused)
      CLog::Log(LOGINFO, "TXSTART set={} legacy-open ({}; {})", m_startGateGen, refused, where);
    else
      TxEngage(where, startFullScreen);
  }
  m_startGateOpenedAt = {};
  m_startGateSince = std::chrono::steady_clock::now();
  m_startGateStarveSince = {};
  m_startGateFullScreenPosted = false;
  // before any audio of the set opens
  m_VideoPlayerAudio->SetStartGated(true);
  m_renderManager.SetModeFirstGate(true, m_startGateGen);
  if (IAE* ae = CServiceBroker::GetActiveAE())
    ae->SetStartGate(true);
  CLog::Log(LOGINFO, "VideoPlayer: start gate armed (set {}, {})", m_startGateGen, where);
  if (aml_transaction_shadow_on())
  {
    LogWordBoundary(true, where);
    // 5.E2-4: the start window W's request shadow reads
    OUTPUT::COutputWorker::Get().ShadowStartPhase(
        this, OUTPUT::COutputWorker::StartPhase::STARTING, 0, "CN arm");
  }
}

// 5.E2-10 D1 item 7: a KEEPALIVE set's verdict at the reuse compare, before the reopen
void CVideoPlayer::TxKeepaliveVerdict(bool reuse)
{
  if (!m_txReopenCandidateGen || m_txReopenCandidateGen != m_startGateGen)
    return;
  m_txReopenCandidateGen = 0;
  if (reuse)
  {
    CLog::Log(LOGINFO, "TRANSACTION reuse match T={} set={}", m_txWordT.load(), m_startGateGen);
    // E210-1: a MATCH inside an open start ends it, as today
    if (m_txSuperseded)
      TxEndSuperseded("keepalive reuse match");
    return;
  }
  constexpr const char* where = "reopen keepalive reuse-fail";
  if (!m_startGateShut)
  {
    if (m_txSuperseded)
      TxEndSuperseded("gate open");
    return;
  }
  if (!TxReopenAllowed(where, TxSegClass::KEEPALIVE))
    return;
  if (m_renderManager.ArmTxPictureGate(m_startGateGen, true, "keepalive reopen"))
    TxRestoreDecoderSpeed("keepalive reopen");
  const bool inStart = m_txSuperseded;
  TxResetStart(true);
  m_txReopenOpen = true;
  m_txReopenInStart = inStart;
  TxEngage(where, false);
  // r1 L-3: the PRIME bound and CN's caps count from the compare
  m_startGateSince = std::chrono::steady_clock::now();
  m_startGateStarveSince = {};
}

const char* CVideoPlayer::TxSegClassName(TxSegClass cls)
{
  switch (cls)
  {
    case TxSegClass::DRAIN:
      return "drain";
    case TxSegClass::DISCARD_CLOSE:
      return "discard-close";
    case TxSegClass::KEEPALIVE:
      return "keepalive";
    default:
      return "none";
  }
}

bool CVideoPlayer::TxReopenAllowed(const char* where, TxSegClass cls)
{
  const char* refused = nullptr;
  if (aml_transaction_skip("segment"))
    refused = "skip segment";
  // r1 M-1, sc M-6: main's set or its retry for an earlier exit is still running
  else if (m_txEndedGen && !m_renderManager.TxOutcomeResolved(m_txEndedGen))
    refused = "earlier exit unresolved";
  else
  {
    m_txEndedGen = 0;
    refused = TxEngageRefusal(where);
    if (!refused && !aml_transaction_skip("decide") && !DISPLAYSTATE::DecideShadowOn())
      refused = "no decide inputs";
    if (!refused)
    {
      const uint32_t t = OUTPUT::COutputWorker::Get().WordReopen(m_txWordT.load(), TxOpen(),
                                                                 TxSegClassName(cls));
      if (t)
        m_txWordT = t;
      else
        refused = "word busy";
    }
  }
  if (!refused)
    return true;
  CLog::Log(LOGINFO, "TXSTART set={} legacy-open ({}; {})", m_startGateGen, refused, where);
  if (m_txSuperseded)
    TxEndSuperseded(refused);
  return false;
}

void CVideoPlayer::TxSupersede(TxSegClass cls)
{
  using State = DISPLAYSTATE::StartJobState;
  if (m_txSuperseded)
  {
    CLog::Log(LOGINFO, "TXSTART set={} T={} row 3: reopen ({}) while superseded: kept",
              m_txEngagedGen, m_txJob->T, TxSegClassName(cls));
    return;
  }
  const State state = m_txJob->state.load();
  if (DISPLAYSTATE::StartJobDropForExit(*m_txJob, "superseded by a reopen"))
  {
    m_txSuperseded = true;
    m_txSupersededAt = std::chrono::steady_clock::now();
    m_txStep = TxStep::NONE;
    CLog::Log(LOGINFO, "TXSTART set={} T={} row 3: reopen ({}) supersedes the job ({})",
              m_txEngagedGen, m_txJob->T, TxSegClassName(cls),
              DISPLAYSTATE::StartJobStateName(state));
    if (const int ms = aml_transaction_hooks().reopenDelayMs; ms > 0)
    {
      m_txReopenDelayUntil = m_txSupersededAt + std::chrono::milliseconds(ms);
      CLog::Log(LOGWARNING, "TXTEST reopendelay {}", ms);
    }
    return;
  }
  // Q3 (E210-2): past the commit CAS
  CLog::Log(LOGINFO, "TXSTART set={} T={} row 3: reopen ({}) after the job's commit: ended to "
                     "legacy",
            m_txEngagedGen, m_txJob->T, TxSegClassName(cls));
  // 5.E2-12b D2.2 (r1 H-2): the committed job's SettleDone stays visible across the transition
  if (aml_disc_screen_hold() && m_txJob)
    m_txNoVideoCarry = STxNoVideoCarry{m_txJob, m_txJob->T, m_txSettle.has_value()};
  EndTransactionToLegacy("segment transition", CRenderManager::TxHow::OWED,
                         CRenderManager::TxNeed::LATER);
}

void CVideoPlayer::TxEndSuperseded(const char* refusal)
{
  const std::string site = std::string("superseded, ") + refusal;
  EndTransactionToLegacy(site.c_str(), CRenderManager::TxHow::OWED,
                         CRenderManager::TxNeed::LATER);
  m_renderManager.TxRaiseOwed(site.c_str());
}

void CVideoPlayer::TxSupersededBound()
{
  if (!m_txSuperseded || !TxOpen() ||
      std::chrono::steady_clock::now() - m_txSupersededAt < 12s)
    return;
  CLog::Log(LOGERROR,
            "TXSTART set={} T={} superseded start: no new OPEN in 12 s: ended to legacy",
            m_txEngagedGen, m_txWordT.load());
  TxEndSuperseded("no open");
}

void CVideoPlayer::TxEngage(const char* where, bool startFullScreen)
{
  const bool decideOnP = !aml_transaction_skip("decide");
  m_txEngaged = true;
  m_txEngagedGen = m_startGateGen;
  const STransactionHooks hooks = aml_transaction_hooks();
  m_txCapForce = hooks.capForce;
  m_txAudioFullForce = hooks.audioFullForce;
  // 5.E2-9b D1.6: a speed inside this start is applied at once, as transaction_off
  m_txSkipPausestart = aml_transaction_skip("pausestart");
  // 5.E2-9c D1, D2
  m_txSkipPrimebound = aml_transaction_skip("primebound");
  m_txSkipReadon = m_txSkipPrimebound || aml_transaction_skip("readon");
  m_txReadOn = TxReadOn::IDLE;
  m_txMenuDomain = false;
#if defined(HAVE_LIBBLURAY)
  if (m_pInputBluray)
    m_txMenuDomain = m_pInputBluray->IsMenuDomainVideo();
#endif
  TxTestHooksAtEngage();
  // E2-8b NOTE: E2-13's owed|posted words carry no '@' and are left to E2-13
  if (const auto at = hooks.noticeAt.find('@'); at != std::string::npos)
  {
    m_txNoticeKind = hooks.noticeAt.substr(0, at);
    m_txNoticePoint = hooks.noticeAt.substr(at + 1);
    if ((m_txNoticeKind != "trigger" && m_txNoticeKind != "changed") ||
        (m_txNoticePoint != "prime" && m_txNoticePoint != "posted" &&
         m_txNoticePoint != "settle"))
    {
      CLog::Log(LOGWARNING, "TXTEST noticeat '{}' not understood", hooks.noticeAt);
      m_txNoticeKind.clear();
    }
  }
  m_renderManager.SetTxStartHeld(true);
  m_renderManager.SetTxEngagedGen(m_startGateGen);
  CLog::Log(LOGINFO, "TXSTART set={} engaged ({}, fullscreen {})", m_startGateGen, where,
            startFullScreen ? "requested" : "gui");
  // 5.E2-8b D2: main shut out and the Start job created, at the engage when the open's T
  // exists, else right after Prepare's WordOpen (no configure can run before it)
  // E2-8b NOTE: the first file's arm comes before WordOpen (E2-8a code review H1), and the
  // job's T is fixed before TxOpenJob publishes it; the shut follows by a few statements
  m_txEndedGen = 0;
  if (decideOnP)
  {
    // r3 L3: the arm's fullscreen term, always true on an engaged open
    m_txFullScreen = true;
    if (m_txWordOpened)
      TxOpenStart();
    else
      m_txOpenPending = true;
  }
}

// 5.E2-1 (plan §3, log only): what the transaction's per-open matrix would say for this set.
// Reads P's own members and latched switches; takes no lock and changes no playback state.
void CVideoPlayer::LogTransactionShadow(bool arm,
                                        bool hasVideo,
                                        bool startFullScreen,
                                        const char* where,
                                        TxSegClass segClass)
{
  if (!aml_transaction_shadow_on())
    return;
  // the file's first set (plan §2.3 item 4): a set armed after the streams are ready is a
  // later one even when no earlier set armed (a windowed or no-video first set)
  const bool firstSet = !m_txShadowSetSeen && !m_State.streamsReady;
  if (arm)
    m_txShadowSetSeen = true;
  const char* why = nullptr;
  if (!m_startGateEnabled)
    why = "gate disabled";
  else if (!hasVideo)
    why = "no video";
  else if (!arm)
    why = "windowed";
  else if (!firstSet && segClass == TxSegClass::KEEPALIVE)
    why = "keepalive: at the compare";
  else if (!firstSet && segClass == TxSegClass::NONE)
    why = "later set";
  else if (!m_scheduledStart)
    why = "scheduled start off";
  else if (!aml_transaction_on())
    why = aml_transaction_reason();
  // the set's number is the one an arm takes next (ArmStartGate bumps it after this)
  const unsigned int set = arm ? m_startGateGen + 1 : m_startGateGen;
  if (!why)
    CLog::Log(LOGINFO,
              "TRANSACTION shadow set={} T={} engaged ({}{}, fullscreen {}; worker, renderer: "
              "n/a at the arm)",
              set, ++m_txShadowSerial, firstSet ? "" : "reopen ",
              firstSet ? where : TxSegClassName(segClass), startFullScreen ? "requested" : "gui");
  else
    CLog::Log(LOGINFO, "TRANSACTION shadow set={} T={} legacy-open ({}; {})", set,
              m_txShadowSerial, why, where);
}

// 5.E2-8a (D1): why the file's first set cannot engage (nullptr: it engages). The session's
// term (aml_transaction_loss_base) is tested by the caller first: the word exists only with it
const char* CVideoPlayer::TxEngageRefusal(const char* where)
{
  // ruling D6 (P5-24): a DVD keeps today's start, a logged legacy open, until one is gated;
  // also refuses its reopens (TxReopenAllowed), so no START and no mid-play path session
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    return "dvd (D6)";
  if (!m_scheduledStart)
    return "scheduled start off";
  if (aml_transaction_skip("start"))
    return "skip start";
  // 5.E2-10 §5: a next file is a segment for the skip word
  if (m_txNextFile && aml_transaction_skip("segment"))
    return "skip segment";
  if (aml_transaction_skip("gate"))
    return "skip gate";
  for (const char* f : {"disctime_off", "streamsplit_video_off", "vconfigure_skip",
                        "geometrytick_skip", "outputdrive_skip"})
    if (XFILE::CFile::Exists(std::string("special://profile/") + f))
      return f;
  return nullptr;
}

bool CVideoPlayer::TxDecided(bool modeDecided)
{
  if (!m_txWordStarting)
  {
    if (!OUTPUT::COutputWorker::Get().WordStarting(m_txWordT.load(), m_txJob != nullptr,
                                                   &m_txStartE))
    {
      // 5.E2-11: W opened a row-9 span on this T since the pass's span read: CN stays shut and
      // the pass after the span retries
      const auto word =
          OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
      if (aml_transaction_loss_in_start() && m_txJob && word.t == m_txWordT.load() &&
          word.state == OUTPUT::COutputWorker::WordState::NONE && word.spanOpen)
      {
        CLog::Log(LOGINFO, "TXSTART STARTING held: the display span is open");
        return false;
      }
      EndTransactionToLegacy("word at the decision", CRenderManager::TxHow::RERAISE,
                             CRenderManager::TxNeed::JOB_OK);
      return true;
    }
    m_txWordStarting = true;
    CLog::Log(LOGINFO, "TXSTART set={} T={} STARTING ({})", m_startGateGen, m_txWordT.load(),
              m_txJob ? (modeDecided ? "transaction settle done" : "transaction no change")
              : modeDecided ? "output mode decided"
                            : "no mode decision needed");
    // r1 H4: the start-time timers cannot fire while CN's gate is shut (the sync pass is gated)
    CLog::Log(LOGINFO, "TXHOLD timers quiet since OPEN (defer {}, caching state {})",
              m_syncStartDeferred ? "armed" : "not", static_cast<int>(m_caching));
    // design §8.1: disc time held from the decision, not from OPEN. 5.E2-8b D6 (r2 N-M2): on P's
    // start DECIDE held it and armed the bound (D4 step 5); this step is the word's only
    if (!m_txJob)
    {
      if (modeDecided)
      {
        m_txDiscHeld = true;
        m_discTime.SetTxHeld(true);
      }
      m_renderManager.ArmTxStartHeldBound();
    }
    if (aml_transaction_hooks().lossAt == "decided")
    {
      m_txLossAtUntil = std::chrono::steady_clock::now() + 3s;
      CLog::Log(LOGWARNING, "TXTEST lossat decided: CN's opening held 3 s for a loss");
    }
  }
  // transaction_lossat: the gate stays shut (the word STARTING) until the window ends
  return m_txLossAtUntil == std::chrono::steady_clock::time_point{} ||
         std::chrono::steady_clock::now() >= m_txLossAtUntil;
}

void CVideoPlayer::TxStartPoint(const char* point, bool guarded)
{
  // 5.E2-11 D1.5: a guarded START took the word and m_txStarted under the guard
  if (!guarded)
  {
    if (m_txWordStarting)
      OUTPUT::COutputWorker::Get().WordStartDone(m_txWordT.load(), point);
    m_txStarted = true;
  }
  TxHintClear(); // 5.E2-9c (r5 M2)
  TxRestoreAudioTimeCap("START"); // 5.E2-9c D6
  // 5.E2-12a (code review H-1): an unheld START has no lead: the window closes at once; a held
  // one sets its lead end after this, on this thread
  if (m_txLeadEndNs == INT64_MAX)
    m_txLeadEndNs = 0;
  CLog::Log(LOGINFO, "TRANSACTION START T={} set={} ({})", m_txWordT.load(), m_txEngagedGen,
            point);
  // 5.E2-8b D7: main let back in, TxOpenJob released; withholding ends at START (E28B-1)
  if (m_txJob)
  {
    const char* midOff = TxMidPlayOff();
    m_renderManager.TxOpenMain(m_txEngagedGen, CRenderManager::TxHow::START,
                               CRenderManager::TxNeed::NONE, !midOff);
    TxMidPlayStart(midOff);
    TxClearOpenJob("START");
    m_txEndedGen = 0;
    // the job's tweak: ReleaseHeldStart's ApplyAudioOffset lands RM's tweak, composed from W's
    // committed mode (the job's)
    // E2-8b NOTE: P does not write the tweak; the job's value is logged against RM's read
    CLog::Log(LOGINFO, "TXSTART T={} tweak job {:.1f} ms, landed {:.1f} ms", m_txJob->T,
              m_txJob->tweakMs, m_renderManager.GetLatencyTweak());
  }
}

const char* CVideoPlayer::TxMidPlayOff()
{
  if (!aml_midplaytx_on())
    return "midplaytx off";
  // code review r1 S5: without composed timing the reset would land main's old-mode tweak
  return !DISPLAYSTATE::DecideShadowOn()                    ? "decide shadow off"
         : !m_renderManager.TxPictureStereoMode().empty() ? "stereoscopic (G-D4)"
         : !m_processInfo->IsVideoHwDecoder()             ? "software decode"
         : !m_renderManager.ComposingTiming()             ? "timing not composed"
                                                          : nullptr;
}

void CVideoPlayer::TxMidPlayStart(const char* off)
{
  if (!aml_midplaytx_on())
    return;
  const uint32_t T = m_txWordT.load();
  if (off)
  {
    CLog::Log(LOGINFO, "MIDPLAY path off T={} ({})", T, off);
    return;
  }
  m_renderManager.TxMidShadowReset();
  m_midTx.owedAtFullscreen = m_midTx.redecide = m_midTx.redecideDue = false;
  m_midTx.retried = m_midTx.retryDue = m_midTx.handback = m_midTx.openLogged = false;
  m_midTx.chain = 0;
  m_midTx.vs10 = -1;
  m_midTx.vs10Fresh = false;
  m_midTx.inputsDue = false;
  m_midTx.avOwed = m_midTx.retryAv = false;
  m_midTx.sticky.reset();
  m_midTx.pickDue = m_midTx.pickFresh = false;
  m_midTx.settingDue = false;
  m_midTx.inputsAtOn = DISPLAYSTATE::DecideInputsSeq();
  m_txMidPath = true;
  m_txMidPathT = T;
  DISPLAYSTATE::PublishMidPlayPath(
      OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f, true);
  DISPLAYSTATE::PublishDecideInputsMidPlay("path on", true);
  CLog::Log(LOGINFO, "MIDPLAY path on T={}", T);
}

void CVideoPlayer::TxMidPlayEnd(const char* why)
{
  if (!std::exchange(m_txMidPath, false))
    return;
  // r2 note b: a later set's arm raises what P still owes as the trigger; the stop and the next
  // file drop it
  const bool laterSet = std::string_view(why) == "later set";
  // 5.G-5: owed = a decision main would make (a pick's alone is none: today's raises nothing)
  const int n = m_renderManager.TxMidPlayEnd(
      laterSet, (m_midTx.avOwed && (m_midTx.owedAtFullscreen || m_midTx.redecide)) ||
                    (m_midTx.retryDue && m_midTx.retryAv));
  DISPLAYSTATE::PublishMidPlayPath(0, false);
  m_renderManager.TxMidShadowReset();
  // B1: the record reset; a job in flight runs to its replies (r2 note c), deciding nothing
  for (SMidJob& j : m_midJobs)
    j.path = false;
  m_midTx.owedAtFullscreen = m_midTx.redecide = m_midTx.redecideDue = false;
  m_midTx.retried = m_midTx.retryDue = m_midTx.handback = false;
  m_midTx.inputsDue = false;
  m_midTx.chain = 0;
  m_midTx.avOwed = m_midTx.retryAv = false;
  // 5.G-5 (§4.5, S4): the sticky target ends with the path session; a pick not yet carried runs
  // today's way at a later set, and is dropped by the stop and the next file
  if (std::exchange(m_midTx.pickDue, false))
  {
    // code review r1 N1: a keep dialog's clear is dropped: sent, the old GUI mode would become
    // the next set's target
    if (laterSet && !m_midTx.pickClear)
      TxMidPickToMain(m_midTx.pickIndex, m_midTx.pickId, "after the path");
    else
      CLog::Log(LOGINFO, "MIDPLAY pick id={} dropped ({})", m_midTx.pickId, why);
  }
  m_midTx.pickFresh = false;
  // a setting not yet run: today's job (its body does nothing once DV is off)
  if (std::exchange(m_midTx.settingDue, false))
    TxMidSettingToday(m_midTx.settingName, "after the path");
  if (m_midTx.sticky)
  {
    CLog::Log(LOGINFO, "MIDPLAY pick sticky {} ended ({})", m_midTx.sticky->strMode, why);
    m_midTx.sticky.reset();
  }
  // S4: a later set lets main in, so the press runs today's way there; the stop and the next
  // file drop it, as they drop the notices
  m_midTx.vs10Fresh = false;
  if (m_midTx.vs10 >= 0)
  {
    if (laterSet)
      TxMidVs10ToMain(m_midTx.vs10Action, m_midTx.vs10Id, "after the path");
    else
      CLog::Log(LOGINFO, "MIDPLAY vs10 request {} dropped ({})", m_midTx.vs10Id, why);
    m_midTx.vs10 = -1;
  }
  if (n < 0)
    CLog::Log(LOGINFO, "MIDPLAY path off T={} ({})", m_txMidPathT, why);
  else
    CLog::Log(LOGINFO, "MIDPLAY path off T={} ({}), {} notices {}", m_txMidPathT, why, n,
              laterSet ? "raised" : "dropped");
}

bool CVideoPlayer::TxMidBusy(bool decisions) const
{
  return std::any_of(m_midJobs.begin(), m_midJobs.end(), [decisions](const SMidJob& j)
                     { return j.path && !(decisions && j.setting); });
}

bool CVideoPlayer::TxMidHoldPending()
{
  // S8 ii: a job POSTED or FROZEN, or a decision due while fullscreen; owed at fullscreen alone
  // does not hold (a seek with the GUI over the video). 5.G-5: a setting job is no decision (the
  // release waits for the count, which it does not bump)
  if (TxMidBusy(true))
    return true;
  // code review r1 S4: a hand-back too (main decides only fullscreen)
  if (!m_midTx.handback && !m_midTx.redecide && !m_midTx.retryDue && m_midTx.vs10 < 0 &&
      !m_renderManager.TxMidNoticesPending())
    return false;
  if (m_midTx.inputsDue)
    return true;
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  return latest.seq <= m_midTx.inputsAtOn || (latest.fullScreenVideo && latest.fullScreenRoot);
}

void CVideoPlayer::TxMidPost(SMidJob& j, const char* reply, bool count)
{
  // 5.G-5: a pick's job posts none, as today's direct set
  if (j.av)
    VideoParamsChange();
  // 5.G-5: a setting job is no decision: no seq bump (§4.5)
  if (count && !j.setting)
    ++m_midTx.counted;
  j.posted = true;
  CLog::Log(LOGINFO, "MIDPLAY reply seq={} {} {}{}", j.job->seq, reply,
            j.av        ? "avchange posted (P)"
            : j.setting ? "no avchange (a setting's)"
                        : "no avchange (a pick's)",
            count || j.setting ? "" : ", counted at the retry's reply");
}

void CVideoPlayer::TxMidClearJobs()
{
  for (SMidJob& j : m_midJobs)
    j.job->ClearSink();
  m_midJobs.clear();
}

// 5.G-3 (§4.0, §4.3 c-f): OW's verdicts and the job's replies
void CVideoPlayer::TxMidInbox(const DISPLAYSTATE::STxInboxEntry& e)
{
  using Mid = DISPLAYSTATE::STxInboxEntry::Mid;
  using Settle = DISPLAYSTATE::StartSettleKind;
  const auto it = std::find_if(m_midJobs.begin(), m_midJobs.end(),
                               [&e](const SMidJob& j) { return j.job->seq == e.jobSeq; });
  if (it == m_midJobs.end())
  {
    CLog::Log(LOGINFO, "MIDPLAY job seq={} reply ignored (no record)", e.jobSeq);
    return;
  }
  SMidJob& j = *it;
  switch (e.mid)
  {
    case Mid::DEFERRED:
    case Mid::HELD:
      CLog::Log(LOGINFO, "MIDPLAY verdict reqId={} seq={} {}", e.reqId, e.jobSeq,
                e.mid == Mid::DEFERRED ? "DEFERRED" : "HELD");
      break;
    case Mid::DROPPED:
      // r2 N2: no redecide; the drop's own re-raise (gap / held clear, replay) is a notice
      CLog::Log(LOGINFO, "MIDPLAY verdict reqId={} seq={} DROPPED", e.reqId, e.jobSeq);
      TxMidVs10GiveBack(j);
      j.settled = true;
      j.main = false;
      TxMidPost(j, "DROPPED");
      // 5.G-5: a dropped setting's body never ran: today's job runs it (never lost)
      if (j.setting)
        TxMidSettingToday(j.job->settingName ? j.job->settingName : "?", "job dropped");
      else if (j.path)
        m_midTx.redecide = false;
      break;
    case Mid::OUTPUT_DONE:
      // §4.3 f: the re-decide point of a commit (and of a no-change)
      if (j.path)
        m_midTx.redecideDue = true;
      break;
    case Mid::SETTLE_DONE:
    {
      j.settled = true;
      const bool failed = e.settle == Settle::FAILED || e.settle == Settle::FAILED_PARK;
      if (e.settle != Settle::NO_CHANGE && !failed && e.settle != Settle::DROPPED)
        break;
      TxMidVs10GiveBack(j);
      j.main = e.settle != Settle::DROPPED;
      if (!j.path)
      {
        TxMidPost(j, e.settle == Settle::NO_CHANGE ? "DONE_NO_CHANGE"
                     : failed                      ? "FAILED"
                                                   : "DROPPED (W)");
        if (j.setting && e.settle == Settle::DROPPED)
          TxMidSettingToday(j.job->settingName ? j.job->settingName : "?", "after the path");
        break;
      }
      m_midTx.redecideDue = true;
      // W's drop set nothing: decided again, and that job's reply posts and counts it (code
      // review r1 S2: one AVCHANGE, as off's one consume)
      if (e.settle == Settle::DROPPED && j.setting)
      {
        // 5.G-5: the body did not run: posted again
        m_midTx.settingDue = true;
        CLog::Log(LOGINFO, "MIDPLAY reply seq={} DROPPED (W): setting posted again", e.jobSeq);
        break;
      }
      if (e.settle == Settle::DROPPED)
      {
        m_midTx.redecide = true;
        m_midTx.avOwed |= j.av;
        CLog::Log(LOGINFO, "MIDPLAY reply seq={} DROPPED (W): decided again", e.jobSeq);
        break;
      }
      if (!failed)
      {
        TxMidPost(j, "DONE_NO_CHANGE");
        break;
      }
      // S9: one re-post, the arm restored as RM's rm retry; counted at the retry's reply, so a
      // seek's held start stays held through it (code review r1 S1)
      if (!std::exchange(m_midTx.retried, true))
      {
        TxMidPost(j, "FAILED", false);
        if (j.armTaken)
          m_renderManager.TxMidRestoreArm();
        m_midTx.retryDue = true;
        m_midTx.retryAv = j.av;
        CLog::Log(LOGINFO, "MIDPLAY retry seq={}", e.jobSeq);
      }
      else
      {
        TxMidPost(j, "FAILED");
        m_midTx.retried = false;
        CLog::Log(LOGINFO, "MIDPLAY retry failed seq={}", e.jobSeq);
      }
      break;
    }
    case Mid::ADOPTED:
      j.adopted = true;
      // main posted it (TxMidAdoptAnswer): counted after GC's set, as RM's count
      if (!j.posted && e.form != DISPLAYSTATE::StartAdoptForm::NO_CHANGE)
      {
        j.posted = true;
        // 5.G-5: a setting job is no decision
        if (!j.setting)
          ++m_midTx.counted;
      }
      if (j.path && e.form != DISPLAYSTATE::StartAdoptForm::FAILED)
        m_midTx.retried = false;
      break;
  }
  if (j.settled && (j.adopted || !j.main))
  {
    j.job->ClearSink();
    m_midJobs.erase(it);
  }
}

// 5.G-3 (§4.3 b): after TxDeferPass, every pass (frozen too)
void CVideoPlayer::TxMidPlayPass()
{
  if (!m_txMidPath)
    return;
  // S3 a: never beside an open start's TxStartGatePass (the B2 end makes it unreachable)
  if (TxOpen())
  {
    if (!std::exchange(m_midTx.openLogged, true))
      CLog::Log(LOGERROR, "MIDPLAY path on with a start open T={}", m_txMidPathT);
    return;
  }
  std::vector<CRenderManager::STxNotice> notices;
  if (m_renderManager.TxMidNoticesPending())
    notices = m_renderManager.TakeTxNotices();
  if (!notices.empty())
  {
    std::string kinds;
    for (const CRenderManager::STxNotice& n : notices)
      kinds += (kinds.empty() ? "" : ",") + std::string(n.kind);
    CLog::Log(LOGINFO, "MIDPLAY notice {} T={}", kinds, m_txMidPathT);
  }
  // main's handed-back decision counts once it is done
  if (m_midTx.handback && !m_renderManager.TxMidHandBackBusy())
  {
    m_midTx.handback = false;
    ++m_midTx.counted;
  }
  const bool busy = TxMidBusy();
  const bool due = std::exchange(m_midTx.redecideDue, false);
  const bool vs10 = std::exchange(m_midTx.vs10Fresh, false);
  const bool pick = std::exchange(m_midTx.pickFresh, false);
  // 5.G-5 (§4.0 compat): main's decision would post an AVCHANGE for these, never for a pick
  if (!notices.empty() || vs10)
    m_midTx.avOwed = true;
  if ((!notices.empty() || vs10 || pick) && (busy || m_midTx.handback))
    m_midTx.redecide = true;
  // a pick's own job waits for the start's window to close: OW would defer it and drop it at
  // StartDone, whose replay raises one decision more than today's replayed pick (E2-12a)
  const bool pickWaits = m_txDeferF && !m_midTx.avOwed;
  const char* why = nullptr;
  if (m_midTx.retryDue)
    why = "retry";
  else if ((!notices.empty() || vs10) && !busy)
    why = notices.empty() ? "vs10" : "notice";
  else if (m_midTx.pickDue && !busy && !pickWaits)
    why = "pick";
  else if (m_midTx.redecide && (!busy || due) && !pickWaits)
    why = "supersede";
  else if (m_midTx.owedAtFullscreen && !busy &&
           DISPLAYSTATE::DecideInputsSeq() > m_midTx.owedSeq)
    why = "owed";
  else if (m_midTx.inputsDue && !busy && DISPLAYSTATE::DecideInputsSeq() > m_midTx.inputsAtOn)
    why = "path-on inputs";
  // 5.G-5 (§4.5, T10): a setting's job in a pass with no DECIDE and no path job in flight (code
  // review r1 B1: its incumbent would be the mode before that job's commit, and its forced commit
  // would revert it), after the start's window (OW would defer it and drop it at StartDone,
  // re-raising a decision)
  if (!why && m_midTx.settingDue && !busy && !m_midTx.handback && !m_txDeferF)
  {
    TxMidSettingJob();
    return;
  }
  if (!why || m_midTx.handback)
    return;
  if (!busy)
    m_midTx.chain = 0;
  else if (!m_midTx.retryDue)
  {
    // the last job is kept; the decision waits for it
    if (m_midTx.chain >= 3)
    {
      if (m_midTx.chain++ == 3)
        CLog::Log(LOGINFO, "MIDPLAY chain cap T={}", m_txMidPathT);
      return;
    }
    ++m_midTx.chain;
  }
  TxMidDecide(why);
}

// 5.G-3 (§4.3 b, c): one DECIDE, one job (W classifies it)
void CVideoPlayer::TxMidDecide(const char* why)
{
  const bool retry = std::exchange(m_midTx.retryDue, false);
  m_midTx.redecide = false;
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  // the publish taken at path on is not in yet: decided once it is, never owed on START's
  // pre-fullscreen inputs
  if (latest.seq <= m_midTx.inputsAtOn)
  {
    if (retry)
      m_midTx.avOwed |= m_midTx.retryAv;
    if (!std::exchange(m_midTx.inputsDue, true))
      CLog::Log(LOGINFO, "MIDPLAY decide waits for the path-on inputs (inputs={})", latest.seq);
    return;
  }
  if (std::exchange(m_midTx.inputsDue, false))
    CLog::Log(LOGINFO, "MIDPLAY path-on inputs={} in", latest.seq);
  // 5.G-5 (§4.0 compat): one AVCHANGE when the DECIDE carries a decision main would make (a
  // notice, a press, or the retry of one); a pick alone posts none, as today's direct set
  bool av = m_midTx.avOwed || (retry && m_midTx.retryAv);
  const bool fullScreen = !latest.seq || (latest.fullScreenVideo && latest.fullScreenRoot);
  // RM:3635's test: not fullscreen, owed (nothing posted, no AVCHANGE, as main's kept flag)
  if (!fullScreen)
  {
    if (av || !m_midTx.pickDue)
    {
      if (!m_midTx.owedAtFullscreen || m_midTx.owedSeq != latest.seq)
        CLog::Log(LOGINFO, "MIDPLAY owed at fullscreen inputs={}", latest.seq);
      m_midTx.owedAtFullscreen = true;
      m_midTx.owedSeq = latest.seq;
      m_midTx.avOwed = av;
    }
    // 5.G-5: a pick sets at once with the GUI over the video, as today's direct set; the owed
    // decision follows at fullscreen
    if (!m_midTx.pickDue)
      return;
    av = false;
  }
  else if (std::exchange(m_midTx.owedAtFullscreen, false))
    CLog::Log(LOGINFO, "MIDPLAY owed decided inputs={}", latest.seq);
  const bool pick = std::exchange(m_midTx.pickDue, false);
  CRenderManager::SStartResolve r = m_renderManager.TxMidResolve(latest, !fullScreen);
  // a press main decides after: written today's way first (W's own vs10 request), so the
  // hand-back's set follows it on the lane
  const auto vs10Today = [this](const char* why)
  {
    const unsigned int mode = static_cast<unsigned int>(std::exchange(m_midTx.vs10, -1));
    CLog::Log(LOGINFO, "MIDPLAY vs10 request {} mode={}: today's write ({})", m_midTx.vs10Id,
              mode, why);
    OUTPUT::COutputWorker::Get().PostRequest(
        OUTPUT::Lane::MIDPLAY, "vs10", [mode] { DISPLAYSTATE::RunVs10Write(mode); }, {});
  };
  if (r.exclusion)
  {
    // 5.G-5: the pick goes today's way; main has nothing to decide for a pick alone
    if (pick)
      TxMidPickToMain(m_midTx.pickIndex, m_midTx.pickId, r.exclusion);
    if (!av)
      return;
    if (m_midTx.vs10 >= 0)
      vs10Today(r.exclusion);
    m_midTx.avOwed = false;
    m_midTx.handback = true;
    m_renderManager.TxMidHandBack(r.exclusion);
    return;
  }
  // S7: the arm consumed (and recorded for a retry) and the anchor, as main's consume; held, as
  // ChooseHeldResolution anchors an unanchored session (code review r1 B1). 5.G-5: a pick's job
  // alone takes neither (today's direct set does not)
  if (av)
  {
    m_renderManager.TxTakeArm(0, r);
    if (r.anchor)
      aml_set_disc_mode_anchored(true);
    else if (r.held && !aml_disc_mode_anchored() && r.width > 0 && r.height > 0)
    {
      aml_set_disc_mode_anchored(true);
      CLog::Log(LOGINFO, "MIDPLAY disc session mode hold: anchoring the session on the "
                         "segment's own resolution ({}x{})",
                r.width, r.height);
    }
  }
  else
    r.frac = false;
  // 5.G-5 (§4.5, D-G5): the explicit target replaces the decided row
  const RESOLUTION decided = r.res;
  const bool sticky = TxMidApplySticky(r, latest);
  // under the GUI only a target is set, never the film's own mode (main's waits for fullscreen)
  if (!fullScreen && !sticky)
  {
    CLog::Log(LOGINFO, "MIDPLAY pick under the GUI without a target: nothing set");
    return;
  }
  if (sticky && r.res != decided && r.frac)
  {
    // the 1000/1001 request was for the row the film chose, not the user's
    r.frac = false;
    CLog::Log(LOGINFO, "MIDPLAY sticky target: the frac request dropped");
  }
  auto job = std::make_shared<DISPLAYSTATE::SStartJob>();
  job->midplay = true;
  job->T = m_txMidPathT;
  job->gen = 0;
  job->seq = ++s_midJobSeq;
  job->res = r.info;
  job->resIndex = static_cast<int>(r.res);
  job->stereo = r.stereo;
  if (job->stereo == RenderStereoMode::HARDWAREBASED)
    job->res.iHeight = r.info.iScreenHeight * 2 + r.info.iBlanking;
  job->hdrType = r.hdrType;
  job->fullScreen = true;
  job->fracRequest = r.frac;
  job->filmStart = false;
  job->tweakMs = r.tweakMs;
  if (const auto state = DISPLAYSTATE::Get())
  {
    job->capsEpoch = state->capsEpoch;
    job->sinkSig = state->sinkSig;
  }
  CLog::Log(LOGINFO,
            "MIDPLAY decide seq={} T={} res={} ({}) frac={} anchor={} sticky={} why={}{}",
            job->seq, job->T, static_cast<int>(r.res), r.info.strMode, r.frac ? 1 : 0,
            av && r.anchor ? 1 : 0, sticky ? r.info.strMode : std::string("-"), why,
            av ? "" : " (a pick's: no avchange)");
  if (av)
    m_midTx.avOwed = false;
  SMidJob& j = m_midJobs.emplace_back();
  j.job = job;
  j.av = av;
  j.armTaken = av;
  j.f = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
  // S11: a BYPASS the disc session suppresses writes nothing: a plain DECIDE, no freeze.
  // 5.G-5: a press rides a decision's job, never a pick's (it stays owed with its decision)
  if (m_midTx.vs10 >= 0 && av)
  {
    if (m_midTx.vs10 == static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS) &&
        aml_dv_disc_engaged())
    {
      CLog::Log(LOGINFO, "MIDPLAY vs10 suppressed (disc BYPASS): plain decide request={} seq={}",
                m_midTx.vs10Id, job->seq);
      m_midTx.vs10 = -1;
    }
    else
    {
      job->vs10 = std::exchange(m_midTx.vs10, -1);
      j.vs10Action = m_midTx.vs10Action;
      j.vs10Id = m_midTx.vs10Id;
      CLog::Log(LOGINFO, "MIDPLAY vs10 mode={} request={} seq={}", job->vs10, j.vs10Id,
                job->seq);
    }
  }
  j.sink = std::make_unique<CTxMidSink>(m_renderManager, job->seq, av);
  job->SetSink(j.sink.get());
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!DISPLAYSTATE::StartJobQueue(*job) || !winSystem || !winSystem->PostMidPlayJob(job))
  {
    // no worker: main decides it (its consume posts the AVCHANGE); the arm goes back with it
    if (job->vs10 >= 0)
    {
      m_midTx.vs10 = job->vs10;
      m_midTx.vs10Id = j.vs10Id;
      vs10Today("no worker");
    }
    job->ClearSink();
    m_midJobs.pop_back();
    // 5.G-5: the pick today's way; a hand-back only for a decision main would make
    if (pick)
      TxMidPickToMain(m_midTx.pickIndex, m_midTx.pickId, "no worker");
    if (!av)
      return;
    m_renderManager.TxMidRestoreArm();
    m_midTx.handback = true;
    m_renderManager.TxMidHandBack("no worker");
  }
}

bool CVideoPlayer::TxMidApplySticky(CRenderManager::SStartResolve& r,
                                    const DISPLAYSTATE::DecideInputs& latest)
{
  if (!m_midTx.sticky)
    return false;
  const RESOLUTION_INFO& t = *m_midTx.sticky;
  const auto same = [&t](const RESOLUTION_INFO& info)
  {
    return info.strId == t.strId && info.dwFlags == t.dwFlags &&
           std::abs(info.fRefreshRate - t.fRefreshRate) <= 0.01f;
  };
  // code review r1 B2: the pick's own index while its row matches (RES_DESKTOP may hold the same
  // mode: GC would land on it and the settings window re-write the screenmode)
  if (const auto own = latest.res.infos.find(m_midTx.stickyIndex);
      m_midTx.stickyIndex >= RES_DESKTOP && own != latest.res.infos.end() && same(own->second))
  {
    r.res = static_cast<RESOLUTION>(m_midTx.stickyIndex);
    r.info = own->second;
    r.change = r.res != latest.res.videoResolution;
    return true;
  }
  for (const auto& [index, info] : latest.res.infos)
  {
    if (index < RES_DESKTOP || info.strId != t.strId || info.dwFlags != t.dwFlags ||
        std::abs(info.fRefreshRate - t.fRefreshRate) > 0.01f)
      continue;
    r.res = static_cast<RESOLUTION>(index);
    r.info = info;
    r.change = r.res != latest.res.videoResolution;
    return true;
  }
  CLog::Log(LOGINFO, "MIDPLAY pick sticky {} not in the table: cleared", t.strMode);
  m_midTx.sticky.reset();
  return false;
}

// 5.G-5 (§4.5, S12, S13)
void CVideoPlayer::TxMidPick(const DISPLAYSTATE::STxInboxEntry& e, uint32_t wordF)
{
  const bool clear = e.kind == DISPLAYSTATE::STxInboxEntry::Kind::MIDPLAY_PICK_CLEAR;
  if (e.f != wordF)
  {
    CLog::Log(LOGINFO, "MIDPLAY pick id={} of F={} ignored (F={} open)", e.reqId, e.f, wordF);
    return;
  }
  // S4: taken after the path ended: today's direct set
  if (!m_txMidPath)
  {
    TxMidPickToMain(e.resIndex, e.reqId, "after the path");
    return;
  }
  if (clear)
  {
    // S13: the revert is a clear and a re-decide of the film's own mode, not a pick
    m_midTx.sticky.reset();
    CLog::Log(LOGINFO, "MIDPLAY pick cleared (keep dialog no) id={}", e.reqId);
    // with the GUI over the video (the settings window) the film's mode is decided at the
    // return to fullscreen, as main's; the revert sets the GUI's mode now, as today. A re-decide
    // under the settings window would resize it to the film's mode, which re-writes the
    // screenmode setting (GUIWindowSettingsCategory) and opens a second keep dialog
    const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
    if (latest.seq && !(latest.fullScreenVideo && latest.fullScreenRoot))
    {
      m_midTx.pickDue = false;
      TxMidPickToMain(e.resIndex, e.reqId, "keep dialog no under the GUI");
      return;
    }
  }
  // in a segment gap or an owed mark OW holds a pick until the next video open but drops P's
  // job and re-raises a decision (one AVCHANGE more than today): the pick goes today's way
  OUTPUT::COutputWorker& ow = OUTPUT::COutputWorker::Get();
  if (aml_disc_screen_hold() && (ow.GapFor(e.f) || ow.MarkOwedFor(e.f)))
  {
    TxMidPickToMain(e.resIndex, e.reqId, "segment gap");
    return;
  }
  if (!clear)
  {
    // a row P's table lacks is never lost: today's set
    const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
    const auto& infos = latest.res.infos;
    if (std::none_of(infos.begin(), infos.end(),
                     [&e](const auto& row)
                     {
                       return row.first >= RES_DESKTOP && row.second.strId == e.res.strId &&
                              row.second.dwFlags == e.res.dwFlags &&
                              std::abs(row.second.fRefreshRate - e.res.fRefreshRate) <= 0.01f;
                     }))
    {
      TxMidPickToMain(e.resIndex, e.reqId, "not in P's table");
      return;
    }
    m_midTx.sticky = e.res;
    m_midTx.stickyIndex = e.resIndex;
    CLog::Log(LOGINFO, "MIDPLAY pick id={} res={} ({}) sticky", e.reqId, e.resIndex,
              e.res.strMode);
  }
  m_midTx.pickDue = m_midTx.pickFresh = true;
  m_midTx.pickClear = clear;
  m_midTx.pickIndex = e.resIndex;
  m_midTx.pickId = e.reqId;
}

void CVideoPlayer::TxMidPickToMain(int res, uint64_t id, const char* why)
{
  CLog::Log(LOGINFO, "MIDPLAY pick id={} res={} {}: to main", id, res, why);
  if (res < RES_DESKTOP)
    return;
  if (const auto messenger = CServiceBroker::GetAppMessenger())
    messenger->PostMsg(TMSG_SETVIDEORESOLUTION, res, 0, nullptr, "user-pick");
}

// 5.G-5 (§4.5, T10)
void CVideoPlayer::TxMidSetting(const DISPLAYSTATE::STxInboxEntry& e, uint32_t wordF)
{
  // S4: never dropped; the path ended (or another file opened): today's job
  if (e.f != wordF || !m_txMidPath)
  {
    TxMidSettingToday(e.name, e.f != wordF ? "another file" : "after the path");
    return;
  }
  // OW drops P's job in a segment gap or an owed mark and re-raises a decision
  OUTPUT::COutputWorker& ow = OUTPUT::COutputWorker::Get();
  if (aml_disc_screen_hold() && (ow.GapFor(e.f) || ow.MarkOwedFor(e.f)))
  {
    TxMidSettingToday(e.name, "segment gap");
    return;
  }
  if (m_midTx.settingDue)
    CLog::Log(LOGINFO, "MIDPLAY setting {} folded into {}", m_midTx.settingName, e.name);
  m_midTx.settingDue = true;
  m_midTx.settingName = e.name;
}

void CVideoPlayer::TxMidSettingToday(const std::string& name, const char* why)
{
  CLog::Log(LOGINFO, "MIDPLAY setting {} {}: today's job", name, why);
  OUTPUT::COutputWorker::Get().Post(OUTPUT::Lane::SETTINGS, "vsvdb setting",
                                    [] { CWinSystemAmlogic::ApplyVsvdbSetting(); });
}

// 5.G-5 (§4.5): not a DECIDE: the incumbent mode, no seq bump, no AVCHANGE (today's post none);
// W runs the body inside G-4's freeze, then the forced commit and SETTLE
void CVideoPlayer::TxMidSettingJob()
{
  m_midTx.settingDue = false;
  const std::string name = m_midTx.settingName;
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  const auto state = DISPLAYSTATE::Get();
  const RESOLUTION incumbent = m_renderManager.TxMidIncumbent(latest);
  // a freeze needs a connected wire on a mode P's table has
  if (!state || !state->connected || !latest.seq || !latest.res.infos.contains(incumbent))
  {
    TxMidSettingToday(name, "no committed mode");
    return;
  }
  // the facts a decision would carry (HDR type, GUI stereo mode); the mode is the incumbent
  const CRenderManager::SStartResolve r = m_renderManager.TxMidResolve(latest, true);
  auto job = std::make_shared<DISPLAYSTATE::SStartJob>();
  job->midplay = true;
  job->T = m_txMidPathT;
  job->gen = 0;
  job->seq = ++s_midJobSeq;
  job->res = latest.res.infos.at(incumbent);
  job->resIndex = static_cast<int>(incumbent);
  job->stereo = r.stereo;
  if (job->stereo == RenderStereoMode::HARDWAREBASED)
    job->res.iHeight = job->res.iScreenHeight * 2 + job->res.iBlanking;
  job->hdrType = r.hdrType;
  job->fullScreen = true;
  job->fracRequest = false;
  job->filmStart = false;
  job->capsEpoch = state->capsEpoch;
  job->sinkSig = state->sinkSig;
  job->setting = [] { CWinSystemAmlogic::ApplyVsvdbSetting(); };
  // a static name for W's log: the job may outlive this record
  static const char* const s_vsvdb = "vsvdb";
  job->settingName = s_vsvdb;
  CLog::Log(LOGINFO, "MIDPLAY setting {} job seq={} T={} res={} ({}): no decision", name,
            job->seq, job->T, static_cast<int>(incumbent), job->res.strMode);
  SMidJob& j = m_midJobs.emplace_back();
  j.job = job;
  j.av = false;
  j.setting = true;
  j.f = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
  j.sink = std::make_unique<CTxMidSink>(m_renderManager, job->seq, false);
  job->SetSink(j.sink.get());
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!DISPLAYSTATE::StartJobQueue(*job) || !winSystem || !winSystem->PostMidPlayJob(job))
  {
    job->ClearSink();
    m_midJobs.pop_back();
    TxMidSettingToday(name, "no worker");
  }
}

bool CVideoPlayer::TxMidVs10Press(int action, unsigned int mode)
{
  const DISPLAYSTATE::MidPlayPathState path = DISPLAYSTATE::MidPlayPath();
  if (!path.on ||
      path.f != OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f)
    return false;
  static std::atomic<uint64_t> s_vs10Id{0};
  DISPLAYSTATE::STxInboxEntry e;
  e.kind = DISPLAYSTATE::STxInboxEntry::Kind::VS10_REQUEST;
  e.reqId = ++s_vs10Id;
  e.f = path.f;
  e.vs10Mode = mode;
  e.action = action;
  CLog::Log(LOGINFO, "MIDPLAY vs10 press request={} mode={} F={}: to P", e.reqId, mode, e.f);
  DISPLAYSTATE::PostTxInbox(std::move(e));
  return true;
}

void CVideoPlayer::TxMidVs10Request(const DISPLAYSTATE::STxInboxEntry& e, uint32_t wordF)
{
  if (e.f != wordF)
  {
    CLog::Log(LOGINFO, "MIDPLAY vs10 request {} of F={} ignored (F={} open)", e.reqId, e.f,
              wordF);
    return;
  }
  if (!m_txMidPath)
  {
    TxMidVs10ToMain(e.action, e.reqId, "after the path");
    return;
  }
  // code review r1 B1: OW drops a mid-play job in a segment gap or owed mark but holds a vs10
  // request and replays it (VS10_RAN): today's route, one decision as off
  OUTPUT::COutputWorker& ow = OUTPUT::COutputWorker::Get();
  if (aml_disc_screen_hold() && (ow.GapFor(e.f) || ow.MarkOwedFor(e.f)))
  {
    const unsigned int mode = e.vs10Mode;
    CLog::Log(LOGINFO, "MIDPLAY vs10 request {} mode={}: today's write (segment gap)", e.reqId,
              mode);
    ow.PostRequest(OUTPUT::Lane::MIDPLAY, "vs10", [mode] { DISPLAYSTATE::RunVs10Write(mode); }, {});
    return;
  }
  if (m_midTx.vs10 >= 0)
    CLog::Log(LOGINFO, "MIDPLAY vs10 request {} superseded by request {}", m_midTx.vs10Id,
              e.reqId);
  m_midTx.vs10 = static_cast<int>(e.vs10Mode);
  m_midTx.vs10Action = e.action;
  m_midTx.vs10Id = e.reqId;
  m_midTx.vs10Fresh = true;
  CLog::Log(LOGINFO, "MIDPLAY vs10 request {} mode={} taken", e.reqId, e.vs10Mode);
}

void CVideoPlayer::TxMidVs10ToMain(int action, uint64_t id, const char* why)
{
  CLog::Log(LOGINFO, "MIDPLAY vs10 request {} {}: to main", id, why);
  if (const auto messenger = CServiceBroker::GetAppMessenger())
    messenger->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                       static_cast<void*>(new CAction(action)));
}

void CVideoPlayer::TxMidVs10GiveBack(const SMidJob& j)
{
  if (j.job->vs10 < 0 || j.job->vs10Written)
    return;
  const uint32_t f = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
  if (j.path && m_txMidPath)
  {
    // the next DECIDE carries it (no DECIDE of its own: a drop's re-raise brings it, r2 N2)
    if (m_midTx.vs10 >= 0)
      CLog::Log(LOGINFO, "MIDPLAY vs10 request {} not written: superseded by request {}",
                j.vs10Id, m_midTx.vs10Id);
    else
    {
      m_midTx.vs10 = j.job->vs10;
      m_midTx.vs10Action = j.vs10Action;
      m_midTx.vs10Id = j.vs10Id;
      CLog::Log(LOGINFO, "MIDPLAY vs10 request {} not written: pending again", j.vs10Id);
    }
  }
  else if (f == j.f)
    TxMidVs10ToMain(j.vs10Action, j.vs10Id, "not written, after the path");
  else
    CLog::Log(LOGINFO, "MIDPLAY vs10 request {} not written: dropped (F={} open)", j.vs10Id, f);
}

void CVideoPlayer::TxStartRelease(int64_t discRunNs)
{
  if (m_txDiscHeld)
    m_discTime.SetTxHeld(false, discRunNs);
  m_txDiscHeld = false;
  m_renderManager.SetTxStartHeld(false);
}

void CVideoPlayer::TxCheckVerdict(const char* where)
{
  const unsigned int verdict = m_renderManager.TxLegacyVerdict();
  if (!verdict)
    return;
  // transaction_forcelegacy aftersync: the verdict is read only once the sync commit is held
  if (aml_transaction_hooks().forceLegacy == "aftersync" && !m_txSyncPassed)
    return;
  if (verdict != m_txEngagedGen)
  {
    CLog::Log(LOGINFO, "TXSTART renderer verdict for set {} ignored (engaged set {})", verdict,
              m_txEngagedGen);
    return;
  }
  CLog::Log(LOGINFO, "TXSTART renderer legacy verdict read at {}", where);
  // 5.E2-8b D8: before the post a reraise; at the release (committed) job-ok
  EndTransactionToLegacy("renderer legacy", CRenderManager::TxHow::RERAISE,
                         CRenderManager::TxNeed::JOB_OK);
}

void CVideoPlayer::EndTransactionToLegacy(const char* site,
                                          CRenderManager::TxHow how,
                                          CRenderManager::TxNeed need,
                                          bool applySpeed)
{
  if (!TxOpen())
    return;
  m_txSuperseded = false;
  // 5.E2-8b D8: the race, then main let back in (the exit's first step after the CAS)
  if (m_txJob)
  {
    using State = DISPLAYSTATE::StartJobState;
    using Need = CRenderManager::TxNeed;
    bool past = false;
    if (need == Need::LEFT && m_txStep == TxStep::POSTED)
      // r9 M1: a fullscreen exit after the post never drops the job; code review A-M1: one GC
      // dropped never reaches the wire and is answered by nobody (reraise)
      past = m_txJob->state.load() != State::DROPPED;
    else if (m_txJob->state.load() == State::NOT_POSTED)
      m_txJob->Cas({State::NOT_POSTED}, State::DROPPED); // never posted: nothing to log
    else if (!DISPLAYSTATE::StartJobDropForExit(*m_txJob, site))
      past = m_txJob->state.load() == State::COMMITTED;
    if (past && need == Need::NONE)
    {
      // E2-8b NOTE: every site past the drop CAS names its need; none named reads as job-ok
      CLog::Log(LOGWARNING, "TXSTART T={} ended ({}) past its drop CAS with no need: job-ok",
                m_txJob->T, site);
      need = Need::JOB_OK;
    }
    // r13 L3: kept for RM::TxFileEnded at a later stop or next file
    m_txEndedGen = m_txEngagedGen;
    m_txEndedLeft = past && need == Need::LEFT;
    m_renderManager.TxOpenMain(m_txEngagedGen, past ? CRenderManager::TxHow::NONE : how,
                               past ? need : Need::NONE);
    TxClearOpenJob("exit");
    m_txStep = TxStep::NONE;
    // code review A-M1: GC's drop landed after the read above (a TxOpenJob copy taken before the
    // clear): no answer comes, so `left x failed` resolves it (TxOnReply covers a later drop)
    if (m_txEndedLeft && m_txJob->state.load() == State::DROPPED)
    {
      CLog::Log(LOGINFO, "TXSTART T={} dropped by a GUI set at the exit: answer failed",
                m_txJob->T);
      m_renderManager.TxAdoptAnswer(m_txEndedGen, CRenderManager::TxAnswer::FAILED);
      m_txEndedLeft = false;
    }
  }
  // the undo list (plan E2-8a, review r3 H-3), in order; 5.E2-11: not after START's CAS
  if (m_txWordStarting && !(m_txJob && m_txJob->started.load()))
    OUTPUT::COutputWorker::Get().WordStartDone(m_txWordT.load(), "ended to legacy");
  m_renderManager.SetTxStartHeld(false);
  m_renderManager.SetTxGraphicsFrom(0); // 5.E2-9b D3
  if (m_txDiscHeld)
    m_discTime.SetTxHeld(false);
  m_txDiscHeld = false;
  // undo 4: E2-6 parity, the legacy release opens C's gate. Undo 6 cannot apply while main
  // decides: the engaged sync commit is START, a scheduled release, or E1's hold
  if (m_txSyncPassed && !m_startHeld)
    CLog::Log(LOGWARNING, "TRANSACTION undo 6 not applicable (sync passed, no E1 hold)");
  m_txExited = true;
  m_txLossAtUntil = {};
  TxHintClear(); // 5.E2-9c (r5 M2)
  // 5.E2-9c D6: a settle+ hold whose settle done will not come
  if (m_txNoPictureSettleMs >= 0)
  {
    m_txNoPictureSettleMs = -1;
    m_VideoPlayerVideo->SetTxNoPicture(0);
    CLog::Log(LOGWARNING, "TXTEST nopicture released at the exit");
  }
  const char* caller = std::string_view(site) == "flush" && m_txFlushCaller ? m_txFlushCaller : "";
  CLog::Log(LOGINFO, "TRANSACTION ended to legacy ({}) T={} set={}{}{}", site, m_txWordT.load(),
            m_txEngagedGen, *caller ? " caller=" : "", caller);
  // 5.E2-11 D1.8: an exit inside a span (not stop, next file or the error exit) joins it with
  // today's freeze; HoldIfDisplayLost tests the span under the walk's lock. P's edge first
  bool spanDisc = std::exchange(m_txSpanDiscLost, false);
  m_txSpanSeen = m_txInSpan = false;
  if (applySpeed && !m_displayLost && aml_transaction_loss_in_start())
  {
    auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
    if (winSystem && winSystem->HoldIfDisplayLost(this))
    {
      spanDisc = false; // the freeze owns disc time
      CLog::Log(LOGINFO, "TXLOSS exit inside the span: today's freeze");
    }
    // code review B M-7: after a deferral the link is uncommitted until the owed Restore (or a
    // marked set) commits; the legacy start waits for that set's reset as for a lost display
    else if (winSystem && winSystem->HoldIfRestoreOwed(this))
    {
      spanDisc = false;
      CLog::Log(LOGINFO, "TXLOSS exit with the Restore owed: held until it commits");
    }
  }
  if (spanDisc && !m_displayLost)
    m_discTime.SetDisplayLost(false);
  // 5.E2-9b D1.4: the speed pending in the start is applied at the exit (not at stop, next file
  // or the error exit). While lost a pause takes the loss conversion's speeds without its
  // callbacks; any other speed is kept for the 15 s conversion or the first pass back
  if (applySpeed && m_txPendingSpeed != INT_MIN)
  {
    const int pending = m_txPendingSpeed;
    if (!m_displayLost)
    {
      ApplySpeed(pending);
      CLog::Log(LOGINFO, "TXSTART speed {} applied at the exit ({}, caching {})", pending, site,
                TxCachingName(m_caching));
    }
    else if (pending == DVD_PLAYSPEED_PAUSE)
    {
      ConvertToPauseSpeeds();
      CLog::Log(LOGINFO, "TXSTART speed {} applied at the exit ({}, caching {})", pending, site,
                TxCachingName(m_caching));
    }
    else
      CLog::Log(LOGINFO, "TXSTART speed {} kept at the exit ({}): the display is lost", pending,
                site);
  }
  // 5.E2-12a D4.2: the window's close (the stop's comes after the word's stop, in OnExit)
  using Outcome = OUTPUT::COutputWorker::DeferOutcome;
  const std::string_view exitSite(site);
  if (exitSite == "next file")
    TxStartDone(static_cast<int>(Outcome::NEXT_FILE), site);
  else if (exitSite == "segment transition")
    TxStartDone(static_cast<int>(Outcome::SEGMENT), site);
  else if (exitSite != "stop")
    TxStartDone(static_cast<int>(Outcome::LEGACY), site);
}

// 5.E2-12b D1, D2 (docs/phase5_e2_12b_impl.md)
bool CVideoPlayer::TxNoVideoInStart() const
{
  if (!m_startGateShut)
    return false;
  if (TxOpen())
    return true;
  return m_txNoVideoCarry && m_txNoVideoCarry->job &&
         m_txNoVideoCarry->job->state.load() == DISPLAYSTATE::StartJobState::COMMITTED;
}

void CVideoPlayer::TxNoVideoReset(const char* why)
{
  if (m_txNoVideoHold || m_txNoVideoStep)
    CLog::Log(LOGINFO, "TXSTART row 3b state reset ({})", why);
  m_txNoVideoCarry.reset();
  m_txNoVideoHold = false;
  m_txNoVideoSince = {};
  m_txNoVideoStep = nullptr;
  m_txNoVideoWaitState = 0;
  m_txNoVideoHeld = m_txNoVideoOwedPosted = m_txNoVideoWaitLogged = false;
  m_txNoVideoWire.reset();
}

void CVideoPlayer::TxRow3cPoint(int point)
{
  using WS = OUTPUT::COutputWorker::WordState;
  if (!aml_disc_screen_hold() || !m_pInputBluray || TxOpen() || m_txNoVideoCarry)
    return;
  const auto w = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
  // code review L-2: a screen 12b-1's step already holds is not re-held
  if (!w.f || (point > 1 && (m_txRow3cHeld || OUTPUT::COutputWorker::Get().HeldFor(w.f))))
    return;
  DISPLAYSTATE::ModeFacts wire;
  if (const auto state = DISPLAYSTATE::Get(); state && state->connected && !state->strId.empty())
    wire = {state->strId, state->screenWidth, state->screenHeight, state->refresh, state->dwFlags};
  if ((w.state != WS::NONE && w.state != WS::STARTING && w.state != WS::LEADING) ||
      (w.state == WS::NONE && w.spanOpen))
  {
    // errata E4: a frozen word (or an open span, code review L-3) at the point: the step at the
    // next NONE{T} without a span
    if (!m_txOwedPoint || point > m_txOwedPoint)
    {
      m_txOwedPoint = point;
      OUTPUT::COutputWorker::Get().MarkOwed(w.f, wire);
      CLog::Log(LOGINFO, "TRANSACTION noVideoMarkOwed set at point ({})", point);
    }
    return;
  }
  if (w.state != WS::NONE)
    return;
  if (point == 1)
  {
    OUTPUT::COutputWorker::Get().GapOpen(w.f, wire);
    TxRequestAt("gap");
    return;
  }
  // points 2, 3: the screen held at W's pinned target (the film's mode), else the wire's
  const auto pinned = OUTPUT::COutputWorker::Get().HeldOutput(w.f);
  const DISPLAYSTATE::ModeFacts mode = pinned ? *pinned : wire;
  if (mode.strId.empty())
    return;
  OUTPUT::COutputWorker::Get().HoldRecord(w.f, mode);
  OUTPUT::COutputWorker::Get().GapClose(w.f, "held", false);
  m_txRow3cHeld = true;
  CLog::Log(LOGINFO, "TRANSACTION row 3c F={} at point ({}): record held at {}", w.f, point,
            DISPLAYSTATE::FactsString(mode));
}

void CVideoPlayer::TxNoVideoNoteWire()
{
  if (m_txNoVideoWire)
    return;
  if (const auto state = DISPLAYSTATE::Get(); state && state->connected && !state->strId.empty())
    m_txNoVideoWire = std::make_shared<DISPLAYSTATE::ModeFacts>(DISPLAYSTATE::ModeFacts{
        state->strId, state->screenWidth, state->screenHeight, state->refresh, state->dwFlags});
}

// row 3b's step (D2.3-D2.6), a P-loop state: the waits, then the end
void CVideoPlayer::TxNoVideoStepPass()
{
  if (!m_txNoVideoStep)
    return;
  using State = DISPLAYSTATE::StartJobState;
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  const uint32_t F =
      OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
  const std::shared_ptr<DISPLAYSTATE::SStartJob> job =
      TxOpen() ? m_txJob : (m_txNoVideoCarry ? m_txNoVideoCarry->job : nullptr);
  const bool committed = job && job->state.load() == State::COMMITTED;
  const bool settled =
      TxOpen() ? m_txSettle.has_value() : (m_txNoVideoCarry && m_txNoVideoCarry->settled);
  // Q1 (r2 M-A): the held mode is W's last committed mode; never an empty one (code review M-1):
  // the job's row if it committed, else the wire mode noted when the hold began, else a wait
  if (!m_txNoVideoHeld)
  {
    TxNoVideoNoteWire();
    std::optional<DISPLAYSTATE::ModeFacts> mode;
    if (committed)
      mode = DISPLAYSTATE::FactsOf(job->res);
    else if (m_txNoVideoWire)
      mode = *m_txNoVideoWire;
    if (!mode)
    {
      if (!std::exchange(m_txNoVideoWaitLogged, true))
        CLog::Log(LOGINFO, "TXSTART T={} row 3b waits for the display (no mode to hold)",
                  m_txWordT.load());
      return;
    }
    m_txNoVideoWaitLogged = false;
    OUTPUT::COutputWorker::Get().HoldRecord(F, *mode);
    m_txNoVideoHeld = true;
  }
  const char* waitFor = nullptr;
  if (committed && !settled)
    waitFor = "the Start job's SettleDone";
  else if (winSystem &&
           (winSystem->RestoreOwed() || winSystem->IsLostSpanOpen() || winSystem->SettleArmed()))
  {
    // r1 H-6, r2 H-B: the owed Restore (now to the held mode) commits and settles first
    if (winSystem->RestoreOwed() && !winSystem->IsLostSpanOpen() && !m_txNoVideoOwedPosted)
    {
      m_txNoVideoOwedPosted = true;
      winSystem->PostOwedRestoreCheck("row 3b", std::chrono::milliseconds(0));
    }
    waitFor = "the owed Restore";
  }
  else if (OUTPUT::COutputWorker::Get().BusyT())
    waitFor = "W's job";
  if (waitFor)
  {
    if (!std::exchange(m_txNoVideoWaitLogged, true))
      CLog::Log(LOGINFO, "TXSTART T={} row 3b waits for {}", m_txWordT.load(), waitFor);
    return;
  }
  const char* point = m_txNoVideoStep;
  const int waitState = m_txNoVideoWaitState;
  const int64_t graceMs =
      m_txNoVideoSince == std::chrono::steady_clock::time_point{}
          ? 0
          : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                  m_txNoVideoSince)
                .count();
  const bool open = TxOpen();
  const auto held = OUTPUT::COutputWorker::Get().HeldOutput(F);
  CLog::Log(LOGINFO,
            "TRANSACTION row 3b T={} at {} (wait state {}, {} ms, {}): record held at {}",
            m_txWordT.load(), point, waitState, graceMs, open ? "open" : "carried",
            held ? DISPLAYSTATE::FactsString(*held) : std::string("(none)"));
  // nothing replays at the step: the window (open or carried) closes with its picks dropped (the
  // next title decides: always its native mode), a VS10 press held, a main decision dropped; so no
  // set can commit between here and CN's open but the owed Restore the waits above saw settled
  if (m_txDeferF)
  {
    const OUTPUT::COutputWorker::SDeferCounts c = OUTPUT::COutputWorker::Get().DeferClose(
        m_txDeferF, OUTPUT::COutputWorker::DeferOutcome::NO_VIDEO);
    CLog::Log(LOGINFO, "TXSTART T={} StartDone (no-video): {} held, {} dropped", m_txWordT.load(),
              c.folded, c.dropped);
    m_txDeferF = 0;
    m_txCarriedAt = {};
    m_txStickyTarget.reset();
    m_txStickyReqId = 0;
    m_txAmendPending = false;
  }
  // D2.5/D2.6: the start ends with nothing owed; replies still queued are logged only
  if (open)
    EndTransactionToLegacy("no-video screen", CRenderManager::TxHow::NONE,
                           CRenderManager::TxNeed::NONE_NEEDED);
  if (job)
    job->ClearSink();
  TxNoVideoReset("row 3b");
  m_renderManager.SetTxStartHeld(false);
  m_discTime.SetTxHeld(false);
  OpenStartGate("transaction: no-video start ended", false);
}

// 5.E2-12a D4 (docs/phase5_e2_12_impl.md): StartDone, the window's only close
void CVideoPlayer::TxStartDone(int outcome, const char* why)
{
  using Outcome = OUTPUT::COutputWorker::DeferOutcome;
  if (!m_txDeferF || !aml_transaction_fold())
    return;
  const auto o = static_cast<Outcome>(outcome);
  const uint32_t T = m_txWordT.load();
  const OUTPUT::COutputWorker::SDeferCounts counts =
      OUTPUT::COutputWorker::Get().DeferClose(m_txDeferF, o);
  if (o == Outcome::SEGMENT)
  {
    m_txCarriedAt = std::chrono::steady_clock::now();
    CLog::Log(LOGINFO, "TXSTART T={} StartDone ({}): {} carried to the next segment", T, why,
              counts.replayed);
    return;
  }
  CLog::Log(LOGINFO, "TXSTART T={} StartDone ({}): {} replayed, {} folded, {} dropped", T, why,
            counts.replayed, counts.folded, counts.dropped);
  m_txDeferF = 0;
  m_txCarriedAt = {};
  m_txStickyTarget.reset();
  m_txStickyReqId = 0;
  m_txAmendPending = false;
  // D4.3: a main decision dropped in the window is made again, once, now that main is let in
  if (counts.mainDropped && o != Outcome::STOP && o != Outcome::NEXT_FILE)
  {
    std::string stereomode;
    m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode, "replay");
  }
}

// code review M-1: the file's F for the stop's / a next file's clears: the one captured before
// WordStop zeroed the word's, else the live word's (a late stop, a next file on a live word)
uint32_t CVideoPlayer::TxClearF()
{
  if (const uint32_t f = std::exchange(m_txStopF, 0))
    return f;
  return OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
}

// 5.E2-12a D1, D4.2: W's entries, then the landed close and a carry's bound; every loop pass
void CVideoPlayer::TxDeferPass()
{
  // the gate hook's repeats fire in every phase (code review G-4)
  if (m_txRequestAtLeft > 0 && std::chrono::steady_clock::now() >= m_txRequestAtNext)
    TxRequestFire();
  if (!aml_transaction_fold())
    return;
  // 5.E2-12b-2 D5: R4's owed point taken at the next NONE{T}
  if (m_txOwedPoint)
  {
    const auto w = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
    if (w.state == OUTPUT::COutputWorker::WordState::NONE && !w.spanOpen)
    {
      // code review H-2: the point's step (the gap, or the hold) first, so the owed jobs meet its
      // verdict when TakeOwed re-queues them (never an ungated commit between segments)
      const int point = std::exchange(m_txOwedPoint, 0);
      CLog::Log(LOGINFO, "TRANSACTION noVideoMarkOwed taken at point ({})", point);
      TxRow3cPoint(point);
      OUTPUT::COutputWorker::Get().TakeOwed(w.f);
    }
  }
  const uint32_t wordF =
      OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
  // the last pick wins (code review H-4, r2 N-1): by the request id alone, before and after START
  const auto supersede = [this](uint64_t newer)
  {
    if (m_txStickyReqId && m_txStickyReqId != newer)
    {
      OUTPUT::COutputWorker::Get().DeferMarkFolded(m_txStickyReqId);
      CLog::Log(LOGINFO, "NOTICE user-pick id={} superseded by id={}", m_txStickyReqId, newer);
    }
    m_txStickyReqId = newer;
  };
  for (const DISPLAYSTATE::STxInboxEntry& e : DISPLAYSTATE::TakeTxInbox())
  {
    if (e.kind == DISPLAYSTATE::STxInboxEntry::Kind::MIDPLAY)
    {
      TxMidInbox(e);
      continue;
    }
    if (e.kind == DISPLAYSTATE::STxInboxEntry::Kind::VS10_REQUEST)
    {
      TxMidVs10Request(e, wordF);
      continue;
    }
    if (e.kind == DISPLAYSTATE::STxInboxEntry::Kind::MIDPLAY_PICK ||
        e.kind == DISPLAYSTATE::STxInboxEntry::Kind::MIDPLAY_PICK_CLEAR)
    {
      TxMidPick(e, wordF);
      continue;
    }
    if (e.kind == DISPLAYSTATE::STxInboxEntry::Kind::SETTING)
    {
      TxMidSetting(e, wordF);
      continue;
    }
    // code review r2 addendum: another file's entry (a stale one after a Next) is never this
    // file's target or trigger
    if (e.f && e.f != wordF)
    {
      CLog::Log(LOGINFO, "NOTICE request {} of F={} ignored (F={} open)", e.reqId, e.f, wordF);
      continue;
    }
    if (e.kind == DISPLAYSTATE::STxInboxEntry::Kind::VS10_RAN)
    {
      // r2 M-2: the replayed press has written the VS10 mode; the wire is re-decided now.
      // 5.G-6: reached on the path too (a segment gap's press, G-4 r1 B1): there the raise is
      // P's notice (the shut). Kept
      CLog::Log(LOGINFO, "TXSTART vs10 request {} ran: its trigger raised", e.reqId);
      TriggerVs10Resolution();
      continue;
    }
    // code review H-3: a pick in a carried window's gap (or a superseded start) is the next
    // decision's sticky target; only a closed window or a START replays it
    if (!m_txDeferF || m_txStarted)
    {
      // gate run 2: picks in the lead also supersede each other (one replay, the newest)
      if (m_txDeferF)
        supersede(e.reqId);
      CLog::Log(LOGINFO, "NOTICE user-pick id={} ({}): replayed at StartDone", e.reqId,
                e.res.strMode);
      continue;
    }
    supersede(e.reqId);
    m_txStickyTarget = e.res;
    // decided and not posted yet (WAIT_GUI): amended once posted
    m_txAmendPending =
        TxOpen() && (m_txStep == TxStep::POSTED || m_txStep == TxStep::WAIT_GUI);
    CLog::Log(LOGINFO, "NOTICE user-pick id={} ({}) for T={}{}", e.reqId, e.res.strMode,
              m_txWordT.load(), m_txAmendPending ? ": after the post" : "");
  }
  if (!m_txDeferF)
    return;
  if (m_txAmendPending && TxOpen() && m_txJob && m_txStep == TxStep::POSTED)
    TxPostAmend("user-pick");
  if (OUTPUT::COutputWorker::Get().DeferCarried(m_txDeferF))
  {
    if (m_txCarriedAt != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - m_txCarriedAt > std::chrono::seconds(12))
      TxStartDone(static_cast<int>(OUTPUT::COutputWorker::DeferOutcome::LEGACY),
                  "carry ended: no segment open in 12 s");
    return;
  }
  if (m_txStarted)
  {
    timespec mono{};
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const int64_t now = static_cast<int64_t>(mono.tv_sec) * 1000000000LL + mono.tv_nsec;
    if (now >= m_txLeadEndNs)
      TxStartDone(static_cast<int>(OUTPUT::COutputWorker::DeferOutcome::LANDED), "landed");
  }
}

// 5.E2-12a gate: transaction_requestat "<point> <kind> [arg] [x<n>]"
void CVideoPlayer::TxRequestAt(const char* point)
{
  if (!m_txRequestAtArmed)
    return;
  const std::vector<std::string> words =
      StringUtils::Split(aml_transaction_hooks().requestAt, ' ');
  if (words.empty() || words[0] != point)
    return;
  m_txRequestAtArmed = false;
  m_txRequestAtLeft = 1;
  for (const std::string& w : words)
    if (w.size() > 1 && w[0] == 'x')
      m_txRequestAtLeft = std::max(1, std::atoi(w.c_str() + 1));
  m_txRequestAtIndex = 0;
  TxRequestFire();
}

void CVideoPlayer::TxRequestFire()
{
  const std::vector<std::string> words =
      StringUtils::Split(aml_transaction_hooks().requestAt, ' ');
  if (words.size() < 2 || m_txRequestAtLeft <= 0)
    return;
  --m_txRequestAtLeft;
  m_txRequestAtNext = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  const std::string& kind = words[1];
  const std::string arg = words.size() > 2 && words[2][0] != 'x' ? words[2] : "";
  CLog::Log(LOGWARNING, "TXTEST requestat {} {} {}", words[0], kind, arg);
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (kind == "pick" && messenger)
  {
    const std::vector<std::string> modes = StringUtils::Split(arg, ',');
    if (modes.empty())
      return;
    const std::string& mode = modes[m_txRequestAtIndex++ % modes.size()];
    int w = 0, h = 0;
    float rate = 0.0f;
    if (std::sscanf(mode.c_str(), "%dx%d@%f", &w, &h, &rate) != 3)
      return;
    const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
    for (const auto& [index, info] : latest.res.infos)
      if (index >= RES_DESKTOP && info.iScreenWidth == w && info.iScreenHeight == h &&
          std::abs(info.fRefreshRate - rate) < 0.05f &&
          !(info.dwFlags & (D3DPRESENTFLAG_MODE3DSBS | D3DPRESENTFLAG_MODE3DTB | D3DPRESENTFLAG_MODE3DFP)))
      {
        messenger->PostMsg(TMSG_SETVIDEORESOLUTION, index, 0, nullptr, "user-pick");
        return;
      }
    CLog::Log(LOGWARNING, "TXTEST requestat pick {}: not in the table", mode);
  }
  else if (kind == "vs10" && messenger)
  {
    const int action = arg == "sdr"     ? ACTION_VS10_SDR
                       : arg == "hdr10" ? ACTION_VS10_HDR10
                       : arg == "dv"    ? ACTION_VS10_DV
                                        : ACTION_VS10_ORIGINAL;
    messenger->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                       static_cast<void*>(new CAction(action)));
  }
  else if (kind == "back" && messenger)
  {
    // r2 M-4: a real Back (GC leaves fullscreen), not a bare desktop set
    messenger->PostMsg(TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                       static_cast<void*>(new CAction(ACTION_NAV_BACK)));
  }
  else if (kind == "main")
  {
    std::string stereomode;
    m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode, "trigger");
  }
}

// 5.E2-12a D2: DECIDE again after the post; a different output is a new Start job of the same T
// that overtakes an uncommitted job, or follows a committed one inside the start's freeze
bool CVideoPlayer::TxPostAmend(const char* origin)
{
  m_txAmendPending = false;
  if (!TxOpen() || !m_txJob || m_txStep != TxStep::POSTED || !aml_transaction_amend())
    return false;
  // code review H-2 (zero tolerance): once CN's gate is open the sink may be open: no second
  // commit before START; the pick replays after START as one mid-play set
  const auto noAmend = [this, origin](const char* why)
  {
    CLog::Log(LOGINFO, "TXSTART T={} no amend ({}): {}", m_txJob->T, origin, why);
    m_txStickyTarget.reset();
    return false;
  };
  if (!m_startGateShut)
    return noAmend("CN's gate is open");
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  CRenderManager::SStartStereo stereo;
  stereo.stream = m_txJobFacts.stereoMode;
  stereo.target = m_txJob->stereo;
  CRenderManager::SStartResolve r = m_renderManager.ResolveStart(
      m_txEngagedGen, latest, m_txRootAtOpen, m_txFullScreen, nullptr, &stereo);
  if (r.exclusion)
    return noAmend(r.exclusion);
  const bool sticky = TxApplySticky(r, latest, false);
  const auto folded = [this, sticky]
  {
    if (!sticky)
      return;
    OUTPUT::COutputWorker::Get().DeferMarkFolded(m_txStickyReqId);
    CLog::Log(LOGINFO, "NOTICE folded user-pick id={} (amend)", m_txStickyReqId);
    m_txStickyTarget.reset();
  };
  const DISPLAYSTATE::SStartJob& old = *m_txJob;
  if (r.info.strId == old.res.strId && r.info.dwFlags == old.res.dwFlags &&
      std::abs(r.info.fRefreshRate - old.res.fRefreshRate) < 0.01f && r.hdrType == old.hdrType)
  {
    CLog::Log(LOGINFO, "TXSTART T={} amend: same decision ({})", old.T, origin);
    folded();
    return true;
  }
  if (m_txAmends >= 3)
  {
    CLog::Log(LOGERROR, "TXSTART T={} amend bound ({}): replayed at StartDone", old.T, origin);
    if (sticky)
      m_txStickyTarget.reset();
    return false;
  }
  auto amend = std::make_shared<DISPLAYSTATE::SStartJob>();
  amend->T = old.T;
  amend->gen = old.gen;
  amend->seq = m_txAmends + 1;
  amend->res = r.info;
  amend->resIndex = static_cast<int>(r.res);
  amend->stereo = r.stereo;
  if (amend->stereo == RenderStereoMode::HARDWAREBASED)
    amend->res.iHeight = r.info.iScreenHeight * 2 + r.info.iBlanking;
  amend->hdrType = r.hdrType;
  amend->fullScreen = m_txFullScreen;
  // the re-clock arm was taken by the start's DECIDE
  amend->fracRequest = old.fracRequest;
  amend->filmStart = r.filmStart;
  amend->tweakMs = r.tweakMs;
  if (const auto state = DISPLAYSTATE::Get())
  {
    amend->capsEpoch = state->capsEpoch;
    amend->sinkSig = state->sinkSig;
  }
  // code review H-2, H-5: a committed job (its commit on the wire, its adopt possibly answered)
  // is never followed by an amend; a job another set dropped keeps that set's exit (M-2)
  const DISPLAYSTATE::AmendOvertake overtake =
      DISPLAYSTATE::StartJobDropForAmend(*m_txJob, amend->seq);
  if (overtake == DISPLAYSTATE::AmendOvertake::COMMITTED)
    return noAmend("the job is committed");
  if (overtake == DISPLAYSTATE::AmendOvertake::DROPPED)
    return noAmend("the job was dropped by another set");
  if (!DISPLAYSTATE::ReplaceTxOpenJob(m_txJob, amend))
    return false;
  ++m_txAmends;
  // the old job's later replies go nowhere; those already queued are dropped by seq
  m_txJob->ClearSink();
  CLog::Log(LOGINFO, "TXSTART T={} amend {}/3 ({}): {} -> {}", old.T, m_txAmends, origin,
            old.res.strMode, r.info.strMode);
  m_txJob = amend;
  m_txSettle.reset();
  m_txAdopt.reset();
  m_txOutputDone = false;
  m_txJobFacts.fps = r.fps;
  m_txJobFacts.width = r.width;
  m_txJobFacts.height = r.height;
  m_txJobFacts.stereoMode = r.stereoMode;
  m_txJobFacts.hdrType = r.hdrType;
  amend->SetSink(m_txSink.get());
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!DISPLAYSTATE::StartJobQueue(*amend) || !winSystem || !winSystem->PostStartJob(amend))
  {
    EndTransactionToLegacy("no worker", CRenderManager::TxHow::RERAISE,
                           CRenderManager::TxNeed::JOB_OK);
    return true;
  }
  folded();
  return true;
}

bool CVideoPlayer::TxApplySticky(CRenderManager::SStartResolve& r,
                                 const DISPLAYSTATE::DecideInputs& latest,
                                 bool markFolded)
{
  if (!m_txStickyTarget)
    return false;
  const RESOLUTION_INFO& t = *m_txStickyTarget;
  for (const auto& [index, info] : latest.res.infos)
  {
    if (index < RES_DESKTOP || info.strId != t.strId || info.dwFlags != t.dwFlags ||
        std::abs(info.fRefreshRate - t.fRefreshRate) > 0.01f)
      continue;
    r.res = static_cast<RESOLUTION>(index);
    r.info = info;
    r.change = r.res != latest.res.videoResolution;
    if (markFolded)
    {
      OUTPUT::COutputWorker::Get().DeferMarkFolded(m_txStickyReqId);
      CLog::Log(LOGINFO, "NOTICE folded user-pick id={} ({})", m_txStickyReqId, info.strMode);
    }
    return true;
  }
  CLog::Log(LOGINFO, "NOTICE user-pick id={} ({}): not in the table, replayed at StartDone",
            m_txStickyReqId, t.strMode);
  m_txStickyTarget.reset();
  return false;
}

// 5.E2-8b D2: OPEN on an engaged start P decides. The job is created with the open's T and
// published (null -> job); main is shut out under RM's m_statelock
void CVideoPlayer::TxOpenStart()
{
  m_txOpenPending = false;
  // 5.E2-9c (r6 L1): before the publish-fail return
  TxHintClear();
  auto job = std::make_shared<DISPLAYSTATE::SStartJob>();
  job->T = m_txWordT.load();
  job->gen = m_txEngagedGen;
  // 5.E2-12a §1 (r2 L-1, r3 M-C): the window before the publish; a reopen of its file keeps the
  // key and re-keys only after a successful publish
  OUTPUT::COutputWorker::SDeferKey deferPrev;
  uint32_t deferF = 0;
  if (aml_transaction_fold())
  {
    deferF = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
    if (deferF)
      deferPrev = OUTPUT::COutputWorker::Get().DeferOpen(deferF, static_cast<uint32_t>(job->T));
    // code review r2 N-2: another start's window is open: this start defers nothing
    if (deferPrev.f && deferPrev.f != deferF)
      deferF = 0;
  }
  // r2 N-M4: another player's job is replaced only after its own exit or START released it
  if (!DISPLAYSTATE::PublishTxOpenJob(job))
  {
    // a reopen's window stays for the exit's StartDone; a first open's closes as legacy (code
    // review M-4: a pick W deferred in between replays), then the previous key is put back
    if (deferF && deferPrev.f != deferF)
    {
      OUTPUT::COutputWorker::Get().DeferClose(deferF, OUTPUT::COutputWorker::DeferOutcome::LEGACY);
      if (deferPrev.f)
        OUTPUT::COutputWorker::Get().DeferRestoreKey(deferPrev);
    }
    // E2-8b NOTE: D2 names no fallback; this start keeps E2-8a's (main decides), as skip decide
    CLog::Log(LOGWARNING, "TXSTART set={} T={} decision on main (another start job is open)",
              m_txEngagedGen, job->T);
    // 5.E2-10 D1 item 8: the superseded start's job shut main out
    if (m_txReopenInStart)
    {
      m_renderManager.TxOpenMain(m_txEngagedGen, CRenderManager::TxHow::OWED,
                                 CRenderManager::TxNeed::NONE);
      TxEndSuperseded("publish failed");
    }
    return;
  }
  if (deferF)
  {
    if (deferPrev.f == deferF)
      OUTPUT::COutputWorker::Get().DeferReKey(deferF, static_cast<uint32_t>(job->T));
    else
    {
      m_txStickyTarget.reset();
      m_txStickyReqId = 0;
    }
    m_txDeferF = deferF;
    m_txCarriedAt = {};
    m_txLeadEndNs = INT64_MAX;
  }
  m_txAmends = 0;
  m_txAmendPending = false;
  m_txRequestAtArmed = !aml_transaction_hooks().requestAt.empty();
  m_txRequestAtLeft = 0;
  if (!m_txSink)
    m_txSink = std::make_unique<CTxStartSink>(m_messenger, m_renderManager);
  m_txJob = std::move(job);
  m_txStep = TxStep::NONE;
  // sc L-5: this set's own pause and configure
  m_txAskPauseSeen = false;
  m_txConfiguredAt = {};
  m_txAskTarget.reset();
  m_txRootAtOpen = DISPLAYSTATE::FullScreenRoot();
  if (m_txReopenOpen)
  {
    // 5.E2-10 D1 item 6: the inputs can be minutes old: DECIDE waits for a forced publish
    m_txExitsAtOpen = DISPLAYSTATE::FullScreenExits();
    m_txReopenSeq = DISPLAYSTATE::GetDecideInputs().seq;
    m_txOpenInputsSeq = m_txReopenSeq + 1;
    m_txReopenOpenAt = std::chrono::steady_clock::now();
    DISPLAYSTATE::RequestDecideInputs();
  }
  else
  {
    // r5 L3: the open message's snapshot (OpenFile on main), not read from main here
    m_txExitsAtOpen = m_txMsgExits;
    m_txOpenInputsSeq = m_txMsgInputsSeq;
  }
  m_renderManager.TxShutMain(m_txEngagedGen, m_txJob->T);
  TxRequestAt("prime");
  CLog::Log(LOGINFO,
            "TXSTART set={} T={} OPEN: decision on P (inputs seq {}, fullscreen exits {}, root {})",
            m_txEngagedGen, m_txJob->T, m_txOpenInputsSeq, m_txExitsAtOpen,
            m_txRootAtOpen ? 1 : 0);
}

void CVideoPlayer::TxReleaseJob(const char* why)
{
  // 5.E2-11 D2.6 (code review B M-6): the film W's owed re-queue would restore ends here
  if (auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
    winSystem->PostOwedFilmEnded(why);
  if (!m_txJob)
    return;
  // D5 (lifetime): no reply reaches this player after it; never waits on what W holds (the
  // sink's calls only queue)
  m_txJob->ClearSink();
  if (TxClearOpenJob(why))
    CLog::Log(LOGINFO, "TXSTART T={} start job released ({})", m_txJob->T, why);
  m_txJob.reset();
}

bool CVideoPlayer::TxClearOpenJob(const char* why)
{
  if (!DISPLAYSTATE::ClearTxOpenJob(m_txJob))
    return false;
  // 5.E2-11 D2.6: a Restore still owed is re-queued through the deferral test: at once only when
  // no set of this player follows (stop, destroyed); else 1500 ms, so the next file's job or main's
  // decision pays it in one commit (an exit's legacy start is held until then)
  const std::string_view site = why;
  if (auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
    winSystem->PostOwedRestoreCheck(why, site == "stop" || site == "player destroyed"
                                             ? std::chrono::milliseconds(0)
                                             : std::chrono::milliseconds(1500));
  return true;
}

namespace
{
const char* TxStereoName(RenderStereoMode m)
{
  switch (m)
  {
    case RenderStereoMode::OFF:
      return "OFF";
    case RenderStereoMode::SPLIT_HORIZONTAL:
      return "SPLIT_HORIZONTAL";
    case RenderStereoMode::SPLIT_VERTICAL:
      return "SPLIT_VERTICAL";
    case RenderStereoMode::ANAGLYPH_RED_CYAN:
      return "ANAGLYPH_RED_CYAN";
    case RenderStereoMode::ANAGLYPH_GREEN_MAGENTA:
      return "ANAGLYPH_GREEN_MAGENTA";
    case RenderStereoMode::ANAGLYPH_YELLOW_BLUE:
      return "ANAGLYPH_YELLOW_BLUE";
    case RenderStereoMode::INTERLACED:
      return "INTERLACED";
    case RenderStereoMode::CHECKERBOARD:
      return "CHECKERBOARD";
    case RenderStereoMode::HARDWAREBASED:
      return "HARDWAREBASED";
    case RenderStereoMode::MONO:
      return "MONO";
    case RenderStereoMode::AUTO:
      return "AUTO";
    default:
      return "UNDEFINED";
  }
}

// RenderSystem.cpp:68-79, and HARDWAREBASED on a 3D display (WinSystemAmlogicGLESContext)
bool TxSupportsStereo(RenderStereoMode m, bool support3d)
{
  return m == RenderStereoMode::OFF || m == RenderStereoMode::SPLIT_HORIZONTAL ||
         m == RenderStereoMode::SPLIT_VERTICAL || m == RenderStereoMode::MONO ||
         (support3d && m == RenderStereoMode::HARDWAREBASED);
}
} // namespace

RenderStereoMode CVideoPlayer::TxStereoApply(RenderStereoMode m,
                                             RenderStereoMode gui,
                                             RenderStereoMode playing,
                                             bool support3d)
{
  const RenderStereoMode apply = m == RenderStereoMode::AUTO ? playing : m;
  if (apply != gui && apply >= RenderStereoMode::OFF && TxSupportsStereo(apply, support3d))
    return apply;
  return gui;
}

// r1 H-1: OnStreamChange's rules in its order, from inputs P can read (never a guess)
CVideoPlayer::STxStereo CVideoPlayer::TxStereoTarget(const std::string& stream,
                                                     const DISPLAYSTATE::DecideInputs& latest)
{
  STxStereo t;
  t.stereoscopic = !stream.empty() && stream != "mono";
  const auto& mirror = OUTPUT::CSettingsMirror::Get();
  const bool mirrored =
      mirror.IsMirrored(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE) &&
      mirror.IsMirrored(CSettings::SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE) &&
      mirror.IsMirrored(CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP);
  t.gui = mirrored ? static_cast<RenderStereoMode>(
                         mirror.GetInt(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE))
                   : latest.res.stereoMode;
  t.target = t.gui;
  const auto state = DISPLAYSTATE::Get();
  bool present = false;
  const bool support3d = state && state->support3d;
  RenderStereoMode playing = StereoVideoModeToGuiMode(
      stream, [support3d](RenderStereoMode m) { return TxSupportsStereo(m, support3d); },
      &present);
  if (playing == RenderStereoMode::UNDEFINED)
    playing = RenderStereoMode::OFF;
  // a 2D stream on a GUI in 2D: nothing SMg could change
  if (!t.stereoscopic && t.gui == RenderStereoMode::OFF)
  {
    t.rule = "2D stream";
    return t;
  }
  if (!mirrored || !state || (t.stereoscopic && !present))
  {
    t.kind = STxStereo::Kind::UNAVAILABLE;
    t.rule = !mirrored ? "settings not mirrored" : !state ? "no display state" : "stream mode";
    return t;
  }
  const int playback = mirror.GetInt(CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE);
  const bool quit = mirror.GetBool(CSettings::SETTING_VIDEOPLAYER_QUITSTEREOMODEONSTOP);
  const auto preferred = static_cast<RenderStereoMode>(
      mirror.GetInt(CSettings::SETTING_VIDEOSCREEN_PREFEREDSTEREOSCOPICMODE));
  if (playback == STEREOSCOPIC_PLAYBACK_MODE_IGNORE && t.gui == RenderStereoMode::OFF)
  {
    t.rule = "ignore";
    return t;
  }
  if (!support3d)
  {
    t.rule = "no 3D display";
    return t;
  }
  if (!t.stereoscopic)
  {
    t.rule = quit ? "2D stream, quit 3D" : "2D stream, 3D kept";
    if (quit)
      t.target = TxStereoApply(RenderStereoMode::OFF, t.gui, playing, support3d);
    return t;
  }
  if (latest.userStereo != RenderStereoMode::UNDEFINED)
  {
    t.rule = "user mode";
    t.target = TxStereoApply(latest.userStereo, t.gui, playing, support3d);
    return t;
  }
  if (t.gui != RenderStereoMode::OFF &&
      (!quit || t.gui == preferred || (preferred == RenderStereoMode::AUTO && t.gui == playing)))
  {
    t.rule = !quit ? "3D kept" : "already preferred";
    return t;
  }
  switch (playback)
  {
    case STEREOSCOPIC_PLAYBACK_MODE_ASK:
      t.kind = STxStereo::Kind::ASK;
      t.rule = "ask";
      m_txAskPlaying = playing;
      m_txAskSupport3d = support3d;
      break;
    case STEREOSCOPIC_PLAYBACK_MODE_PREFERRED:
      t.rule = "preferred";
      t.target = TxStereoApply(preferred, t.gui, playing, support3d);
      break;
    case STEREOSCOPIC_PLAYBACK_MODE_MONO:
      t.rule = "mono";
      t.target = TxStereoApply(RenderStereoMode::MONO, t.gui, playing, support3d);
      break;
    default:
      t.rule = "ignore";
      break;
  }
  return t;
}

// 5.E2-10 9d (r1 M-5): the dialog is SMg's pause/unpause pair; its answer is the user mode main
// publishes after the unpause (SMg sets it first)
void CVideoPlayer::TxAskPass()
{
  const auto now = std::chrono::steady_clock::now();
  const auto& mirror = OUTPUT::CSettingsMirror::Get();
  const auto gui = static_cast<RenderStereoMode>(
      mirror.GetInt(CSettings::SETTING_VIDEOSCREEN_STEREOSCOPICMODE));
  if (!m_txAskOpen)
  {
    if (!m_txAskPauseSeen)
    {
      // a hint DECIDE has no configure: counted from the step
      const auto from =
          m_txConfiguredAt != std::chrono::steady_clock::time_point{} ? m_txConfiguredAt
                                                                       : m_txAskSince;
      if (now - from < 2s)
        return;
      // sc M-8: SMg did nothing; a dialog that opens later goes through main's trigger
      CLog::Log(LOGERROR, "TXSTART set={} T={} 3D question did not open in 2 s: GUI stereo mode {}",
                m_txEngagedGen, m_txJob->T, TxStereoName(gui));
      m_txAskTarget = gui;
      m_txStep = TxStep::NONE;
      TxDecide();
      return;
    }
    m_txAskOpen = true;
  }
  if (m_txPendingSpeed == DVD_PLAYSPEED_PAUSE)
    return;
  if (!m_txAskInputsSeq)
  {
    m_txAskInputsSeq = DISPLAYSTATE::GetDecideInputs().seq + 1;
    m_txAskClosedAt = now;
    DISPLAYSTATE::RequestDecideInputs();
  }
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  if (latest.seq < m_txAskInputsSeq)
  {
    if (now - m_txAskClosedAt < 1s)
      return;
    CLog::Log(LOGINFO, "TXSTART set={} inputs seq {} after the 3D question, {} published",
              m_txEngagedGen, m_txAskInputsSeq, latest.seq);
    EndTransactionToLegacy("inputs stale", CRenderManager::TxHow::RERAISE,
                           CRenderManager::TxNeed::JOB_OK);
    return;
  }
  if (latest.userStereo != RenderStereoMode::UNDEFINED)
  {
    m_txAskTarget = TxStereoApply(latest.userStereo, gui, m_txAskPlaying, m_txAskSupport3d);
    CLog::Log(LOGINFO, "TXSTART set={} T={} 3D question answered: user {}, target {}",
              m_txEngagedGen, m_txJob->T, TxStereoName(latest.userStereo),
              TxStereoName(*m_txAskTarget));
  }
  else
  {
    // Q2
    m_txAskTarget = gui;
    CLog::Log(LOGINFO, "TXSTART set={} T={} 3D question cancelled: GUI stereo mode {}",
              m_txEngagedGen, m_txJob->T, TxStereoName(gui));
  }
  m_txStep = TxStep::NONE;
  TxDecide();
}

// 5.E2-11 D4: the 3D dialog open for W's deferral test (a Restore then commits the GUI's mode so
// the dialog is seen). Any pause in ASK counts, read at the intake and each pass (TxAskPass does
// not run in a span): erring to "open" costs a re-lock, never a dark TV
void CVideoPlayer::TxAskOpenUpdate()
{
  if (!m_txJob)
    return;
  const bool open =
      TxOpen() && m_txStep == TxStep::ASK && UserSpeed() == DVD_PLAYSPEED_PAUSE;
  if (m_txJob->askOpen.exchange(open) == open)
    return;
  CLog::Log(LOGINFO, "TXSTART T={} 3D question {} (a Restore is not deferred while open)",
            m_txJob->T, open ? "open" : "closed");
  // a Restore deferred before the dialog opened is committed now (the GUI's mode)
  if (open)
    if (auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
      winSystem->PostOwedRestoreCheck("3D question", std::chrono::milliseconds(0));
}

// 5.E2-8b D4: DECIDE at the set's first configure, steps 1-7
void CVideoPlayer::TxDecide()
{
  using How = CRenderManager::TxHow;
  using Need = CRenderManager::TxNeed;
  // transaction_decidehold (5.E2-11, test only): DECIDE waits on P's loop, W free
  if (const int ms = aml_transaction_hooks().decideHoldMs; ms > 0)
  {
    const auto now = std::chrono::steady_clock::now();
    if (m_txDecideHoldUntil == std::chrono::steady_clock::time_point{})
    {
      m_txDecideHoldUntil = now + std::chrono::milliseconds(ms);
      CLog::Log(LOGWARNING, "TXTEST decidehold {} T={}", ms, m_txJob->T);
    }
    if (now < m_txDecideHoldUntil)
      return;
  }
  // 5.E2-9c D3: the PRIME bound's latch (no picture; TxStartGatePass clears it for one)
  const bool hint = m_txHintDecide;
  // as main's UpdateResolution (a3f8a5614c): a window the fullscreen switch closes may set its
  // own mode on the way out; a Back ends the wait for step 1
  if (!DISPLAYSTATE::FullScreenVideoWindowActive() &&
      DISPLAYSTATE::FullScreenExits() == m_txExitsAtOpen)
  {
    // a hint decision's render timeout counts from the bound (no configure)
    if (!m_renderManager.TxRenderTimedOut(
            hint ? m_txHintBoundAt : std::chrono::steady_clock::time_point{}, m_txSpanClosedAt))
    {
      if (!std::exchange(m_txWindowWaitLogged, true))
        CLog::Log(LOGINFO, "TXSTART set={} T={} DECIDE waits for the fullscreen window",
                  m_txEngagedGen, m_txJob->T);
      return;
    }
    CLog::Log(LOGWARNING,
              "TXSTART set={} T={} DECIDE at the render timeout (no fullscreen window)",
              m_txEngagedGen, m_txJob->T);
  }
  else if (m_txWindowWaitLogged)
    CLog::Log(LOGINFO, "TXSTART set={} T={} DECIDE after the fullscreen window wait",
              m_txEngagedGen, m_txJob->T);
  // step 1: a fullscreen exit since OPEN (r4 M2: main does nothing while windowed, RM:3038)
  if (DISPLAYSTATE::FullScreenExits() != m_txExitsAtOpen)
  {
    EndTransactionToLegacy("fullscreen exit", How::RERAISE, Need::LEFT);
    return;
  }
  // D2 (r4 M1): main's open publishes the inputs forced; an open not made through OpenFile
  // carries none
  // 5.E2-10 D1 item 6: a reopen reads its inputs untaken until the forced publish (1 s bound)
  if (m_txReopenOpen && DISPLAYSTATE::GetDecideInputs().seq < m_txOpenInputsSeq &&
      std::chrono::steady_clock::now() - m_txReopenOpenAt < 1s)
  {
    if (!std::exchange(m_txReopenWaitLogged, true))
      CLog::Log(LOGINFO, "TXSTART set={} T={} reopen inputs: waiting for a publish after seq {}",
                m_txEngagedGen, m_txJob->T, m_txReopenSeq);
    return;
  }
  // 5.E2-10 9d: read untaken (the pump keeps running) until DECIDE goes past the 3D question
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs();
  if (!m_txOpenInputsSeq || latest.seq < m_txOpenInputsSeq)
  {
    CLog::Log(LOGINFO, "TXSTART set={} inputs seq {} at the open, {} published", m_txEngagedGen,
              m_txOpenInputsSeq, latest.seq);
    EndTransactionToLegacy("inputs stale", How::RERAISE, Need::JOB_OK);
    return;
  }
  // step 2 (ResolveStart logs main's "Display resolution ADJUST" lines itself)
  CRenderManager::SStartFacts facts;
  if (hint)
    facts = TxDecideFromHints();
  // 5.E2-10 9d: the stereo mode the GUI is about to take, decided up front
  std::optional<CRenderManager::SStartStereo> stereo;
  if (!aml_transaction_skip("ask"))
  {
    stereo.emplace();
    stereo->stream = hint ? facts.stereoMode : m_renderManager.TxPictureStereoMode();
    if (m_txAskTarget)
      stereo->target = *m_txAskTarget;
    else
    {
      const STxStereo t = TxStereoTarget(stereo->stream, latest);
      if (t.kind == STxStereo::Kind::UNAVAILABLE)
      {
        CLog::Log(LOGINFO, "TXSTART set={} T={} 3D inputs unavailable ({})", m_txEngagedGen,
                  m_txJob->T, t.rule);
        EndTransactionToLegacy("3D inputs unavailable", How::RERAISE, Need::JOB_OK);
        return;
      }
      if (t.kind == STxStereo::Kind::ASK)
      {
        m_txStep = TxStep::ASK;
        m_txAskStream = stereo->stream;
        m_txAskOpen = m_txAskBoundLogged = false;
        m_txAskInputsSeq = 0;
        m_txAskSince = std::chrono::steady_clock::now();
        CLog::Log(LOGINFO, "TXSTART set={} T={} DECIDE waits for the 3D question (stream '{}')",
                  m_txEngagedGen, m_txJob->T, stereo->stream);
        return;
      }
      if (t.stereoscopic || t.gui != RenderStereoMode::OFF)
        CLog::Log(LOGINFO, "TXSTART set={} T={} 3D: no question ({}): target {} (GUI {})",
                  m_txEngagedGen, m_txJob->T, t.rule, TxStereoName(t.target),
                  TxStereoName(t.gui));
      stereo->target = t.target;
    }
  }
  DISPLAYSTATE::GetDecideInputs(true);
  CRenderManager::SStartResolve r =
      m_renderManager.ResolveStart(m_txEngagedGen, latest, m_txRootAtOpen, m_txFullScreen,
                                   hint ? &facts : nullptr, stereo ? &*stereo : nullptr);
  if (hint)
  {
    // the fps fact of an fps-invalid stream is the chosen mode's rate (r4 M3)
    if (facts.fpsInvalid)
      facts.fps = r.info.fRefreshRate;
    facts.frac = r.frac;
    CLog::Log(LOGERROR,
              "TXSTART set={} T={} no-picture DECIDE (5 s bound) facts fps {:.3f} {}x{} hdr {} "
              "frac {} fpsInvalid {} interlaceHint {}",
              m_txEngagedGen, m_txJob->T, facts.fps, facts.width, facts.height,
              static_cast<int>(facts.hdrType), facts.frac ? 1 : 0, facts.fpsInvalid ? 1 : 0,
              facts.interlaceHint ? 1 : 0);
  }
  // step 3: D1's exclusions
  if (r.exclusion)
  {
    EndTransactionToLegacy(r.exclusion, How::RERAISE, Need::JOB_OK);
    return;
  }
  TxApplySticky(r, latest);
  // step 4: the arm consumed (today's "consumed ... may re-clock" line), nothing counted
  m_renderManager.TxTakeArm(m_txEngagedGen, r, hint);
  // step 5 (r2 N-M2): disc time held from DECIDE, the 13 s bounds armed
  m_txDiscHeld = true;
  m_discTime.SetTxHeld(true);
  m_renderManager.ArmTxStartHeldBound();
  // the request, by value (D5)
  DISPLAYSTATE::SStartJob& job = *m_txJob;
  job.res = r.info;
  job.resIndex = static_cast<int>(r.res);
  job.stereo = r.stereo;
  // sc M-7: a frame packing mode's full height, as GC's set and W's restore job give it
  if (job.stereo == RenderStereoMode::HARDWAREBASED)
    job.res.iHeight = r.info.iScreenHeight * 2 + r.info.iBlanking;
  job.hdrType = r.hdrType;
  job.fullScreen = m_txFullScreen;
  job.fracRequest = r.frac;
  job.filmStart = r.filmStart;
  job.tweakMs = r.tweakMs;
  // 5.E2-11 D2.5: the caps this decision read, for W's check at the job's run (one snapshot)
  if (const auto state = DISPLAYSTATE::Get())
  {
    job.capsEpoch = state->capsEpoch;
    job.sinkSig = state->sinkSig;
  }
  // D3 (code review A-L1): the notices after the post compare with the facts DECIDE read (a
  // notice raised between the last take and ResolveStart's read is in them)
  m_txJobFacts = {};
  m_txJobFacts.kind = "decide";
  m_txJobFacts.fps = hint ? facts.fps : r.fps;
  m_txJobFacts.width = r.width;
  m_txJobFacts.height = r.height;
  m_txJobFacts.stereoMode = r.stereoMode;
  m_txJobFacts.hdrType = r.hdrType;
  m_txJobFactsSet = true;
  // 5.E2-9c D3: the same facts for the picture's compares
  if (hint)
    m_txHintFacts = facts;
  // step 6
  CLog::Log(LOGINFO, "DECIDE start set={} T={} {} frac={} hdr={} tweak={:.1f} (P){}",
            m_txEngagedGen, job.T, r.info.strMode, r.frac ? 1 : 0, static_cast<int>(r.hdrType),
            r.tweakMs, r.refreshOff ? " (refresh off)" : "");
  // step 7
  TxTryPost();
}

// 5.E2-8b D4 step 7 (r4 H1, r5 M1): P posts only while no GUI set is in flight. GC counts its set
// in, then CASes QUEUED|PARKING -> DROPPED; P CASes NOT_POSTED -> QUEUED, then re-reads the count
void CVideoPlayer::TxTryPost()
{
  using How = CRenderManager::TxHow;
  using Need = CRenderManager::TxNeed;
  DISPLAYSTATE::SStartJob& job = *m_txJob;
  // transaction_posthold (5.E2-11, test only): decided and stamped, the post held (WAIT_GUI,
  // NOT_POSTED: no OUTPUT bound runs), W free
  if (const int ms = aml_transaction_hooks().postHoldMs; ms > 0)
  {
    const auto now = std::chrono::steady_clock::now();
    if (m_txPostHoldUntil == std::chrono::steady_clock::time_point{})
    {
      m_txPostHoldUntil = now + std::chrono::milliseconds(ms);
      CLog::Log(LOGWARNING, "TXTEST posthold {} T={}", ms, job.T);
    }
    if (now < m_txPostHoldUntil)
    {
      m_txStep = TxStep::WAIT_GUI;
      return;
    }
  }
  if (const int n = DISPLAYSTATE::GuiSetsInFlight())
  {
    m_txStep = TxStep::WAIT_GUI;
    if (!std::exchange(m_txGuiWaitLogged, true))
      CLog::Log(LOGINFO, "DECIDE start T={} waits: {} GUI set(s) in flight", job.T, n);
    return;
  }
  // r6 L3: the guiSet notice counter before the CAS
  m_txGuiSetSnap = DISPLAYSTATE::ReadGuiSetNotice(0).seq;
  if (!DISPLAYSTATE::StartJobQueue(job))
  {
    // GC drops only QUEUED|PARKING: a QUEUED left by an earlier step was dropped
    EndTransactionToLegacy("gui set", How::RERAISE, Need::FRESH);
    return;
  }
  if (DISPLAYSTATE::GuiSetsInFlight())
  {
    if (!DISPLAYSTATE::StartJobUnqueue(job))
    {
      // GC won the CAS: no post, D8's `gui set` row (not committed)
      EndTransactionToLegacy("gui set", How::RERAISE, Need::FRESH);
      return;
    }
    m_txStep = TxStep::WAIT_GUI;
    return;
  }
  // r12 L4: a Back during the wait is step 1's exit
  if (DISPLAYSTATE::FullScreenExits() != m_txExitsAtOpen)
  {
    EndTransactionToLegacy("fullscreen exit", How::RERAISE, Need::LEFT);
    return;
  }
  if (m_txGuiWaitLogged)
    CLog::Log(LOGINFO, "DECIDE start T={} posts after the GUI set(s)", job.T);
  // registered at the post (D5 lifetime); PostIfRunning: never run inline on P (r3 L2)
  job.SetSink(m_txSink.get());
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem || !winSystem->PostStartJob(m_txJob))
  {
    EndTransactionToLegacy("no worker", How::RERAISE, Need::JOB_OK);
    return;
  }
  m_txStep = TxStep::POSTED;
  if (m_txDeferF)
    OUTPUT::COutputWorker::Get().DeferPosted(m_txDeferF);
  TxRequestAt("posted");
  m_txPostedAt = std::chrono::steady_clock::now();
  m_txBoundInCommitLogged = false;
  m_txLostAt = {};
  m_txLostPaused = {};
  m_txSpanPaused = {};
  m_txSpanPausedFrom = {};
  // 5.E2-9c D3: RM owes the hint job's first picture its compare; counted when posted (r5 L4)
  if (std::exchange(m_txHintDecide, false))
  {
    m_txHintJob = true;
    ++m_txNoPictureDecides;
    CRenderManager::STxNotice picture;
    const CRenderManager::TxHintResult at =
        m_renderManager.TxHintPosted(m_txEngagedGen, m_txHintFacts, picture);
    if (at != CRenderManager::TxHintResult::NONE)
    {
      const bool same = at == CRenderManager::TxHintResult::SAME;
      CLog::Log(LOGINFO, "TXSTART T={} no-picture DECIDE: picture arrived at the post ({})",
                job.T, same ? "same" : "different");
      if (!same)
        TxHintContradicted(picture, "picture arrived at the post");
      else
      {
        // its notices were folded before the post: no later notice is its first (code review L6)
        m_txHintChecked = m_txHintNoticeSeen = true;
      }
    }
  }
}

CRenderManager::SStartFacts CVideoPlayer::TxDecideFromHints()
{
  const CDVDStreamInfo& hint = m_CurrentVideo.hint;
  CRenderManager::SStartFacts f;
  f.fpsInvalid = !(hint.fpsrate && hint.fpsscale);
  if (!f.fpsInvalid)
  {
    // r3 M8: VPV's OpenStream rate, not doubled for interlace (its first Configure's)
    f.fps = static_cast<float>(
        DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(static_cast<double>(DVD_TIME_BASE) *
                                                               hint.fpsscale / hint.fpsrate));
    // VPV's OpenStream forces 25 outside its range (code review L2)
    if (f.fps > 120.0f || f.fps < 5.0f)
      f.fps = 25.0f;
  }
  else
  {
    // as VPV's PrepareOutput for an fps-invalid stream: the GUI's rate, or W's committed one
    f.fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
    DISPLAYSTATE::ModeTiming committed;
    if (aml_displaystate_on() && DISPLAYSTATE::CommitTimingOn() &&
        DISPLAYSTATE::CommittedTiming(committed, "fps@hint-decide"))
      f.fps = committed.fps;
  }
  f.width = hint.width;
  f.height = hint.height;
  // VPV's stereo mode for a picture the decoder leaves unset: the user's forced mode, else the
  // hint's (code review L3)
  const CVideoSettings settings = m_processInfo->GetVideoSettings();
  std::string stereoMode;
  switch (static_cast<RenderStereoMode>(settings.m_StereoMode))
  {
    case RenderStereoMode::SPLIT_VERTICAL:
      stereoMode = settings.m_StereoInvert ? "right_left" : "left_right";
      break;
    case RenderStereoMode::SPLIT_HORIZONTAL:
      stereoMode = settings.m_StereoInvert ? "bottom_top" : "top_bottom";
      break;
    case RenderStereoMode::HARDWAREBASED:
      stereoMode = settings.m_StereoInvert ? "block_rl" : "block_lr";
      break;
    default:
      stereoMode = hint.stereo_mode;
      break;
  }
  if (stereoMode != "mono")
    f.stereoMode = stereoMode;
  // the type after the open's VS10 rewrite. An HDR10/HDR10+ hint left native is DVCA's
  // candidate: its first keyframe re-fakes DV when VS10 or HDR2DV maps it (DVCA:1146-1147)
  f.hdrType = hint.hdrType;
  if (hint.hdrType == StreamHdrType::HDR_TYPE_HDR10 ||
      hint.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS)
  {
    const bool dv = aml_vs10_by_hdrtype(hint.hdrType, hint.bitdepth) !=
                        DOLBY_VISION_OUTPUT_MODE_BYPASS ||
                    aml_convert_to_dv_by_vs_engine(hint.hdrType);
    if (dv)
      f.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
    CLog::Log(LOGINFO, "TXSTART set={} hdr from hint (HDR10+ candidate: {})", m_txEngagedGen,
              dv ? "DV" : "HDR10");
  }
  f.frac = hint.reclockEligible && !f.fpsInvalid;
  f.interlaceHint = hint.interlaced;
  return f;
}

void CVideoPlayer::TxHintContradicted(const CRenderManager::STxNotice& picture, const char* where)
{
  const CRenderManager::SStartFacts& h = m_txHintFacts;
  CLog::Log(LOGERROR,
            "TXSTART set={} T={} no-picture decision contradicted (exit) hint {{fps {:.3f} {}x{} "
            "hdr {} stereo '{}'}} picture {{fps {:.3f} {}x{} hdr {} stereo '{}'}} ({})",
            m_txEngagedGen, m_txJob->T, h.fps, h.width, h.height, static_cast<int>(h.hdrType),
            h.stereoMode, picture.fps, picture.width, picture.height,
            static_cast<int>(picture.hdrType), picture.stereoMode, where);
  ++m_txNoPictureContradicted;
  // the 12 s cap is re-armed for main's set (a no-op once CN is open)
  m_startGateSince = std::chrono::steady_clock::now();
  m_startGateStarveSince = {};
  // 5.E2-12a D2.7 (E29C-1): the picture's decision amends the hint job inside the start
  // code review M-1: an amend not posted (gate open, committed, exclusion) exits as today
  if (!(aml_transaction_amend() && m_txStep == TxStep::POSTED && m_txAmends < 3 &&
        m_startGateShut && (TxHintClear(), TxPostAmend("picture"))))
    EndTransactionToLegacy("input changed", CRenderManager::TxHow::RERAISE,
                           CRenderManager::TxNeed::FRESH);
  // code review H1: RM configured the picture before P saw it start; its PLAYER_STARTED follows,
  // and no start on audio may come before it (the first-picture wait bounds it to 1 s again)
  if (m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_WAITSYNC)
  {
    m_firstPictureWaitSince = {};
    m_firstPictureWaitExpired = false;
  }
}

// 5.E2-9c D6: the gate's hooks, read at the engage
void CVideoPlayer::TxTestHooksAtEngage()
{
  const STransactionHooks hooks = aml_transaction_hooks();
  if (!hooks.noPicture.empty())
  {
    const std::string& word = hooks.noPicture;
    constexpr std::string_view settle = "settle+";
    if (word.compare(0, settle.size(), settle) == 0)
    {
      m_txNoPictureSettleMs = std::max(0, std::atoi(word.c_str() + settle.size()));
      m_VideoPlayerVideo->SetTxNoPicture(INT64_MAX);
    }
    else
    {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(std::max(0, std::atoi(word.c_str())));
      m_VideoPlayerVideo->SetTxNoPicture(
          std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
    }
    CLog::Log(LOGWARNING, "TXTEST nopicture {} set={}", word, m_txEngagedGen);
  }
  if (hooks.audioTimeCapS > 0.0 && m_txAudioTimeCapSaved == 0.0)
  {
    int data = 0;
    int maxData = 0;
    double time = 0.0;
    double cap = 0.0;
    bool timeFull = false;
    m_VideoPlayerAudio->GetQueueFill(data, maxData, time, cap, timeFull);
    if (cap > 0.0)
    {
      m_txAudioTimeCapSaved = cap;
      m_VideoPlayerAudio->SetMaxTimeSize(hooks.audioTimeCapS);
      CLog::Log(LOGWARNING, "TXTEST audiotimecap {:.3f} s set={} (was {:.1f} s)",
                hooks.audioTimeCapS, m_txEngagedGen, cap);
    }
  }
}

void CVideoPlayer::TxRestoreAudioTimeCap(const char* why)
{
  if (m_txAudioTimeCapSaved == 0.0)
    return;
  const double saved = std::exchange(m_txAudioTimeCapSaved, 0.0);
  // a cap set since (the menu-domain depth) is not overridden (code review L7)
  int data = 0;
  int maxData = 0;
  double time = 0.0;
  double cap = 0.0;
  bool timeFull = false;
  m_VideoPlayerAudio->GetQueueFill(data, maxData, time, cap, timeFull);
  if (std::abs(cap - aml_transaction_hooks().audioTimeCapS) > 0.001)
  {
    CLog::Log(LOGWARNING, "TXTEST audiotimecap not restored ({}): the cap is {:.1f} s now", why,
              cap);
    return;
  }
  m_VideoPlayerAudio->SetMaxTimeSize(saved);
  CLog::Log(LOGWARNING, "TXTEST audiotimecap restored to {:.1f} s ({})", saved, why);
}

void CVideoPlayer::TxHintClear()
{
  m_txHintDecide = false;
  m_txHintJob = false;
  m_txHintChecked = m_txHintNoticeSeen = m_txHint2xLogged = false;
  m_txSettleSince = {};
  m_txSettleLostSeen = m_txSettleAudioLogged = false;
}

// 5.E2-9c D5 (r3 M3): a hint-decided start with no picture by settle done starts on audio from
// +5 s, and ends to legacy at +12 s with no START
bool CVideoPlayer::TxSettleTimerPass()
{
  if (!TxOpen() || !m_txJob || !m_txHintJob ||
      m_txSettleSince == std::chrono::steady_clock::time_point{})
    return false;
  // 5.E2-11: re-armed once after a row-9 span, not on each of its passes
  if (m_txInSpan)
    return false;
  const auto now = std::chrono::steady_clock::now();
  // a loss's time never counts (the lost branch ends its passes before this one)
  if (std::exchange(m_txSettleLostSeen, false))
  {
    m_txSettleSince = now;
    CLog::Log(LOGINFO, "TXSTART set={} settle timer re-armed after the display span",
              m_txEngagedGen);
    return false;
  }
  if (now - m_txSettleSince >= 12s)
  {
    CLog::Log(LOGERROR, "TXSTART set={} T={} no start in 12 s after settle: exit", m_txEngagedGen,
              m_txJob->T);
    ++m_txSettleExits;
    EndTransactionToLegacy("no start in 12 s after settle", CRenderManager::TxHow::RERAISE,
                           CRenderManager::TxNeed::JOB_OK);
    return false;
  }
  if (now - m_txSettleSince < 5s || m_CurrentVideo.id < 0 ||
      m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC ||
      m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_WAITSYNC)
    return false;
  if (!std::exchange(m_txSettleAudioLogged, true))
    CLog::Log(LOGINFO, "TXSTART set={} no picture by settle done + 5 s: START on audio",
              m_txEngagedGen);
  return true;
}

// 5.E2-9c D4 (r3 M1, r6 L5): keyed on RM's configured record, not its hint flag (never set when
// the picture came at the post); RM's live facts, as a same-rate picture raises no notice
void CVideoPlayer::TxHintPictureCheck(const char* where)
{
  if (!TxOpen() || !m_txJob || !m_txHintJob || m_txHintChecked)
    return;
  CRenderManager::STxNotice picture;
  if (!m_renderManager.TxHintConfigured(m_txEngagedGen, picture))
    return;
  m_txHintChecked = true;
  bool fps2x = false;
  if (CRenderManager::TxHintFactsSame(m_txHintFacts, picture.fps, picture.width, picture.height,
                                      picture.stereoMode, picture.hdrType, true, &fps2x))
  {
    if (fps2x && !std::exchange(m_txHint2xLogged, true))
      CLog::Log(LOGINFO, "TXSTART set={} fps 2x accepted (interlace flag carried over, E29A-6)",
                m_txEngagedGen);
    CLog::Log(LOGINFO, "TXSTART set={} T={} the picture is the hint decision's ({})",
              m_txEngagedGen, m_txJob->T, where);
    return;
  }
  TxHintContradicted(picture, where);
}

void CVideoPlayer::TxNoPictureSummary(const char* why)
{
  const unsigned int late = m_renderManager.TxLatePictureContradicted();
  const unsigned int lateNew = late - std::exchange(m_txLateContradictedSeen, late);
  if (m_txNoPictureDecides || m_txNoPictureContradicted || m_txSettleExits || lateNew)
    CLog::Log(LOGINFO,
              "TXSTART no-picture summary ({}): DECIDEs {}, contradicted before START {}, after "
              "START {}, no start in 12 s after settle {}",
              why, m_txNoPictureDecides, m_txNoPictureContradicted, lateNew, m_txSettleExits);
  // per file (code review L5)
  m_txNoPictureDecides = m_txNoPictureContradicted = m_txSettleExits = 0;
}

void CVideoPlayer::TxOnReply(CDVDMsg& msg)
{
  const STxReply r = static_cast<CDVDMsgType<STxReply>&>(msg).m_value;
  const char* what = r.kind == STxReply::Kind::OUTPUT_DONE   ? "output done"
                     : r.kind == STxReply::Kind::SETTLE_DONE ? "settle done"
                     : r.kind == STxReply::Kind::RESTORED    ? "restored"
                                                             : "adopted";
  // D5 (lifetime): a reply for an ended start is logged only
  if (!m_txJob || r.T != m_txJob->T || !TxOpen() || m_txStep != TxStep::POSTED)
  {
    CLog::Log(LOGINFO, "TXSTART T={} {} after the start ended: logged", r.T, what);
    // 5.E2-12b D2.2: the carried committed job's SettleDone ends row 3b's wait for it
    if (m_txNoVideoCarry && r.T == m_txNoVideoCarry->T && r.kind == STxReply::Kind::SETTLE_DONE)
      m_txNoVideoCarry->settled = true;
    // code review A-M1: a job GC dropped after a `left` exit took it past its CAS answers
    // nobody; its SettleDone{dropped} resolves the outcome as `failed` (logged if resolved)
    if (r.kind == STxReply::Kind::SETTLE_DONE &&
        r.settle == DISPLAYSTATE::StartSettleKind::DROPPED && m_txJob && r.T == m_txJob->T &&
        std::exchange(m_txEndedLeft, false) && m_txEndedGen && m_txEndedGen == m_txJob->gen)
      m_renderManager.TxAdoptAnswer(m_txEndedGen, CRenderManager::TxAnswer::FAILED);
    return;
  }
  // 5.E2-12a D2.1 (r2 M-5): an overtaken or superseded job's reply, queued before its sink was
  // cleared; RESTORED is the word's (START's e), never filtered
  if (r.kind != STxReply::Kind::RESTORED && r.jobSeq != m_txJob->seq)
  {
    CLog::Log(LOGINFO, "TXSTART T={} reply {} of job {} dropped (job {} is current)", r.T, what,
              r.jobSeq, m_txJob->seq);
    return;
  }
  switch (r.kind)
  {
    case STxReply::Kind::OUTPUT_DONE:
      // D5 step 4: C's pre-roll term for the shut gen (committed or no-change: the output is final)
      m_txOutputDone = true;
      m_renderManager.SetTxOutputDone(m_txEngagedGen);
      if (r.committed)
        TxRequestAt("settle");
      break;
    case STxReply::Kind::SETTLE_DONE:
      m_txSettle = r.settle;
      break;
    case STxReply::Kind::ADOPTED:
      m_txAdopt = r.form;
      break;
    case STxReply::Kind::RESTORED:
      // 5.E2-11 D3.2: START's CAS expects the Restore's settled e
      m_txStartE = r.e;
      m_txRestoredE = static_cast<int64_t>(r.e);
      m_txRestoreMissingSince = {};
      break;
  }
}

bool CVideoPlayer::TxNoticeSame(const CRenderManager::STxNotice& notice, bool firstConfigure) const
{
  // code review A-M2: a live VS10 action changes W's output mode, an input of the job's set
  if (std::string_view(notice.kind) == "vs10")
    return false;
  // a parameterless trigger (GC's, a settings change): root, fullscreen, stereo
  if (std::string_view(notice.kind) == "trigger")
    return DISPLAYSTATE::FullScreenRoot() == m_txRootAtOpen &&
           CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo() == m_txFullScreen &&
           notice.stereoMode == m_txJobFacts.stereoMode;
  // 5.E2-9c D4 (r5 L1): a hint job's notices with RM's rules (f/2f, fpsInvalid)
  if (m_txHintJob)
  {
    bool fps2x = false;
    const bool same = CRenderManager::TxHintFactsSame(m_txHintFacts, notice.fps, notice.width,
                                                      notice.height, notice.stereoMode,
                                                      notice.hdrType, firstConfigure, &fps2x);
    if (same && fps2x && !std::exchange(m_txHint2xLogged, true))
      CLog::Log(LOGINFO, "TXSTART set={} fps 2x accepted (interlace flag carried over, E29A-6)",
                m_txEngagedGen);
    return same;
  }
  // E2-8b NOTE: "fps class" read as the same rate within 0.05 % (23.976 = 23.98, not 24)
  const float a = notice.fps;
  const float b = m_txJobFacts.fps;
  const bool sameFps = std::abs(a - b) <= 0.0005f * std::max(a, b);
  const bool sameSize = (!m_txJobFacts.width && !m_txJobFacts.height) ||
                        (notice.width == m_txJobFacts.width && notice.height == m_txJobFacts.height);
  return sameFps && sameSize && notice.hdrType == m_txJobFacts.hdrType &&
         notice.stereoMode == m_txJobFacts.stereoMode;
}

// test (transaction_noticeat): prime = the start's first pass (before the post); posted = the
// first pass after the post with the job past its drop CAS (W's park ack: with transaction_stopat
// parked, inside its hold); settle = the first pass after OutputDone
// E2-8b NOTE: posted waits for the park ack so a changed notice meets a committed job (D10 item
// 4's "after the park ack but before output done"; the chain writes changed@posted for it)
void CVideoPlayer::TxTestNoticeAt()
{
  if (m_txNoticeKind.empty() || !m_txJob)
    return;
  const bool posted = m_txStep == TxStep::POSTED;
  const bool reached =
      m_txNoticePoint == "prime"    ? !posted
      : m_txNoticePoint == "posted" ? posted && m_txJob->state.load() ==
                                                   DISPLAYSTATE::StartJobState::COMMITTED
                                    : posted && m_txOutputDone;
  if (!reached)
    return;
  CLog::Log(LOGWARNING, "TXTEST noticeat {}@{} T={}", m_txNoticeKind, m_txNoticePoint,
            m_txJob->T);
  m_renderManager.TxTestNotice(m_txNoticeKind == "changed");
  m_txNoticeKind.clear();
}

// 5.E2-8b: one CheckStartGate pass of the open start P decides (not lost)
void CVideoPlayer::TxStartGatePass(CRenderManager::ModeFirstState state)
{
  TxTestNoticeAt();
  // D9's read order: RM's notices first, then the exit counter, then the replies
  std::vector<CRenderManager::STxNotice> notices = m_renderManager.TakeTxNotices();
  if (m_txStep != TxStep::POSTED)
  {
    // D3: before the post every notice is folded (DECIDE reads RM's current facts; code review
    // A-L1: the job's facts are DECIDE's own read, not the newest notice)
    // 5.E2-9c D3 (r5 L4): a hint decision waiting for a GUI set meets its picture: one that
    // differs takes the decision back (not posted: nothing to undo)
    bool withdraw = false;
    for (const CRenderManager::STxNotice& n : notices)
    {
      const std::string_view kind = n.kind;
      if (m_txStep == TxStep::WAIT_GUI && m_txHintDecide && !withdraw &&
          (kind == "configure" || kind == "fps" || kind == "reclock") &&
          !CRenderManager::TxHintFactsSame(m_txHintFacts, n.fps, n.width, n.height, n.stereoMode,
                                           n.hdrType, true))
        withdraw = true;
      CLog::Log(LOGINFO, "NOTICE folded {}", n.kind);
    }
    if (withdraw)
    {
      m_txStep = TxStep::NONE;
      m_txHintDecide = false;
      CLog::Log(LOGINFO, "DECIDE start set={} T={} no-picture DECIDE withdrawn (picture in WAIT_GUI)",
                m_txEngagedGen, m_txJob->T);
    }
    // a picture during the hint decision's window wait takes the picture path (r5 L4)
    if (m_txStep == TxStep::NONE && m_txHintDecide &&
        state != CRenderManager::ModeFirstState::UNCONFIGURED)
    {
      m_txHintDecide = false;
      CLog::Log(LOGINFO, "TXSTART set={} T={} picture before the no-picture DECIDE: decided on it",
                m_txEngagedGen, m_txJob->T);
    }
    if (m_txStep == TxStep::NONE &&
        (state != CRenderManager::ModeFirstState::UNCONFIGURED || m_txHintDecide))
      TxDecide();
    else if (m_txStep == TxStep::WAIT_GUI)
      TxTryPost();
    else if (m_txStep == TxStep::ASK)
    {
      TxAskPass();
      TxAskOpenUpdate();
    }
    return;
  }
  using How = CRenderManager::TxHow;
  using Need = CRenderManager::TxNeed;
  // D9: the guiSet notice with RM's, then GC's exit counter (it is bumped before Back's trigger
  // and desktop set, so a Back racing these reads is seen as the exit)
  const DISPLAYSTATE::GuiSetNotice gui = DISPLAYSTATE::ReadGuiSetNotice(m_txGuiSetSnap);
  const auto exitPending = [this]
  { return DISPLAYSTATE::FullScreenExits() != m_txExitsAtOpen; };
  // r11 M1: a fullscreen exit outranks `gui set` and `input changed`; the job is never dropped
  const auto endLeft = [&](size_t from)
  {
    for (size_t i = from; i < notices.size(); ++i)
      CLog::Log(LOGINFO, "NOTICE folded {} (fullscreen exit)", notices[i].kind);
    EndTransactionToLegacy("fullscreen exit", How::RERAISE, Need::LEFT);
  };
  if (exitPending())
  {
    endLeft(0);
    return;
  }
  if (gui.seq > m_txGuiSetSnap)
  {
    if (exitPending())
    {
      endLeft(0);
      return;
    }
    // D9: not committed -> reraise (GC dropped the job); committed -> fresh or gui-own by the tag
    CLog::Log(LOGINFO, "TXSTART T={} GUI set after the post ({}-activation)", m_txJob->T,
              gui.preActivation ? "pre" : "post");
    EndTransactionToLegacy("gui set", How::RERAISE,
                           gui.preActivation ? Need::FRESH : Need::GUI_OWN);
    return;
  }
  // D3: after the post a notice is compared with the job's facts; nothing is replayed
  for (size_t i = 0; i < notices.size(); ++i)
  {
    const CRenderManager::STxNotice& n = notices[i];
    // 5.E2-9c D4: the gen's first picture notice
    const std::string_view kind = n.kind;
    const bool pictureNotice = kind == "configure" || kind == "fps" || kind == "reclock";
    const bool first = m_txHintJob && pictureNotice && !std::exchange(m_txHintNoticeSeen, true);
    if (TxNoticeSame(n, first))
    {
      CLog::Log(LOGINFO, "NOTICE same {}", n.kind);
      continue;
    }
    // r12 L4: the exit counter re-read before acting on `input changed`
    if (exitPending())
    {
      endLeft(i);
      return;
    }
    CLog::Log(LOGINFO,
              "TXSTART T={} notice {} differs: fps {:.3f}/{:.3f} size {}x{}/{}x{} hdr {}/{} "
              "stereo '{}'/'{}'",
              m_txJob->T, n.kind, n.fps, m_txJobFacts.fps, n.width, n.height, m_txJobFacts.width,
              m_txJobFacts.height, static_cast<int>(n.hdrType),
              static_cast<int>(m_txJobFacts.hdrType), n.stereoMode, m_txJobFacts.stereoMode);
    // 5.E2-9c D4 (ruling 3): a hint decision's contradiction before START
    if (m_txHintJob)
    {
      TxHintContradicted(n, "notice");
      return;
    }
    EndTransactionToLegacy("input changed", How::RERAISE, Need::FRESH);
    return;
  }
  TxRepliesPass();
}

// 5.E2-8b D8: CN's cap and audio-full bound end a start whose job is not past its drop CAS
// (reraise, the gate opened by the caller); a committed job is not ended: D6 opens CN, the
// backstop bounds it
bool CVideoPlayer::TxBoundEnds(const char* site)
{
  if (!TxOpen())
    return true;
  // 5.E2-10 9d: the 3D dialog has no timeout
  if (m_txJob && m_txStep == TxStep::ASK)
  {
    if (!std::exchange(m_txAskBoundLogged, true))
      CLog::Log(LOGINFO, "TXSTART T={} {}: the 3D question is open, not ended", m_txJob->T, site);
    return false;
  }
  // 5.E2-9a (D7, C2 review B1): a posted job belongs to the OUTPUT bound (15 s from the post) and
  // the backstop: CN's cap, armed before the post, must not end it to legacy behind a hung W
  if (m_txJob && m_txStep == TxStep::POSTED)
  {
    if (!std::exchange(m_txBoundMetCommit, true))
      CLog::Log(LOGINFO, "TXSTART T={} {}: the job is posted ({}), not ended", m_txJob->T, site,
                DISPLAYSTATE::StartJobStateName(m_txJob->state.load()));
    return false;
  }
  EndTransactionToLegacy(site, CRenderManager::TxHow::RERAISE, CRenderManager::TxNeed::JOB_OK);
  return true;
}

// 5.E2-8b D6: CN opens on SettleDone{done|no-change|not-armed} and main's adopt answer
// {adopted|behind-gui}, both counted by D5b's provisional row; the backstop from the post
void CVideoPlayer::TxRepliesPass()
{
  using Settle = DISPLAYSTATE::StartSettleKind;
  using Form = DISPLAYSTATE::StartAdoptForm;
  const bool settleOk = m_txSettle && (*m_txSettle == Settle::DONE ||
                                       *m_txSettle == Settle::NO_CHANGE ||
                                       *m_txSettle == Settle::NOT_ARMED);
  const bool adoptOk = m_txAdopt && (*m_txAdopt == Form::ADOPTED || *m_txAdopt == Form::BEHIND_GUI);
  // transaction_capforce / audiofullforce (E2-8a D8 item 3): the bounds forced on an engaged set
  // E2-8b NOTE: a forced bound that met a committed job (not ended, D8) lets D6 open it
  // 5.E2-9c D1: inside the read-on's scope audiofullforce is not read here
  if (settleOk && adoptOk &&
      (!(m_txCapForce || (m_txAudioFullForce && !TxReadOnOpen())) || m_txBoundMetCommit))
  {
    if (!TxDecided(*m_txSettle != Settle::NO_CHANGE))
      return;
    // 5.E2-9c D5: a hint-decided start's settle timer
    if (m_txHintJob)
      m_txSettleSince = std::chrono::steady_clock::now();
    // 5.E2-9c D6: transaction_nopicture settle+<ms>
    if (m_txNoPictureSettleMs >= 0)
    {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(std::exchange(m_txNoPictureSettleMs, -1));
      m_VideoPlayerVideo->SetTxNoPicture(
          std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
      CLog::Log(LOGWARNING, "TXTEST nopicture: pictures withheld to settle done + hook time");
    }
    OpenStartGate(*m_txSettle == Settle::NO_CHANGE ? "transaction no change"
                                                   : "transaction settle done",
                  false);
    return;
  }
  // D8: every other reply ends the start; the job is past its drop CAS (job-ok), except a job GC
  // dropped (`gui set`, not committed)
  if (m_txSettle && !settleOk)
  {
    using How = CRenderManager::TxHow;
    using Need = CRenderManager::TxNeed;
    switch (*m_txSettle)
    {
      case Settle::DROPPED:
        EndTransactionToLegacy("gui set", How::RERAISE, Need::FRESH);
        return;
      case Settle::FAILED:
        // the rm-retry flag rides on main's answer (FAILED_SET)
        EndTransactionToLegacy("failed", How::RERAISE, Need::JOB_OK);
        return;
      case Settle::FAILED_PARK:
        EndTransactionToLegacy("failed-park", How::RERAISE, Need::JOB_OK);
        return;
      case Settle::SUPERSEDED:
        EndTransactionToLegacy("superseded", How::RERAISE, Need::JOB_OK);
        return;
      case Settle::CANCELLED:
        EndTransactionToLegacy("cancelled", How::RERAISE, Need::JOB_OK);
        return;
      case Settle::STOPPING:
        EndTransactionToLegacy("stopping", How::RERAISE, Need::JOB_OK);
        return;
      default:
        break;
    }
  }
  if (m_txAdopt && !adoptOk)
  {
    // r9 L3: job-ok on behind and failed; a recovering answer (main's catch posted its recovery
    // set) gets no TxAdopted for RECOVERED: P ends job-ok, RM holds CN (commit-in-flight) until
    // the recovery set's reply counts (testbench/e28b_outcome_model.py: job-ok x recovering)
    EndTransactionToLegacy(*m_txAdopt == Form::BEHIND   ? "adopt behind"
                           : *m_txAdopt == Form::FAILED ? "adopt failed"
                                                        : "adopt recovering",
                           CRenderManager::TxHow::RERAISE, CRenderManager::TxNeed::JOB_OK);
    return;
  }
  if (m_txSettle && m_txAdopt)
    return;
  const auto now = std::chrono::steady_clock::now();
  // 5.E2-11 D1.9: a span pauses every bound ("30 s in all" too); a freeze from the start's own
  // set with no span still counts
  const auto spanPaused = TxSpanPausedNow();
  const auto all = now - m_txPostedAt - spanPaused;
  const auto paused = std::max(m_txLostPaused, spanPaused) - spanPaused;
  // 5.E2-9a (D7, r1 H4, r2 N-M2): the OUTPUT bound, 15 s from the post to W's first answer
  // (paused while lost), tested before the backstop; W silent at 30 s in all is the no-answer rule
  if (!m_txOutputDone && !m_txSettle)
  {
    if (all > 30s)
      TxNoAnswerExit("site backstop");
    else if (all - paused > 15s)
      TxOutputBoundExit();
    return;
  }
  // the backstop (r2 N-H3, r3 M3, r4 L5): 15 s from the post, paused while lost, 30 s in all
  if (all - paused <= 15s && all <= 30s)
    return;
  TxBackstopExit("site backstop");
}

void CVideoPlayer::TxOutputBoundExit()
{
  // r2 N-M3: W past its commit CAS but still inside the commit: wait for its answer to 30 s in all
  if (m_txJob->state.load() == DISPLAYSTATE::StartJobState::COMMITTED)
  {
    if (!m_txBoundInCommitLogged)
    {
      m_txBoundInCommitLogged = true;
      CLog::Log(LOGWARNING, "TXSTART T={} output bound met inside the commit: waiting to 30 s",
                m_txJob->T);
    }
    return;
  }
  TxErrorExit("output bound");
}

void CVideoPlayer::TxNoAnswerExit(const char* site)
{
  // r3 N3-M1, r4 N4-H1: to legacy only for a job still queued through a real disconnect (a
  // mode set's own lost walk marks the display lost too); else W hangs: the error exit
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (m_txJob->state.load() == DISPLAYSTATE::StartJobState::QUEUED && winSystem &&
      winSystem->IsLostSpanOpen())
  {
    CLog::Log(LOGWARNING, "TXSTART T={} start reply missing: real loss, to legacy ({})",
              m_txJob->T, site);
    TxBackstopExit(site);
    return;
  }
  CLog::Log(LOGERROR, "TXSTART T={} start reply missing: W hung ({})", m_txJob->T, site);
  TxErrorExit("no answer from W");
}

void CVideoPlayer::TxErrorExit(const char* why)
{
  // ruling (1) 2026-11-15: the error exit is for the output never answering; the exit's drop CAS
  // keeps the GUI on the incumbent when it wins
  CLog::Log(LOGERROR, "TXSTART T={} {}: start not committed", m_txJob->T, why);
  EndTransactionToLegacy(why, CRenderManager::TxHow::NONE, CRenderManager::TxNeed::NONE_NEEDED,
                         false);
  m_error = true;
  m_bAbortRequest = true;
}

void CVideoPlayer::TxBackstopExit(const char* site)
{
  // E28B-10: main's answer may be queued behind the backstop: take it before answering `failed`
  if (!m_txAdopt)
  {
    const uint64_t T = m_txJob->T;
    const auto ours = [T](CDVDMsg& m)
    {
      return m.IsType(CDVDMsg::PLAYER_TX_REPLY) &&
             static_cast<CDVDMsgType<STxReply>&>(m).m_value.T == T;
    };
    std::shared_ptr<CDVDMsg> msg;
    while (!m_txAdopt && m_messenger.GetFirstOf(msg, ours, 0ms) == MSGQ_OK)
      TxOnReply(*msg);
    if (m_txAdopt)
    {
      CLog::Log(LOGINFO, "TXSTART T={} backstop ({}): main's answer was queued, taken", T, site);
      return;
    }
  }
  const bool answer = !m_txAdopt;
  CLog::Log(LOGERROR, "TXSTART T={} start reply missing ({}) ({})", m_txJob->T,
            !m_txSettle && !m_txAdopt ? "settle, adopt"
            : !m_txSettle            ? "settle"
                                     : "adopt",
            site);
  const unsigned int gen = m_txEngagedGen;
  EndTransactionToLegacy("backstop", CRenderManager::TxHow::RERAISE,
                         CRenderManager::TxNeed::JOB_OK);
  // D5b: the backstop's answer is `failed` when main's is missing
  if (answer)
    m_renderManager.TxAdoptAnswer(gen, CRenderManager::TxAnswer::FAILED);
}

// 5.E2-8b D9: a failed Start job's own set lost the display, and only main's rm retry (D8's
// job-ok x failed hand-back) brings it back, as legacy's failed set retries at once. So its
// reply ends the start while P is lost, with the same pass as when not lost; the other replies
// wait for the display (the set that supersedes, cancels or follows the job brings it back)
// unless D6's backstop is met
void CVideoPlayer::TxLostFailedPass()
{
  if (!TxOpen() || !m_txJob || m_txStep != TxStep::POSTED)
    return;
  using Settle = DISPLAYSTATE::StartSettleKind;
  const uint64_t T = m_txJob->T;
  const auto failed = [T](CDVDMsg& m)
  {
    if (!m.IsType(CDVDMsg::PLAYER_TX_REPLY))
      return false;
    const STxReply& r = static_cast<CDVDMsgType<STxReply>&>(m).m_value;
    if (r.T != T)
      return false;
    if (r.kind == STxReply::Kind::SETTLE_DONE)
      return r.settle == Settle::FAILED || r.settle == Settle::FAILED_PARK;
    return r.kind == STxReply::Kind::ADOPTED && r.form == DISPLAYSTATE::StartAdoptForm::FAILED;
  };
  std::shared_ptr<CDVDMsg> msg;
  if (m_messenger.GetFirstOf(msg, failed, 0ms) == MSGQ_OK)
  {
    TxOnReply(*msg);
    CLog::Log(LOGINFO, "TXSTART T={} start job failed while the display is lost", T);
    TxStartGatePass(m_renderManager.GetModeFirstState(m_startGateGen));
    return;
  }
  // D6: the backstop's 30 s in all bound P while lost too; a reply queued behind the loss is not
  // missing
  if ((m_txSettle && m_txAdopt) ||
      std::chrono::steady_clock::now() - m_txPostedAt - TxSpanPausedNow() <= 30s) // 5.E2-11 D1.9
    return;
  const auto ours = [T](CDVDMsg& m)
  {
    return m.IsType(CDVDMsg::PLAYER_TX_REPLY) &&
           static_cast<CDVDMsgType<STxReply>&>(m).m_value.T == T;
  };
  while (!(m_txSettle && m_txAdopt) && m_messenger.GetFirstOf(msg, ours, 0ms) == MSGQ_OK)
    TxOnReply(*msg);
  if (m_txSettle && m_txAdopt)
    return;
  // 5.E2-9a (D7): no first answer from W at 30 s: the no-answer rule
  if (!m_txOutputDone && !m_txSettle)
    TxNoAnswerExit("site lost");
  else
    TxBackstopExit("site lost");
}

std::chrono::steady_clock::duration CVideoPlayer::TxSpanPausedNow() const
{
  if (m_txSpanPausedFrom == std::chrono::steady_clock::time_point{})
    return m_txSpanPaused;
  return m_txSpanPaused + (std::chrono::steady_clock::now() - m_txSpanPausedFrom);
}

const char* CVideoPlayer::TxSpanStateName(bool starting) const
{
  if (starting)
    return "STARTING";
  switch (m_txStep)
  {
    case TxStep::NONE:
      return m_txConfiguredAt != std::chrono::steady_clock::time_point{} || m_txHintDecide
                 ? "DECIDE"
                 : "PRIME";
    case TxStep::WAIT_GUI:
      return "DECIDE";
    case TxStep::ASK:
      return "ASK";
    case TxStep::POSTED:
      break;
  }
  return DISPLAYSTATE::StartJobStateName(m_txJob->state.load());
}

void CVideoPlayer::TxSpanEdgeClear(const char* why)
{
  if (!m_txSpanSeen)
    return;
  m_txSpanSeen = false;
  if (std::exchange(m_txSpanDiscLost, false) && !m_displayLost)
    m_discTime.SetDisplayLost(false);
  CLog::Log(LOGINFO, "TXLOSS span in the start closed after {} ms ({})",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_txSpanOpenedAt)
                .count(),
            why);
}

// 5.E2-11 D1.3, D1.9: P's span input, derived from the word each pass (W writes no P member)
void CVideoPlayer::TxSpanPass()
{
  using WordState = OUTPUT::COutputWorker::WordState;
  const uint32_t ownT = m_txWordT.load();
  const auto word = ownT ? OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord())
                         : OUTPUT::COutputWorker::SWord{};
  const auto now = std::chrono::steady_clock::now();
  // D1.9: from the post on, the word's span alone (a real freeze inside it counts too)
  const bool lossInStart = aml_transaction_loss_in_start();
  const bool posted = TxOpen() && m_txJob && m_txStep == TxStep::POSTED && lossInStart;
  if (posted && word.spanOpen)
  {
    if (m_txSpanPausedFrom == std::chrono::steady_clock::time_point{})
      m_txSpanPausedFrom = now;
  }
  else if (m_txSpanPausedFrom != std::chrono::steady_clock::time_point{})
  {
    m_txSpanPaused += now - m_txSpanPausedFrom;
    m_txSpanPausedFrom = {};
  }
  m_txInSpan = TxOpen() && m_txJob && !m_displayLost && lossInStart &&
               word.t == ownT && word.spanOpen &&
               (word.state == WordState::NONE || word.state == WordState::STARTING);
  // D3.3: a Restore committed in this start: the next picture becomes the pre-roll still, once
  // per value (frozen passes too)
  if (TxOpen() && m_txJob)
  {
    const uint64_t commit = m_txJob->restoreCommit.load();
    if (commit != m_txReShowSeen)
    {
      m_txReShowSeen = commit;
      if (m_renderManager.TxReShowPreRoll(m_startGateGen, static_cast<uint32_t>(commit)))
        m_txPreRollReShown = true;
    }
  }
  // D3.4: STARTING after a span with e moved and no RESTORED: 15 s, then legacy
  if (lossInStart && TxOpen() && m_txJob && m_txWordStarting && !m_displayLost &&
      !word.spanOpen && word.t == ownT && word.state == WordState::STARTING &&
      word.e != m_txStartE)
  {
    if (m_txRestoreMissingSince == std::chrono::steady_clock::time_point{})
      m_txRestoreMissingSince = now;
    else if (now - m_txRestoreMissingSince >= 15s)
    {
      CLog::Log(LOGERROR, "TXSTART restore settle missing e={} (start e={})", word.e,
                m_txStartE);
      m_txRestoreMissingSince = {};
      EndTransactionToLegacy("restore settle missing", CRenderManager::TxHow::RERAISE,
                             CRenderManager::TxNeed::JOB_OK);
      return;
    }
  }
  else
    m_txRestoreMissingSince = {};
  if (!m_txInSpan)
  {
    if (m_txSpanSeen)
    {
      TxSpanEdgeClear(m_displayLost ? "a freeze took it" : !TxOpen() ? "the start ended"
                                                                     : "display back");
      // the bounds of PRIME..STARTING re-arm on the next pass, as after a freeze; DECIDE's
      // render timeout and ASK's 2 s from here
      m_txSpanClosedAt = now;
      if (m_txConfiguredAt != std::chrono::steady_clock::time_point{})
        m_txConfiguredAt = now;
      if (m_txAskSince != std::chrono::steady_clock::time_point{})
        m_txAskSince = now;
    }
    return;
  }
  if (!m_txSpanSeen)
  {
    m_txSpanSeen = true;
    m_txSpanOpenedAt = now;
    m_txSpanE = word.e;
    m_txSpanSpeedTaken = m_txSpanConverted = false;
    // D:222: P feeds the span to disc time
    m_discTime.SetDisplayLost(true);
    m_txSpanDiscLost = true;
    CLog::Log(LOGINFO, "TXLOSS span in the start open (state {}) T={} e={}",
              TxSpanStateName(word.state == WordState::STARTING), ownT, word.e);
  }
  // the lost branch's marks: CN's bounds, the settle timer and the holds re-arm after the span
  m_txLostSinceArm = true;
  m_txGateLostSeen = true;
  m_txSettleLostSeen = true;
  TxLostFailedPass();
  TxAskOpenUpdate();
  TxSpanConvertPass();
}

// 5.E2-11 D1.6: the 15 s conversion inside a start: a pending pause, START held as a pause by
// 9b's sync commit; a speed taken in this span stands (D1.7)
void CVideoPlayer::TxSpanConvertPass()
{
  if (!m_txInSpan || m_txSpanConverted || m_txSpanSpeedTaken)
    return;
  if (m_txSkipPausestart)
  {
    m_txSpanConverted = true;
    CLog::Log(LOGINFO, "TXLOSS start conversion off (skip pausestart)");
    return;
  }
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem || winSystem->DisconnectedForMs() < 15000 ||
      UserSpeed() == DVD_PLAYSPEED_PAUSE || !CanPause())
    return;
  m_txSpanConverted = true;
  CLog::Log(LOGINFO, "VideoPlayer: display lost for {} ms, now an ordinary pause",
            winSystem->DisconnectedForMs());
  m_txPendingSpeed = m_playSpeed == DVD_PLAYSPEED_PAUSE ? INT_MIN : DVD_PLAYSPEED_PAUSE;
  m_txPendingTempo = false;
  // an ordinary pause: disc time runs through it
  if (std::exchange(m_txSpanDiscLost, false))
    m_discTime.SetDisplayLost(false);
  ConvertToPauseCallbacks();
  CLog::Log(LOGINFO, "TXLOSS start converted: pause pending (START held as a pause)");
}

// 5.E2-3 (log only): the §4.7 boundary the transaction path would take at CN's arm or opening.
// Reads the shadow word; CASes nothing
void CVideoPlayer::LogWordBoundary(bool arm, const char* why)
{
  using WordState = OUTPUT::COutputWorker::WordState;
  const uint64_t value = OUTPUT::COutputWorker::Get().LoadWord();
  const auto word = OUTPUT::COutputWorker::UnpackWord(value);
  if (arm)
  {
    // a next file's first set reads the previous file's word (Prepare's open comes after): the
    // transaction aborts it and opens, 28+1 (E2-3 review L6)
    const bool firstSet = std::string_view(why) == "first set";
    const char* row;
    if (word.state == WordState::FREEZING)
      row = word.sub ? "2, waits for the park" : "2";
    else if (firstSet && (word.f || word.state != WordState::NONE))
      row = "28+1";
    else if (word.state == WordState::NONE)
      row = "1";
    else
      row = "25";
    CLog::Log(LOGINFO, "WORD shadow STARTING set={} (row {}; CN arm, {}) word {}",
              m_startGateGen, row, why, OUTPUT::COutputWorker::WordText(value));
    return;
  }
  // the cached m_scheduledStart && !IsRealtime(): no call into an inputstream (E2-3 review L1)
  const bool lead = m_displayResumeOnGrid && m_playSpeed == DVD_PLAYSPEED_NORMAL;
  CLog::Log(LOGINFO, "WORD shadow START candidate set={} -> {} (row {}; CN open, {}) word {}",
            m_startGateGen, lead ? "LEADING" : "NONE", lead ? "4" : "5", why,
            OUTPUT::COutputWorker::WordText(value));
}

std::pair<const char*, bool> CVideoPlayer::WordHeadNow(LossState expected) const
{
  const LossState now = m_lossState.load();
  return {LossStateName(now), now != expected};
}

uint8_t CVideoPlayer::WordMirror(LossState state)
{
  using WordState = OUTPUT::COutputWorker::WordState;
  switch (state)
  {
    case LossState::NONE:
      break;
    case LossState::LOST:
      return static_cast<uint8_t>(WordState::LOST);
    case LossState::CONVERTING:
      return static_cast<uint8_t>(WordState::CONVERTING);
    case LossState::PAUSED:
      return static_cast<uint8_t>(WordState::PAUSED);
    case LossState::RETURNING:
      return static_cast<uint8_t>(WordState::RETURNING);
  }
  return static_cast<uint8_t>(WordState::NONE);
}

const char* CVideoPlayer::LossStateName(LossState state)
{
  switch (state)
  {
    case LossState::NONE:
      return "NONE";
    case LossState::LOST:
      return "LOST";
    case LossState::CONVERTING:
      return "CONVERTING";
    case LossState::PAUSED:
      return "PAUSED";
    case LossState::RETURNING:
      return "RETURNING";
  }
  return "?";
}

void CVideoPlayer::SampleWord()
{
  const uint32_t ownT = m_txWordT.load();
  if (!ownT)
    return;
  const uint64_t value = OUTPUT::COutputWorker::Get().LoadWord();
  const auto word = OUTPUT::COutputWorker::UnpackWord(value);
  const LossState head = m_lossState.load();
  const bool own = word.t == ownT;
  // 5.E2-8a (D2b): the open start's own STARTING agrees with HEAD's state until START or the exit
  const bool startAgree = own && word.state == OUTPUT::COutputWorker::WordState::STARTING &&
                          m_txWordStarting && TxOpen();
  const bool agree =
      startAgree || (own && static_cast<uint8_t>(word.state) == WordMirror(head));
  const int pair = own ? (word.sub << 8 | static_cast<int>(word.state) << 4 |
                          static_cast<int>(head))
                       : (0x1000 | static_cast<int>(head));
  const auto now = std::chrono::steady_clock::now();
  if (pair != m_wordSamplePair)
  {
    m_wordSamplePair = pair;
    m_wordSampleSince = now;
  }
  if (pair == m_wordSampleLogged)
    return;
  // 5.E2-7 (e27u p5f x finding): the span's claim (row 11, FREEZING claimed) stands through the
  // whole lost walk, and the player's body runs after the earlier listeners; with keepsink off
  // ActiveAE's DISPLAYLOST closes its sink first (~200 ms), past the 200 ms tolerance, though
  // nothing disagrees: the body is still to come. That pair (HEAD's NONE, or RETURNING for a
  // re-freeze) gets the walk's own bound (ActiveAE's 5 s sync, + 1 s); a claim never parked still
  // logs DISAGREE after it
  using WordState = OUTPUT::COutputWorker::WordState;
  const bool claimPending = own && word.state == WordState::FREEZING && !word.sub &&
                            (head == LossState::NONE || head == LossState::RETURNING);
  if (own && !agree && now - m_wordSampleSince < (claimPending ? 6000ms : 200ms))
    return;
  m_wordSampleLogged = pair;
  CLog::Log(own && !agree ? LOGWARNING : LOGINFO, "WORD sample {} T={} lossState={} word {}",
            !own ? "other T" : startAgree ? "agree (start)" : agree ? "agree" : "DISAGREE", ownT,
            LossStateName(head),
            OUTPUT::COutputWorker::WordText(value));
}

// 5.E2-5 (shadow): P's ResolveOutput for the CN set (armed: CN's fullscreen predicate held),
// from the inputs main published (the root as it stood at the open) and RM's own decision inputs
void CVideoPlayer::DecideShadow()
{
#if defined(HAS_LIBAMCODEC)
  const DISPLAYSTATE::DecideInputs latest = DISPLAYSTATE::GetDecideInputs(true);
  // the root as published at GC's writes (E2-5 review M1: no state of main's open read here)
  m_renderManager.DecideShadowPlayer(m_startGateGen, latest, DISPLAYSTATE::FullScreenRoot(),
                                     true);
#endif
}

void CVideoPlayer::CheckStartGate()
{
  // 5.E2-10 §3: no pass on a superseded job
  if (!m_startGateShut || m_txSuperseded)
    return;
  // 5.E2-12b D1 (r2 H-A): only row 3b's step opens a no-video hold; CN's 12 s cap, counted from
  // the previous arm and re-armed while the display is lost, is its one bound
  if (m_txNoVideoHold)
  {
    const auto now = std::chrono::steady_clock::now();
    auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
    if (m_displayLost || m_txInSpan || (winSystem && winSystem->IsLostSpanOpen()))
    {
      m_startGateSince = now;
      return;
    }
    if (now - m_startGateSince < 12s)
      return;
    CLog::Log(LOGWARNING, "TRANSACTION no-video hold: CN's 12 s cap opens the gate");
    TxNoVideoReset("cap");
    m_txNoVideoCapped = true;
  }
  using State = CRenderManager::ModeFirstState;
  // the state first: a mode set that loses the display lands before its count
  State state = m_renderManager.GetModeFirstState(m_startGateGen);
  // 5.E2-8b: an engaged start P decides (its job open)
  const bool txP = TxOpen() && m_txJob;
  // 5.E2-5 (shadow): DECIDE's ResolveOutput on the set's first configure (5.E2-8b: P's own
  // DECIDE takes the inputs instead)
  if (state != State::UNCONFIGURED && m_decideShadowGen != m_startGateGen &&
      DISPLAYSTATE::DecideShadowOn())
  {
    m_decideShadowGen = m_startGateGen;
    if (!txP)
      DecideShadow();
  }
  // the session's fullscreen video window from its first picture, as the first commit
  // posts it ungated: else the GUI stays up through the mode set and its settle (so also
  // while lost). Once it is active, the commit's own switch does nothing.
  if (state != State::UNCONFIGURED && !m_startGateFullScreenPosted &&
      m_playerOptions.fullscreen && !m_State.streamsReady)
  {
    m_startGateFullScreenPosted = true;
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
    CLog::Log(LOGINFO, "VideoPlayer: start gate: fullscreen at the first picture (set {})",
              m_startGateGen);
  }
  // 5.E2-9a (D4, E28A-8): an engaged open's bounds do not count the lost time; the loss is
  // marked by Process's lost branch (this pass does not run there), this return guards a
  // pass from inside a demux read. 5.E2-11 D1.4: a row-9 span too (DECIDE never reads the caps W
  // published at the loss)
  if (m_displayLost || m_txInSpan)
    return;
  if (m_txGateLostSeen)
  {
    m_txGateLostSeen = false;
    if (TxOpen())
    {
      m_startGateSince = std::chrono::steady_clock::now();
      m_startGateStarveSince = {};
      CLog::Log(LOGINFO, "TXSTART set={} CN's bounds re-armed after the display span",
                m_txEngagedGen);
    }
  }
  // transaction_exc1 (D2): sync exception 1's flush injected in PRIME, once per open
  if (txP && m_txStep == TxStep::NONE && !m_txExc1Done && aml_transaction_hooks().exc1)
  {
    m_txExc1Done = true;
    CLog::Log(LOGWARNING, "TXTEST exc1 set={} T={}", m_txEngagedGen, m_txWordT.load());
    m_txFlushCaller = "exc1 (test)";
    FlushBuffers(DVD_NOPTS_VALUE, true, true);
    return;
  }
  if (txP)
  {
    // 5.E2-10 9d: the configure raised SMg's AVChange
    if (state != State::UNCONFIGURED &&
        m_txConfiguredAt == std::chrono::steady_clock::time_point{})
      m_txConfiguredAt = std::chrono::steady_clock::now();
    // D6's backstop does not count a loss (the main loop stamps its start)
    if (m_txLostAt != std::chrono::steady_clock::time_point{})
    {
      m_txLostPaused += std::chrono::steady_clock::now() - m_txLostAt;
      m_txLostAt = {};
    }
    // 5.E2-9c D2 (ruling 1): no picture 5 s into PRIME: the mode is decided from the hints
    if (TxPrimeBoundOpen() && state == State::UNCONFIGURED && m_txStep == TxStep::NONE &&
        !m_txHintDecide && std::chrono::steady_clock::now() - m_startGateSince >= 5s)
    {
      m_txHintDecide = true;
      m_txHintBoundAt = std::chrono::steady_clock::now();
      CLog::Log(LOGINFO, "TXSTART set={} T={} PRIME bound (5 s): no picture, DECIDE from the hints",
                m_txEngagedGen, m_txJob->T);
      const int64_t flushNs = m_VideoPlayerVideo->LastDecoderFlushNs();
      const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                m_txHintBoundAt.time_since_epoch())
                                .count();
      if (flushNs && nowNs - flushNs <= 1'000'000'000)
        CLog::Log(LOGINFO, "TXSTART set={} decoder flushed at the bound ({} ms before)",
                  m_txEngagedGen, (nowNs - flushNs) / 1'000'000);
      // the window, as the first picture posts it (r2 M6)
      if (!m_startGateFullScreenPosted && m_playerOptions.fullscreen && !m_State.streamsReady)
      {
        m_startGateFullScreenPosted = true;
        CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
        CLog::Log(LOGINFO, "VideoPlayer: start gate: fullscreen at the PRIME bound (set {})",
                  m_startGateGen);
      }
    }
    TxStartGatePass(state);
    // C2 review B2: the error exit stops the player; nothing of legacy's gate runs after it
    if (!m_startGateShut || m_bAbortRequest)
      return;
    // ended to legacy in this pass: RM's state from here, as legacy reads it
    if (!TxOpen())
      state = m_renderManager.GetModeFirstState(m_startGateGen);
  }
  // transaction_capforce / audiofullforce (E2-8a D8 item 3): the bounds forced on an engaged set
  // 5.E2-8b D6: on P's open start RM's DECIDED/UNCHANGED do not open CN (the m_txJob term).
  // 5.E3: the RM opener is kept for legacy, exits and 5.G
  if ((state == State::DECIDED || state == State::UNCHANGED) &&
      !(TxOpen() && (m_txCapForce || m_txAudioFullForce || m_txJob)))
  {
    // 5.E2-8a (D2, D3): the engaged start's word and disc time, before the gate opens
    if (TxOpen() && !TxDecided(state == State::DECIDED))
      return;
    OpenStartGate(state == State::DECIDED ? "output mode decided" : "no mode decision needed",
                  false);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // 5.E2-9c D1: inside the read-on's scope the 1 s audio-full bound is not checked;
  // transaction_audiofullforce only ends the read-on, for the open
  if (TxReadOnOpen())
  {
    if (m_txAudioFullForce && m_txReadOn != TxReadOn::FORCED)
    {
      // logged once, entered or not (r4 L12)
      if (m_txReadOn != TxReadOn::ACTIVE)
      {
        m_txReadOnPeakData = 0;
        m_txReadOnPeakTime = 0.0;
        m_txReadOn = TxReadOn::ACTIVE;
      }
      TxReadOnEnd("forced");
      m_txReadOn = TxReadOn::FORCED;
    }
    m_startGateStarveSince = {};
  }
  // a full audio queue stops the reads, and a video decoder that needs more input
  // never configures (Sync's audioFullVideoStarving)
  else if ((TxOpen() && m_txAudioFullForce) ||
      (state == State::UNCONFIGURED && m_CurrentAudio.id >= 0 &&
       !m_VideoPlayerAudio->AcceptsData() && m_processInfo->GetLevelVQ() < 10))
  {
    if (m_startGateStarveSince == std::chrono::steady_clock::time_point{})
      m_startGateStarveSince = now;
    else if (now - m_startGateStarveSince >= 1s)
    {
      // 5.E2-8b D8: a committed Start job is not ended (D6 opens CN; the backstop bounds it)
      if (!TxBoundEnds("audio-full bound"))
        return;
      OpenStartGate("audio queue full before the first picture", true);
      return;
    }
  }
  else
    m_startGateStarveSince = {};
  // 5.E2-9c D2: before the post the PRIME bound replaces the 12 s cap for an unconfigured set.
  // 5.E3: the cap is kept: the only bound for a menu-domain, skip-primebound or WAIT_GUI open
  const bool primeBoundOnly = TxPrimeBoundOpen() && state == State::UNCONFIGURED &&
                              m_txStep == TxStep::NONE && !m_txHintDecide;
  if (!primeBoundOnly && now - m_startGateSince > 12s)
  {
    if (!TxBoundEnds("12 s cap"))
      return;
    OpenStartGate("no mode decision in 12s", true);
  }
}

bool CVideoPlayer::TxKeepsFlush(const char* caller) const
{
  // main decides this open (skip decide, another start job): E2-8b's exit (r1 M3)
  if (!m_txJob)
    return false;
  if (aml_transaction_skip("reprime"))
  {
    CLog::Log(LOGINFO, "TXSKIP reprime set={}: the flush ends the start", m_txEngagedGen);
    return false;
  }
  // keepalive (a segment with no video), set program, a failed seek, the DVD hop and the next file
  // keep E2-8b's exit (r1 M6, L2)
  static constexpr std::string_view reprimeCallers[] = {"user seek",        "chapter seek",
                                              "sync video no start", "sync stuck",
                                              "audio stall resync", "realtime stall",
                                              "GENERAL_FLUSH",    "exc1 (test)"};
  if (!caller)
    return false;
  for (const auto& k : reprimeCallers)
    if (k == caller)
      return true;
  return false;
}

void CVideoPlayer::TxReprime(const char* caller)
{
  CLog::Log(LOGINFO, "TXSTART set={} T={} re-PRIME ({}) at step {}{}", m_txEngagedGen,
            m_txWordT.load(), caller, static_cast<int>(m_txStep),
            m_startHeld ? ", E1 hold dropped without START" : "");
  // r1 H1: CheckHeldStart would release START from the pre-flush clock before the re-sync
  if (m_startHeld)
  {
    m_startHeld = false;
    m_clock.SetStartHeld(false);
    m_VideoPlayerVideo->SetStartHeld(false);
    m_renderManager.SetStartHeld(false);
    // the graphics and disc-time bounds count from this re-PRIME, as from E1's hold
    m_renderManager.ArmTxStartHeldBound();
    if (m_txDiscHeld)
      m_discTime.SetTxHeld(true);
  }
  // 5.E2-9c D1 (r3 M6): after the flush a stall gets the read-on again (forced stays off)
  if (m_txReadOn != TxReadOn::FORCED)
    m_txReadOn = TxReadOn::IDLE;
  // D4 (r2 N-M1): re-armed only while no job is posted, so no second decision
  if (m_txStep == TxStep::NONE)
  {
    m_startGateSince = std::chrono::steady_clock::now();
    m_startGateStarveSince = {};
    // 5.E2-9c D1 (r4 M2): before the post the PRIME bound restarts from the seek
    m_txHintDecide = false;
  }
  // 5.E2-9c D5 (r5 L9): once SettleDone armed it
  if (m_txSettleSince != std::chrono::steady_clock::time_point{})
    m_txSettleSince = std::chrono::steady_clock::now();
  // transaction_seekdelay (D3, r2 N-L1): after the re-arm, so CN's cap does not expire inside it
  if (std::string_view(caller) == "user seek")
    if (const int ms = aml_transaction_hooks().seekDelayMs; ms > 0)
    {
      CLog::Log(LOGWARNING, "TXTEST seekdelay {} ms set={}", ms, m_txEngagedGen);
      CThread::Sleep(std::chrono::milliseconds(ms));
    }
}

// 5.E2-9c D1 (ruling 2): the gated audio queue stops the demuxer, so a held start's video
// decoder starves of the packets for its first picture
bool CVideoPlayer::TxReadOnBypass()
{
  if (!TxReadOnOpen() || m_txReadOn == TxReadOn::ENDED || m_txReadOn == TxReadOn::FORCED)
    return false;
  // the pause must reach the demuxer (the read gate's pause branch)
  if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
    return false;
  if (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_STARTING ||
      m_processInfo->GetLevelVQ() >= 10)
    return false;
  int data = 0;
  int maxData = 0;
  double time = 0.0;
  double cap = 0.0;
  bool timeFull = false;
  m_VideoPlayerAudio->GetQueueFill(data, maxData, time, cap, timeFull);
  if (m_txReadOn == TxReadOn::IDLE)
  {
    // a data-full queue below LvLAudioMAX is reopened by the sizer's staircase
    if (!timeFull && maxData < LvLAudioMAX)
      return false;
    m_txReadOn = TxReadOn::ACTIVE;
    m_txReadOnMax = maxData;
    m_txReadOnCap = cap;
    m_txReadOnPeakData = data;
    m_txReadOnPeakTime = time;
    CLog::Log(LOGINFO,
              "TXSTART set={} reading on for a starving video decoder (audio max {:.1f} MB time "
              "cap {:.1f} s)",
              m_txEngagedGen, static_cast<double>(maxData) / SIZE_1M, cap);
  }
  const int64_t dataCap = static_cast<int64_t>(m_txReadOnMax) + 8 * SIZE_1M;
  if (m_VideoPlayerAudio->IsOverFull(dataCap, m_txReadOnCap + 2.0))
  {
    m_VideoPlayerAudio->GetQueueFill(data, maxData, time, cap, timeFull);
    m_txReadOnPeakData = std::max(m_txReadOnPeakData, data);
    m_txReadOnPeakTime = std::max(m_txReadOnPeakTime, time);
    TxReadOnEnd(static_cast<int64_t>(data) > dataCap ? "data cap" : "time cap");
    return false;
  }
  m_txReadOnPeakData = std::max(m_txReadOnPeakData, data);
  m_txReadOnPeakTime = std::max(m_txReadOnPeakTime, time);
  return true;
}

void CVideoPlayer::TxReadOnPass()
{
  // r3 answer 1: a menu page waiting makes the open a menu-domain one from this pass
  if (TxOpen() && m_menuPageWaiting && !m_txMenuDomain)
  {
    if (m_txReadOn == TxReadOn::ACTIVE)
      TxReadOnEnd("menu");
    m_txMenuDomain = true;
  }
  if (m_txReadOn != TxReadOn::ACTIVE)
    return;
  if (!TxOpen())
    TxReadOnEnd(m_txStarted ? "start" : "exit");
  else if (m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_STARTING)
    TxReadOnEnd("picture");
}

void CVideoPlayer::TxReadOnEnd(const char* why)
{
  if (m_txReadOn != TxReadOn::ACTIVE)
    return;
  m_txReadOn = TxReadOn::ENDED;
  CLog::Log(LOGINFO, "TXSTART set={} reading on ended ({}) peak {:.1f} MB {:.1f} s",
            m_txEngagedGen, why, static_cast<double>(m_txReadOnPeakData) / SIZE_1M,
            m_txReadOnPeakTime);
}

void CVideoPlayer::OpenStartGate(const char* why, bool bound)
{
  m_startGateShut = false;
  m_startGateOpenedAt = std::chrono::steady_clock::now();
  m_gatedAudioWaitLogged = false;
  m_VideoPlayerAudio->SetStartGated(false);
  // a flush can remove it, and then its own GENERAL_FLUSH wakes the player
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_START_GATE), 1);
  m_renderManager.SetModeFirstGate(false, m_startGateGen);
  if (IAE* ae = CServiceBroker::GetActiveAE())
    ae->SetStartGate(false);
  const double shut =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startGateSince).count();
  CLog::Log(bound ? LOGWARNING : LOGINFO,
            "VideoPlayer: start gate opened after {:.3f}s (set {}): {}", shut, m_startGateGen, why);
  if (aml_transaction_shadow_on())
    LogWordBoundary(false, why);
}

bool CVideoPlayer::ScheduleUserResume()
{
  // the lead of a scheduled start: what the audio output queues ahead
  constexpr double RESUME_LEAD = 0.35;
  const double held = m_clock.GetClock();
  double pts = DVD_NOPTS_VALUE;
  double maxStep = 0.0;
  int64_t startNs = 0;
  double startClock = 0.0;
  // no frame out yet (the first speed of a file): nothing to resume to
  if (!m_renderManager.GetResumePts(pts, maxStep))
    return false;
  // issue 27: after a starvation freeze the outputs ran dry at different points, and the frames
  // queued to the display ahead of the clock are already out: the resume starts at the sound's
  // next sample, and the picture on screen holds until the clock reaches its next frame, so no
  // sound is dropped and no picture is shown twice
  if (std::exchange(m_starveResumeArmed, false))
  {
    if (m_CurrentAudio.id < 0)
      CLog::Log(LOGINFO, "STARVE resume at the next frame {:.3f} (no audio stream)",
                pts / DVD_TIME_BASE);
    else if (m_starveAudioNext == DVD_NOPTS_VALUE)
      CLog::Log(LOGINFO,
                "STARVE resume at the next frame {:.3f} (no sound sent on before the resume)",
                pts / DVD_TIME_BASE);
    else
    {
      const double sound = m_starveAudioNext - m_clock.GetAudioOffset();
      // the held clock goes back when the sound ran dry before it (the freeze point is the
      // picture's), so the resume never drops sound that was not played
      bool atSound = sound < pts && sound >= held - DVD_MSEC_TO_TIME(1);
      if (!atSound && sound < held - DVD_MSEC_TO_TIME(1) && held - sound < DVD_MSEC_TO_TIME(2000) &&
          m_clock.MoveHeldClock(sound))
      {
        CLog::Log(LOGINFO, "STARVE held clock moved back {:.0f} ms to the sound's end {:.3f}",
                  (held - sound) / 1000.0, sound / DVD_TIME_BASE);
        atSound = true;
      }
      if (atSound)
      {
        CLog::Log(LOGINFO,
                  "STARVE resume at the sound's next {:.3f} (the next frame {:.3f}: the picture "
                  "holds {:.0f} ms)",
                  sound / DVD_TIME_BASE, pts / DVD_TIME_BASE, (pts - sound) / 1000.0);
        pts = sound;
      }
      else if (sound >= pts)
        CLog::Log(LOGINFO,
                  "STARVE resume at the next frame {:.3f} (the sound's next {:.3f} lands {:.0f} ms "
                  "after it, silence before it; the held clock {:.3f})",
                  pts / DVD_TIME_BASE, sound / DVD_TIME_BASE, (sound - pts) / 1000.0,
                  held / DVD_TIME_BASE);
      else
        CLog::Log(LOGINFO,
                  "STARVE resume at the next frame {:.3f} (the sound's next {:.3f}, the held "
                  "clock {:.3f})",
                  pts / DVD_TIME_BASE, sound / DVD_TIME_BASE, held / DVD_TIME_BASE);
    }
  }
  if (!m_clock.ScheduleResumeAt(pts, maxStep, RESUME_LEAD, startNs, startClock))
  {
    CLog::Log(LOGINFO,
              "VideoPlayer: resume not scheduled (clock {:.3f}, next frame {:.3f}, step limit "
              "{:.1f} ms)",
              held / DVD_TIME_BASE, pts / DVD_TIME_BASE, maxStep * 1000.0 / DVD_TIME_BASE);
    return false;
  }
  CLog::Log(LOGINFO,
            "VideoPlayer: scheduled resume: clock {:.3f} -> {:.3f} at {} ns ({:.0f} ms from now)",
            held / DVD_TIME_BASE, startClock / DVD_TIME_BASE, startNs, RESUME_LEAD * 1000.0);
  // the stall check would take the lead for a stall (design 15.38)
  m_syncTimer.Set(std::chrono::milliseconds(std::lround((RESUME_LEAD + 1.0) * 1000.0)));
  return true;
}

void CVideoPlayer::TxRestoreDecoderSpeed(const char* why)
{
  // r2 M2: V re-applies CodecSpeed() on a speed message; the gate's hold is gone now
  m_VideoPlayerVideo->SetSpeed(m_streamPlayerSpeed);
  CLog::Log(LOGINFO, "TXGATE decoder speed restored ({})", why);
}

bool CVideoPlayer::TxGuardedStartOn() const
{
  return TxOpen() && m_txJob && m_txWordStarting && aml_transaction_loss_in_start() &&
         dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
}

bool CVideoPlayer::TxStartGuarded(TxStartPath path,
                                  STxStartDone& done,
                                  const std::function<void()>& commitSteps)
{
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  // the word and e a refused CAS met: unchanged, no CAS can apply (no section, no WORD line)
  if (m_txStartHeldWord && OUTPUT::COutputWorker::Get().LoadWord() == m_txStartHeldWord &&
      m_txStartE == m_txStartHeldE)
    return false;
  const STransactionHooks hooks = aml_transaction_hooks();
  if (hooks.startCasDelayAt == "before" && hooks.startCasDelayMs > 0 &&
      !std::exchange(m_txStartCasDelayDone, true))
  {
    CLog::Log(LOGWARNING, "TXTEST startcasdelay before {}", hooks.startCasDelayMs);
    CThread::Sleep(std::chrono::milliseconds(hooks.startCasDelayMs));
  }
  const std::shared_ptr<DISPLAYSTATE::SStartJob> job = m_txJob;
  const uint32_t T = m_txWordT.load();
  const uint32_t e = m_txStartE;
  bool started = false;
  // The player guard (m_resourceSection, the lock W's lost walk holds from WordLoss through the
  // walk): a loss lands wholly before the CAS (row 9, the CAS fails) or after a finished START
  // (row 11). Inside it nothing calls the demuxer, the input stream, an add-on, the GUI or the
  // messenger, takes a lock main or W can hold while taking the section, or waits: only the
  // clock, the VPA/VPV queues, DTO, RM's state and present locks, atomics and P members
  const bool ran = winSystem->RunUnderPlayerGuard(
      [&]
      {
        job->started = true;
        if (!OUTPUT::COutputWorker::Get().WordStart(T, e))
        {
          job->started = false;
          return;
        }
        started = true;
        if (hooks.startCasDelayAt == "after" && hooks.startCasDelayMs > 0)
        {
          CLog::Log(LOGWARNING, "TXTEST startcasdelay after {}", hooks.startCasDelayMs);
          CThread::Sleep(std::chrono::milliseconds(hooks.startCasDelayMs));
        }
        if (commitSteps)
          commitSteps();
        if (path == TxStartPath::UNHELD)
        {
          SetCaching(CACHESTATE_DONE);
          TxStartRelease(0);
        }
        else
          TxReleaseClock(done);
        m_txStarted = true;
      },
      true);
  if (!ran)
  {
    if (!std::exchange(m_txGuardBusyLogged, true))
      CLog::Log(LOGINFO, "TXSTART START deferred: player guard busy");
    return false;
  }
  m_txGuardBusyLogged = false;
  if (!started)
  {
    const uint64_t word = OUTPUT::COutputWorker::Get().LoadWord();
    m_txStartHeldE = e;
    if (std::exchange(m_txStartHeldWord, word) != word)
      CLog::Log(LOGINFO, "TXSTART START held: word {} (start e={})",
                OUTPUT::COutputWorker::WordText(word), e);
    return false;
  }
  m_txStartHeldWord = 0;
  return true;
}

void CVideoPlayer::TxReleaseClock(STxStartDone& done)
{
  // ReleaseHeldStart's clock part, in its order (inside the guard)
  done.held = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startHeldSince)
                  .count();
  // after HoldStart: the start's branch lands the offset for the start
  if (m_audioOffsetToAudio)
    ApplyAudioOffset();
  m_startHeld = false;
  m_startReleasedClock = m_clock.GetClock();
  constexpr double SCHEDULED_START_LEAD = 0.35;
  if (m_scheduledStart)
  {
    if (m_clock.ScheduleResume(m_playSpeed, SCHEDULED_START_LEAD, done.startNs, done.startClock,
                               m_startAtFrame))
    {
      done.lead = SCHEDULED_START_LEAD;
      done.discRunNs = done.startNs;
      m_txLeadT0Ns = done.startNs;
      // 5.E2-9b D3 (E28B-1): the overlays withheld to T0, stored before the holds clear below
      m_renderManager.SetTxGraphicsFrom(done.startNs);
    }
  }
  else
    m_clock.SetSpeed(m_playSpeed);
  m_discTime.SetStartHeld(false, done.discRunNs);
  m_clock.SetStartHeld(false);
  m_VideoPlayerVideo->SetStartHeld(false);
  m_renderManager.SetStartHeld(false);
  m_VideoPlayerAudio->SetSpeed(m_playSpeed);
  m_VideoPlayerVideo->SetSpeed(m_playSpeed);
  TxStartRelease(done.discRunNs);
  m_streamPlayerSpeed = m_playSpeed;
}

void CVideoPlayer::TxStartAfterGuard(TxStartPath path, const STxStartDone& done, const char* why)
{
  if (m_txRestoredE >= 0)
    CLog::Log(LOGINFO,
              "TXSTART T={} restore settled e={}: START on the start picture, pre-roll {}",
              m_txWordT.load(), m_txRestoredE, m_txPreRollReShown ? "re-shown" : "none");
  TxStartPoint(why, true);
  // the gate opens after V's speed message: the decoder's speed is sent again
  if (m_renderManager.OpenTxPictureGate(why))
    TxRestoreDecoderSpeed(why);
  if (path == TxStartPath::UNHELD)
    return;
  if (done.lead > 0.0)
  {
    CLog::Log(LOGINFO, "VideoPlayer: scheduled start: clock {:.3f} at {} ns ({:.0f} ms from now)",
              done.startClock / DVD_TIME_BASE, done.startNs, done.lead * 1000.0);
    // a loss after the guard cancelled the schedule: RESUME's landing governs the overlays, not
    // this T0. A read, not a CAS: a loss right after it costs only a past overlay instant (r4 L-6)
    if (m_displayLost)
    {
      m_renderManager.SetTxGraphicsFrom(0);
      CLog::Log(LOGINFO, "TXSTART overlays not withheld: the schedule was cancelled by a loss");
    }
  }
  if (m_txEngaged && m_txSyncPassed && !m_State.streamsReady)
    SignalStreamsReady();
  m_syncTimer.Set(3000ms + std::chrono::milliseconds(std::lround(done.lead * 1000.0)));
  if (aml_transaction_shadow_on())
  {
    using StartPhase = OUTPUT::COutputWorker::StartPhase;
    if (path == TxStartPath::SCHEDULED && !m_State.streamsReady)
      OUTPUT::COutputWorker::Get().ShadowStartPhase(this, StartPhase::STARTING, 0, "start held",
                                                    static_cast<int>(StartPhase::NONE));
    m_txLeadEndNs = done.lead > 0.0 ? done.discRunNs : 0;
    if (done.lead > 0.0)
      TxRequestAt("lead");
    if (done.lead > 0.0)
      OUTPUT::COutputWorker::Get().ShadowStartPhase(this, StartPhase::LEADING, done.discRunNs,
                                                    "scheduled start",
                                                    static_cast<int>(StartPhase::STARTING));
    else
      OUTPUT::COutputWorker::Get().ShadowStartPhase(this, StartPhase::NONE, 0, why,
                                                    static_cast<int>(StartPhase::STARTING));
  }
  CLog::Log(LOGINFO, "VideoPlayer: E1 start released after {:.3f}s: {} (clock {:.3f})", done.held,
            why, m_clock.GetClock() / DVD_TIME_BASE);
}

void CVideoPlayer::ReleaseHeldStart(const char* why)
{
  // 5.E2-11 D1.5: an E1-held guarded start's release is START under the guard; the verdict first
  if (TxGuardedStartOn() && std::string_view(why) != "flush")
  {
    TxCheckVerdict("release");
    if (TxGuardedStartOn())
    {
      STxStartDone done;
      if (TxStartGuarded(TxStartPath::RELEASE, done, nullptr))
        TxStartAfterGuard(TxStartPath::RELEASE, done, why);
      return;
    }
  }
  // 5.E2-9b D3 (r3 M3): this release is the open start's (not a legacy one)
  const bool isStart = TxOpen();
  // 5.E2-8a (D4): START on the word (a flush release is an exit, taken at FlushBuffers' entry)
  if (TxOpen())
  {
    TxCheckVerdict("release");
    if (TxOpen())
      TxStartPoint(why);
  }
  // 5.E2-6 (D7): START opens the picture gate, before V's speed below re-applies CodecSpeed()
  m_renderManager.OpenTxPictureGate(why);
  const double held =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startHeldSince).count();
  // the mode decision can set the latency tweak after this loop's check: the
  // start lands it, not a re-land after it
  if (m_audioOffsetToAudio)
    ApplyAudioOffset();
  m_startHeld = false;
  m_startReleasedClock = m_clock.GetClock();

  // A scheduled start (design §15, step 2.2): the clock resumes a lead from
  // now, exactly then, and the audio output lands its first sample for that
  // instant instead of muting and walking onto a clock already running. The
  // lead covers what the audio output queues ahead of a new buffer (engine
  // queue and device ring, up to 0.2 s each); the sink logs the pad left over.
  constexpr double SCHEDULED_START_LEAD = 0.35;
  double lead = 0.0;
  int64_t discRunNs = 0;
  if (m_scheduledStart && std::string_view(why) != "flush")
  {
    int64_t startNs = 0;
    double startClock = 0.0;
    if (m_clock.ScheduleResume(m_playSpeed, SCHEDULED_START_LEAD, startNs, startClock,
                               m_startAtFrame))
    {
      lead = SCHEDULED_START_LEAD;
      discRunNs = startNs;
      CLog::Log(LOGINFO, "VideoPlayer: scheduled start: clock {:.3f} at {} ns ({:.0f} ms from now)",
                startClock / DVD_TIME_BASE, startNs, lead * 1000.0);
      // 5.E2-9b D3 (E28B-1): the start's overlays withheld to T0, stored before the holds
      // clear below (a verdict exit at the release leaves it 0)
      if (isStart && m_txStarted && !m_txExited && lead > 0.0)
        m_renderManager.SetTxGraphicsFrom(startNs);
    }
  }
  else
    m_clock.SetSpeed(m_playSpeed);
  m_discTime.SetStartHeld(false, discRunNs);

  // after the schedule: a frame selected meanwhile sees one or the other
  m_clock.SetStartHeld(false);
  m_VideoPlayerVideo->SetStartHeld(false);
  m_renderManager.SetStartHeld(false);
  m_VideoPlayerAudio->SetSpeed(m_playSpeed);
  m_VideoPlayerVideo->SetSpeed(m_playSpeed);
  // 5.E2-8a (D4): the transaction's holds end with E1's, at the schedule's T0
  if (m_txStarted && m_txEngaged && !m_txExited)
    TxStartRelease(discRunNs);
  // D4's predicate: a release after the sync commit sends onAVStarted (an engaged start held
  // by E1, or the legacy release after the exit)
  if (m_txEngaged && m_txSyncPassed && std::string_view(why) != "flush" &&
      !m_State.streamsReady)
    SignalStreamsReady();
  // The players start with the clock, so their stall grace does too. Armed at
  // sync instead, it had run out during a hold through a TV relock (about 6 s),
  // and the stall check took the scheduled lead for a stall: it started
  // caching, which paused the clock, cancelling the schedule, and resumed it
  // some 50 ms later, off the vsync grid and 300 ms early (design 15.38).
  m_syncTimer.Set(3000ms + std::chrono::milliseconds(std::lround(lead * 1000.0)));
  m_streamPlayerSpeed = m_playSpeed;
  // 5.E2-4: the start window W's request shadow reads ends with the lead
  if (aml_transaction_shadow_on())
  {
    using StartPhase = OUTPUT::COutputWorker::StartPhase;
    m_txLeadEndNs = lead > 0.0 ? discRunNs : 0;
    if (lead > 0.0)
      TxRequestAt("lead");
    if (lead > 0.0)
      OUTPUT::COutputWorker::Get().ShadowStartPhase(this, StartPhase::LEADING, discRunNs,
                                                    "scheduled start",
                                                    static_cast<int>(StartPhase::STARTING));
    else
      OUTPUT::COutputWorker::Get().ShadowStartPhase(this, StartPhase::NONE, 0, why,
                                                    static_cast<int>(StartPhase::STARTING));
  }
  CLog::Log(LOGINFO, "VideoPlayer: E1 start released after {:.3f}s: {} (clock {:.3f})", held, why,
            m_clock.GetClock() / DVD_TIME_BASE);
}

bool CVideoPlayer::IsValidStream(const CCurrentStream& stream)
{
  if(stream.id<0)
    return true; // we consider non selected as valid

  int source = STREAM_SOURCE_MASK(stream.source);
  if(source == STREAM_SOURCE_TEXT)
    return true;
  if (source == STREAM_SOURCE_DEMUX_SUB)
  {
    CDemuxStream* st = m_pSubtitleDemuxer->GetStream(stream.demuxerId, stream.id);
    if(st == NULL || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;
    return true;
  }
  if (source == STREAM_SOURCE_DEMUX)
  {
    CDemuxStream* st = m_pDemuxer->GetStream(stream.demuxerId, stream.id);
    if(st == NULL || st->disabled)
      return false;
    if(st->type != stream.type)
      return false;

    if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    {
      if (stream.type == StreamType::AUDIO && st->dvdNavId != m_dvd.iSelectedAudioStream)
        return false;
      if (stream.type == StreamType::SUBTITLE && st->dvdNavId != m_dvd.iSelectedSPUStream)
        return false;
    }

    return true;
  }
  if (source == STREAM_SOURCE_VIDEOMUX)
  {
    CDemuxStream* st = m_pCCDemuxer->GetStream(stream.id);
    if (st == NULL || st->disabled)
      return false;
    if (st->type != stream.type)
      return false;
    return true;
  }

  return false;
}

bool CVideoPlayer::IsBetterStream(const CCurrentStream& current, CDemuxStream* stream)
{
  // Do not reopen non-video streams if we're in video-only mode
  if (m_playerOptions.videoOnly && current.type != StreamType::VIDEO)
    return false;

  if(stream->disabled)
    return false;

  if (m_pInputStream && (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) ||
                         m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY)))
  {
    int source_type;

    source_type = STREAM_SOURCE_MASK(current.source);
    if (source_type != STREAM_SOURCE_DEMUX &&
        source_type != STREAM_SOURCE_NONE)
      return false;

    source_type = STREAM_SOURCE_MASK(stream->source);
    // "same stream" must compare the demuxer too: after a BD menu->title
    // demuxer reopen the new demuxer's video stream reuses uniqueId 0, the
    // same id as the (orphaned) current - without the demuxerId check it is
    // misjudged as already-current and video never reopens
    if(source_type != STREAM_SOURCE_DEMUX ||
       stream->type != current.type ||
       (stream->uniqueId == current.id && stream->demuxerId == current.demuxerId))
      return false;

#if defined(HAVE_LIBBLURAY)
    // BD: dictated streams are resolved LIVE against the current playitem's
    // stream table instead of the event-cached pid in m_dvd. The BD
    // selection events are edge-triggered (fire only when the stream NUMBER
    // changes) and resolve against whatever clip was loaded at event time -
    // a menu->title jump where the number stays 1 but the PID changes
    // leaves the cache pointing at the old clip's PID and the title's audio
    // never reopens (packets drop MSGQ_NOT_INITIALIZED = silent title;
    // S&M benchmark demos, user report). CheckBetterStream runs per demux
    // packet, so with live resolution the reopen is guaranteed no later
    // than the new segment's first audio packet.
    if (std::shared_ptr<CDVDInputStreamBluray> bluray =
            std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
    {
      if (current.type == StreamType::AUDIO)
      {
        const int dictated = bluray->GetDictatedAudioPid();
        if (stream->dvdNavId == dictated)
          return true;
        // the disc explicitly deselected primary audio (0xff): authored
        // silence - a fallback open would turn it into sound
        if (bluray->IsAudioDictatedNone())
          return false;
        if (dictated < 0 && current.id < 0)
        {
          // resolution unavailable mid-transition and no audio is open:
          // prefer the demuxer's FIRST announced audio stream (PSR default
          // is stream 1), not whichever pid's packet wins the arrival race
          // (a commentary pid arriving first would play until the
          // corrective reopen)
          for (CDemuxStream* st : m_pDemuxer->GetStreams())
          {
            if (st->type == StreamType::AUDIO)
              return st->uniqueId == stream->uniqueId &&
                     st->demuxerId == stream->demuxerId;
          }
          return true;
        }
        // orphan rescue, symmetric with video below: dictation transiently
        // unresolvable while the current audio belongs to a closed demuxer
        // (DISCARD_KEEPALIVE) - its packets can never arrive again; accept
        // the new demuxer's stream instead of waiting out the stall-close
        if (dictated < 0 && current.id >= 0 &&
            stream->demuxerId != current.demuxerId)
          return true;
        return false;
      }
      if (current.type == StreamType::SUBTITLE)
        // live dictation ONLY: the stale event-cached pid
        // (m_dvd.iSelectedSPUStream) was the audio-reopen bug's twin
        // (review A2) - after a menu->title jump it points at the OLD
        // clip's pid, which in the new clip can be a different language
        // track, causing wrong-language subs / open ping-pong. User
        // selection goes through SelectSubtitleStream (bd_select_stream),
        // which updates the dictation itself.
        return stream->dvdNavId == bluray->GetDictatedPgPid();
    }
    else
#endif
    {
      if (current.type == StreamType::AUDIO && stream->dvdNavId == m_dvd.iSelectedAudioStream)
        return true;
      if (current.type == StreamType::SUBTITLE && stream->dvdNavId == m_dvd.iSelectedSPUStream)
        return true;
    }
    // a current video stream from a DIFFERENT demuxer is orphaned - the
    // demuxer was closed and reopened (BD menu->title discard keeps the
    // stream players alive for decoder reuse) and its packets can never
    // arrive again; without this the new demuxer's video stream is never
    // opened when the disc dictates audio/subs (OpenDefaultStreams defers)
    // and the video decoder starves on the last menu frame
    if (current.type == StreamType::VIDEO &&
        (current.id < 0 || stream->demuxerId != current.demuxerId))
      return true;
  }
  else
  {
    if(stream->source == current.source &&
       stream->uniqueId == current.id &&
       stream->demuxerId == current.demuxerId)
      return false;

    if(stream->type != current.type)
      return false;

    if (current.type == StreamType::SUBTITLE)
      return false;

    if(current.id < 0)
      return true;
  }
  return false;
}

void CVideoPlayer::CheckBetterStream(CCurrentStream& current, CDemuxStream* stream)
{
  IDVDStreamPlayer* player = GetStreamPlayer(current.player);
  if (!IsValidStream(current) && (player == NULL || player->IsStalled()))
    {
      CTxDrainSite txSite(m_txDrainSite, "CheckBetterStream");
      CloseStream(current, true);
    }

  if (IsBetterStream(current, stream))
    OpenStream(current, stream->demuxerId, stream->uniqueId, stream->source);
}

void CVideoPlayer::TxResetStart(bool reopen)
{
  m_txStarted = m_txExited = m_txSyncPassed = m_txWordStarting = false;
  m_txExc1Done = m_txLostSinceArm = m_txGateLostSeen = false;
  TxHintClear();
  m_txStep = TxStep::NONE;
  m_txJobFacts = {};
  m_txJobFactsSet = m_txGuiWaitLogged = m_txWindowWaitLogged = m_txBoundMetCommit =
      m_txOutputDone = false;
  m_txSettle.reset();
  m_txAdopt.reset();
  m_txPostedAt = m_txLostAt = {};
  m_txLostPaused = {};
  // 5.E2-11
  TxSpanEdgeClear("open");
  m_txInSpan = false;
  m_txStartE = 0;
  m_txRestoredE = -1;
  m_txReShowSeen = 0;
  m_txPreRollReShown = false;
  m_txRestoreMissingSince = {};
  m_txDecideHoldUntil = m_txPostHoldUntil = {};
  m_txGuardBusyLogged = false;
  m_txStartHeldWord = 0;
  m_txStartCasDelayDone = false;
  m_txLeadT0Ns = 0;
  m_txSpanPaused = {};
  m_txSpanPausedFrom = m_txSpanClosedAt = {};
  m_txNoticeKind.clear();
  m_txNoticePoint.clear();
  m_txLossAtUntil = {};
  m_txReopenOpen = m_txReopenInStart = m_txReopenWaitLogged = m_txSuperseded = false;
  m_txAskTarget.reset();
  m_txAskPauseSeen = false;
  m_txConfiguredAt = {};
  if (reopen)
  {
    // the engage sets the hook, read-on and menu-domain flags itself; the word's open, the
    // set flags, the pending speed, the audio time cap and the disc-time hold are kept
    TxReleaseJob("reopen");
    return;
  }
  m_txShadowSetSeen = false;
  m_txGateSetSeen = false;
  m_txEngaged = m_txDiscHeld = false;
  m_txEngagedGen = 0;
  m_txCapForce = m_txAudioFullForce = false;
  m_txPendingSpeed = INT_MIN;
  m_txPendingTempo = m_txSkipPausestart = false;
  m_txInboxSpeedLoss = 0;
  m_txSkipReadon = m_txSkipPrimebound = m_txMenuDomain = m_txReadOnBypassed = false;
  m_txReadOn = TxReadOn::IDLE;
  // 5.E2-9c D6: the hooks end with the file
  TxRestoreAudioTimeCap("file end");
  m_txNoPictureSettleMs = -1;
  if (m_VideoPlayerVideo)
    m_VideoPlayerVideo->SetTxNoPicture(0);
  m_renderManager.SetTxGraphicsFrom(0); // 5.E2-9b D3: next file
  // 5.E2-8b: the last open's job is released by its stop or next file; P's start state reset
  TxReleaseJob("next open");
  m_txOpenPending = m_txWordOpened = m_txFullScreen = m_txRootAtOpen = false;
  m_txExitsAtOpen = m_txOpenInputsSeq = m_txGuiSetSnap = 0;
  m_txSegClass = TxSegClass::NONE;
  m_txReopenCandidateGen = 0;
  m_txReopenDelayUntil = {};
  m_discMenuOnly = false;
  m_menuOnlyCandidateSince = {};
}

void CVideoPlayer::Prepare(bool nextFile)
{
  // a start-pts defer window belongs to the previous open's sync attempt
  m_syncStartDeferred = false;
  CFFmpegLog::SetLogLevel(1);
  SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
  m_processInfo->SetSpeed(1.0);
  m_processInfo->SetTempo(1.0);
  m_processInfo->SetFrameAdvance(false);
  m_State.Clear();
  m_txNextFile = nextFile;
  // gate E29B-1 item 5b: a dropped FF/RW/tempo speed was already notified at its request; the
  // NORMAL posted above notifies the return to 1x, as transaction_off's applied speed does
  m_txDroppedSpeed = INT_MIN;
  if (m_txPendingSpeed != INT_MIN && m_txPendingSpeed != DVD_PLAYSPEED_PAUSE &&
      m_txPendingSpeed != DVD_PLAYSPEED_NORMAL && nextFile && IsPlaying())
  {
    m_txDroppedSpeed = m_txPendingSpeed;
    CLog::Log(LOGINFO, "TXSTART pending FF/RW/tempo {} dropped at the next file (the next speed "
                       "change notifies it)",
              m_txDroppedSpeed);
  }
  TxResetStart(false);
  m_CurrentVideo.hint.Clear();
  m_CurrentAudio.hint.Clear();
  m_CurrentSubtitle.hint.Clear();
  m_CurrentTeletext.hint.Clear();
  m_CurrentRadioRDS.hint.Clear();
  m_CurrentAudioID3.hint.Clear();
  m_SpeedState.Reset(DVD_NOPTS_VALUE);
  m_offset_pts = 0;
  m_seamOffsetPts = 0.0;
  ClearSeams();
  m_bdStreamReuseVideo = false;
  m_bdStreamReuseAudio = false;
  m_menuWrapVideoGap = 0.0;
  m_timelineRestartStamped = false;
  // CVideoPlayer is reused across files, so an arm left over from a disc that
  // was stopped at a boundary would otherwise be consumed by the first
  // sub-second forward step of whatever plays next.
  m_seamStepPending = false;
  m_seamStepArmedDts = DVD_NOPTS_VALUE;
  m_stillJoinCorrection = DVD_NOPTS_VALUE;
  m_stillJoinOldEnd = false;
  if (m_menuDomainLowLatency)
  {
    m_menuDomainLowLatency = false;
    m_VideoPlayerAudio->SetMaxTimeSize(m_messageQueueTimeSize);
    m_VideoPlayerVideo->SetMaxTimeSize(m_messageQueueTimeSize);
  }
  m_CurrentAudio.lastdts = DVD_NOPTS_VALUE;
  m_CurrentVideo.lastdts = DVD_NOPTS_VALUE;
  m_discTimelineEvents.clear();
  ResetDiscClockHold();
  m_menuDomainClampSeconds = static_cast<double>(
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoMenuDomainQueueTimeSize);
  aml_set_disc_mode_hold(false);
  // New session: the next held segment re-anchors from its own content.
  aml_set_disc_mode_anchored(false);

  IPlayerCallback *cb = &m_callback;
  CFileItem fileItem = m_item;
  // 5.V3-1 (log only): the landing, timed against this file's first release. "first": an open
  // on a new or closed player; "next": a playlist next or a Player.Open over a playing file,
  // which arrive alike (CDVDMsgOpenFile, options with no origin), so the gate's scenario splits
  // them. "Landed" also when the database did not open and nothing was applied
  const unsigned int fileGen = m_renderManager.FileGen();
  m_renderManager.NoteFileOrigin(fileGen, nextFile, m_playerOptions.fullscreen);
  m_outboundEvents->Submit([=, this]() {
    m_renderManager.SettingsLoadTestStall();
    cb->RequestVideoSettings(fileItem);
    m_renderManager.NoteSettingsLanded(fileGen);
    // 5.V3-7: the landed view mode as a request
    m_renderManager.SettingsLanded(fileGen);
  });

  if (!OpenInputStream())
  {
    m_bAbortRequest = true;
    m_error = true;
    return;
  }

  bool discStateRestored = false;
  if (std::shared_ptr<CDVDInputStream::IMenus> ptr =
          std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
  {
    CLog::Log(LOGINFO, "VideoPlayer: playing a file with menus");

    if (!m_playerOptions.state.empty() && !(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
                                            m_State.menuType == MenuType::NONE))

    {
      discStateRestored = ptr->SetState(m_playerOptions.state);
    }
    else if (std::shared_ptr<CDVDInputStreamNavigator> nav =
                 std::dynamic_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream))
    {
      nav->EnableSubtitleStream(m_processInfo->GetVideoSettings().m_SubtitleOn);
    }
  }

  // A BD-J title can open on a screen that plays no playlist (a disc's own
  // resume prompt, a menu over its background plane). Probing a demuxer then
  // would park this thread in the input stream's idle loop: no key would
  // reach the disc and nothing would be presented. Defer the demuxer to the
  // main loop, which opens it once the disc starts a playlist.
  // the file's first segment is a generation of its own (6.1)
  ++m_segmentGen;
  m_segmentGenPublished = m_segmentGen;
  m_renderManager.SetIncomingSegmentGen(m_segmentGen);

  const bool deferDemux = DiscWaitState(false) != 0;
  if (deferDemux)
    CLog::Log(LOGINFO, "VideoPlayer: disc shows a screen with no playlist, deferring the demuxer");
  else if (!OpenDemuxStream())
  {
    m_bAbortRequest = true;
    m_error = true;
    return;
  }
  ArmStartGate("first set");

  if (m_processInfo)
    m_processInfo->SetStateStreaming(EvaluateIsStreaming());

  // give players a chance to reconsider now codecs are known
  CreatePlayers();

  if (!discStateRestored && !deferDemux)
    OpenDefaultStreams();

  // Update stack and offsets in fileItem (for Blurays/DVDs)
  m_pInputStream->UpdateStack(fileItem);

  /*
   * Check to see if the demuxer should start at something other than time 0. This will be the case
   * if there was a start time specified as part of the "Start from where last stopped" (aka
   * auto-resume) feature or if there is an EDL cut or commercial break that starts at time 0.
   */
  std::chrono::milliseconds starttime = 0ms;
  if (m_playerOptions.starttime > 0 || m_playerOptions.startpercent > 0)
  {
    if (m_playerOptions.startpercent > 0 && m_pDemuxer)
    {
      std::chrono::milliseconds playerStartTime =
          std::chrono::milliseconds(static_cast<int>((static_cast<double>(
              m_pDemuxer->GetStreamLength() * (m_playerOptions.startpercent / 100.0)))));
      starttime = m_Edl.GetTimeAfterRestoringCuts(playerStartTime);
    }
    else
    {
      starttime =
          m_Edl.GetTimeAfterRestoringCuts(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::seconds(static_cast<int>(m_playerOptions.starttime))));
    }
    CLog::Log(LOGDEBUG, "{} - Start position set to last stopped position: {}", __FUNCTION__,
              starttime.count());
  }
  else
  {
    const auto hasEdit = m_Edl.InEdit(starttime);
    if (hasEdit)
    {
      const auto& edit = hasEdit.value();
      // save last edit times
      m_Edl.SetLastEditTime(edit->start);
      m_Edl.SetLastEditActionType(edit->action);

      if (edit->action == EDL::Action::CUT)
      {
        starttime = edit->end;
        CLog::Log(LOGDEBUG, "{} - Start position set to end of first cut: {}", __FUNCTION__,
                  starttime.count());

        // If the cut end lands at the start of a commercial break and auto-skip
        // is enabled, advance past it now
        if (m_SkipCommercials)
        {
          const auto commEdit = m_Edl.InEdit(starttime);
          if (commEdit && commEdit.value()->action == EDL::Action::COMM_BREAK)
          {
            CLog::Log(LOGDEBUG,
                      "{} - Start position advanced past commercial break [{} - {}] to: {}",
                      __FUNCTION__, StringUtils::MillisecondsToTimeString(commEdit.value()->start),
                      StringUtils::MillisecondsToTimeString(commEdit.value()->end),
                      commEdit.value()->end.count());
            m_Edl.SetLastEditTime(commEdit.value()->end);
            m_Edl.SetLastEditActionType(EDL::Action::COMM_BREAK);
            starttime = commEdit.value()->end;
          }
        }
      }
      else if (edit->action == EDL::Action::COMM_BREAK)
      {
        if (m_SkipCommercials)
        {
          starttime = edit->end;
          CLog::Log(LOGDEBUG, "{} - Start position set to end of first commercial break: {}",
                    __FUNCTION__, starttime.count());
        }

        CVariant announcement(
            StringUtils::SecondsToTimeString(edit->end.count(), TIME_FORMAT_MM_SS));
        CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnCommercial",
                                                           announcement);
      }
    }
  }

  if (starttime > 0ms && deferDemux)
  {
    // nothing to seek yet; the disc decides where its playback starts
    CLog::Log(LOGINFO, "{} - start position {} ms not applied: the disc has no playlist yet",
              __FUNCTION__, starttime.count());
  }
  else if (starttime > 0ms)
  {
    double startpts = DVD_NOPTS_VALUE;
    if (m_pDemuxer)
    {
      if (m_pDemuxer->SeekTime(starttime.count(), true, &startpts))
        CLog::Log(LOGDEBUG, "{} - starting demuxer from: {}", __FUNCTION__, starttime.count());
      else
        CLog::Log(LOGDEBUG, "{} - failed to start demuxing from: {}", __FUNCTION__,
                  starttime.count());
    }

    if (m_pSubtitleDemuxer)
    {
      if (m_pSubtitleDemuxer->SeekTime(starttime.count(), true, &startpts))
        CLog::Log(LOGDEBUG, "{} - starting subtitle demuxer from: {}", __FUNCTION__,
                  starttime.count());
      else
        CLog::Log(LOGDEBUG, "{} - failed to start subtitle demuxing from: {}", __FUNCTION__,
                  starttime.count());
    }

    m_clock.Discontinuity(DVD_MSEC_TO_TIME(starttime.count()));
  }

  UpdatePlayState(0);

  SetCaching(CACHESTATE_FLUSH);

  // started while the display is lost: held like the players already open
  // (what it plays is not known yet: a deferred disc demux, the last device)
  m_holdOnDisplayLoss = true;
  // 5.E2-3: the shadow word's open, mirroring this player's loss state (kept across a next file)
  if (aml_transaction_shadow_on())
  {
    const auto mirror =
        static_cast<OUTPUT::COutputWorker::WordState>(WordMirror(m_lossState.load()));
    m_txWordT = OUTPUT::COutputWorker::Get().WordOpen(m_txWordT, mirror);
    m_wordSamplePair = -1;
    m_wordSampleLogged = -1;
  }
  m_txWordOpened = true;
  // 5.E2-8b D2: the first set's OPEN, with the open's T
  if (m_txOpenPending && TxOpen())
    TxOpenStart();
  m_txOpenPending = false;
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  // 5.E2-11 D1.10: no OPEN waits for a span; a P-decided open sees it from the word
  if (TxOpen() && m_txJob && aml_transaction_loss_in_start())
  {
    if (winSystem && winSystem->IsLostSpanOpen())
      CLog::Log(LOGINFO, "TXLOSS row 9 (no body) T={}: opened inside the span", m_txWordT.load());
  }
  else if (winSystem && winSystem->HoldIfDisplayLost(this))
    CLog::Log(LOGINFO, "VideoPlayer: started while the display is lost, held until it is back");
}

void CVideoPlayer::PublishHoldOnDisplayLoss()
{
  IAE* ae = CServiceBroker::GetActiveAE();
  m_holdOnDisplayLoss = m_HasVideo || m_discMenuOnly || !ae || ae->OutputIsHdmi();
}

void CVideoPlayer::ConvertLostDisplayToPause()
{
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem || winSystem->DisconnectedForMs() < 15000 ||
      UserSpeed() == DVD_PLAYSPEED_PAUSE || !CanPause())
    return;
  LossState expected = LossState::LOST;
  if (!m_lossState.compare_exchange_strong(expected, LossState::CONVERTING))
    return;
  // 5.E2-3: T read once per callback, 0 after the stop (E2-3 review M2, L4)
  const uint32_t wordT = aml_transaction_shadow_on() ? m_txWordT.load() : 0;
  if (wordT)
    OUTPUT::COutputWorker::Get().WordConverting(wordT, ToWordHead(WordHeadNow(LossState::CONVERTING)));
  CLog::Log(LOGINFO, "VideoPlayer: display lost for {} ms, now an ordinary pause",
            winSystem->DisconnectedForMs());
  // as a user pause (SetSpeed and PLAYER_SETSPEED), without a hold point: the
  // output's audio was already taken back at the loss. The return waits for
  // these steps only; the callbacks and the PVR come after
  ConvertToPauseSpeeds();
  expected = LossState::CONVERTING;
  if (!m_lossState.compare_exchange_strong(expected, LossState::PAUSED))
    CLog::Log(LOGERROR, "VideoPlayer: the display came back during the pause conversion");
  // an ordinary pause: disc time (BD-J and HDMV timers) runs through it
  m_discTime.SetDisplayLost(false);
  if (wordT)
    OUTPUT::COutputWorker::Get().WordPaused(wordT, ToWordHead(WordHeadNow(LossState::PAUSED)));
  ConvertToPauseCallbacks();
}

// 5.E2-7 (C2): the conversion's two halves, moved out unchanged so the real path's two P steps
// (docs/phase5_e2_7_impl.md D3) run the same code
void CVideoPlayer::ConvertToPauseSpeeds()
{
  m_processInfo->SetNewSpeed(0);
  m_processInfo->SetSpeed(0.0f);
  m_processInfo->SetFrameAdvance(false);
  m_playSpeed = DVD_PLAYSPEED_PAUSE;
  m_txPendingSpeed = INT_MIN; // 5.E2-9b D1.2: one speed owner
  m_caching = CACHESTATE_DONE;
  m_cachingStall = false;
  // the clock is held for the display: this is its speed after the hold
  m_clock.SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_VideoPlayerAudio->SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_VideoPlayerVideo->SetSpeed(DVD_PLAYSPEED_PAUSE);
  m_streamPlayerSpeed = DVD_PLAYSPEED_PAUSE;
}

void CVideoPlayer::ConvertToPauseCallbacks()
{
  IPlayerCallback* cb = &m_callback;
  m_outboundEvents->Submit([=]() { cb->OnPlayBackPaused(); });
  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER))
    std::static_pointer_cast<CInputStreamPVRBase>(m_pInputStream)->Pause(true);
}

void CVideoPlayer::Process()
{
  OUTPUT::SetThreadRole("P");
  // Try to resolve the correct mime type. This can take some time, for example if a requested
  // item is located at a slow/not reachable remote source. So, do mime type detection in vp worker
  // thread, not directly when initializing the player to keep GUI responsible.
  m_item.SetMimeTypeForInternetFile();

#if defined(HAS_LIBAMCODEC)
  TestLegacyRenderLoopClient();
  // 5.V2-5: posted, never waiting on main's render loop
  if (aml_vconfigure_on())
  {
    m_renderLoopHandle = std::make_shared<RenderLoopHandle>();
    m_renderLoopHandle->client = this;
    m_renderLoopHandle->gen = ++m_renderLoopGen;
    m_renderLoopHandle->testRegisterDelay = TestRegisterDelay();
    CServiceBroker::GetWinSystem()->PostRegisterRenderLoop(m_renderLoopHandle);
    CLog::Log(LOGINFO, "RENDERLOOP register posted gen={}", m_renderLoopGen);
  }
  else
#endif
    CServiceBroker::GetWinSystem()->RegisterRenderLoop(this);

  Prepare();

  unsigned int emptyPackets = 0;

  while (!m_bAbortRequest)
  {
    PublishHoldOnDisplayLoss();
    if (aml_transaction_shadow_on())
      SampleWord();
    TxSpanPass();
    TxDeferPass();
    TxMidPlayPass();
    if (m_txMidPath)
      m_renderManager.TxMidShadowPass(m_txMidPathT);
    TxNoVideoStepPass();
    // check display lost
    if (m_displayLost)
    {
      // 5.E2-9a (D4, E28A-8): this branch ends the pass before CheckStartGate, so the loss is
      // marked here for CN's bounds as for the holds' (re-armed on the first pass after it)
      if (TxOpen())
      {
        m_txLostSinceArm = true;
        m_txGateLostSeen = true;
        m_txSettleLostSeen = true; // 5.E2-9c D5
      }
      // 5.E2-8b D6: the backstop is paused while P is lost
      if (TxOpen() && m_txJob && m_txStep == TxStep::POSTED &&
          m_txLostAt == std::chrono::steady_clock::time_point{})
        m_txLostAt = std::chrono::steady_clock::now();
      TxLostFailedPass();
      // 5.E2-7 (D3): a real freeze's states; P keeps its stop, a next file in a span and the wake
      if (m_txFreezeReal.load(std::memory_order_acquire))
      {
        TxFrozenPass();
        continue;
      }
      // 5.E2-7 (code review L3): a legacy freeze leaves no real-path pass state behind
      TxResetPassState();
      ConvertLostDisplayToPause();
      // nor can a menu page waiting for its picture
      if (m_menuPageWaiting)
        m_menuPageWaitSince = std::chrono::steady_clock::now();
      // the players are paused: a start held at a boundary cannot progress
      if (m_boundaryStartWaitSince != std::chrono::steady_clock::time_point{})
        m_boundaryStartWaitSince = std::chrono::steady_clock::now();
      CThread::Sleep(50ms);
      continue;
    }
    // 5.E2-9b D1.4 (r7 L3, L5): a speed an exit kept while lost, applied on the first pass back,
    // before anything below reads the speed
    if (m_txPendingSpeed != INT_MIN && !TxOpen())
    {
      const int pending = m_txPendingSpeed;
      if (m_startHeld)
      {
        // the release applies it (HandleSetSpeed's held start)
        m_playSpeed = pending;
        m_txPendingSpeed = INT_MIN;
        CLog::Log(LOGERROR, "TXSTART speed {} after the display return kept for the release of "
                            "the held start", pending);
      }
      else
      {
        ApplySpeed(pending);
        CLog::Log(LOGINFO, "TXSTART speed {} applied after the display return (caching {})",
                  pending, TxCachingName(m_caching));
      }
    }

#if defined(HAVE_LIBBLURAY)
    // display came back from a mode switch (OnResetDisplay): a still disc menu
    // presents no new video frames, so its retained overlay composition is
    // never re-composed after the reinit - repost it (no-op without overlays;
    // replace-on-add dedups one that survived)
    if (m_repostDiscOverlays.exchange(false))
    {
      if (std::shared_ptr<CDVDInputStreamBluray> bluray =
              std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
        bluray->RedrawMenuOverlays();
    }
#endif

    // the stall check would take a scheduled display resume's lead for a stall
    if (m_displayResumeScheduled.exchange(false))
      m_syncTimer.Set(1350ms);
    if (m_audioOffsetToAudio)
      ApplyAudioOffset();

    // check if in an edit (cut or commercial break) that should be automatically skipped
    CheckAutoSceneSkip();

    // handle messages send to this thread, like seek or demuxer reset requests
    HandleMessages();

    if (m_bAbortRequest)
      break;

    TxSupersededBound(); // 5.E2-10 §3

    // should we open a new input stream?
    if (!m_pInputStream)
    {
      if (OpenInputStream() == false)
      {
        m_bAbortRequest = true;
        break;
      }
    }

    // should we open a new demuxer?
    if (!m_pDemuxer)
    {
      // a disc screen with no playlist: keep handling input and presenting
      // the disc's graphics until it starts one (see Prepare)
      if (const int waitState = DiscWaitState(true))
      {
        // 5.E2-12b D2 point (1): the wait (1 or 2) for 3 s inside a start is row 3b (Q3's own timer)
        if (aml_disc_screen_hold() && TxNoVideoInStart() && !m_txNoVideoCapped)
        {
          m_txNoVideoHold = true;
          TxNoVideoNoteWire();
          const auto now = std::chrono::steady_clock::now();
          if (m_txNoVideoSince == std::chrono::steady_clock::time_point{})
          {
            m_txNoVideoSince = now;
            CLog::Log(LOGINFO, "TXSTART T={} row 3b grace starts (wait state {})",
                      m_txWordT.load(), waitState);
          }
          else if (!m_txNoVideoStep && now - m_txNoVideoSince >= 3s)
          {
            m_txNoVideoStep = "wait";
            m_txNoVideoWaitState = waitState;
          }
        }
        // 5.E2-8a (D5): a BD-J screen with no playlist (1) before START
        else if (waitState == 1 && TxOpen())
          EndTransactionToLegacy("no-playlist screen", CRenderManager::TxHow::OWED,
                                 CRenderManager::TxNeed::LATER);
        // 5.E2-12b-2 D4 point (2): during playback (no start open or carried)
        else if (!TxOpen() && !m_txNoVideoCarry)
          TxRow3cPoint(2);
        // a screen with no playlist: no picture will come for a waiting page
        if (m_menuPageWaiting)
        {
          m_overlayContainer.ReleasePendingMenu();
          m_menuPageWaiting = false;
        }
        UpdatePlayState(200);
        CheckMenuOnlyStart(waitState == 1);
        // a gate still shut from the last set keeps its bound here
        CheckStartGate();
        CThread::Sleep(20ms);
        continue;
      }
      m_menuOnlyCandidateSince = {};
      if (!m_txNoVideoStep)
        m_txNoVideoSince = {};

      // transaction_reopendelay (5.E2-10): the OPEN after a supersede waits, P keeps running
      if (m_txReopenDelayUntil != std::chrono::steady_clock::time_point{})
      {
        if (std::chrono::steady_clock::now() < m_txReopenDelayUntil)
        {
          UpdatePlayState(200);
          CThread::Sleep(20ms);
          continue;
        }
        m_txReopenDelayUntil = {};
      }

      if (m_pInputStream->NextStream() == CDVDInputStream::NEXTSTREAM_NONE)
        break;

      if (m_pInputStream->IsEOF())
        break;

      if (OpenDemuxStream() == false)
      {
        m_bAbortRequest = true;
        break;
      }
      if (m_pInputBluray)
        ArmStartGate("Blu-ray set");

      if (m_processInfo)
        m_processInfo->SetStateStreaming(EvaluateIsStreaming());

      // on channel switch we don't want to close stream players at this
      // time. we'll get the stream change event later
      if (!m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) ||
          !m_SelectionStreams.m_Streams.empty())
        OpenDefaultStreams();
      // gate hook transaction_novideo: its forced set's video stream is not kept open
      const bool hookNoVideo = std::exchange(m_txNoVideoForced, false);
      if (hookNoVideo && m_CurrentVideo.id >= 0)
        CloseStream(m_CurrentVideo, false);
      // a segment without video: no picture replaces a kept one
      if (m_SelectionStreams.CountType(StreamType::VIDEO) == 0 || hookNoVideo)
      {
        aml_drop_kept_frame("segment without video");
        // 5.E2-12b D2 point (2): at once (a DISCARD_KEEPALIVE reaches only this point, E2)
        if (m_txNoVideoHold && !m_txNoVideoStep)
          m_txNoVideoStep = "novideo";
        // 5.E2-12b-2 D4 point (3): during playback
        else if (!m_txNoVideoHold && !TxOpen() && !m_txNoVideoCarry)
          TxRow3cPoint(3);
      }
      // a segment without video: no picture will come for a waiting page
      if (m_menuPageWaiting && m_SelectionStreams.CountType(StreamType::VIDEO) == 0)
      {
        m_overlayContainer.ReleasePendingMenu();
        m_menuPageWaiting = false;
      }

#if defined(HAVE_LIBBLURAY)
      // stream reopens keep non-flushable menu overlays alive, but repost the
      // disc menu composition anyway in case it was lost (libbluray only
      // resends graphics when they change; the replace-on-add logic dedups a
      // composition that did survive)
      if (std::shared_ptr<CDVDInputStreamBluray> bluray =
              std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
        bluray->RedrawMenuOverlays();
#endif

      UpdatePlayState(0);
    }

    // handle eventual seeks due to playspeed
    HandlePlaySpeed();
    // 5.E2-8a (D1, r2 M4): RM's late verdict, read outside HEAD's freeze
    if (TxOpen())
      TxCheckVerdict("loop");
    // code review L3: the holds' bounds do not count a loss inside the open start
    if (TxOpen() && (m_txWordStarting || (m_txJob && m_txDiscHeld)) && m_txLostSinceArm)
    {
      m_txLostSinceArm = false;
      if (m_txDiscHeld)
        m_discTime.SetTxHeld(true);
      m_renderManager.ArmTxStartHeldBound();
    }
    CheckHeldStart();
    CheckStartGate();
    TxHintPictureCheck("pass"); // 5.E2-9c D4
    m_discTime.CheckBounds();
    PublishSegmentGen();
    CheckMenuPageWait();

    // update player state
    UpdatePlayState(200);

    HandleDynamicBufferLevel();

    // restore full queue read-ahead as soon as menu domain ends (grow-only)
    UpdateMenuDomainQueueDepth(false);
    ApplyDiscTimelineEvents(false);

    // make sure we run subtitle process here
    m_VideoPlayerSubtitle->Process(m_clock.GetClock() + m_State.time_offset - m_VideoPlayerVideo->GetSubtitleDelay(), m_State.time_offset);

    // Must precede the read gate below: that gate is what a dead player turns
    // into a deadlock.
    CheckStreamPlayerAlive(m_CurrentAudio, m_VideoPlayerAudio.get(), m_audioPlayerRestarts,
                           m_audioPlayerDeadPolls, "audio");
    CheckStreamPlayerAlive(m_CurrentVideo, m_VideoPlayerVideo.get(), m_videoPlayerRestarts,
                           m_videoPlayerDeadPolls, "video");

    // tell demuxer if we want to fill buffers
    if (m_demuxerSpeed != DVD_PLAYSPEED_PAUSE)
    {
      int audioLevel = 90;
      int videoLevel = 90;
      bool fillBuffer = false;
      if (m_CurrentAudio.id >= 0)
        audioLevel = m_VideoPlayerAudio->GetLevel();
      if (m_CurrentVideo.id >= 0)
        videoLevel = m_processInfo->GetLevelVQ();
      if (videoLevel < 85 && audioLevel < 85)
      {
        fillBuffer = true;
      }
      if (m_pDemuxer)
        m_pDemuxer->FillBuffer(fillBuffer);
    }

    // 5.E2-9c D1: a held start keeps reading for a starving video decoder
    TxReadOnPass();
    // if the queues are full, no need to read more
    const bool audioFull = !m_VideoPlayerAudio->AcceptsData() && m_CurrentAudio.id >= 0;
    const bool videoFull = !m_VideoPlayerVideo->AcceptsData() && m_CurrentVideo.id >= 0;
    m_txReadOnBypassed = audioFull && !videoFull && TxReadOnBypass();
    if ((audioFull || videoFull) && !m_txReadOnBypassed)
    {
#if defined(HAVE_LIBBLURAY)
      // a BD-J application acting on a key press (stop this playlist, play
      // another) must not wait behind the whole buffer: take its events now.
      // A playlist stop posts GENERAL_FLUSH, handled on the next iteration.
      if (m_pInputBluray)
        m_pInputBluray->PollEvents();
#endif
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE &&
          m_demuxerSpeed != DVD_PLAYSPEED_PAUSE)
      {
        if (m_pDemuxer)
          m_pDemuxer->SetSpeed(DVD_PLAYSPEED_PAUSE);
        m_demuxerSpeed = DVD_PLAYSPEED_PAUSE;
      }
      CThread::Sleep(10ms);
      continue;
    }

    // adjust demuxer speed; some rtsp servers wants to know for i.e. ff
    // delay pause until queue is full
    if (m_playSpeed != DVD_PLAYSPEED_PAUSE &&
        m_demuxerSpeed != m_playSpeed)
    {
      if (m_pDemuxer)
        m_pDemuxer->SetSpeed(m_playSpeed);
      m_demuxerSpeed = m_playSpeed;
    }

#if defined(HAVE_LIBBLURAY)
    // Seamless playitem seams no longer stop the read (see
    // CDVDInputStreamBluray::SetSeamlessGlideAllowed): the boundary arrives
    // here as a flag rather than as a NULL packet, so that libavformat never
    // sees an i/o error mid-picture and the incoming clip's first access unit
    // survives intact. Publish the player's half of the glide test first -
    // the video stream has to be open and in sync, or the transition would be
    // classified as something other than SEAMLESS and we must not have
    // committed to the incoming clip's bytes.
    if (m_pInputBluray)
    {
      m_pInputBluray->SetSeamlessGlideAllowed(
          m_CurrentVideo.id >= 0 &&
          (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC ||
           (m_seamByPos && !m_pInputBluray->IsNavigationMode()) || m_earlyGlide));
      if (m_pInputBluray->TakePendingSeamlessTransition())
      {
        // Re-classified here, one iteration after the glide armed it: if the
        // pipeline changed underneath us (player death, a menu crossing, the
        // user pressing Menu) this is no longer a SEAMLESS transition and
        // BdSegmentTransition closes the demuxer. Never fall through to
        // ReadPacket after that - the NULL packet it returns would reach
        // NextStream() and run the whole transition a second time.
        // Direct playlist playback has no navigation to reopen from, so a
        // teardown there would end playback: only the seamless transition runs.
        if (!m_pInputBluray->IsNavigationMode() &&
            ClassifyBdTransition(true) != EBdTransition::SEAMLESS)
        {
          CLog::Log(LOGDEBUG, "VideoPlayer: glided seam no longer seamless - ignored");
        }
        else
        {
          BdSegmentTransition(true);
          continue;
        }
      }
    }
#endif

    DemuxPacket* pPacket = NULL;
    CDemuxStream *pStream = NULL;
    ReadPacket(pPacket, pStream);
    if (pPacket && !pStream)
    {
      /* probably a empty packet, just free it and move on */
      CDVDDemuxUtils::FreeDemuxPacket(pPacket);

      /* a source that cannot keep up hands these back as fast as we ask */
      if (++emptyPackets > 10)
        CThread::Sleep(10ms);

      continue;
    }

    emptyPackets = 0;

    if (!pPacket)
    {
      // when paused, demuxer could be returning empty (at the end of
      // the input, or at a BD-J end): pace the retry, as a full queue is
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
      {
        CThread::Sleep(10ms);
        continue;
      }

      // check for a still frame state
      if (std::shared_ptr<CDVDInputStream::IMenus> pStream = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
      {
        // stills will be skipped
        if(m_dvd.state == DVDSTATE_STILL)
        {
          if (m_dvd.iDVDStillTime > 0ms)
          {
            const auto now = std::chrono::steady_clock::now();
            const auto duration = now - m_dvd.iDVDStillStartTime;

            if (duration >= m_dvd.iDVDStillTime)
            {
              m_dvd.iDVDStillTime = 0ms;
              m_dvd.iDVDStillStartTime = {};
              m_dvd.state = DVDSTATE_NORMAL;
              pStream->SkipStill();
              continue;
            }
          }
        }
      }

      if (WaitStartAtBoundary())
      {
        CThread::Sleep(10ms);
        continue;
      }

#if defined(HAVE_LIBBLURAY)
      // the demuxer ended at a BD-J playlist's end: everything read is
      // delivered, and its END_OF_PLAYLIST can come due (design 15.54)
      const bool bdjAtEnd = m_pInputBluray && m_pInputBluray->BdjReaderAtEnd();
      if (bdjAtEnd)
        m_bdjAllDelivered = true;
#else
      const bool bdjAtEnd = false;
#endif

#if defined(HAVE_LIBBLURAY)
      // 5.E2-9c D1 (r3 M4): the VM's transition waits behind the full audio queue, as today
      if (m_txReadOnBypassed && m_pInputBluray && m_pInputBluray->IsNavigationMode())
      {
        TxReadOnEnd("segment boundary");
        CThread::Sleep(10ms);
        continue;
      }
#endif

      // if there is another stream available, reopen demuxer
      CDVDInputStream::ENextStream next = m_pInputStream->NextStream();
      if(next == CDVDInputStream::NEXTSTREAM_OPEN)
      {
        BdSegmentTransition();
        continue;
      }

      // input stream asked us to just retry (at a BD-J end: the timeline and
      // the presentation run in this loop meanwhile)
      if(next == CDVDInputStream::NEXTSTREAM_RETRY)
      {
        CThread::Sleep(bdjAtEnd ? 10ms : 100ms);
        continue;
      }

      if (m_CurrentVideo.inited)
      {
        m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::VIDEO_DRAIN));
      }

      m_CurrentAudio.inited = false;
      m_CurrentVideo.inited = false;
      m_CurrentSubtitle.inited = false;
      m_CurrentTeletext.inited = false;
      m_CurrentRadioRDS.inited = false;
      m_CurrentAudioID3.inited = false;

      // if we are caching, start playing it again
      SetCaching(CACHESTATE_DONE);

      // while players are still playing, keep going to allow seekbacks
      if (m_VideoPlayerAudio->HasData() ||
          m_VideoPlayerVideo->HasData())
      {
        CThread::Sleep(100ms);
        continue;
      }

      if (!m_pInputStream->IsEOF())
        CLog::Log(LOGINFO, "{} - eof reading from demuxer", __FUNCTION__);

      break;
    }

    // see if we can find something better to play
    CheckBetterStream(m_CurrentAudio,    pStream);
    // a DV enhancement-layer packet must never promote the EL substream to
    // the main video stream (the codec merges EL into BL; attaching the
    // player to the EL kills BL delivery entirely = black screen)
    if (!pPacket->isELPackage)
      CheckBetterStream(m_CurrentVideo, pStream);
    CheckBetterStream(m_CurrentSubtitle, pStream);
    CheckBetterStream(m_CurrentTeletext, pStream);
    CheckBetterStream(m_CurrentRadioRDS, pStream);
    CheckBetterStream(m_CurrentAudioID3, pStream);

    // if it's a video packet then set the 3D MVC subtitle plane
    if (CheckIsCurrent(m_CurrentVideo, pStream, pPacket)) {
      pPacket->subtitlePlane = m_CurrentSubtitle.hint.m_3dSubtitlePlane;
    }

    // demux video stream
    if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_SUBTITLES_PARSECAPTIONS) && CheckIsCurrent(m_CurrentVideo, pStream, pPacket))
    {
      if (m_pCCDemuxer)
      {
        bool first = true;
        while (!m_bAbortRequest)
        {
          DemuxPacket *pkt = m_pCCDemuxer->Read(first ? pPacket : NULL);
          if (!pkt)
            break;

          first = false;
          if (m_pCCDemuxer->GetNrOfStreams() !=
              m_SelectionStreams.CountTypeOfSource(StreamType::SUBTITLE, STREAM_SOURCE_VIDEOMUX))
          {
            m_SelectionStreams.Clear(StreamType::SUBTITLE, STREAM_SOURCE_VIDEOMUX);
            m_SelectionStreams.Update(NULL, m_pCCDemuxer.get(), "");
            UpdateContent();
            OpenDefaultStreams(false);
          }
          CDemuxStream *pSubStream = m_pCCDemuxer->GetStream(pkt->iStreamId);
          if (pSubStream && m_CurrentSubtitle.id == pkt->iStreamId && m_CurrentSubtitle.source == STREAM_SOURCE_VIDEOMUX)
            ProcessSubData(pSubStream, pkt);
          else
            CDVDDemuxUtils::FreeDemuxPacket(pkt);
        }
      }
    }

    if (IsInMenuInternal())
    {
      if (std::shared_ptr<CDVDInputStream::IMenus> menu = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
      {
        double correction = menu->GetTimeStampCorrection();
        if (pPacket->dts != DVD_NOPTS_VALUE && pPacket->dts > correction)
          pPacket->dts -= correction;
        if (pPacket->pts != DVD_NOPTS_VALUE && pPacket->pts > correction)
          pPacket->pts -= correction;
      }
      if (m_dvd.syncClock)
      {
        m_clock.Discontinuity(pPacket->dts);
        m_dvd.syncClock = false;
      }
    }

    // process the packet
    ProcessPacket(pStream, pPacket);
  }
}

bool CVideoPlayer::CheckIsCurrent(const CCurrentStream& current,
                                  CDemuxStream* stream,
                                  DemuxPacket* pkg)
{
  if(current.id == pkg->iStreamId &&
     current.demuxerId == stream->demuxerId &&
     current.source == stream->source &&
     current.type == stream->type)
    return true;
  else
    return false;
}

void CVideoPlayer::ProcessPacket(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  // data again: the end (design 15.54) is not reached yet
  m_bdjAllDelivered = false;
  // marks waiting for their clip's packets: this one may be it (design 15.55);
  // after the stream's own processing, whose correction it carries
  const int64_t packetPos = pPacket->streamPos;
  const uint32_t packetGen = pPacket->streamGen;
  struct ResolveAfter
  {
    CVideoPlayer* player;
    int64_t pos;
    uint32_t gen;
    ~ResolveAfter()
    {
      if (player->m_bdjMarksPending)
        player->ResolvePendingMarks(pos, gen);
    }
  } resolveAfter{this, packetPos, packetGen};
  // process packet if it belongs to selected stream.
  // for dvd's don't allow automatic opening of streams*/

  if (CheckIsCurrent(m_CurrentAudio, pStream, pPacket))
    ProcessAudioData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentVideo, pStream, pPacket))
  {
    if (!m_pendingElPackets.empty())
      SendPendingElPackets(pPacket);
    ProcessVideoData(pStream, pPacket);
  }
  else if (CheckIsCurrent(m_CurrentSubtitle, pStream, pPacket))
    ProcessSubData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentTeletext, pStream, pPacket))
    ProcessTeletextData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentRadioRDS, pStream, pPacket))
    ProcessRadioRDSData(pStream, pPacket);
  else if (CheckIsCurrent(m_CurrentAudioID3, pStream, pPacket))
    ProcessAudioID3Data(pStream, pPacket);
  else if (pPacket->isELPackage)
  {
    CLog::Log(LOGDEBUG, "CVideoPlayer::ProcessPacket packet from enhancement layer: size:{:d} dts:{:.3f} pts:{:.3f} dur:{:.3f}ms",
      pPacket->iSize, pPacket->dts/DVD_TIME_BASE, pPacket->pts/DVD_TIME_BASE, pPacket->duration/1000.0);
    // a disc clip's EL can be muxed ahead of its first BL: before the BL opens
    // the video stream the queue is closed, and losing the IDR's EL cost the
    // clip its first GOP (13.u3)
    if (!m_VideoPlayerVideo->IsInited())
    {
      m_pendingElPackets.push_back(pPacket);
      if (m_pendingElPackets.size() > 48)
      {
        CLog::Log(LOGWARNING, "CVideoPlayer: more than 48 EL packets before the video stream "
                  "opened, the oldest ({:.3f}) is dropped",
                  m_pendingElPackets.front()->dts / DVD_TIME_BASE);
        CDVDDemuxUtils::FreeDemuxPacket(m_pendingElPackets.front());
        m_pendingElPackets.pop_front();
      }
      return;
    }
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, false));
  }
  else
  {
    CDVDDemuxUtils::FreeDemuxPacket(pPacket); // free it since we won't do anything with it
  }
}

void CVideoPlayer::SendPendingElPackets(const DemuxPacket* firstBl)
{
  if (!m_VideoPlayerVideo->IsInited())
    return;
  const double blTime = firstBl->dts != DVD_NOPTS_VALUE ? firstBl->dts : firstBl->pts;
  int sent = 0;
  int dropped = 0;
  for (DemuxPacket* el : m_pendingElPackets)
  {
    const double elTime = el->dts != DVD_NOPTS_VALUE ? el->dts : el->pts;
    // only those on the first BL's timeline
    if (blTime != DVD_NOPTS_VALUE && elTime != DVD_NOPTS_VALUE &&
        elTime >= blTime - DVD_MSEC_TO_TIME(100) && elTime <= blTime + DVD_MSEC_TO_TIME(2000))
    {
      m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(el, false));
      ++sent;
    }
    else
    {
      CDVDDemuxUtils::FreeDemuxPacket(el);
      ++dropped;
    }
  }
  m_pendingElPackets.clear();
  CLog::Log(LOGINFO, "CVideoPlayer: {} EL packets from before the video stream opened sent ahead "
            "of its first BL ({:.3f}), {} dropped", sent, blTime / DVD_TIME_BASE, dropped);
}

void CVideoPlayer::ClearPendingElPackets()
{
  for (DemuxPacket* el : m_pendingElPackets)
    CDVDDemuxUtils::FreeDemuxPacket(el);
  m_pendingElPackets.clear();
}

void CVideoPlayer::CheckStreamChanges(CCurrentStream& current, CDemuxStream* stream)
{
  if (current.stream  != (void*)stream
  ||  current.changes != stream->changes)
  {
    /* check so that dmuxer hints or extra data hasn't changed */
    /* if they have, reopen stream */

    if (current.hint != CDVDStreamInfo(*stream, true))
    {
      m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_DEMUX);
      m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
      UpdateContent();
      OpenDefaultStreams(false);
    }

    current.stream = (void*)stream;
    current.changes = stream->changes;
  }
}

void CVideoPlayer::ProcessAudioData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentAudio, pStream);

  // 13.u: the first audio pts of a stream, against its clip's IN time
  if (m_CurrentAudio.dts == DVD_NOPTS_VALUE)
    CLog::Log(LOGINFO, "CVideoPlayer::ProcessAudioData - first audio packet pts {:.3f}",
              pPacket->pts / DVD_TIME_BASE);

  bool checkcont = CheckContinuity(m_CurrentAudio, pPacket);
  UpdateTimestamps(m_CurrentAudio, pPacket);
  // with no video, a BD-J mark converts with the audio (design 15.55)
  if (pPacket->dts != DVD_NOPTS_VALUE && pPacket->streamPos >= 0 && pPacket->rawDts != INT64_MIN)
  {
    m_CurrentAudio.markRefDts = pPacket->dts;
    m_CurrentAudio.markRefRawDts = pPacket->rawDts;
    m_CurrentAudio.markRefPos = pPacket->streamPos;
    m_CurrentAudio.markRefGen = pPacket->streamGen;
  }
  if (pPacket->pts != DVD_NOPTS_VALUE)
  {
    const double end = pPacket->pts + (pPacket->duration > 0 ? pPacket->duration : 0.0);
    if (m_segmentAudioPtsEnd == DVD_NOPTS_VALUE || end > m_segmentAudioPtsEnd)
      m_segmentAudioPtsEnd = end;
  }

  if (checkcont && (m_CurrentAudio.avsync == CCurrentStream::AV_SYNC_CHECK))
    m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;

  bool drop = false;
  if (CheckPlayerInit(m_CurrentAudio))
    drop = true;

  /*
   * If CheckSceneSkip() returns true then demux point is inside an EDL cut and the packets are dropped.
   */
  if (CheckSceneSkip(m_CurrentAudio))
  {
    drop = true;
  }
  else
  {
    const auto hasEdit = m_Edl.InEdit(GetEdlTime(m_CurrentAudio));
    if (hasEdit && hasEdit.value()->action == EDL::Action::MUTE)
      drop = true;
  }
  CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayer::ProcessAudioData size:{:d} dts:{:.3f} pts:{:.3f} dur:{:.3f}ms, clock:{:.3f} drop:{:d} level:{:d}",
    pPacket->iSize, pPacket->dts/DVD_TIME_BASE, pPacket->pts/DVD_TIME_BASE, pPacket->duration/1000.0, m_clock.GetClock() / DVD_TIME_BASE, drop, m_VideoPlayerAudio->GetLevel());

  // issue 27: the first sound sent on after a starvation freeze, where its resume starts: its
  // time after the continuity corrections (a seam, a gap, a resync), the dts when it has no pts
  if (m_starveResumeArmed && m_starveAudioNext == DVD_NOPTS_VALUE && !drop)
    m_starveAudioNext = pPacket->pts != DVD_NOPTS_VALUE ? pPacket->pts : pPacket->dts;
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if (!drop)
    m_CurrentAudio.packets++;
}

void CVideoPlayer::ProcessVideoData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentVideo, pStream);
  bool checkcont = false;

  if( pPacket->iSize != 4) //don't check the EOF_SEQUENCE of stillframes
  {
    m_videoKeptUnconfirmed = false;
    checkcont = CheckContinuity(m_CurrentVideo, pPacket);
    UpdateTimestamps(m_CurrentVideo, pPacket);
    // the segment's last picture (design 15.54), on the player's timeline
    if (pPacket->pts != DVD_NOPTS_VALUE && !m_videoKeptUnconfirmed)
    {
      const double end = pPacket->pts + (pPacket->duration > 0 ? pPacket->duration : 0.0);
      if (m_segmentVideoPtsEnd == DVD_NOPTS_VALUE || end > m_segmentVideoPtsEnd)
        m_segmentVideoPtsEnd = end;
    }
    // the packet a BD-J mark read now converts with: its correction is final
    // here (CheckContinuity ran), unless it is an unconfirmed jump's keyframe
    if (pPacket->dts != DVD_NOPTS_VALUE && pPacket->streamPos >= 0 &&
        pPacket->rawDts != INT64_MIN && !m_videoKeptUnconfirmed)
    {
      m_CurrentVideo.markRefDts = pPacket->dts;
      m_CurrentVideo.markRefRawDts = pPacket->rawDts;
      m_CurrentVideo.markRefPos = pPacket->streamPos;
      m_CurrentVideo.markRefGen = pPacket->streamGen;
    }
  }
  if (checkcont && (m_CurrentVideo.avsync == CCurrentStream::AV_SYNC_CHECK))
    m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;

  bool drop = false;
  if (CheckPlayerInit(m_CurrentVideo))
    drop = true;

  if (CheckSceneSkip(m_CurrentVideo))
    drop = true;

#if defined(HAS_LIBAMCODEC)
  // GT-D15: per packet on P, through the video path's log queue (no I/O on P)
  GRAPHICSTICK::GTLogF(LOGDEBUG, "CVideoPlayer::ProcessVideoData size:{:d} dts:{:.3f} pts:{:.3f} dur:{:.3f}ms, clock:{:.3f} level:{:d}",
    pPacket->iSize, pPacket->dts/1000000, pPacket->pts/1000000, pPacket->duration/1000.0,
    m_clock.GetClock()/1000000.0,m_processInfo->GetLevelVQ());
#else
  CLog::Log(LOGDEBUG, "CVideoPlayer::ProcessVideoData size:{:d} dts:{:.3f} pts:{:.3f} dur:{:.3f}ms, clock:{:.3f} level:{:d}",
    pPacket->iSize, pPacket->dts/1000000, pPacket->pts/1000000, pPacket->duration/1000.0,
    m_clock.GetClock()/1000000.0,m_processInfo->GetLevelVQ());
#endif
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if (!drop)
    m_CurrentVideo.packets++;
}

void CVideoPlayer::ProcessSubData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentSubtitle, pStream);

  // Blu-ray PGS packets can reach us with no usable timestamp (libbluray drops
  // the PTS on some discs). With neither pts nor dts, GetAbsoluteTimes leaves the
  // overlay start at the decoder's ~0 relative value; that start then caps the
  // PREVIOUS caption's stop time to ~0 in the overlay container, where stop == 0
  // is the "no end, persist until the next one" sentinel (see
  // CDVDOverlayContainer::CleanUp) - so the caption stays on screen until the
  // next one appears. Anchor a timestamp-less BD subtitle packet to the current
  // subtitle clock LOCALLY so the container caps the previous caption to a real,
  // non-zero stop. This is the subtitle-local correction that intentionally
  // avoids touching the global m_offset_pts (whose cross-stream subtitle
  // correction whipsawed A/V sync and was reverted). When the PGS decoder has
  // its own pts, CDVDOverlayCodecFFmpeg's pts_offset logic self-corrects this
  // anchor back to the exact decoder pts, so nothing is lost when it isn't
  // needed; menu (interactive-graphics) overlays never take this path.
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) &&
      pPacket->pts == DVD_NOPTS_VALUE && pPacket->dts == DVD_NOPTS_VALUE)
  {
    // The player clock is on the packets' pts timeline. m_State.time_offset is
    // display time minus pts (UpdatePlayState); adding it stamped the anchor in
    // DISPLAY time, which only matched on discs whose timestamps start at 0 -
    // on one starting at 4200 s the caption landed 4200 s from where it belongs.
    pPacket->pts = m_clock.GetClock();
  }

  UpdateTimestamps(m_CurrentSubtitle, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentSubtitle))
    drop = true;

  if (CheckSceneSkip(m_CurrentSubtitle))
    drop = true;

  m_VideoPlayerSubtitle->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));

  if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_NORMAL);
}

void CVideoPlayer::ProcessTeletextData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentTeletext, pStream);

  UpdateTimestamps(m_CurrentTeletext, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentTeletext))
    drop = true;

  if (CheckSceneSkip(m_CurrentTeletext))
    drop = true;

  m_VideoPlayerTeletext->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

void CVideoPlayer::ProcessRadioRDSData(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentRadioRDS, pStream);

  UpdateTimestamps(m_CurrentRadioRDS, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentRadioRDS))
    drop = true;

  if (CheckSceneSkip(m_CurrentRadioRDS))
    drop = true;

  m_VideoPlayerRadioRDS->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

void CVideoPlayer::ProcessAudioID3Data(CDemuxStream* pStream, DemuxPacket* pPacket)
{
  CheckStreamChanges(m_CurrentAudioID3, pStream);

  UpdateTimestamps(m_CurrentAudioID3, pPacket);

  bool drop = false;
  if (CheckPlayerInit(m_CurrentAudioID3))
    drop = true;

  if (CheckSceneSkip(m_CurrentAudioID3))
    drop = true;

  m_VideoPlayerAudioID3->SendMessage(std::make_shared<CDVDMsgDemuxerPacket>(pPacket, drop));
}

CacheInfo CVideoPlayer::GetCachingTimes()
{
  CacheInfo info{};

  if (!m_pInputStream || !m_pDemuxer)
    return info;

  XFILE::SCacheStatus status;
  if (!m_pInputStream->GetCacheStatus(&status))
    return info;

  const uint64_t& maxforward = status.maxforward;
  const uint64_t& cached = status.forward;
  const uint32_t& currate = status.currate;
  const uint32_t& maxrate = status.maxrate;
  const uint32_t& lowrate = status.lowrate;

  int64_t length = m_pInputStream->GetLength();
  int64_t remain = length - m_pInputStream->Seek(0, SEEK_CUR);

  if (length <= 0 || remain < 0)
    return info;

  double queueTime = GetQueueTime();
  double play_sbp = DVD_MSEC_TO_TIME(m_pDemuxer->GetStreamLength()) / length;
  double queued = 1000.0 * queueTime / play_sbp;

  info.level = 0.0;
  info.offset = (cached + queued) / length;
  info.time = 0.0;
  info.valid = true;

  if (currate == 0)
    return info;

  // estimated playback time of current cached bytes
  const double cacheTime = (static_cast<double>(cached) / currate) + (queueTime / 1000.0);

  // cache level as current forward bytes / max forward bytes [0.0 - 1.0]
  const double cacheLevel = (maxforward > 0) ? static_cast<double>(cached) / maxforward : 0.0;

  info.time = cacheTime;

  if (lowrate > 0)
  {
    // buffer is full & our read rate is too low
    CLog::Log(LOGDEBUG, "Readrate {} was too low with {} required", lowrate, maxrate);
    info.level = -1.0;
  }
  else
    info.level = cacheLevel;

  return info;
}

bool CVideoPlayer::ShouldDeferSync(bool ready, std::chrono::steady_clock::time_point now)
{
  // EOF can report a NOPTS start while the video worker keeps decoding. Ordinary
  // PLAYER_STARTED messages put the worker in WAITSYNC, where waiting cannot help.
  if (!ready || !m_CurrentVideo.starttimePending || m_CurrentAudio.starttime != DVD_NOPTS_VALUE ||
      m_CurrentVideo.starttime != DVD_NOPTS_VALUE)
  {
    m_syncStartPtsWait.reset();
    return false;
  }

  if (!m_syncStartPtsWait)
    m_syncStartPtsWait = now;
  else if (now - *m_syncStartPtsWait >= 2000ms)
  {
    m_syncStartPtsWait.reset();
    CLog::Log(LOGWARNING,
              "VideoPlayer::Sync - no valid start pts after 2000ms, anchoring clock anyway");
    return false;
  }
  return true;
}

void CVideoPlayer::HandlePlaySpeed()
{
  const bool isInMenu = IsInMenuInternal();
  const bool tolerateStall =
      isInMenu || (m_CurrentVideo.hint.flags & StreamFlags::FLAG_STILL_IMAGES);
  // at a BD-J playlist's end the stream has ended, not starved (design 15.54):
  // no stall detection; a start's caching is left alone
#if defined(HAVE_LIBBLURAY)
  const bool streamEnded = m_pInputBluray && m_pInputBluray->BdjReaderAtEnd();
#else
  const bool streamEnded = false;
#endif

  if (tolerateStall && m_caching != CACHESTATE_DONE)
    SetCaching(CACHESTATE_DONE);
  // issue 27 (log only): whether a starvation now would be a stall, for the coordinator
  PRESENTATION::PlayerReads().stallEligible.store(
      m_playSpeed == DVD_PLAYSPEED_NORMAL && m_caching == CACHESTATE_DONE && !tolerateStall &&
          !streamEnded && !m_startHeld && !StartGateShut() && !m_pInputStream->IsRealtime(),
      std::memory_order_relaxed);

  if (m_caching == CACHESTATE_FULL)
  {
    CacheInfo cache = GetCachingTimes();
    if (cache.valid)
    {
      if (cache.level < 0.0)
      {
        CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "SourceSlow");
        SetCaching(CACHESTATE_INIT);
      }
      // Note: Previously used cache.level >= 1 would keep video stalled
      // event after cache was full
      // Talk link: https://github.com/xbmc/xbmc/pull/23760
      if (cache.time > m_messageQueueTimeSize)
        SetCaching(CACHESTATE_INIT);
    }
    else
    {
      if ((!m_VideoPlayerAudio->AcceptsData() && m_CurrentAudio.id >= 0) ||
          (!m_VideoPlayerVideo->AcceptsData() && m_CurrentVideo.id >= 0))
        SetCaching(CACHESTATE_INIT);
    }

    // if audio stream stalled, wait until demux queues have filled to 10% before
    // resuming playback. Both audio AND video need to be checked here - checking
    // audio alone means buffering can end while video hasn't recovered at all
    if (m_pInputStream->IsRealtime())
    {
      const bool audioReady = m_CurrentAudio.id < 0 || m_VideoPlayerAudio->GetLevel() > 10;
      const bool videoReady = m_CurrentVideo.id < 0 || m_processInfo->GetLevelVQ() > 10;

      if (audioReady && (videoReady || m_cachingTimer.IsTimePast()))
      {
        if (!videoReady)
        {
          CLog::Log(LOGDEBUG,
                    "Stream stalled, caching timeout reached before video recovered. "
                    "Audio: {} - Video: {}",
                    m_VideoPlayerAudio->GetLevel(), m_processInfo->GetLevelVQ());
        }
        SetCaching(CACHESTATE_INIT);
      }
    }
  }

  if (m_caching == CACHESTATE_INIT)
  {
    // if all enabled streams have been inited we are done
    if ((m_CurrentVideo.id >= 0 || m_CurrentAudio.id >= 0) &&
        (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_STARTING) &&
        (m_CurrentAudio.id < 0 || m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_STARTING))
      SetCaching(CACHESTATE_PLAY);

    // handle exceptions
    if (m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0 && !StartGateShut())
    {
      if ((!m_VideoPlayerAudio->AcceptsData() || !m_VideoPlayerVideo->AcceptsData()) &&
          m_cachingTimer.IsTimePast())
      {
        SetCaching(CACHESTATE_DONE);
      }
    }
  }

  if (m_caching == CACHESTATE_PLAY)
  {
    // if all enabled streams have started playing we are done
    if ((m_CurrentVideo.id < 0 || !m_VideoPlayerVideo->IsStalled()) &&
        (m_CurrentAudio.id < 0 || !m_VideoPlayerAudio->IsStalled()))
      SetCaching(CACHESTATE_DONE);
  }

  if (m_caching == CACHESTATE_DONE)
  {
    if (m_playSpeed == DVD_PLAYSPEED_NORMAL && !tolerateStall && !streamEnded && !m_startHeld &&
        !StartGateShut())
    {
      // take action if audio or video stream is stalled
      if (((m_VideoPlayerAudio->IsStalled() && m_CurrentAudio.inited) ||
           (m_VideoPlayerVideo->IsStalled() && m_CurrentVideo.inited)) &&
          m_syncTimer.IsTimePast())
      {
        if (m_pInputStream->IsRealtime())
        {
          if ((m_CurrentAudio.id >= 0 && m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
               m_VideoPlayerAudio->IsStalled()) ||
              (m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
               m_processInfo->GetLevelVQ() == 0))
          {
            CLog::Log(LOGDEBUG, "Stream stalled, start buffering. Audio: {} - Video: {}",
                      m_VideoPlayerAudio->GetLevel(), m_processInfo->GetLevelVQ());

            if (m_VideoPlayerAudio->AcceptsData() && m_VideoPlayerVideo->AcceptsData())
              SetCaching(CACHESTATE_FULL);
            else
            {
              m_txFlushCaller = "realtime stall";
              FlushBuffers(DVD_NOPTS_VALUE, false, true);
            }
          }
        }
        else
        {
          // start caching if audio and video have run dry
          if (m_VideoPlayerAudio->GetLevel() <= 50 &&
              m_processInfo->GetLevelVQ() <= 50)
          {
            SetCaching(CACHESTATE_FULL);
          }
          else if (m_CurrentAudio.id >= 0 && m_CurrentAudio.inited &&
                   m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC &&
                   m_VideoPlayerAudio->GetLevel() == 0)
          {
            CLog::Log(LOGDEBUG,"CVideoPlayer::HandlePlaySpeed - audio stream stalled, triggering re-sync");
            m_txFlushCaller = "audio stall resync";
            FlushBuffers(DVD_NOPTS_VALUE, true, true);
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = false;
            mode.accurate = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
      }
      // care for live streams
      else if (m_pInputStream->IsRealtime())
      {
        // Skip SpeedAdjust for passthrough: the message queue level metric is
        // meaningless because passthrough packets are tiny and drain instantly,
        // so GetLevel() stays near 0. This triggers SetSpeedAdjust(-0.05) immediately
        // and the >10 restore threshold is never reached, causing permanent clock
        // drift at -50ms/s and persistent A/V sync oscillation.
        if (m_CurrentAudio.id >= 0 && !IsPassthrough() && m_clock.GetClock() > DVD_MSEC_TO_TIME(1000))
        {
          double adjust = -1.0; // a unique value
          if (m_clock.GetSpeedAdjust() >= 0 && m_VideoPlayerAudio->GetLevel() < 1) {
            CLog::Log(LOGDEBUG, "VideoPlayer:Speed adjust:-0.05 aq:{:d}", m_VideoPlayerAudio->GetLevel());
             adjust = -0.05;
          }
          if (m_clock.GetSpeedAdjust() < 0 && m_VideoPlayerAudio->GetLevel() > 4) {
            CLog::Log(LOGDEBUG, "VideoPlayer:Speed adjust:0.0 aq:{:d}", m_VideoPlayerAudio->GetLevel());
            adjust = 0.0;
          }
          if (adjust != -1.0)
          {
            m_clock.SetSpeedAdjust(adjust);
          }
        }
      }
    }
  }

  // A source that keeps feeding the demuxer while neither stream can make
  // anything of it is broken, not slow. Reads advancing is the discriminator: if
  // they are not, this is stalled I/O (a sleeping disk, a network share hiccup)
  // and none of our business. Opt-in, and never in a menu or on still images,
  // where a long stall is entirely normal.
  const bool brokenFileGate =
      m_pDemuxer && m_pInputStream && !m_pInputStream->IsRealtime() &&
      m_playSpeed == DVD_PLAYSPEED_NORMAL && !tolerateStall && !streamEnded &&
      m_caching == CACHESTATE_DONE && m_CurrentAudio.inited && m_CurrentVideo.inited &&
      m_VideoPlayerAudio->IsStalled() && m_VideoPlayerVideo->IsStalled() &&
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(
          CSettings::SETTING_COREELEC_DETECT_BROKEN_FILES);

  if (brokenFileGate)
  {
    const auto now = std::chrono::steady_clock::now();
    if (m_brokenFileStallStart == std::chrono::steady_clock::time_point{})
    {
      m_brokenFileStallStart = now;
      m_brokenFileStallBytes = m_pDemuxer->GetSourceReadBytes();
    }
    else if (now - m_brokenFileStallStart >= std::chrono::seconds(5))
    {
      const int64_t readBytes = m_pDemuxer->GetSourceReadBytes();
      if (readBytes >= 0 && m_brokenFileStallBytes >= 0 &&
          readBytes - m_brokenFileStallBytes >= CDVDDemux::BROKEN_SOURCE_MIN_SCAN_BYTES)
      {
        if (!m_brokenFileNotified)
        {
          m_brokenFileNotified = true;
          CLog::Log(LOGERROR, "CVideoPlayer::HandlePlaySpeed - audio and video both stalled for 5s "
                              "while the demuxer read {} MiB; treating the source as broken and "
                              "stopping playback",
                    (readBytes - m_brokenFileStallBytes) / (1024 * 1024));
          const auto& strings = CServiceBroker::GetResourcesComponent().GetLocalizeStrings();
          CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, strings.Get(60363),
                                                strings.Get(60364), TOAST_DISPLAY_TIME * 2);
        }
        m_pDemuxer->MarkBroken();
      }
      else if (!m_brokenFileStallStarveLogged)
      {
        m_brokenFileStallStarveLogged = true;
        CLog::Log(LOGWARNING, "CVideoPlayer::HandlePlaySpeed - audio and video stalled for 5s with "
                              "no demuxer read progress; treating as I/O starvation, not a broken "
                              "file");
      }
    }
  }
  else
  {
    m_brokenFileStallStart = {};
    m_brokenFileStallBytes = -1;
    m_brokenFileStallStarveLogged = false;
  }

  // 5.E2-9c D5: outside the WAITSYNC test, so it runs with neither player there
  const bool txSettleStart = TxSettleTimerPass();
  // sync streams to clock; a shut start gate's audio cannot arrive, and the start
  // would otherwise go ahead without it (no commit, no stuck-sync flushes)
  // 5.E2-11 D1.5: not inside a row-9 span (the START CAS under the guard is the guarantee)
  if (!StartGateShut() && !m_txInSpan &&
      ((m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
       (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC)))
  {
    unsigned int threshold = 20;
    if (m_pInputStream->IsRealtime())
      threshold = 40;

    // Video is at the handshake, audio's queue is full, and audio has not
    // arrived - the shape a stream player leaves when it has stopped running.
    // Evaluated here, and disarmed on every path that is not it, so the timer
    // below can never be left armed from an earlier, unrelated stall.
    // Not while the audio engine is suspended (a display mode change, until
    // videoscreen.delayrefreshchange has run out): then audio is not dead but
    // waiting for its device - CAudioSinkAE::Create blocks for it - and a flush
    // would throw away the read-ahead (a resume skips forward by the queue
    // depth) and restart the same wait. The suspension is bounded, so a thread
    // that really has stopped is still caught once the engine is back.
    IAE* const audioEngine = CServiceBroker::GetActiveAE();
    const bool syncStuck = m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0 &&
                           !m_VideoPlayerAudio->AcceptsData() &&
                           m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC &&
                           m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_WAITSYNC &&
                           !(audioEngine && audioEngine->IsSuspended());
    if (!syncStuck)
      m_syncStuckArmed = false;

    // A full audio queue stops the demuxer, so a video decoder that needs more
    // input would never deliver its first picture: the start then goes ahead
    // on audio. E1 takes the output mode from that first picture, and a start
    // committed ahead of it is not held (Superman UHD: committed 80 ms before
    // its first picture configured the renderer). Give it a bounded wait; the
    // deadlock the shortcut breaks still breaks, 1 s later.
    // The same goes for audio read ahead of the first video packet: after a
    // seek in a loosely interleaved file, 20 audio packets can come 80 ms
    // before it, and a start committed on them is neither held nor scheduled.
    bool audioFullVideoStarving =
        (!m_VideoPlayerAudio->AcceptsData() && m_processInfo->GetLevelVQ() < 10) ||
        (m_CurrentVideo.packets == 0 && m_CurrentAudio.packets > threshold);
    if (audioFullVideoStarving &&
        (m_heldStartEnabled || (m_scheduledStart && !m_pInputStream->IsRealtime())) &&
        m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_STARTING)
    {
      const auto now = std::chrono::steady_clock::now();
      if (m_firstPictureWaitSince == std::chrono::steady_clock::time_point{})
      {
        m_firstPictureWaitSince = now;
        CLog::Log(LOGDEBUG, "VideoPlayer::Sync - audio ahead of video, waiting for the first picture");
      }
      else if (now - m_firstPictureWaitSince >= 1s && !m_firstPictureWaitExpired)
      {
        m_firstPictureWaitExpired = true;
        CLog::Log(LOGINFO, "VideoPlayer::Sync - no first picture in 1s, starting on audio");
      }
      audioFullVideoStarving = m_firstPictureWaitExpired;
    }
    else
    {
      m_firstPictureWaitSince = {};
      m_firstPictureWaitExpired = false;
    }
    // the wait's own expiry is its only exit: exception 1 below (video does
    // not start) would otherwise flush the buffers after one video timeout
    const bool waitingFirstPicture =
        m_firstPictureWaitSince != std::chrono::steady_clock::time_point{} &&
        !m_firstPictureWaitExpired;
    bool video = (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
                 audioFullVideoStarving || txSettleStart;
    // The video queue fills while the start gate holds the audio, so at the
    // gate's opening the "video full, audio starved" shortcut below would count
    // audio ready before its first frame: the start is then scheduled before
    // the audio's sink opens and lands up to a frame late. That audio has
    // packets and reports on its own, so the shortcut waits for it, bounded.
    bool gatedAudioPending = false;
    if (m_startGateOpenedAt != std::chrono::steady_clock::time_point{} &&
        m_CurrentAudio.id >= 0 && m_CurrentAudio.packets > 0 &&
        m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_WAITSYNC)
    {
      if (std::chrono::steady_clock::now() - m_startGateOpenedAt < 1s)
        gatedAudioPending = true;
      else
      {
        m_startGateOpenedAt = {};
        CLog::Log(LOGINFO, "VideoPlayer::Sync - the gated audio did not report in 1s after the "
                           "gate opened (set {}), starting without it",
                  m_startGateGen);
      }
      if (gatedAudioPending && !m_gatedAudioWaitLogged && !m_VideoPlayerVideo->AcceptsData() &&
          m_VideoPlayerAudio->GetLevel() < 10)
      {
        m_gatedAudioWaitLogged = true;
        CLog::Log(LOGINFO,
                  "VideoPlayer::Sync - video full at the gate's opening (set {}), waiting for "
                  "the gated audio to report",
                  m_startGateGen);
      }
    }
    bool audio = m_CurrentAudio.id < 0 || (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC) ||
                 (m_CurrentAudio.packets == 0 && m_CurrentVideo.packets > threshold) ||
                 (!gatedAudioPending && !m_VideoPlayerVideo->AcceptsData() &&
                  m_VideoPlayerAudio->GetLevel() < 10);

    const bool syncAudio = m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC &&
                           (m_CurrentAudio.avsync == CCurrentStream::AV_SYNC_CONT ||
                            m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC);
    const bool syncVideo = m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC &&
                           (m_CurrentVideo.avsync == CCurrentStream::AV_SYNC_CONT ||
                            m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_INSYNC);
    const bool deferNoPts = ShouldDeferSync(video && audio && !syncAudio && !syncVideo,
                                            std::chrono::steady_clock::now());

    // Arm the no-start-pts defer window (below) as soon as both streams are
    // ready, not only once ShouldDeferSync has given up, so the two waits run
    // side by side and the window bounds the whole wait at 2.5 s (was 6 s).
    // Every place a start attempt ends clears it together with
    // m_syncStartPtsWait, so an armed flag is never left from a dead attempt.
    const bool videoHasStart =
        m_CurrentVideo.starttime != DVD_NOPTS_VALUE && m_CurrentVideo.packets > 0;
    const bool audioHasStart =
        m_CurrentAudio.starttime != DVD_NOPTS_VALUE && m_CurrentAudio.packets > 0;
    if (video && audio && !syncAudio && !syncVideo && !videoHasStart && !audioHasStart &&
        !m_syncStartDeferred)
    {
      m_syncStartDeferred = true;
      m_syncStartDeferTimer.Set(2500ms);
      CLog::Log(LOGDEBUG, "VideoPlayer::Sync - no stream has a start pts yet, deferring sync");
    }

    if (syncAudio)
    {
      CLog::Log(LOGDEBUG, LOGAUDIO, "VideoPlayer::Sync - Audio - Waiting, clock: {:.3f}", m_clock.GetClock());
      m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;
      m_syncStartDeferred = false;
      if (m_recueJoinPending)
      {
        // the re-cued track lands on the running clock at a vblank, as a start
        // does: the lead covers what the audio output queues ahead
        m_recueJoinPending = false;
        int64_t startNs = 0;
        double startClock = 0.0;
        if (m_clock.ScheduleJoin(0.35, startNs, startClock))
          CLog::Log(LOGINFO, "VideoPlayer: re-cued audio joins the clock at {:.3f} ({} ns)",
                    startClock / DVD_TIME_BASE, startNs);
      }
      m_VideoPlayerAudio->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, m_clock.GetClock()), 1);
    }
    else if (syncVideo)
    {
      CLog::Log(LOGDEBUG, LOGVIDEO, "VideoPlayer::Sync - Video - Waiting, clock: {:.3f}", m_clock.GetClock());
      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;
      m_CurrentVideo.starttimePending = false;
      m_syncStartDeferred = false;
      m_VideoPlayerVideo->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, m_clock.GetClock()), 1);
    }
    // ShouldDeferSync gives up for one call only and then re-arms for another
    // 2 s, so on its own it would outlast the no-start-pts window below; that
    // window (armed together with it, above) is the bound, so once it has run
    // out the commit goes ahead whatever ShouldDeferSync says.
    else if (video && audio &&
             (!deferNoPts || (m_syncStartDeferred && m_syncStartDeferTimer.IsTimePast())))
    {
      // A flush during a disc transition clears both streams' start pts while
      // the demuxer may still be seeking to the next segment; if the sync gate
      // opens inside that gap (e.g. the audio stream closed with it), every
      // clock branch below fails and the fallthrough would sync the clock to 0
      // against packets stamped minutes ahead - wedging playback on discs
      // whose timeline does not start at 0. Defer until a stream reports a
      // usable start pts, bounded so a stream that never delivers one still
      // starts with the old behaviour.
      bool commit = true;
      if (!videoHasStart && !audioHasStart)
      {
        commit = !m_syncStartDeferred || m_syncStartDeferTimer.IsTimePast();
        if (commit)
          CLog::Log(LOGWARNING,
                    "VideoPlayer::Sync - no start pts within defer window, syncing clock to 0");
      }
      // 5.E2-9c D4: the hint job's picture compared before the commit's START. After an exit
      // with no PLAYER_STARTED yet nothing commits on audio: the picture's commit holds for
      // main's set (code review H1)
      if (commit && TxOpen() && m_txHintJob)
      {
        TxHintPictureCheck("sync commit");
        if (!TxOpen() && m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_WAITSYNC)
          commit = false;
      }
      // 5.E2-11 D1.5: a guarded start's verdict comes ahead of START's CAS: its exit ends the
      // start before any START step
      if (commit && TxGuardedStartOn())
        TxCheckVerdict(m_scheduledStart && !m_pInputStream->IsRealtime() ? "release" : "sync");
      if (commit)
      {
      // 5.E2-11 D1.5: what a missed START (word or guard) puts back; nothing else has run then
      const bool txGuardOn = TxGuardedStartOn();
      const bool deferredBefore = m_syncStartDeferred;
      const auto gateOpenedBefore = m_startGateOpenedAt;
      const int playSpeedBefore = m_playSpeed;
      const int pendingBefore = m_txPendingSpeed;
      m_syncStartDeferred = false;
      m_startGateOpenedAt = {};
      double clock = 0;
      if (m_CurrentAudio.syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
        CLog::Log(LOGDEBUG, "VideoPlayer::Sync - Audio - pts: {:.3f}, cache: {:.3f}, totalcache: {:.3f}, packets:{:d} level:{:d}",
                             m_CurrentAudio.starttime / DVD_TIME_BASE, m_CurrentAudio.cachetime / DVD_TIME_BASE, m_CurrentAudio.cachetotal / DVD_TIME_BASE, m_CurrentAudio.packets, m_VideoPlayerAudio->GetLevel());
      if (m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
        CLog::Log(LOGDEBUG, "VideoPlayer::Sync - Video - pts: {:.3f}, cache: {:.3f}, totalcache: {:.3f}, packets:{:d} level:{:d}",
                             m_CurrentVideo.starttime / DVD_TIME_BASE, m_CurrentVideo.cachetime / DVD_TIME_BASE, m_CurrentVideo.cachetotal / DVD_TIME_BASE, m_CurrentVideo.packets, m_processInfo->GetLevelVQ());

      // a scheduled start keeps the audio before an accurate seek's target
      // (FlushBuffers), so it can report first; the video's packets up to the
      // target count as dropped although its keyframe is the first picture
      const bool videoPictured = m_CurrentVideo.starttime != DVD_NOPTS_VALUE &&
                                 (m_CurrentVideo.packets > 0 || m_scheduledStart);
      std::optional<unsigned int> heldDecisions;
      // 5.E3: a P-decided open's sync commit schedules: main is shut out until START
      // (TxShutMain), so RM's decision could only end a hold at the 12 s fallback
      const bool txSync = TxOpen() && m_txJob;
      // 5.G-3 (§4.3 e): on the path session P decides: its job or a due decision holds.
      // 5.G-6: so RM's pending test is never asked on the path (main decides only a hand-back,
      // which TxMidHoldPending holds); kept for legacy, midplaytx_off and path-off sessions
      const bool midSync = !txSync && m_txMidPath;
      const bool holdStart = !txSync && m_heldStartEnabled && m_CurrentVideo.id >= 0 &&
                             videoPictured &&
                             (midSync ? TxMidHoldPending()
                                      : m_renderManager.IsResolutionUpdatePending(heldDecisions));
      // A start scheduled on the clock (design §15, step 2.2) that no mode set
      // holds: held and released at once, so it resumes on a vblank a lead
      // ahead with the audio landing there. The clock starts at the first
      // picture (or the first audio, without video); audio queued before it
      // is pre-roll. Live streams keep their own start (a second behind).
      const bool videoStart = videoPictured;
      const bool audioOnlyStart = m_CurrentVideo.id < 0 &&
                                  m_CurrentAudio.starttime != DVD_NOPTS_VALUE &&
                                  m_CurrentAudio.packets > 0;
      // 5.E2-9c D5 (E29A-1): a hint-decided start with no picture yet is scheduled on its first
      // audio (the audio-only clock below); the late picture joins the running clock
      const bool videoOpenNoPicture =
          m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_WAITSYNC;
      const bool hintAudioStart = TxOpen() && m_txJob && m_txHintJob && videoOpenNoPicture &&
                                  m_CurrentAudio.starttime != DVD_NOPTS_VALUE &&
                                  m_CurrentAudio.packets > 0;
      // 5.E2-9b D1.3 (b): an engaged open takes the held path at any speed (a pending or applied
      // pause is START held as a pause)
      const bool scheduleStart = !holdStart && m_scheduledStart &&
                                 (m_playSpeed == DVD_PLAYSPEED_NORMAL ||
                                  (TxOpen() && !m_txSkipPausestart)) &&
                                 !m_pInputStream->IsRealtime() &&
                                 (videoStart || audioOnlyStart || hintAudioStart);
      // 5.E2-11 D1.5: START's steps run under the player guard (an E1 hold is no START)
      const bool txGuarded = txGuardOn && !holdStart;
      int txUnheldSpeed = INT_MIN;
      // (c): a pending speed is the held path's (its release applies it), else applied here,
      // before the clock selection below and the resyncs
      if (m_txPendingSpeed != INT_MIN)
      {
        const int pending = m_txPendingSpeed;
        if (holdStart || scheduleStart)
        {
          m_playSpeed = pending;
          m_txPendingSpeed = INT_MIN;
        }
        else if (txGuarded)
        {
          // the selection below reads it; the guard applies it
          m_playSpeed = pending;
          m_txPendingSpeed = INT_MIN;
          txUnheldSpeed = pending;
        }
        else
        {
          ApplySpeed(pending, true);
          CLog::Log(LOGINFO, "TXSTART speed {} applied at the commit (caching {})", pending,
                    TxCachingName(m_caching));
        }
      }
      if (scheduleStart)
      {
        // without video the clock starts at the first audio: the audio player
        // reports the last frame it queued, a sink fill (0.29 s measured) later
        clock = videoStart                                             ? m_CurrentVideo.starttime
                : m_CurrentAudio.firststarttime != DVD_NOPTS_VALUE ? m_CurrentAudio.firststarttime
                                                                   : m_CurrentAudio.starttime;
      }
      else if (videoPictured && (m_playSpeed == DVD_PLAYSPEED_PAUSE || holdStart))
      {
        clock = m_CurrentVideo.starttime;
      }
      else if (m_CurrentAudio.starttime != DVD_NOPTS_VALUE && m_CurrentAudio.packets > 0)
      {
        if (m_pInputStream->IsRealtime())
          clock = m_CurrentAudio.starttime - m_CurrentAudio.cachetime - DVD_MSEC_TO_TIME(1000);
        else
          clock = m_CurrentAudio.starttime - m_CurrentAudio.cachetime;

        if (videoPictured)
        {
          if (m_CurrentVideo.starttime - m_CurrentVideo.cachetotal < clock)
          {
            clock = m_CurrentVideo.starttime - m_CurrentVideo.cachetotal;
          }
          else if (m_CurrentVideo.starttime > m_CurrentAudio.starttime &&
                   !m_pInputStream->IsRealtime())
          {
            const int audioLevel = m_VideoPlayerAudio->GetLevel();
            const double audioTimeMs = m_messageQueueTimeSize * 1000.0 * audioLevel / 100.0;
            const double maxAudioTime = clock + DVD_MSEC_TO_TIME(audioTimeMs);
            if ((m_CurrentVideo.starttime - m_CurrentVideo.cachetotal) > maxAudioTime)
              clock = maxAudioTime;
            else
              clock = m_CurrentVideo.starttime - m_CurrentVideo.cachetotal;
          }
        }
      }
      else if (m_CurrentVideo.starttime != DVD_NOPTS_VALUE && m_CurrentVideo.packets > 0)
      {
        clock = m_CurrentVideo.starttime - m_CurrentVideo.cachetotal;
      }

      const auto commitClock = [&]
      {
      // the start clock is the first picture's pts: its landing is on a frame
      m_startAtFrame = videoPictured && clock == m_CurrentVideo.starttime;
      m_syncStartPtsWait.reset();
      m_syncStartDeferred = false;
      m_firstPictureWaitSince = {};
      m_firstPictureWaitExpired = false;
      m_clock.Discontinuity(clock);
      m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_NONE;
      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_NONE;
      m_CurrentVideo.starttimePending = false;
      // A held start pauses its streams before the resync: the audio player
      // resumes its stream at a resync unless it is paused, and a stream that
      // runs for a moment has the start of its sound consumed by its sync. The
      // caching pause stays in place for the same reason (design 13.ab).
      if (holdStart || scheduleStart)
      {
        m_caching = CACHESTATE_DONE;
        m_clock.SetSpeedAdjust(0);
        // scheduleStart alone never read the count: today's snapshot (r1 L3)
        HoldStart(holdStart ? heldDecisions : std::nullopt,
                  holdStart && midSync ? std::optional<uint64_t>(m_midTx.counted)
                                       : std::nullopt);
        // 5.E2-4: a file's held start is inside the window until its release (E2-4 review H1);
        // a seek's resync is no start (5.E2-11: a guarded START's after the guard)
        if (!txGuarded && aml_transaction_shadow_on() && !m_State.streamsReady)
          OUTPUT::COutputWorker::Get().ShadowStartPhase(
              this, OUTPUT::COutputWorker::StartPhase::STARTING, 0, "start held",
              static_cast<int>(OUTPUT::COutputWorker::StartPhase::NONE));
        if (holdStart)
          m_discTime.SetStartHeld(true);
        // the clock ran from the discontinuity to the pause (up to 0.9 ms
        // measured): stand it at the first picture again
        m_clock.Discontinuity(clock);
      }
      m_VideoPlayerAudio->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, clock), 1);
      m_VideoPlayerVideo->SendMessage(
          std::make_shared<CDVDMsgDouble>(CDVDMsg::GENERAL_RESYNC, clock), 1);
      };
      STxStartDone txDone;
      if (txGuarded &&
          !TxStartGuarded(scheduleStart ? TxStartPath::SCHEDULED : TxStartPath::UNHELD, txDone,
                          [&]
                          {
                            if (txUnheldSpeed != INT_MIN)
                              ApplySpeed(txUnheldSpeed, true);
                            commitClock();
                          }))
      {
        // no START this pass: the sync stays WAITSYNC and the next pass retries
        m_syncStartDeferred = deferredBefore;
        m_startGateOpenedAt = gateOpenedBefore;
        m_playSpeed = playSpeedBefore;
        m_txPendingSpeed = pendingBefore;
      }
      else
      {
      if (!txGuarded)
        commitClock();
      if (txUnheldSpeed != INT_MIN)
        CLog::Log(LOGINFO, "TXSTART speed {} applied at the commit (caching {})", txUnheldSpeed,
                  TxCachingName(m_caching));
      // after the resyncs, so the audio player takes its resync paused and
      // resumes with the release
      if (scheduleStart && txGuarded)
        TxStartAfterGuard(TxStartPath::SCHEDULED, txDone, "scheduled start");
      else if (scheduleStart)
        ReleaseHeldStart("scheduled start");
      else if (txGuarded)
      {
        TxStartAfterGuard(TxStartPath::UNHELD, txDone, "sync, start not held");
        if (aml_transaction_shadow_on())
          OUTPUT::COutputWorker::Get().ShadowStartPhase(
              this, OUTPUT::COutputWorker::StartPhase::NONE, 0, "sync, start not held");
      }
      else if (!holdStart)
      {
        SetCaching(CACHESTATE_DONE);
        // 5.E2-6 (r1 H2, r2 M2): a start the sync leaves unheld (a pause or FF in the start, a
        // realtime first set) opens the picture gate here; the decoder's speed is restored
        // 5.E2-8a (D4): an unheld sync commit is the engaged start's START point
        if (TxOpen())
        {
          TxCheckVerdict("sync");
          if (TxOpen())
          {
            TxStartPoint("sync, start not held");
            TxStartRelease(0);
          }
        }
        if (m_renderManager.OpenTxPictureGate("sync, start not held"))
          TxRestoreDecoderSpeed("sync, start not held");
        // 5.E2-4: an unheld start's window ends at its sync (E2-4 review H1)
        if (aml_transaction_shadow_on())
          OUTPUT::COutputWorker::Get().ShadowStartPhase(
              this, OUTPUT::COutputWorker::StartPhase::NONE, 0, "sync, start not held");
      }
      UpdatePlayState(0);

      // a released start armed the grace for its lead (ReleaseHeldStart)
      if (!scheduleStart)
        m_syncTimer.Set(3000ms);

      // 5.E2-8a (D4): an engaged start sends onAVStarted at START; held by E1, at its release
      if (!m_State.streamsReady)
      {
        if (!m_txEngaged || m_txExited || m_txStarted)
          SignalStreamsReady();
        else
          m_txSyncPassed = true;
      }
      }
      }
    }
    else
    {
      // exceptions for which stream players won't start properly
      // 1. videoplayer has not detected a keyframe within length of demux buffers
      if (m_CurrentAudio.id >= 0 && m_CurrentVideo.id >= 0 &&
          !m_VideoPlayerAudio->AcceptsData() &&
          m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_STARTING &&
          m_VideoPlayerVideo->IsStalled() &&
          m_CurrentVideo.packets > 10 && !waitingFirstPicture)
      {
        m_VideoPlayerAudio->AcceptsData();
        m_firstPictureWaitSince = {};
        m_firstPictureWaitExpired = false;
        CLog::Log(LOGWARNING, "VideoPlayer::Sync - stream player video does not start, flushing buffers");
        m_txFlushCaller = "sync video no start";
        FlushBuffers(DVD_NOPTS_VALUE, true, true);
        m_syncStuckArmed = false;
      }
      // 2. one player has arrived at the handshake and the other never will.
      //    Case 1 only covers video still SYNC_STARTING; once video has reached
      //    SYNC_WAITSYNC there was no bound at all, and the wait is indefinite:
      //    an audio queue that is full and never drains also stops the demuxer
      //    (see the !AcceptsData gate in Process), so nothing else can break the
      //    deadlock either. Observed 2026-09-06 when CVideoPlayerAudio's thread
      //    died at startup and playback froze for 22s with nothing logged.
      //
      //    Unlike case 1 this state IS reachable transiently, so it is timed
      //    rather than acted on immediately.
      else if (syncStuck)
      {
        if (!m_syncStuckArmed)
        {
          m_syncStuckArmed = true;
          m_syncStuckTimer.Set(SYNC_STUCK_TIMEOUT);
        }
        else if (m_syncStuckTimer.IsTimePast())
        {
          // Loud: the partner is not merely slow, it has stopped participating.
          CLog::Log(LOGERROR,
                    "VideoPlayer::Sync - audio has not reached sync within {}ms while its queue "
                    "is full (level {}%, syncState {}) - flushing. If this repeats, the audio "
                    "player thread is not running.",
                    SYNC_STUCK_TIMEOUT.count(), m_VideoPlayerAudio->GetLevel(),
                    static_cast<int>(m_CurrentAudio.syncState));
          m_syncStuckArmed = false;
          m_txFlushCaller = "sync stuck";
          FlushBuffers(DVD_NOPTS_VALUE, true, true);
        }
      }
    }
  }
  else
  {
    m_syncStartPtsWait.reset();
    m_syncStartDeferred = false;
    m_firstPictureWaitSince = {};
    m_firstPictureWaitExpired = false;
    // Neither player is at the handshake, so nothing can be stuck at it.
    m_syncStuckArmed = false;
  }

  // handle ff/rw
  if (m_playSpeed != DVD_PLAYSPEED_NORMAL && m_playSpeed != DVD_PLAYSPEED_PAUSE)
  {
    if (isInMenu)
    {
      // this can't be done in menu
      SetPlaySpeed(DVD_PLAYSPEED_NORMAL);

    }
    else
    {
      bool check = true;

      // only check if we have video
      if (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC)
        check = false;
      // video message queue either initiated or already seen eof
      else if (m_CurrentVideo.inited == false && m_playSpeed >= 0)
        check = false;
      // don't check if time has not advanced since last check
      else if (m_SpeedState.lasttime == GetTime())
        check = false;
      // skip if frame at screen has no valid timestamp
      else if (m_VideoPlayerVideo->GetCurrentPts() == DVD_NOPTS_VALUE)
        check = false;
      // skip if frame on screen has not changed
      else if (m_SpeedState.lastpts == m_VideoPlayerVideo->GetCurrentPts() &&
               (m_SpeedState.lastpts > m_State.dts || m_playSpeed > 0))
        check = false;
      // no trick-play seek over one the video has not carried out: at speed
      // the clock test asks for seeks faster than the decoder resets
      else if (m_VideoPlayerVideo->IsFlushQueued())
        check = false;

      if (check)
      {
        m_SpeedState.lastpts  = m_VideoPlayerVideo->GetCurrentPts();
        m_SpeedState.lasttime = GetTime();
        m_SpeedState.lastabstime = m_clock.GetAbsoluteClock();

        double error;
        error  = m_clock.GetClock() - m_SpeedState.lastpts;
        error *= m_playSpeed / abs(m_playSpeed);

        // allow a bigger error when going ff, the faster we go
        // the the bigger is the error we allow
        if (m_playSpeed > DVD_PLAYSPEED_NORMAL)
        {
          double errorwin = static_cast<double>(m_playSpeed) / DVD_PLAYSPEED_NORMAL;
          if (errorwin > 8.0)
            errorwin = 8.0;
          error /= errorwin;
        }
        CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayer::Process - ffd/rwd: lastpts:{:.3f} clock:{:.3f} lastseekpts:{:.3f} speed:{:d} error:{:.3f}",
          m_SpeedState.lastpts / 1000000.0, m_clock.GetClock() / 1000000.0, m_SpeedState.lastseekpts / 1000000.0, (int)m_playSpeed, error / 1000000.0);

        if (std::abs(error) > DVD_MSEC_TO_TIME(1000))
        {
          error  = (m_clock.GetClock() - m_SpeedState.lastseekpts) / 1000;

          if (std::abs(error) > 1000 || (m_VideoPlayerVideo->IsRewindStalled() && std::abs(error) > 100))
          {
            m_SpeedState.lastseekpts = m_clock.GetClock();
            int direction = (m_playSpeed > 0) ? 1 : -1;
            double iTime = (m_clock.GetClock() + m_State.time_offset + 1000000.0 * direction) / 1000;
            CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayer::Process - Seeking to catch up, error was: {:.3f} time:{:.3f}", error / 1000.0, iTime/1000.0);
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = iTime;
            mode.backward = (m_playSpeed < 0);
            mode.accurate = false;
            mode.restore = false;
            mode.trickplay = true;
            mode.sync = false;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
      }
    }
  }

  // reset tempo
  if (!m_State.cantempo)
  {
    float currentTempo = m_processInfo->GetNewTempo();
    if (currentTempo != 1.0f)
    {
      SetTempo(1.0f);
    }
  }
}

bool CVideoPlayer::CheckPlayerInit(CCurrentStream& current)
{
  if (current.inited)
    return false;

  if (current.startpts != DVD_NOPTS_VALUE)
  {
    if(current.dts == DVD_NOPTS_VALUE)
    {
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayer::{} - Dropping packet type:{:d} dts:{:.3f} to get to start point at {:.3f}",
        __FUNCTION__, current.player,  current.dts / 1000000.0, current.startpts / 1000000.0);
      return true;
    }

    if ((current.startpts - current.dts) > DVD_SEC_TO_TIME(20))
    {
      CLog::Log(LOGDEBUG, "{} - too far to decode before finishing seek", __FUNCTION__);
      if(m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
        m_CurrentAudio.startpts = current.dts;
      if(m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
        m_CurrentVideo.startpts = current.dts;
      if(m_CurrentSubtitle.startpts != DVD_NOPTS_VALUE)
        m_CurrentSubtitle.startpts = current.dts;
      if(m_CurrentTeletext.startpts != DVD_NOPTS_VALUE)
        m_CurrentTeletext.startpts = current.dts;
      if(m_CurrentRadioRDS.startpts != DVD_NOPTS_VALUE)
        m_CurrentRadioRDS.startpts = current.dts;
      if (m_CurrentAudioID3.startpts != DVD_NOPTS_VALUE)
        m_CurrentAudioID3.startpts = current.dts;
    }

    if(current.dts < current.startpts)
    {
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayer::{} - dropping packet type:{:d} dts:{:.3f} to get to start point at {:.3f}",
        __FUNCTION__, current.player,  current.dts / 1000000.0, current.startpts / 1000000.0);
      return true;
    }
  }

  if (current.dts != DVD_NOPTS_VALUE)
  {
    current.inited = true;
    current.startpts = current.dts;
  }
  return false;
}

void CVideoPlayer::TakeSeamMarks()
{
#if defined(HAVE_LIBBLURAY)
  if (m_pInputBluray)
  {
    for (const auto& mark : m_pInputBluray->TakeSeamMarks())
    {
      // a seek back meets a seam again, from the first read after libbluray's
      // event, which can start later than before: the seam keeps one step, at
      // the earlier position (P4-22)
      const auto same = std::find_if(m_seamMarks.begin(), m_seamMarks.end(),
                                     [&mark](const SeamMark& m)
                                     { return m.clipPos && m.clipPos == mark.clipPos; });
      if (same != m_seamMarks.end())
      {
        if (same->pos <= mark.pos)
          continue;
        m_seamMarks.erase(same);
      }
      const auto at = std::lower_bound(m_seamMarks.begin(), m_seamMarks.end(), mark.pos,
                                       [](const SeamMark& m, int64_t pos) { return m.pos < pos; });
      if (at == m_seamMarks.end() || at->pos != mark.pos)
        m_seamMarks.insert(at, {mark.pos, static_cast<double>(mark.step) * DVD_TIME_BASE / 90000.0,
                                mark.gen, mark.clipPos});
    }
  }
#endif
}

double CVideoPlayer::CrossSeams(const DemuxPacket* packet, double& crossed)
{
  SeamStream& stream = m_seamStreams[{packet->demuxerId, packet->iStreamId}];
  // a frame that does not start a PES has no position: it is where the last one was
  if (packet->streamPos >= 0)
  {
    stream.lastPos = packet->streamPos;
    stream.lastGen = packet->streamGen;
  }
  // the steps of every seam the stream is past: a read back across one undoes it
  double offset = 0.0;
  for (const SeamMark& mark : m_seamMarks)
  {
    if (stream.lastPos < mark.pos)
      break;
    // an older playlist's tail (its title bytes run higher) is not past a seam
    // of a playlist read after it
    if (static_cast<int32_t>(mark.gen - stream.lastGen) > 0)
      continue;
    offset += mark.step;
  }
  crossed = offset - stream.offset;
  stream.offset = offset;
  return offset;
}

void CVideoPlayer::ClearSeams()
{
#if defined(HAVE_LIBBLURAY)
  if (m_pInputBluray)
    m_pInputBluray->TakeSeamMarks();
#endif
  m_seamMarks.clear();
  m_seamStreams.clear();
  ClearRecue();
}

void CVideoPlayer::NoteDelivered(const DemuxPacket* packet)
{
  const std::pair<int64_t, int> key{packet->demuxerId, packet->iStreamId};
  const auto seam = m_seamStreams.find(key);
  if (seam == m_seamStreams.end() || seam->second.lastPos < 0)
    return;
  DeliveredMark& mark = m_delivered[key];
  mark.pos = seam->second.lastPos;
  // a packet without a dts (a PES split by the parser) keeps the last one
  if (packet->demuxDts != DVD_NOPTS_VALUE)
    mark.dts = packet->demuxDts;
  mark.el = packet->isELPackage;
  if (packet->iStreamId == m_CurrentVideo.id && packet->demuxerId == m_CurrentVideo.demuxerId &&
      packet->streamPos >= 0 && packet->dts != DVD_NOPTS_VALUE)
  {
    m_videoPosHistory.emplace_back(packet->streamPos, packet->dts);
    if (m_videoPosHistory.size() > 1500)
      m_videoPosHistory.pop_front();
  }
}

void CVideoPlayer::ClearRecue()
{
  if (!m_recueGate.empty())
    CLog::Log(LOGINFO, "VideoPlayer: audio re-cue gate cleared with {} streams still in it",
              m_recueGate.size());
  m_delivered.clear();
  m_videoPosHistory.clear();
  m_recueGate.clear();
  m_recueJoinPending = false;
}

bool CVideoPlayer::PrepareRecue(int64_t& pos)
{
#if defined(HAVE_LIBBLURAY)
  const char* refused = !m_recueEnabled                                         ? "off"
                        : !m_pInputBluray || !m_pDemuxer                          ? "no disc"
                        : !m_recueGate.empty()                                    ? "re-cue running"
                        : m_playSpeed != DVD_PLAYSPEED_NORMAL                     ? "speed"
                        : m_caching != CACHESTATE_DONE                            ? "caching"
                        : m_startHeld                                             ? "start held"
                        : m_seamStepPending                                       ? "seam step pending"
                        : m_CurrentVideo.id < 0                                   ? "no video"
                        : m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC ? "video not in sync"
                        : m_pInputBluray->IsInMenu()                              ? "menu"
                                                                                  : nullptr;
  if (refused)
  {
    CLog::Log(LOGINFO, "VideoPlayer: audio track switch not re-cued: {}", refused);
    return false;
  }

  // from the video packet decoded half a second past the clock: the audio
  // before it would be skipped to land on the clock anyway
  const double target = m_clock.GetClock() + DVD_MSEC_TO_TIME(500);
  pos = -1;
  for (auto it = m_videoPosHistory.rbegin(); it != m_videoPosHistory.rend(); ++it)
  {
    if (it->second <= target)
    {
      pos = it->first;
      break;
    }
  }
  const auto video = m_delivered.find({m_CurrentVideo.demuxerId, m_CurrentVideo.id});
  if (pos < 0 || video == m_delivered.end() || video->second.pos <= pos ||
      !m_pInputBluray->CanRecueFrom(pos))
  {
    CLog::Log(LOGINFO,
              "VideoPlayer: audio track switch not re-cued: from byte {} (video delivered to {}, "
              "{} in history), not within the playitem being read",
              pos, video == m_delivered.end() ? -1 : video->second.pos, m_videoPosHistory.size());
    return false;
  }

  m_recueGate.clear();
  for (const auto& [key, mark] : m_delivered)
  {
    // a stream last delivered before the re-read (a sparse subtitle, maybe in
    // an earlier playitem on another timeline) has nothing in it to drop
    if (mark.pos < pos)
      continue;
    if (mark.el || key == video->first ||
        (m_CurrentSubtitle.id >= 0 &&
         key == std::make_pair(static_cast<int64_t>(m_CurrentSubtitle.demuxerId),
                               m_CurrentSubtitle.id)))
    {
      m_recueGate[key] = mark;
      m_recueGate[key].unique = mark.el || key == video->first;
    }
  }
  return true;
#else
  return false;
#endif
}

bool CVideoPlayer::StartRecue(int64_t pos)
{
#if defined(HAVE_LIBBLURAY)
  // the new track failed to open: nothing to re-read for
  if (m_CurrentAudio.id < 0)
  {
    m_recueGate.clear();
    return false;
  }
  // a stream the switch opened (a forced subtitle) takes everything it reads
  for (auto it = m_recueGate.begin(); it != m_recueGate.end();)
  {
    const bool current =
        it->second.el ||
        it->first == std::make_pair(static_cast<int64_t>(m_CurrentVideo.demuxerId),
                                    m_CurrentVideo.id) ||
        it->first == std::make_pair(static_cast<int64_t>(m_CurrentSubtitle.demuxerId),
                                    m_CurrentSubtitle.id);
    it = current ? std::next(it) : m_recueGate.erase(it);
  }
  if (!m_pInputBluray->Recue(pos))
  {
    m_recueGate.clear();
    return false;
  }
  m_pDemuxer->Flush();
  for (auto& [key, mark] : m_recueGate)
    mark.readPos = -1;
  m_recueSince = std::chrono::steady_clock::now();
  m_recueDropped = 0;
  // what the new track has before the clock is dropped, what it has after
  // lands on the running clock (a join, not a start)
  m_CurrentAudio.startpts = m_clock.GetClock();
  m_recueJoinPending = true;
  // the video queue drains while the re-read catches up: a stall grace, as a
  // seek gets at its sync
  m_syncTimer.Set(3000ms);
  CLog::Log(LOGINFO,
            "VideoPlayer: audio track switch re-cued from title byte {} (clock {:.3f}), {} streams "
            "gated, the picture runs on",
            pos, m_clock.GetClock() / DVD_TIME_BASE, m_recueGate.size());
  return true;
#else
  return false;
#endif
}

bool CVideoPlayer::RecueGateDrops(const DemuxPacket* packet)
{
  if (std::chrono::steady_clock::now() - m_recueSince > 10s)
  {
    // a sparse stream (subtitles) may send nothing for that long; a video
    // stream still gated means the re-read never caught up
    const bool videoGated = std::any_of(m_recueGate.begin(), m_recueGate.end(),
                                        [](const auto& entry) { return entry.second.unique; });
    CLog::Log(videoGated ? LOGWARNING : LOGINFO,
              "VideoPlayer: audio re-cue gate closed after 10 s with {} streams gated{}, {} "
              "packets dropped",
              m_recueGate.size(), videoGated ? " (video among them)" : "", m_recueDropped);
    m_recueGate.clear();
    // a track that never reached its sync by now joins nothing later
    m_recueJoinPending = false;
    return false;
  }
  if (packet->iStreamId < 0)
    return false;
  const auto it = m_recueGate.find({packet->demuxerId, packet->iStreamId});
  if (it == m_recueGate.end())
    return false;
  DeliveredMark& mark = it->second;
  // a packet that does not start a PES is where the last one was
  if (packet->streamPos >= 0)
    mark.readPos = packet->streamPos;
  // the demuxer's own dts, as recorded (the gate runs before any correction)
  const double dts = packet->dts;
  bool delivered;
  if (mark.unique && dts != DVD_NOPTS_VALUE && mark.dts != DVD_NOPTS_VALUE)
  {
    // Within a playitem (a re-cue never crosses one) a stream's dts rises in
    // decode order, while the re-read's byte positions need not match the first
    // read's: measured on M3GAN (FEL), the frame delivered before last came back
    // past the last delivered byte, and by position both layers took it twice.
    // A packet sharing the last delivered dts is that frame. Subtitles keep the
    // position: a PGS segment is one PES whose position the re-read keeps (the
    // video parser's packet boundaries are what moved), while a display set's
    // dts need not rise in read order.
    constexpr double SAME_DTS = DVD_MSEC_TO_TIME(1);
    delivered = dts <= mark.dts + SAME_DTS;
  }
  else
    delivered = mark.readPos < mark.pos ||
                (mark.readPos == mark.pos &&
                 (dts == DVD_NOPTS_VALUE || mark.dts == DVD_NOPTS_VALUE || dts <= mark.dts));
  if (delivered)
  {
    m_recueDropped++;
    return true;
  }
  CLog::Log(LOGINFO,
            "VideoPlayer: stream {} leaves the re-cue gate at byte {} dts {:.3f} (last delivered "
            "byte {} dts {:.3f})",
            packet->iStreamId, mark.readPos, dts / DVD_TIME_BASE, mark.pos,
            mark.dts / DVD_TIME_BASE);
  m_recueGate.erase(it);
  if (m_recueGate.empty())
    CLog::Log(LOGINFO, "VideoPlayer: audio re-cue gate closed: {} packets dropped in {} ms",
              m_recueDropped,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - m_recueSince)
                  .count());
  return false;
}

double CVideoPlayer::VideoOffsetPts() const
{
  const auto it = m_seamStreams.find({m_CurrentVideo.demuxerId, m_CurrentVideo.id});
  return m_offset_pts + (it != m_seamStreams.end() ? it->second.offset : 0.0);
}

void CVideoPlayer::UpdateCorrection(DemuxPacket* pkt, double correction)
{
  // the packet's total: a boundary packet is corrected again after the read
  pkt->m_ptsOffsetCorrection += correction;

  if(pkt->dts != DVD_NOPTS_VALUE)
    pkt->dts -= correction;
  if(pkt->pts != DVD_NOPTS_VALUE)
    pkt->pts -= correction;
}

void CVideoPlayer::UpdateTimestamps(CCurrentStream& current, DemuxPacket* pPacket)
{
  double dts = current.dts;
  /* update stored values */
  if(pPacket->dts != DVD_NOPTS_VALUE)
    dts = pPacket->dts;
  else if(pPacket->pts != DVD_NOPTS_VALUE)
    dts = pPacket->pts;

  /* calculate some average duration */
  if(pPacket->duration != DVD_NOPTS_VALUE)
    current.dur = pPacket->duration;
  else if(dts != DVD_NOPTS_VALUE && current.dts != DVD_NOPTS_VALUE)
    current.dur = 0.1 * (current.dur * 9 + (dts - current.dts));

  current.dts = dts;

  current.dispTime = pPacket->dispTime;
}

static void UpdateLimits(double& minimum, double& maximum, double dts)
{
  if(dts == DVD_NOPTS_VALUE)
    return;
  if(minimum == DVD_NOPTS_VALUE || minimum > dts) minimum = dts;
  if(maximum == DVD_NOPTS_VALUE || maximum < dts) maximum = dts;
}

bool CVideoPlayer::CheckContinuity(CCurrentStream& current, DemuxPacket* pPacket)
{
  if (m_playSpeed < DVD_PLAYSPEED_PAUSE)
    return false;

  if( pPacket->dts == DVD_NOPTS_VALUE || current.dts == DVD_NOPTS_VALUE)
    return false;

  double mindts = DVD_NOPTS_VALUE, maxdts = DVD_NOPTS_VALUE;
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts);
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts);
  UpdateLimits(mindts, maxdts, m_CurrentAudio.dts_end());
  UpdateLimits(mindts, maxdts, m_CurrentVideo.dts_end());

  /* if we don't have max and min, we can't do anything more */
  if( mindts == DVD_NOPTS_VALUE || maxdts == DVD_NOPTS_VALUE )
    return false;

  double correction = 0.0;
  bool backwardRestart = false;
  // A timeline jump is joined onto where the old stream ended. When a disc
  // held a still past that end (a short menu clip shown for as long as the
  // disc waits), the clock has run on, and the joined stream would start in
  // the past: every frame late and dropped (Halo S2's intro, joined 21 s
  // behind the clock). Join such a jump just ahead of the clock instead, with
  // one correction for every stream of the jump.
  const double clock = m_clock.GetClock();
  const auto joinCorrection = [&](double end) -> double
  {
    // decided once per jump: the stream that flags it first moves its own end
    // ahead of the clock, which would fail the test for the confirming stream
    if (m_stillJoinCorrection != DVD_NOPTS_VALUE)
      return m_stillJoinCorrection;
    if (m_stillJoinOldEnd)
      return pPacket->dts - end;
    // every stream ran out a second before the clock got here - and the clock
    // is theirs: before a start is presented it is the previous playlist's,
    // and a jump read then (John Wick 3's menu: the reader at the next
    // playitem before the start) belongs on the old end (design 15.59)
    if (clock == DVD_NOPTS_VALUE || clock < maxdts + DVD_MSEC_TO_TIME(1000))
      return pPacket->dts - end;
    if (!ClockOnStreams())
    {
      m_stillJoinOldEnd = true;
      CLog::Log(LOGDEBUG,
                "CVideoPlayer::CheckContinuity - the clock is {:.3f}s past the old stream's end "
                "but not yet these streams' clock: joining onto the old end",
                (clock - maxdts) / DVD_TIME_BASE);
      return pPacket->dts - end;
    }
    m_stillJoinCorrection = pPacket->dts - (clock + DVD_MSEC_TO_TIME(500));
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - the clock ran {:.3f}s past the old stream's end: "
              "joining ahead of the clock (video sync {}, audio sync {})",
              (clock - maxdts) / DVD_TIME_BASE, static_cast<int>(m_CurrentVideo.syncState),
              static_cast<int>(m_CurrentAudio.syncState));
    return m_stillJoinCorrection;
  };
  if( pPacket->dts > maxdts + DVD_MSEC_TO_TIME(1000))
  {
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - resync forward :{}, prev:{:f}, curr:{:f}, diff:{:f}",
              current.type, current.dts, pPacket->dts, pPacket->dts - maxdts);
    correction = joinCorrection(maxdts);
  }

  /* if it's large scale jump, correct for it after having confirmed the jump */
  if (pPacket->dts + DVD_MSEC_TO_TIME(1000) < current.dts_end())
  {
    CLog::Log(
        LOGDEBUG,
        "CVideoPlayer::CheckContinuity - resync backward :{}, prev:{:f}, curr:{:f}, diff:{:f}",
        current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
    correction = joinCorrection(current.dts_end());
    backwardRestart = true;
  }
  else if(pPacket->dts < current.dts)
  {
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - wrapback :{}, prev:{:f}, curr:{:f}, diff:{:f}",
              current.type, current.dts, pPacket->dts, pPacket->dts - current.dts);
  }

  /* A Blu-ray playitem boundary joins two independently timed clips, so the
   * incoming one seldom picks up exactly where the outgoing one stopped. A
   * backward step is closed below for glided seams. A FORWARD step is dead
   * time: the renderer waits it out with
   * nothing to show and the audio bitstream simply stops, which on a TrueHD
   * passthrough sink is long enough to drop lock. It is not elapsed content -
   * a playitem boundary is a cut - so it must be closed, not waited through.
   *
   * The generic forward resync above cannot do it: its 1000ms threshold is
   * there to distinguish a real seek from ordinary jitter, and these steps are
   * a third of a second. M3GAN 2.0's 00801.mpls alternates overlap and gap
   * across its 28 playitems (-0.159, +0.332, -0.430, +0.291, -0.464, +0.346
   * ...), so most of the film's boundaries are affected and none of them reach
   * the threshold. The later ones do (+24.9s, +48.1s) and are already handled.
   * The smallest is +0.059 (playitem 12): the margin is 20 ms, as for an
   * overlap below. Only the playlist's own step (IN - OUT) is closed: a
   * menu loop's outgoing clip can end on a frame gap (its truncated last
   * frame dropped) while the playlist steps back to the loop start, and
   * closing that gap moves audio, which has no such gap.
   *
   * Correct against this stream's OWN end, exactly as the backward restart
   * does. maxdts would be wrong here: it is the furthest-ahead stream, and
   * audio and video do not end a clip on the same timestamp, so using it
   * leaves whichever stream is behind still discontinuous. Measured at the
   * 00058 -> 00059 seam: video gap 0.374s (9 frames, quantised), audio gap
   * 0.333s, playlist step 0.332s - one global correction of each stream's own
   * gap lands both back on their own last dts_end. */
  bool seamStep = false;
  if (m_seamStepPending && m_seamStepArmedDts != DVD_NOPTS_VALUE &&
      current.dts > m_seamStepArmedDts + DVD_MSEC_TO_TIME(1000))
  {
    // the boundary is behind us and no step came - let it go
    m_seamStepPending = false;
    m_seamStepArmedDts = DVD_NOPTS_VALUE;
  }
  const bool forwardGap = correction == 0.0 && m_seamStepPending &&
                          m_playSpeed == DVD_PLAYSPEED_NORMAL &&
                          current.dts_end() != DVD_NOPTS_VALUE &&
                          pPacket->dts > current.dts_end() + DVD_MSEC_TO_TIME(20) &&
                          pPacket->dts < current.dts_end() + DVD_MSEC_TO_TIME(1000);
  const bool playlistStep =
      m_seamStepPlaylistStep && *m_seamStepPlaylistStep > 0.0 &&
      std::abs(pPacket->dts - current.dts_end() - *m_seamStepPlaylistStep) <
          DVD_MSEC_TO_TIME(100);
  if (forwardGap && !playlistStep)
  {
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - forward gap :{} of {:f} left open: the playlist "
              "steps {:f} here",
              current.type, pPacket->dts - current.dts_end(),
              m_seamStepPlaylistStep ? *m_seamStepPlaylistStep : DVD_NOPTS_VALUE);
  }
  if (forwardGap && playlistStep)
  {
    correction = pPacket->dts - current.dts_end();
    seamStep = true;
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - seam step :{}, prev:{:f}, curr:{:f}, closing {:f}",
              current.type, current.dts, pPacket->dts, correction);
  }
  // A backward step in the video timeline: left open, video drops the
  // overlapped frames while audio plays them and is re-timed ~5 s later, which
  // audio then skips to recover. Video only: an audio-only overlap (a cc 5
  // audio frame) must not move video.
  else if (correction == 0.0 && m_seamStepPending && m_seamStepOverlapOk &&
           current.type == StreamType::VIDEO &&
           m_playSpeed == DVD_PLAYSPEED_NORMAL && current.dts_end() != DVD_NOPTS_VALUE &&
           pPacket->dts < current.dts_end() - DVD_MSEC_TO_TIME(20) &&
           pPacket->dts > current.dts_end() - DVD_MSEC_TO_TIME(1000))
  {
    correction = pPacket->dts - current.dts_end();
    seamStep = true;
    CLog::Log(LOGDEBUG,
              "CVideoPlayer::CheckContinuity - seam overlap :{}, prev:{:f}, curr:{:f}, closing {:f}",
              current.type, current.dts, pPacket->dts, correction);
  }

  double lastdts = pPacket->dts;
  if(correction != 0.0)
  {
    // we want the dts values of two streams to close, or for one to be invalid (e.g. from a missing audio stream)
    double this_dts = pPacket->dts;
    double that_dts =
        current.type == StreamType::AUDIO ? m_CurrentVideo.lastdts : m_CurrentAudio.lastdts;

    // Stamp the packet that OPENS the jump. A Blu-ray seamless playitem
    // boundary restarts the clip timeline, and both branches below erase the
    // evidence: the confirmed branch rewrites dts/pts through UpdateCorrection,
    // the unconfirmed one replaces them with DVD_NOPTS_VALUE. The decoder needs
    // to know, because HEVC reference state carries across the seam, and it
    // cannot be told out of band - BD_EVENT_PLAYITEM is raised only after this
    // packet has already been dispatched. Stamping here rides the flag through
    // on the packet itself, ahead of the keyframe it belongs to.
    //
    // Only the first packet of a jump is stamped: the unconfirmed branch
    // re-enters this block for every packet until another stream confirms.
    // Enhancement-layer packets never reach here - ProcessPacket routes them
    // straight to the video player - so the stamp always rides the base layer.
    // The speed test belongs here, not at the consumer: PLAYER_SETSPEED is a
    // priority-1 message and overtakes the packet backlog, so a codec-side gate
    // could read a speed set seconds after this packet was queued and silently
    // drop the flush for a jump that has already been stamped.
    if (backwardRestart && current.type == StreamType::VIDEO && !m_timelineRestartStamped &&
        m_playSpeed == DVD_PLAYSPEED_NORMAL)
    {
      pPacket->timelineRestartSeq = ++m_timelineRestartSeq;
      m_timelineRestartStamped = true;
      CLog::Log(LOGDEBUG,
                "CVideoPlayer::CheckContinuity - timeline restart #{} stamped on video packet "
                "(correction {:f})",
                m_timelineRestartSeq, correction);
    }

    // DEMUX truth, not the presented IsInMenu(): this correction operates on
    // packets at demux time - a wrap occurring inside the presented-state
    // deferral window (BD-J popup path) would silently miss the video-gap
    // preference with the presented gate (review finding A13)
    bool bdMenu = false;
#if defined(HAVE_LIBBLURAY)
    if (m_pInputBluray)
      bdMenu = m_pInputBluray->IsInMenuDemux();
#endif

    if (m_CurrentAudio.id == -1 || m_CurrentVideo.id == -1 ||
       current.lastdts == DVD_NOPTS_VALUE ||
       fabs(this_dts - that_dts) < DVD_MSEC_TO_TIME(1000))
    {
      // BD menu loop wrap: a single global offset can make only ONE stream
      // exactly continuous across the wrap, and the streams' gaps differ
      // (the segment cut is not frame-aligned across audio/video). Stock
      // applies the CONFIRMING stream's gap - usually audio - leaving video
      // ~1-2 frames discontinuous at every wrap (a visible micro-freeze).
      // Prefer video's own gap (recorded below when video first flagged the
      // jump) so the wrap is visually gapless; the small audio residual is
      // absorbed by the audio player's sync skew. The confirmation logic
      // itself is untouched - only the applied value changes, so there is no
      // double-apply risk.
      double applied = correction;
      if (bdMenu && current.type == StreamType::AUDIO && m_menuWrapVideoGap != 0.0 &&
          fabs(m_menuWrapVideoGap - correction) < DVD_MSEC_TO_TIME(100))
      {
        applied = m_menuWrapVideoGap;
        CLog::Log(LOGDEBUG,
                  "CVideoPlayer::CheckContinuity - menu wrap: video-aligned correction {:f} (audio gap {:f})",
                  applied, correction);
      }
      m_menuWrapVideoGap = 0.0;
      m_stillJoinCorrection = DVD_NOPTS_VALUE;
      m_stillJoinOldEnd = false;
      // a correction closing a glided seam: audio must not take it for a
      // timeline step of its own (it reaches a parser-delayed outgoing frame)
      const bool glidedSeam = m_seamStepPending && m_seamStepOverlapOk;
      // the seam has been closed - one boundary, one correction
      m_seamStepPending = false;
      m_seamStepArmedDts = DVD_NOPTS_VALUE;
      m_offset_pts += applied;
      UpdateCorrection(pPacket, applied);
      if (glidedSeam)
      {
        m_seamOffsetPts += applied;
        pPacket->m_seamOffsetCorrection += applied;
      }
      lastdts = pPacket->dts;
      CLog::Log(LOGDEBUG, "CVideoPlayer::CheckContinuity - update correction: {:f}", applied);
      if (current.avsync == CCurrentStream::AV_SYNC_CHECK)
        current.avsync = CCurrentStream::AV_SYNC_CONT;
    }
    else
    {
      // record video's own gap from its FIRST unconfirmed flag of this jump -
      // consumed above if audio ends up being the confirming stream
      if (bdMenu && current.type == StreamType::VIDEO && m_menuWrapVideoGap == 0.0)
        m_menuWrapVideoGap = correction;

      // A seamless playitem boundary is the one case where blanking costs more
      // than it protects. The packet that opens the restart is the incoming
      // clip's IRAP, and it reaches the Amlogic decoder as the single access
      // unit that re-establishes decode after the branch. Blanked, it is fed
      // with dts = pts = DVD_NOPTS_VALUE, which skips the ptsserver check-in
      // in CAMLCodec entirely (check_in_pts() only records a timestamp when
      // avpts != UINT64_0) - so the keyframe the whole new clip is predicted
      // from carries no time base at all. Exactly one access unit per boundary
      // was fed this way, measured, at every boundary of every run.
      //
      // We are not guessing at its timestamp: `correction` here is the value
      // the confirmation applies moments later, and it has been identical at
      // every restart measured - -59225833.333333 from both the stamp and the
      // "update correction" line, boundary after boundary. Apply it to this
      // packet alone and leave the confirmation flow untouched: m_offset_pts is
      // still only advanced once another stream agrees, so nothing downstream
      // sees a correction that was never confirmed, and no packet is corrected
      // twice.
      // A seam step reaches here for the same reason a backward restart does -
      // the other stream is too far away to confirm yet - and the packet in
      // hand is the same one: the incoming clip's IRAP. Blanking it costs
      // exactly what it costs there (dts = pts = NOPTS skips the ptsserver
      // check-in in CAMLCodec, so the keyframe the new clip is predicted from
      // reaches the decoder with no time base), which is the defect the
      // preceding two commits exist to fix. Same treatment, same reasoning.
      if ((backwardRestart || seamStep) && current.type == StreamType::VIDEO &&
          m_playSpeed == DVD_PLAYSPEED_NORMAL)
      {
        UpdateCorrection(pPacket, correction);
        m_videoKeptUnconfirmed = true;
        CLog::Log(LOGDEBUG,
                  "CVideoPlayer::CheckContinuity - {}: keeping the boundary "
                  "keyframe's timestamps (dts {:f}) rather than blanking them",
                  seamStep ? "seam step" : "timeline restart", pPacket->dts);
      }
      else
      {
        // not sure yet - flags the packets as unknown until we get confirmation on another audio/video packet
        pPacket->dts = DVD_NOPTS_VALUE;
        pPacket->pts = DVD_NOPTS_VALUE;
      }
    }
  }
  else
  {
    if (current.type == StreamType::VIDEO)
      m_timelineRestartStamped = false;

    if (current.avsync == CCurrentStream::AV_SYNC_CHECK)
      current.avsync = CCurrentStream::AV_SYNC_CONT;
  }
  current.lastdts = lastdts;
  return true;
}

std::chrono::milliseconds CVideoPlayer::GetEdlTime(const CCurrentStream& current) const
{
  // Input streams that own their timeline (bluray://, dvd://) hand the demuxer a raw stream
  // whose timestamps restart from zero whenever the demuxer is reopened - for instance after
  // an EDL skip that crosses a clip boundary. CheckContinuity() absorbs that jump into
  // m_offset_pts to keep the player clock monotonic, which leaves (dts + m_offset_pts) back
  // near zero rather than at the real position. Such streams stamp packets with the
  // input stream's own display time, so prefer that as the EDL reference.
  if (current.dispTime > 0)
    return std::chrono::milliseconds(current.dispTime);

  return std::chrono::milliseconds(DVD_TIME_TO_MSEC(current.dts + VideoOffsetPts()));
}

std::chrono::milliseconds CVideoPlayer::GetSourceStreamLength() const
{
  // Sources played through an input stream that provides its own timings (eg. bluray:// and
  // dvd://) present the demuxer with a raw stream carrying no container duration, so
  // GetStreamLength() reports 0. Fall back to the input stream, which knows the real length.
  if (m_pDemuxer)
  {
    if (const auto length{std::chrono::milliseconds(m_pDemuxer->GetStreamLength())}; length > 0ms)
      return length;
  }

  if (m_pInputStream)
  {
    CDVDInputStream::ITimes* pTimes = m_pInputStream->GetITimes();
    CDVDInputStream::IDisplayTime* pDisplayTime = m_pInputStream->GetIDisplayTime();
    if (CDVDInputStream::ITimes::Times times; pTimes && pTimes->GetTimes(times))
      return std::chrono::milliseconds(DVD_TIME_TO_MSEC(times.ptsEnd - times.ptsStart));
    if (pDisplayTime && pDisplayTime->GetTotalTime() > 0)
      return std::chrono::milliseconds(pDisplayTime->GetTotalTime());
  }

  return 0ms;
}

bool CVideoPlayer::CheckSceneSkip(const CCurrentStream& current)
{
  if (!m_Edl.HasEdits())
    return false;

  if(current.dts == DVD_NOPTS_VALUE)
    return false;

  if(current.inited == false)
    return false;

  const auto hasEdit = m_Edl.InEdit(GetEdlTime(current));
  return hasEdit && hasEdit.value()->action == EDL::Action::CUT;
}

void CVideoPlayer::QueueAutoSceneSkip(std::chrono::milliseconds seekTime)
{
  const auto streamLength{GetSourceStreamLength()};

  // Use the same 50ms tolerance as the chapter-seek EOF guard (see HandleMessages PLAYER_SEEK_CHAPTER):
  // cut/skip time arithmetic can produce a result fractionally past the true stream end due to
  // millisecond rounding, which would cause the demuxer to stall rather than ending cleanly.
  if (streamLength > 0ms && seekTime + 50ms >= streamLength)
  {
    CLog::Log(LOGDEBUG, "{} - Resolved EDL skip target [{}] is at/near EOF [{}]. Ending playback.",
              __FUNCTION__, StringUtils::MillisecondsToTimeString(seekTime),
              StringUtils::MillisecondsToTimeString(streamLength));

    SetCaching(CACHESTATE_DONE);

    // Abort the processing loop immediately, but keep the playback result as "ended".
    // The caller sets LastEditTime and suppresses any re-trigger (the player is terminating)
    m_bCloseRequest = false;
    m_error = false;
    m_bAbortRequest = true;

    // No point queueing the seek - the loop is terminating, and seeking to the stream end
    // only costs a demuxer seek and a buffer flush before it does.
    return;
  }

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = seekTime.count();
  mode.backward = true;
  mode.accurate = true;
  mode.restore = false;
  mode.trickplay = false;
  mode.sync = true;
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
}

void CVideoPlayer::CheckAutoSceneSkip()
{
  if (!m_Edl.HasEdits())
    return;

  // Check that there is an audio and video stream.
  if((m_CurrentAudio.id < 0 || m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
     (m_CurrentVideo.id < 0 || m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC))
    return;

  // If there is a startpts defined for either the audio or video stream then VideoPlayer is still
  // still decoding frames to get to the previously requested seek point.
  if (m_CurrentAudio.inited == false ||
      m_CurrentVideo.inited == false)
    return;

  const std::chrono::milliseconds clock{GetTime()};

  const std::chrono::milliseconds correctClock = m_Edl.GetTimeAfterRestoringCuts(clock);
  const auto hasEdit = m_Edl.InEdit(correctClock);
  if (!hasEdit)
  {
    // @note: Users are allowed to jump back into EDL commercial breaks
    // do not reset the last edit time if the last surpassed edit is a commercial break
    if (m_Edl.GetLastEditActionType() != EDL::Action::COMM_BREAK)
    {
      m_Edl.ResetLastEditTime();
    }
    return;
  }

  const auto& edit = hasEdit.value();
  if (edit->action == EDL::Action::CUT)
  {
    if ((m_playSpeed > 0 && correctClock < (edit->start + 1s)) ||
        (m_playSpeed < 0 && correctClock < (edit->end - 1s)))
    {
      // Seeking either goes to the start or the end of the cut depending on the play direction.
      std::chrono::milliseconds seek = m_playSpeed >= 0 ? m_Edl.GetNextPlayableTime(edit->end)
                                                        : m_Edl.GetPrevPlayableTime(edit->start);

      // If the resolved seek target lands at the start of a commercial break and
      // auto-skip is enabled, absorb it into this seek so no frames are rendered
      // before the second CheckAutoSceneSkip cycle would otherwise fire.
      // The COMM_BREAK edit is preserved so the user can still seek back into it.
      const std::chrono::milliseconds cutEndSeek{seek};
      std::optional<EDL::Edit> absorbedCommBreak;
      if (m_playSpeed >= 0 && m_SkipCommercials)
      {
        const auto commEdit = m_Edl.InEdit(seek);
        if (commEdit && commEdit.value()->action == EDL::Action::COMM_BREAK)
        {
          absorbedCommBreak = *commEdit.value();
          seek = m_Edl.GetNextPlayableTime(commEdit.value()->end);
        }
      }

      // Act - and log - only once per resolved target. This is re-entered on every pass for as
      // long as the clock still resolves into the cut, which happens whenever playback sits at
      // the end of a leading cut: that position maps back to the cut itself.
      if (m_Edl.GetLastEditTime() != seek)
      {
        // Report correctClock, not clock: the edit boundaries are in raw source time, whereas
        // clock has already had the preceding cuts removed and so would not line up with them.
        CLog::Log(LOGDEBUG, "{} - Clock in EDL cut [{} - {}]: {}. Automatically skipping over.",
                  __FUNCTION__, StringUtils::MillisecondsToTimeString(edit->start),
                  StringUtils::MillisecondsToTimeString(edit->end),
                  StringUtils::MillisecondsToTimeString(correctClock));

        if (absorbedCommBreak)
        {
          CLog::Log(LOGDEBUG,
                    "{} - CUT seek target [{}] lands in commercial break [{} - {}]; "
                    "advancing past it in single seek.",
                    __FUNCTION__, StringUtils::MillisecondsToTimeString(cutEndSeek),
                    StringUtils::MillisecondsToTimeString(absorbedCommBreak->start),
                    StringUtils::MillisecondsToTimeString(absorbedCommBreak->end));
        }

        if (m_playSpeed >= 0 && seek != edit->end)
        {
          CLog::Log(LOGDEBUG, "{} - Resolved cut end [{}] to next playable point [{}].",
                    __FUNCTION__, StringUtils::MillisecondsToTimeString(edit->end),
                    StringUtils::MillisecondsToTimeString(seek));
        }
        else if (m_playSpeed < 0 && seek != edit->start)
        {
          CLog::Log(LOGDEBUG, "{} - Resolved cut start [{}] to prev playable point [{}].",
                    __FUNCTION__, StringUtils::MillisecondsToTimeString(edit->start),
                    StringUtils::MillisecondsToTimeString(seek));
        }

        QueueAutoSceneSkip(seek);
        m_Edl.SetLastEditTime(seek);
        m_Edl.SetLastEditActionType(edit->action);
      }
    }
  }
  else if (edit->action == EDL::Action::COMM_BREAK)
  {
    // marker for commbreak may be inaccurate. allow user to skip into break from the back
    const std::chrono::milliseconds seek = m_Edl.GetNextPlayableTime(edit->end);

    // Compare against correctClock: edit->end is in raw source time, whereas clock has already
    // had the preceding cuts removed and would make this test progressively too permissive as
    // total cut time grows.
    if (m_playSpeed >= 0 && m_Edl.GetLastEditTime() != seek && correctClock < edit->end - 1s)
    {
      CVariant announcement{StringUtils::SecondsToTimeString(
          std::chrono::duration_cast<std::chrono::seconds>(edit->end - edit->start).count(),
          TIME_FORMAT_MM_SS)};
      CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnCommercial",
                                                         announcement);

      // use resolved seek target, not edit->start, to also suppress
      // adjacent commercial breaks encountered while seeking
      m_Edl.SetLastEditTime(seek);
      m_Edl.SetLastEditActionType(edit->action);

      if (m_SkipCommercials)
      {
        // Report correctClock, not clock - see the equivalent note in the CUT branch above.
        CLog::Log(LOGDEBUG,
                  "{} - Clock in commercial break [{} - {}]: {}. Automatically skipping to next "
                  "playable point [{}].",
                  __FUNCTION__, StringUtils::MillisecondsToTimeString(edit->start),
                  StringUtils::MillisecondsToTimeString(edit->end),
                  StringUtils::MillisecondsToTimeString(correctClock),
                  StringUtils::MillisecondsToTimeString(seek));

        QueueAutoSceneSkip(seek);
      }
    }
  }
}

void CVideoPlayer::SynchronizeDemuxer()
{
  if(IsCurrentThread())
    return;
  if(!m_messenger.IsInited())
    return;

  auto message = std::make_shared<CDVDMsgGeneralSynchronize>(500ms, SYNCSOURCE_PLAYER);
  m_messenger.Put(message);
  message->Wait(m_bStop, 0);
}

IDVDStreamPlayer* CVideoPlayer::GetStreamPlayer(unsigned int target)
{
  if(target == VideoPlayer_AUDIO)
    return m_VideoPlayerAudio.get();
  if(target == VideoPlayer_VIDEO)
    return m_VideoPlayerVideo.get();
  if(target == VideoPlayer_SUBTITLE)
    return m_VideoPlayerSubtitle.get();
  if(target == VideoPlayer_TELETEXT)
    return m_VideoPlayerTeletext.get();
  if(target == VideoPlayer_RDS)
    return m_VideoPlayerRadioRDS.get();
  if (target == VideoPlayer_ID3)
    return m_VideoPlayerAudioID3.get();
  return NULL;
}

void CVideoPlayer::SendPlayerMessage(std::shared_ptr<CDVDMsg> pMsg, unsigned int target)
{
  IDVDStreamPlayer* player = GetStreamPlayer(target);
  if(player)
    player->SendMessage(std::move(pMsg), 0);
}

void CVideoPlayer::OnExit()
{
  CLog::Log(LOGINFO, "CVideoPlayer::OnExit()");
  TxMidPlayEnd("stop");
  TxMidClearJobs();
  // 5.E2-8a (D5): a stop before START, ahead of the word's stop and the closes
  // 5.E2-8b D5b (r10 L3, r12 M2): a stop after an earlier exit: RM drops what the ended start
  // still owes (need none-needed, commit-in-flight, a hand-back trigger)
  const unsigned int txEndedGen = TxOpen() ? 0 : m_txEndedGen;
  if (TxOpen())
    EndTransactionToLegacy("stop", CRenderManager::TxHow::NONE,
                           CRenderManager::TxNeed::NONE_NEEDED, false);
  TxNoPictureSummary("stop");
  if (txEndedGen)
    m_renderManager.TxFileEnded(txEndedGen);
  m_txEndedGen = 0;
  m_renderManager.SetTxGraphicsFrom(0); // 5.E2-9b D3: stop
  // 5.E2-8b D5 (lifetime): the job's sink cleared at the player's exit
  TxReleaseJob("stop");
  // 5.E2-3: the shadow word's stop. 5.E2-7 (code review M1): on a real loss path session at a
  // natural end the stop's CAS comes after the two draining closes, so a loss inside them is a
  // real freeze whose held wait the OnExit hook ends; transaction_off keeps it here
  const bool txLateStop = !m_bAbortRequest && aml_transaction_loss_real();
  if (aml_transaction_shadow_on())
  {
    if (const uint32_t wordT = txLateStop ? 0 : m_txWordT.exchange(0))
    {
      m_txStopF = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
      OUTPUT::COutputWorker::Get().WordStop(wordT);
    }
    OUTPUT::COutputWorker::Get().ShadowStartPhase(this, OUTPUT::COutputWorker::StartPhase::NONE,
                                                  0, "stop");
  }
  // 5.E2-12a D4.2 (r1 M-6): after the word's stop
  TxStartDone(static_cast<int>(OUTPUT::COutputWorker::DeferOutcome::STOP), "stop");
  // 5.E2-12b D3: the disc session's hold ends with the stop (held presses dropped)
  // code review M-1 (TWIN of 12b-1's stop clear): the F captured before WordStop
  const uint32_t stopF = TxClearF();
  if (aml_disc_screen_hold())
  {
    OUTPUT::COutputWorker::Get().ClearHeld(stopF, "stop", true);
    TxNoVideoReset("stop");
    OUTPUT::COutputWorker::Get().TakeOwed(stopF);
    OUTPUT::COutputWorker::Get().GapClose(stopF, "stop", true);
    OUTPUT::COutputWorker::Get().ForgetCommitted(stopF);
    m_txOwedPoint = 0;
    m_txRow3cHeld = false;
    m_txNoVideoCapped = false;
  }
  m_renderManager.ArmTestStall("stop");

  ClearPendingElPackets();

  // set event to inform openfile something went wrong in case openfile is still waiting for this event
  SetCaching(CACHESTATE_DONE);

  // close each stream
  if (!m_bAbortRequest)
    CLog::Log(LOGINFO, "VideoPlayer: eof, waiting for queues to empty");

  CFileItem fileItem(m_item);
  UpdateFileItemStreamDetails(fileItem, UpdateStreamDetails::UPDATE_IF_FLAGGED);

  // For blurays/DVDs played through the menu, the last stream may not be the main title
  // In which case get the main title stream details
  // If we determine it was stopped in a menu then m_bCloseRequest should be false (and the OnPlaybackEnded routines triggered)
  //  and m_State can be set to show we are finished (so a bookmark isn't created) by setting time to timeMax
  // If we decide nothing was played then the state is cleared
  // For other input streams no action is taken (as NONE is returned by the default virtual function)
  constexpr double STREAM_FINISHED{std::numeric_limits<double>::max()};
  const CDVDInputStream::UpdateState updateState{
      m_pInputStream->UpdateItemFromSavedStates(fileItem, m_State.time, m_bCloseRequest)};
  switch (updateState)
  {
    using enum CDVDInputStream::UpdateState;
    case FINISHED:
      m_State.timeMax = m_State.time = STREAM_FINISHED;
      break;
    case NOT_PLAYED:
      m_State.Clear();
      break;
    case NONE:
      break;
  }

  // 5.E2-7 test (transaction_exitfreeze): at a natural end, wait up to 10 s for a freeze before
  // the closes (the driver unplugs on this line), so the OnExit hook's row is deterministic
  m_txExitHookLogged = false;
  if (!m_bAbortRequest && aml_transaction_loss_real() && aml_transaction_hooks().exitFreeze)
  {
    CLog::Log(LOGWARNING, "TXLOSS test: exit freeze armed (waiting up to 10 s for a loss)");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!m_bAbortRequest && !m_displayLost && std::chrono::steady_clock::now() < until)
      CThread::Sleep(50ms);
    CLog::Log(LOGWARNING, "TXLOSS test: exit freeze {}", m_displayLost ? "taken" : "not seen");
  }
  {
    CTxDrainSite txSite(m_txDrainSite, "OnExit");
    CloseStream(m_CurrentAudio, !m_bAbortRequest);
  }
  // after the audio's close: cleared before it, a queued packet could open the sink in the
  // old mode. The engine's flag is engine-wide, so it is cleared on every exit
  if (m_startGateShut)
  {
    m_startGateShut = false;
    m_renderManager.SetModeFirstGate(false, m_startGateGen);
    CLog::Log(LOGINFO, "VideoPlayer: start gate ended (set {}): player exit", m_startGateGen);
  }
  m_VideoPlayerAudio->SetStartGated(false);
  if (IAE* ae = CServiceBroker::GetActiveAE())
    ae->SetStartGate(false);
  {
    CTxDrainSite txSite(m_txDrainSite, "OnExit");
    CloseStream(m_CurrentVideo, !m_bAbortRequest);
  }
  if (txLateStop)
  {
    // the ABORT CAS (row 28) before the exit's clear below (D7)
    if (const uint32_t wordT = m_txWordT.exchange(0))
      OUTPUT::COutputWorker::Get().WordStop(wordT);
  }
  // a picture kept at the last join, with no decoder left to replace it
  aml_drop_kept_frame("player exit");
  ReleaseAudioSessionHold();
  CloseStream(m_CurrentTeletext,!m_bAbortRequest);
  CloseStream(m_CurrentRadioRDS, !m_bAbortRequest);
  CloseStream(m_CurrentAudioID3, !m_bAbortRequest);
  // the generalization principle was abused for subtitle player. actually it is not a stream player like
  // video and audio. subtitle player does not run on its own thread, hence waitForBuffers makes
  // no sense here. waitForBuffers is abused to flush the overlay container (false flushes)
  // subtitles are added from video player. after video player has finished, overlays have to be cleared.
  CloseStream(m_CurrentSubtitle, false); // flush overlay container
  // the flush above spares non-flushable overlays (persistent BD menu
  // compositions, kept so they survive in-playback stream reopens) - but this
  // player instance is reused for the next file, so end of playback must wipe
  // the container completely or a menu composition left on screen at stop
  // would leak into the next playback session.
  m_overlayContainer.Clear();

  if (m_renderLoopHandle)
  {
    CServiceBroker::GetWinSystem()->PostUnregisterRenderLoop(m_renderLoopHandle);
    CLog::Log(LOGINFO, "RENDERLOOP unregister posted gen={}", m_renderLoopHandle->gen);
  }
  else
    CServiceBroker::GetWinSystem()->UnregisterRenderLoop(this);

  // 5.E2-7 (fix verification N-L1): an owed conversion step 2 (OnPlayBackPaused) before the
  // file's close callbacks, as HEAD's one-pass conversion
  if (m_txFreezeReal.load() && m_txConvertStep == 1)
    TxConvertStep2();
  IPlayerCallback *cb = &m_callback;
  CVideoSettings vs = m_processInfo->GetVideoSettings();
  m_outboundEvents->Submit([=]() {
    cb->StoreVideoSettings(fileItem, vs);
  });

  CBookmark bookmark;
  bookmark.totalTimeInSeconds = 0;
  bookmark.timeInSeconds = 0;
  if (m_State.startTime == 0)
  {
    bookmark.totalTimeInSeconds = m_State.timeMax / 1000;
    bookmark.timeInSeconds = m_State.time / 1000;
  }
  bookmark.player = m_name;
  bookmark.playerState = GetPlayerState();
  m_outboundEvents->Submit([=]() {
    cb->OnPlayerCloseFile(fileItem, bookmark);
  });

  // destroy objects
  m_renderManager.Flush(false, false);
  m_pDemuxer.reset();
  m_pSubtitleDemuxer.reset();
  m_subtitleDemuxerMap.clear();
  m_pCCDemuxer.reset();
  m_discTime.End();
  // 5.E2-7 (D7): a real freeze ends here, after the closes and the disc time's end (the ABORT
  // CAS came first, at the top)
  if (m_txFreezeReal.load())
    TxClearAtEnd("stop");
  // 5.E2-7 (D9, r2 L6): a VS10 epoch still owed to B2-8 does not outlive the file
  DISPLAYSTATE::P533Clear(0, "stop");
  m_pInputBluray.reset();
  aml_set_disc_mode_hold(false);
  aml_set_disc_mode_anchored(false);
  if (m_pInputStream.use_count() > 1)
    throw std::runtime_error("m_pInputStream reference count is greater than 1");
  m_pInputStream.reset();
  // after the decoders and the disc session have let go of the DV state
  aml_hdr_conversion_player_open(false);

  // clean up all selection streams
  m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_NONE);

  m_messenger.End();

  CFFmpegLog::ClearLogLevel();
  m_bStop = true;

  bool error = m_error;
  bool close = m_bCloseRequest;
  m_outboundEvents->Submit([=]() {
    if (close)
      cb->OnPlayBackStopped();
    else if (error)
      cb->OnPlayBackError();
    else
      cb->OnPlayBackEnded();
  });
}

// 5.E2-7 (C2): HandleMessages' PLAYER_OPENFILE branch, moved out unchanged so a frozen player
// can take a next file (docs/phase5_e2_7_impl.md D3, D7)
void CVideoPlayer::HandleOpenFile(const std::shared_ptr<CDVDMsg>& pMsg)
{
  TxMidPlayEnd("next file");
  // 5.E2-8a (D5): a next file before the engaged start's START
  // 5.E2-8b D5b: a next file after an earlier exit, before BeginFile clears RM's record
  const unsigned int txEndedGen = TxOpen() ? 0 : m_txEndedGen;
  if (TxOpen())
    EndTransactionToLegacy("next file", CRenderManager::TxHow::NONE,
                           CRenderManager::TxNeed::NONE_NEEDED, false);
  TxNoPictureSummary("next file");
  // 5.E2-12a D4.2: a window still open or carried past an earlier exit
  TxStartDone(static_cast<int>(OUTPUT::COutputWorker::DeferOutcome::NEXT_FILE), "next file");
  // code review M-1 (TWIN): the inbox's next file stopped the word first (F captured there)
  const uint32_t nextF = TxClearF();
  if (aml_disc_screen_hold())
  {
    OUTPUT::COutputWorker::Get().ClearHeld(nextF, "next file", true);
    TxNoVideoReset("next file");
    OUTPUT::COutputWorker::Get().TakeOwed(nextF);
    OUTPUT::COutputWorker::Get().GapClose(nextF, "next file", true);
    OUTPUT::COutputWorker::Get().ForgetCommitted(nextF);
    m_txOwedPoint = 0;
    m_txRow3cHeld = false;
    m_txNoVideoCapped = false;
  }
  if (txEndedGen)
    m_renderManager.TxFileEnded(txEndedGen);
  m_txEndedGen = 0;
  TxReleaseJob("next file");
  CDVDMsgOpenFile& msg(*std::static_pointer_cast<CDVDMsgOpenFile>(pMsg));

  IPlayerCallback *cb = &m_callback;
  CFileItem fileItem(m_item);
  UpdateFileItemStreamDetails(fileItem, UpdateStreamDetails::UPDATE_IF_FLAGGED);
  CVideoSettings vs = m_processInfo->GetVideoSettings();
  m_outboundEvents->Submit([=]() {
    cb->StoreVideoSettings(fileItem, vs);
  });

  CBookmark bookmark;
  bookmark.totalTimeInSeconds = 0;
  bookmark.timeInSeconds = 0;
  if (m_State.startTime == 0)
  {
    bookmark.totalTimeInSeconds = m_State.timeMax / 1000;
    bookmark.timeInSeconds = m_State.time / 1000;
  }
  bookmark.player = m_name;
  bookmark.playerState = GetPlayerState();
  m_outboundEvents->Submit([=]() {
    cb->OnPlayerCloseFile(fileItem, bookmark);
  });

  m_item = msg.GetItem();
  m_playerOptions = msg.GetOptions();
  // 5.E2-8b D2: the open message's snapshot
  m_txMsgInputsSeq = msg.GetParams().m_txInputsSeq;
  m_txMsgExits = msg.GetParams().m_txExits;

  m_processInfo->SetPlayTimes(0,0,0,0);

  m_outboundEvents->Submit([this]() {
    m_callback.OnPlayBackStarted(m_item);
  });

  FlushBuffers(DVD_NOPTS_VALUE, true, true);
  m_renderManager.ArmTestStall("nextfile");
  m_renderManager.Flush(false, false);
  m_renderManager.BeginFile();
  m_pDemuxer.reset();
  m_pSubtitleDemuxer.reset();
  m_subtitleDemuxerMap.clear();
  m_pCCDemuxer.reset();
  m_discTime.End();
  m_pInputBluray.reset();
  if (m_pInputStream.use_count() > 1)
    throw std::runtime_error("m_pInputStream reference count is greater than 1");
  m_pInputStream.reset();

  m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_NONE);

  // 5.E2-7 (D7): a next file taken in a span: the freeze cleared after the flush and the disc
  // time's end, before the open (whose newcomer freeze takes the span again, row 29)
  if (m_txClearBeforePrepare)
  {
    m_txClearBeforePrepare = false;
    TxClearAtEnd("next file");
  }

  Prepare(true);
}

void CVideoPlayer::HandleMessages()
{
  std::shared_ptr<CDVDMsg> pMsg = nullptr;

  while (!TxSpeedWaits() && m_messenger.Get(pMsg, 0ms) == MSGQ_OK)
  {
    if (pMsg->IsType(CDVDMsg::PLAYER_OPENFILE) &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_OPENFILE) == 0)
    {
      HandleOpenFile(pMsg);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SEEK) &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK) == 0 &&
        m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
    {
      CDVDMsgPlayerSeek& msg(*std::static_pointer_cast<CDVDMsgPlayerSeek>(pMsg));

      if (!m_State.canseek)
      {
        m_processInfo->SetStateSeeking(false);
        continue;
      }

      // skip seeks if player has not finished the last seek
      if (m_CurrentVideo.id >= 0 &&
          m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC)
      {
        double now = m_clock.GetAbsoluteClock();
        if (UserSpeed() == DVD_PLAYSPEED_NORMAL &&
            (now - m_State.lastSeek)/1000 < 2000 &&
            !msg.GetAccurate())
        {
          m_processInfo->SetStateSeeking(false);
          continue;
        }
      }

      if (!msg.GetTrickPlay())
      {
        m_processInfo->SeekFinished(0);
        SetCaching(CACHESTATE_FLUSH);
      }

      double start = DVD_NOPTS_VALUE;

      double time = msg.GetTime();
      if (msg.GetRelative())
        time = (m_clock.GetClock() + m_State.time_offset) / 1000l + time;

      time = msg.GetRestore()
                 ? m_Edl.GetTimeAfterRestoringCuts(std::chrono::milliseconds(std::lround(time)))
                       .count()
                 : time;

      // if input stream doesn't support ISeekTime, convert back to pts
      //! @todo
      //! After demuxer we add an offset to input pts so that displayed time and clock are
      //! increasing steadily. For seeking we need to determine the boundaries and offset
      //! of the desired segment. With the current approach calculated time may point
      //! to nirvana
      if (m_pInputStream->GetIPosTime() == nullptr)
        time -= m_State.time_offset/1000l;

      CLog::Log(LOGDEBUG, "demuxer seek to: {:f}", time);
      if (m_pDemuxer && m_pDemuxer->SeekTime(time, msg.GetBackward(), &start))
      {
        CLog::Log(LOGDEBUG, "demuxer seek to: {:f}, success", time);
        if(m_pSubtitleDemuxer)
        {
          if(!m_pSubtitleDemuxer->SeekTime(time, msg.GetBackward()))
            CLog::Log(LOGDEBUG, "failed to seek subtitle demuxer: {:f}, success", time);
        }
        // dts after successful seek
        if (start == DVD_NOPTS_VALUE)
          start = DVD_MSEC_TO_TIME(time) - m_State.time_offset;

        m_State.dts = start;
        m_State.lastSeek = m_clock.GetAbsoluteClock();

        m_txFlushCaller = "user seek";
        FlushBuffers(start, msg.GetAccurate(), msg.GetSync());
        CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayer::HandleMessages: flush buffers: dts:{:.3f} lastSeek:{:.3f} clock:{:.3f}", start / 1000000., m_State.lastSeek / 1000000.0, m_clock.GetClock() / 1000000.0);
      }
      else if (m_pDemuxer)
      {
        CLog::Log(LOGDEBUG, "VideoPlayer: seek failed or hit end of stream");
        // dts after successful seek
        if (start == DVD_NOPTS_VALUE)
          start = DVD_MSEC_TO_TIME(time) - m_State.time_offset;

        m_State.dts = start;

        m_txFlushCaller = "failed seek";
        FlushBuffers(start, false, true);
        if (m_playSpeed != DVD_PLAYSPEED_PAUSE)
        {
          SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
        }
      }

      // set flag to indicate we have finished a seeking request
      if(!msg.GetTrickPlay())
      {
        m_processInfo->SeekFinished(0);
      }

      // dvd's will issue a HOP_CHANNEL that we need to skip
      if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        m_dvd.state = DVDSTATE_SEEK;

      m_processInfo->SetStateSeeking(false);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SEEK_CHAPTER) &&
             m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK) == 0 &&
             m_messenger.GetPacketCount(CDVDMsg::PLAYER_SEEK_CHAPTER) == 0)
    {
      m_processInfo->SeekFinished(0);
      SetCaching(CACHESTATE_FLUSH);

      CDVDMsgPlayerSeekChapter& msg(*std::static_pointer_cast<CDVDMsgPlayerSeekChapter>(pMsg));
      double start = DVD_NOPTS_VALUE;
      int64_t offset = 0;
      int newChapter{std::max(1, msg.GetChapter())};
      bool inCut{false};
      bool seekDone{false};
      const int64_t beforeSeek = GetTime();

      const auto FinishChapterSeek = [this, &offset, &start, &newChapter, &seekDone, beforeSeek]()
      {
        m_txFlushCaller = "chapter seek";
        FlushBuffers(start, true, true);
        if (start != DVD_NOPTS_VALUE)
        {
          const int64_t targetTime{
              m_Edl.GetTimeWithoutCuts(std::chrono::milliseconds(DVD_TIME_TO_MSEC(start))).count()};
          offset = targetTime - beforeSeek;
        }
        m_callback.OnPlayBackSeekChapter(newChapter);
        m_processInfo->SeekFinished(offset);
        seekDone = true;
      };

      // newChapter is kept in "visible" chapter numbering throughout (as reported by
      // GetChapter()/GetChapterCount()); it is translated to the demuxer/inputstream's own
      // chapter numbering via ToRawChapter() only at the point of calling into them.
      if (m_pDemuxer)
      {
        const bool forward{newChapter > GetChapter()};
        std::chrono::milliseconds position{m_pDemuxer->GetChapterPos(ToRawChapter(newChapter))};
        const int chapterCount{GetChapterCount()};
        auto inEdit{m_Edl.InEdit(position)};
        inCut = inEdit && inEdit.value()->action == EDL::Action::CUT;
        if (inCut)
        {
          const int step{forward ? 1 : -1};
          while (inCut && ((forward && newChapter < chapterCount) || (!forward && newChapter > 1)))
          {
            newChapter += step;
            position = m_pDemuxer->GetChapterPos(ToRawChapter(newChapter));
            inEdit = m_Edl.InEdit(position);
            inCut = inEdit && inEdit.value()->action == EDL::Action::CUT;
          }
        }

        if (inCut && newChapter == 1)
        {
          // If the first chapter is in an edit, we can't seek to it, so just seek to the end of the edit
          if (m_pDemuxer->SeekTime(static_cast<double>(inEdit.value()->end.count()), true, &start))
            FinishChapterSeek();
        }
        else if (!inCut)
        {
          if (m_pDemuxer->SeekChapter(ToRawChapter(newChapter), &start))
            FinishChapterSeek();
        }
      }
      if (!inCut && !seekDone && m_pInputStream)
      {
        CDVDInputStream::IChapter* pChapter = m_pInputStream->GetIChapter();
        if (pChapter)
        {
          const int rawChapter{ToRawChapter(newChapter)};
          if (pChapter->SeekChapter(rawChapter))
          {
            // IChapter::SeekChapter() has no startpts out param, unlike the demuxer's
            // SeekChapter()/SeekTime(), so approximate the landed position with the
            // chapter's nominal (raw) start instead of leaving start/offset at 0.
            start =
                DVD_MSEC_TO_TIME(static_cast<double>(pChapter->GetChapterPos(rawChapter).count()));
            FinishChapterSeek();
          }
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_RESET))
    {
      m_CurrentAudio.stream = NULL;
      m_CurrentVideo.stream = NULL;
      m_CurrentSubtitle.stream = NULL;

      // we need to reset the demuxer, probably because the streams have changed
      if(m_pDemuxer)
        m_pDemuxer->Reset();
      if(m_pSubtitleDemuxer)
        m_pSubtitleDemuxer->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_AUDIOSTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetAudioStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(StreamType::AUDIO, pMsg2->GetStreamId());
      if(st.source != STREAM_SOURCE_NONE)
      {
        if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if(pStream->SetActiveAudioStream(st.id))
          {
            m_dvd.iSelectedAudioStream = -1;
            CloseStream(m_CurrentAudio, false);
            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = true;
            mode.accurate = true;
            mode.trickplay = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
        else
        {
          bool open = true;
#if defined(HAVE_LIBBLURAY)
          // BD: route the choice into PSR1 (bd_select_stream) so the per-packet
          // live dictation agrees with the user instead of reverting the choice
          // on the next packet (review finding A11). Every Blu-ray mode, not
          // just navmode - see below.
          if (std::shared_ptr<CDVDInputStreamBluray> bluray =
                  std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
              bluray && STREAM_SOURCE_MASK(st.source) == STREAM_SOURCE_DEMUX)
          {
            CDemuxStream* ds =
                m_pDemuxer ? m_pDemuxer->GetStream(st.demuxerId, st.id) : nullptr;
            const bool selected = ds && bluray->SelectAudioStream(ds->dvdNavId);

            // The dictation above is NOT navmode-gated, so it reverts a manual
            // pick in every mode; the write-back must run wherever it runs, or
            // "Play main movie"/simplified-menu/resume playback loses the user's
            // choice on the next audio packet (A11 was only ever fixed for
            // navmode). A refusal is authoritative ONLY in navmode, where a VM
            // really is refusing via the UO mask and the input stream has already
            // toasted; elsewhere a false return just means the pid is absent from
            // this playitem's STN table, and declining to switch at all would turn
            // the control dead instead of merely ineffective.
            if (bluray->GetSupportedMenuType() == MenuType::NATIVE)
              open = selected;
          }
#endif
          if (open)
          {
            int64_t recuePos = -1;
            const bool recue = PrepareRecue(recuePos);
            CloseStream(m_CurrentAudio, false);
            OpenStream(m_CurrentAudio, st.demuxerId, st.id, st.source);
            AdaptForcedSubtitles();

            if (!recue || !StartRecue(recuePos))
            {
              CDVDMsgPlayerSeek::CMode mode;
              mode.time = (int)GetUpdatedTime();
              mode.backward = true;
              mode.accurate = true;
              mode.trickplay = true;
              mode.sync = true;
              m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
            }
          }
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_VIDEOSTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetVideoStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(StreamType::VIDEO, pMsg2->GetStreamId());
      if (st.source != STREAM_SOURCE_NONE)
      {
        if (st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if (pStream->SetAngle(st.id))
          {
            m_dvd.iSelectedVideoStream = st.id;

            CDVDMsgPlayerSeek::CMode mode;
            mode.time = (int)GetUpdatedTime();
            mode.backward = true;
            mode.accurate = true;
            mode.trickplay = true;
            mode.sync = true;
            m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
          }
        }
        else
        {
          CloseStream(m_CurrentVideo, false);
          OpenStream(m_CurrentVideo, st.demuxerId, st.id, st.source);
          CDVDMsgPlayerSeek::CMode mode;
          mode.time = (int)GetUpdatedTime();
          mode.backward = true;
          mode.accurate = true;
          mode.trickplay = true;
          mode.sync = true;
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM))
    {
      auto pMsg2 = std::static_pointer_cast<CDVDMsgPlayerSetSubtitleStream>(pMsg);

      SelectionStream& st = m_SelectionStreams.Get(StreamType::SUBTITLE, pMsg2->GetStreamId());
      if(st.source != STREAM_SOURCE_NONE)
      {
        if(st.source == STREAM_SOURCE_NAV && m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
        {
          std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
          if(pStream->SetActiveSubtitleStream(st.id))
          {
            m_dvd.iSelectedSPUStream = -1;
            CloseStream(m_CurrentSubtitle, false);
          }
        }
        else
        {
          bool open = true;
#if defined(HAVE_LIBBLURAY)
          // BD: route the choice into PSR2 - see the audio handler; the PG
          // dictation is likewise not navmode-gated, so the write-back runs in
          // every Blu-ray mode and a refusal is authoritative only in navmode.
          // DEMUX-source streams only: external subtitles (.srt over a disc)
          // are not in the VM's stream table and must open normally (judge
          // finding - the unconditional gate locked external subs out).
          if (std::shared_ptr<CDVDInputStreamBluray> bluray =
                  std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
              bluray && STREAM_SOURCE_MASK(st.source) == STREAM_SOURCE_DEMUX)
          {
            CDemuxStream* ds =
                m_pDemuxer ? m_pDemuxer->GetStream(st.demuxerId, st.id) : nullptr;
            const bool selected = ds && bluray->SelectSubtitleStream(ds->dvdNavId, true);
            if (bluray->GetSupportedMenuType() == MenuType::NATIVE)
              open = selected;
          }
#endif
          if (open)
          {
            CloseStream(m_CurrentSubtitle, false);
            OpenStream(m_CurrentSubtitle, st.demuxerId, st.id, st.source);

            // For embedded subtitles the demuxer is ahead of playback (AV buffers
            // are full), so the subtitle packets for the current playback time have
            // already been read and discarded. Seek back to the current time so they
            // get re-read, mirroring what audio stream switching does.
            if (STREAM_SOURCE_MASK(st.source) == STREAM_SOURCE_DEMUX)
            {
              CDVDMsgPlayerSeek::CMode mode;
              mode.time = static_cast<double>(GetUpdatedTime());
              mode.backward = true;
              mode.accurate = true;
              mode.trickplay = true;
              mode.sync = true;
              m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
            }
          }
        }
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE))
    {
      bool isVisible = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;

      const auto& ss = m_SelectionStreams.Get(StreamType::SUBTITLE, GetSubtitle());
      const auto& as = m_SelectionStreams.Get(StreamType::AUDIO, GetAudioStream());

      // Image type subtitles (e.g. VOBSUB), ensure to have the stream opened to read overlays
      if (isVisible && StreamUtils::IsCodecSupportForcedOverlay(ss.codecId) &&
          m_CurrentSubtitle.id == -1)
      {
        SetSubtitle(GetSubtitle());
      }

      // SetEnableStream only if not visible, when visible OpenStream already implied that stream is enabled
      if (!isVisible)
        SetEnableStream(m_CurrentSubtitle, false);

#if defined(HAVE_LIBBLURAY)
      // BD: mirror the visibility into PSR2's enable bit so the dictation
      // agrees with what the user sees - every Blu-ray mode, since the
      // dictation itself is not navmode-gated (demux-source streams only -
      // external subs are not in the VM's stream table)
      if (std::shared_ptr<CDVDInputStreamBluray> bluray =
              std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
          bluray && m_CurrentSubtitle.id >= 0 && m_pDemuxer &&
          STREAM_SOURCE_MASK(m_CurrentSubtitle.source) == STREAM_SOURCE_DEMUX)
      {
        if (CDemuxStream* ds =
                m_pDemuxer->GetStream(m_CurrentSubtitle.demuxerId, m_CurrentSubtitle.id))
          bluray->SelectSubtitleStream(ds->dvdNavId, isVisible);
      }
#endif

      SetSubtitleVisibleInternal(isVisible);

      // Image type subtitles (e.g. VOBSUB) can support "forced" flag on overlays (images)
      // so you need to keep the stream open to parse "forced" flag on each image
      // since we leave the stream open by default, it is necessary to close it
      // if the language does not match the preferences.
      if (!isVisible && StreamUtils::IsCodecSupportForcedOverlay(ss.codecId) &&
          !ss.language.Matches(as.language))
      {
        CloseStream(m_CurrentSubtitle, false);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_PROGRAM))
    {
      auto msg = std::static_pointer_cast<CDVDMsgInt>(pMsg);
      if (m_pDemuxer)
      {
        m_pDemuxer->SetProgram(msg->m_value);
        m_txFlushCaller = "set program";
        FlushBuffers(DVD_NOPTS_VALUE, false, true);
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_STATE))
    {
      m_cachingStall = false;
      SetCaching(CACHESTATE_FLUSH);

      auto pMsgPlayerSetState = std::static_pointer_cast<CDVDMsgPlayerSetState>(pMsg);

      if (std::shared_ptr<CDVDInputStream::IMenus> ptr = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream))
      {
        if(ptr->SetState(pMsgPlayerSetState->GetState()))
        {
          m_dvd.state = DVDSTATE_NORMAL;
          m_dvd.iDVDStillStartTime = {};
          m_dvd.iDVDStillTime = 0ms;
        }
      }

      m_processInfo->SeekFinished(0);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      m_txFlushCaller = "GENERAL_FLUSH";
      FlushBuffers(DVD_NOPTS_VALUE, true, true);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      HandleSetSpeed(pMsg);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_FRAME_ADVANCE))
    {
      if (m_playSpeed == DVD_PLAYSPEED_PAUSE)
      {
        int frames = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
        double time = DVD_TIME_BASE / static_cast<double>(m_processInfo->GetVideoFps()) * frames;
        m_processInfo->SetFrameAdvance(true);
        m_clock.Advance(time);
      }
      // 5.E2-9b D1.2: the applied speed is not a pause yet
      else if (UserSpeed() == DVD_PLAYSPEED_PAUSE)
        CLog::Log(LOGINFO, "TXSTART frame advance ignored: pause pending (applied at START)");
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_GUI_ACTION))
      OnAction(std::static_pointer_cast<CDVDMsgType<CAction>>(pMsg)->m_value);
    else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
    {
      SStartMsg& msg = std::static_pointer_cast<CDVDMsgType<SStartMsg>>(pMsg)->m_value;
      if (msg.player == VideoPlayer_AUDIO)
      {
        m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
        m_CurrentAudio.cachetime = msg.cachetime;
        m_CurrentAudio.cachetotal = msg.cachetotal;
        m_CurrentAudio.starttime = msg.timestamp;
        m_CurrentAudio.firststarttime = msg.firstTimestamp;
      }
      // 5.E2-6 (review L2): a re-accept's re-send after the video synced would re-sync: ignored
      if (msg.player == VideoPlayer_VIDEO && msg.txReaccept &&
          m_CurrentVideo.syncState == IDVDStreamPlayer::SYNC_INSYNC)
        CLog::Log(LOGWARNING, "TXGATE re-accept's started ignored: video already synced "
                              "(re-anchor owed to E2-8)");
      else if (msg.player == VideoPlayer_VIDEO)
      {
        m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
        m_CurrentVideo.cachetime = msg.cachetime;
        m_CurrentVideo.cachetotal = msg.cachetotal;
        m_CurrentVideo.starttime = msg.timestamp;
        m_CurrentVideo.starttimePending = msg.timestampPending;
      }
      CLog::Log(LOGDEBUG, "CVideoPlayer::HandleMessages - player started {}", msg.player);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REPORT_STATE))
    {
      SStateMsg& msg = std::static_pointer_cast<CDVDMsgType<SStateMsg>>(pMsg)->m_value;
      if (msg.player == VideoPlayer_AUDIO)
      {
        m_CurrentAudio.syncState = msg.syncState;
      }
      if (msg.player == VideoPlayer_VIDEO)
      {
        m_CurrentVideo.syncState = msg.syncState;
      }
      CLog::Log(LOGDEBUG, "CVideoPlayer::HandleMessages - player {} reported state: {}", msg.player,
                msg.syncState);
    }
    else if (pMsg->IsType(CDVDMsg::SUBTITLE_ADDFILE))
    {
      int id = AddSubtitleFile(std::static_pointer_cast<CDVDMsgType<std::string>>(pMsg)->m_value);
      if (id >= 0)
      {
        SetSubtitle(id);
        SetSubtitleVisibleInternal(true);
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_PLAYER))
        CLog::Log(LOGDEBUG, "CVideoPlayer - CDVDMsg::GENERAL_SYNCHRONIZE");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_AVCHANGE))
    {
      CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
      CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
      CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
      IPlayerCallback *cb = &m_callback;
      m_outboundEvents->Submit([=]() {
        cb->OnAVChange();
      });
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_ABORT))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayer - CDVDMsg::PLAYER_ABORT");
      m_bAbortRequest = true;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SET_UPDATE_STREAM_DETAILS))
      m_updateStreamDetails = true;
    // 5.E2-8b D6: a Start job's reply, acted on at CheckStartGate (D9's order)
    else if (pMsg->IsType(CDVDMsg::PLAYER_TX_REPLY))
      TxOnReply(*pMsg);
  }
}

void CVideoPlayer::SetCaching(ECacheState state)
{
  // only the stall check asks for FULL itself; a flush comes as FLUSH
  const bool stall = state == CACHESTATE_FULL && m_caching == CACHESTATE_DONE &&
                     m_streamPlayerSpeed == DVD_PLAYSPEED_NORMAL &&
                     m_playSpeed == DVD_PLAYSPEED_NORMAL && m_scheduledStart && !m_startHeld &&
                     !m_pInputStream->IsRealtime();
  if(state == CACHESTATE_FLUSH)
  {
    CacheInfo cache = GetCachingTimes();
    if (cache.valid)
      state = CACHESTATE_FULL;
    else
      state = CACHESTATE_INIT;
  }

  if(m_caching == state)
    return;

  // a user pause or a stall keeps the output's audio (design §16.22)
  // (a stall moving on from FULL to INIT is still the same pause)
  double holdFrom = 0.0;
  double holdStep = 0.0;
  if (!(stall && m_renderManager.GetResumePts(holdFrom, holdStep)))
    holdFrom = 0.0;
  else
    holdFrom += m_clock.GetAudioOffset();
  const bool samePause =
      m_cachingStall && (state == CACHESTATE_FULL || state == CACHESTATE_INIT);
  if (!samePause)
    m_clock.SetHoldFrom(holdFrom);
  CLog::Log(LOGDEBUG, LOGVIDEO, "CVideoPlayer::SetCaching - caching state {:d} clock:{:.3f} start pts:{:.3f}",
    state, m_clock.GetClock() / 1000000.0,
    m_CurrentVideo.starttime == DVD_NOPTS_VALUE ? -1.0 : m_CurrentVideo.starttime / 1000000.0);
  if (state == CACHESTATE_FULL ||
      state == CACHESTATE_INIT)
  {
    m_renderManager.SetTxGraphicsFrom(0); // 5.E2-9b D3
    m_clock.SetSpeed(DVD_PLAYSPEED_PAUSE);

    m_VideoPlayerAudio->SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_VideoPlayerVideo->SetSpeed(DVD_PLAYSPEED_PAUSE);
    m_streamPlayerSpeed = DVD_PLAYSPEED_PAUSE;
    if (m_caching == CACHESTATE_DONE)
    {
      m_cachingStall = holdFrom > 0.0;
      m_stallSince = std::chrono::steady_clock::now();
      if (stall)
      {
        PRESENTATION::PlayerReads().stalls.fetch_add(1, std::memory_order_relaxed);
        CLog::Log(LOGINFO, "VideoPlayer: stalled at clock {:.3f}, caching (audio kept from {:.3f})",
                  m_clock.GetClock() / DVD_TIME_BASE, holdFrom / DVD_TIME_BASE);
      }
    }

    m_cachingTimer.Set(5000ms);
  }

  if (state == CACHESTATE_PLAY ||
     (state == CACHESTATE_DONE && m_caching != CACHESTATE_PLAY))
  {
    // a stall resumes on the vblank grid, its sound landing with the picture
    const bool scheduled = m_cachingStall && m_streamPlayerSpeed == DVD_PLAYSPEED_PAUSE &&
                           m_playSpeed == DVD_PLAYSPEED_NORMAL && !m_startHeld &&
                           !m_displayLost && ScheduleUserResume();
    if (m_caching == CACHESTATE_FULL || m_caching == CACHESTATE_INIT)
      CLog::Log(LOGINFO, "VideoPlayer: caching over after {:.3f}s, resume {}",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - m_stallSince)
                    .count(),
                scheduled ? "scheduled" : "at once");
    m_cachingStall = false;
    m_starveResumeArmed = false;
    if (!scheduled)
      m_clock.SetSpeed(m_playSpeed);
    m_VideoPlayerAudio->SetSpeed(m_playSpeed);
    m_VideoPlayerVideo->SetSpeed(m_playSpeed);
    m_streamPlayerSpeed = m_playSpeed;
  }
  m_caching = state;

  m_clock.SetSpeedAdjust(0);
}

// 5.E2-7b (C1): HandleMessages' PLAYER_SETSPEED branch, moved out unchanged (its `continue`
// is this `return`), so the frozen inbox runs the same code (docs/phase5_e2_7b_impl.md D3)
void CVideoPlayer::HandleSetSpeed(const std::shared_ptr<CDVDMsg>& pMsg)
{
  const auto msg = std::static_pointer_cast<CDVDMsgPlayerSetSpeed>(pMsg);
  const int speed = msg->GetSpeed();
  // 5.E2-11 D1.7: a speed change taken in a row-9 span stands over its 15 s conversion
  if (m_txInSpan && speed != UserSpeed())
  {
    m_txSpanSpeedTaken = true;
    CLog::Log(LOGINFO, "TXLOSS speed {} taken in this span: no 15 s pause conversion", speed);
  }
  // 5.E2-9b D3: a speed change ends the overlays' wait for T0 (a play in the lead does not)
  if (speed != m_playSpeed)
    m_renderManager.SetTxGraphicsFrom(0);

  // correct our current clock, as it would start going wrong otherwise; 5.E2-7b (D3 (b)): no
  // time passes on a held picture
  if (m_State.timestamp > 0 && m_txInboxTaking)
    m_State.timestamp = m_clock.GetAbsoluteClock();
  else if (m_State.timestamp > 0)
  {
    double offset;
    offset = m_clock.GetAbsoluteClock() - m_State.timestamp;
    offset *= m_playSpeed / DVD_PLAYSPEED_NORMAL;
    offset = DVD_TIME_TO_MSEC(offset);
    if (offset > 1000)
      offset = 1000;
    if (offset < -1000)
      offset = -1000;
    m_State.time += offset;
    m_State.timestamp = m_clock.GetAbsoluteClock();
  }

  // 5.E2-9b D1.2: the callbacks and the PVR follow the user's speed (a pending one included);
  // the first speed after a next file reads the one Prepare dropped (gate E29B-1 item 5b)
  const int notifiedSpeed = m_txDroppedSpeed != INT_MIN ? m_txDroppedSpeed : UserSpeed();
  m_txDroppedSpeed = INT_MIN;
  if (speed != DVD_PLAYSPEED_PAUSE && notifiedSpeed != DVD_PLAYSPEED_PAUSE &&
      speed != notifiedSpeed)
  {
    m_callback.OnPlayBackSpeedChanged(speed / DVD_PLAYSPEED_NORMAL);
    m_processInfo->SeekFinished(0);
  }

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_PVRMANAGER) && speed != UserSpeed())
  {
    std::shared_ptr<CInputStreamPVRBase> pvrinputstream = std::static_pointer_cast<CInputStreamPVRBase>(m_pInputStream);
    pvrinputstream->Pause(speed == 0);
  }

  const bool isTempoSpeed = msg->IsTempo();
  assert(!(isTempoSpeed &&
           !m_processInfo->IsTempoAllowed(static_cast<float>(speed) / DVD_PLAYSPEED_NORMAL)));

  const bool wasFFRW =
      (m_playSpeed != DVD_PLAYSPEED_NORMAL && m_playSpeed != DVD_PLAYSPEED_PAUSE &&
       !m_processInfo->IsTempoAllowed(static_cast<float>(m_playSpeed) / DVD_PLAYSPEED_NORMAL));

  // Seek when returning to normal 1.0x or tempo play from FF/RW
  // back from RW: clock is not in sync with current pts
  // back from FF: fill the empty audio queue to avoid no audio
  if ((speed == DVD_PLAYSPEED_NORMAL || isTempoSpeed) && wasFFRW)
  {
    double iTime = m_VideoPlayerVideo->GetCurrentPts();
    if (iTime == DVD_NOPTS_VALUE)
      iTime = m_clock.GetClock();
    iTime = (iTime + m_State.time_offset) / 1000;

    CDVDMsgPlayerSeek::CMode mode;
    mode.time = iTime;
    mode.backward = m_playSpeed < 0;
    mode.accurate = true;
    mode.trickplay = true;
    mode.sync = true;
    mode.restore = false;
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  }

  if (isTempoSpeed)
    m_processInfo->SetTempo(static_cast<float>(speed) / DVD_PLAYSPEED_NORMAL);
  else
    m_processInfo->SetSpeed(static_cast<float>(speed) / DVD_PLAYSPEED_NORMAL);

  m_processInfo->SetFrameAdvance(false);

  // 5.E2-9b D1.1 (R26, R31): inside an engaged start (PRIME..SETTLE, no E1 hold) the speed is
  // pending until START; the applied speed, the clock and the players stay as they are
  if (!m_txInboxTaking && TxOpen() && !m_startHeld && !m_pInputStream->IsRealtime() &&
      !m_txSkipPausestart)
  {
    if (speed == m_playSpeed)
    {
      if (m_txPendingSpeed != INT_MIN)
        CLog::Log(LOGINFO, "TXSTART set={} speed {} pending cleared (the applied speed)",
                  m_txEngagedGen, speed);
      m_txPendingSpeed = INT_MIN;
      m_txPendingTempo = false;
      return;
    }
    m_txPendingSpeed = speed;
    m_txPendingTempo = isTempoSpeed;
    if (speed == DVD_PLAYSPEED_PAUSE)
      m_txAskPauseSeen = true; // 5.E2-10 9d (r1 M-5)
    TxAskOpenUpdate();
    CLog::Log(LOGINFO, "TXSTART set={} speed {} pending (applied at START)", m_txEngagedGen,
              speed);
    return;
  }

  m_playSpeed = speed;
  m_txPendingSpeed = INT_MIN; // D1.2: one speed owner

  m_caching = CACHESTATE_DONE;
  m_cachingStall = false;
  // design §16.22: a user pause keeps the audio not yet heard, for the
  // scheduled resume
  double holdFrom = 0.0;
  double holdStep = 0.0;
  if (m_txInboxTaking)
  {
    // 5.E2-7b (D3 (a), r3 N-L1): while frozen the cut is the loss's, VPA's own value when it
    // wrote one, else the freeze's record (0 for a pause failing HEAD's terms). A value already
    // there at the freeze (an offset reland's, an earlier freeze's) is not this freeze's, unless
    // it is this picture's own cut (a paused picture VPA cut again, its play in flight)
    m_txInboxHoldValue = m_clock.GetHoldFrom();
    m_txInboxHoldKept = m_txInboxHoldValue != 0.0 &&
                        (m_txInboxHoldValue != m_txLossHoldBefore.load() ||
                         m_txInboxHoldValue == m_txLossPicture.load());
    if (!m_txInboxHoldKept)
    {
      if (speed != DVD_PLAYSPEED_PAUSE ||
          (m_streamPlayerSpeed == DVD_PLAYSPEED_NORMAL && m_scheduledStart &&
           !m_pInputStream->IsRealtime()))
        m_txInboxHoldValue = m_txLossHoldFrom.load();
      m_clock.SetHoldFrom(m_txInboxHoldValue);
    }
  }
  else
  {
    if (!(speed == DVD_PLAYSPEED_PAUSE && m_streamPlayerSpeed == DVD_PLAYSPEED_NORMAL &&
          m_scheduledStart && !m_pInputStream->IsRealtime() &&
          m_renderManager.GetResumePts(holdFrom, holdStep)))
      holdFrom = 0.0;
    // the audio heard with that picture is offset from it
    else
      holdFrom += m_clock.GetAudioOffset();
    m_clock.SetHoldFrom(holdFrom);
  }
  // a held start stays paused: the release applies the speed
  if (m_startHeld)
  {
    CLog::Log(LOGINFO, "VideoPlayer: speed {} kept for the release of the held start", speed);
    return;
  }
  // live streams keep resuming at once (not testable here)
  if (!(m_streamPlayerSpeed == DVD_PLAYSPEED_PAUSE && speed == DVD_PLAYSPEED_NORMAL &&
        !isTempoSpeed && m_scheduledStart && !m_displayLost &&
        !m_pInputStream->IsRealtime() && ScheduleUserResume()))
    m_clock.SetSpeed(speed);
  m_VideoPlayerAudio->SetSpeed(speed);
  m_VideoPlayerVideo->SetSpeed(speed);
  m_streamPlayerSpeed = speed;
}

void CVideoPlayer::SetPlaySpeed(int speed)
{
  if (IsPlaying())
  {
    CDVDMsgPlayerSetSpeed::SpeedParams params = { speed, false };
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSpeed>(params));
  }
  else
  {
    m_playSpeed = speed;
    m_streamPlayerSpeed = speed;
  }
}

void CVideoPlayer::ApplySpeed(int speed, bool atCommit)
{
  m_playSpeed = speed;
  m_txPendingSpeed = INT_MIN;
  // SetCaching's PLAY entry applies m_playSpeed (a seek's FLUSH -> INIT, r3 M5)
  if (!atCommit && (m_caching == CACHESTATE_INIT || m_caching == CACHESTATE_FULL))
    return;
  // nothing was heard before the commit: no hold point (HoldStart and the flush write 0 too)
  m_clock.SetHoldFrom(0.0);
  m_cachingStall = false;
  m_clock.SetSpeed(speed);
  m_VideoPlayerAudio->SetSpeed(speed);
  m_VideoPlayerVideo->SetSpeed(speed);
  m_streamPlayerSpeed = speed;
  // m_caching left alone: PLAY's caching end still runs; DONE stays DONE (HandleSetSpeed's body)
}

bool CVideoPlayer::CanPause() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.canpause;
}

void CVideoPlayer::Pause()
{
  // toggle between pause and normal speed
  if (m_processInfo->GetNewSpeed() == 0)
  {
    SetSpeed(1);
  }
  else
  {
    SetSpeed(0);
  }
}

void CVideoPlayer::SignalStreamsReady()
{
  // Activate the fullscreen-video skin now that streams are ready, so
  // video frames will fully paint the swap chain from the first frame
  // the skin is visible.
  if (m_playerOptions.fullscreen)
  {
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_SWITCHTOFULLSCREEN);
  }

  IPlayerCallback *cb = &m_callback;
  CFileItem fileItem = m_item;
  m_outboundEvents->Submit([=]() {
    cb->OnAVStarted(fileItem);
  });
  m_State.streamsReady = true;
}

int CVideoPlayer::DiscWaitState(bool drain)
{
#if defined(HAVE_LIBBLURAY)
  if (std::shared_ptr<CDVDInputStreamBluray> bluray =
          std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
    return bluray->GetBdjWaitState(drain);
#endif
  return 0;
}

void CVideoPlayer::CheckMenuOnlyStart(bool noPlaylist)
{
  // Playback start is normally announced when the streams sync. A disc screen
  // with no playlist has no streams, and a real player shows it over black:
  // once the disc has put menu graphics up, announce the start so the busy
  // dialog releases the remote and the fullscreen video window presents the
  // screen. Most BD-J discs draw something (a spinner, a logo) before their
  // first playlist, so only a screen that stays without one qualifies.
  constexpr auto MENU_ONLY_GRACE = 3s;

  bool candidate = !m_State.streamsReady && noPlaylist && m_CurrentVideo.id < 0 &&
                   m_CurrentAudio.id < 0;
#if defined(HAVE_LIBBLURAY)
  std::shared_ptr<CDVDInputStreamBluray> bluray =
      std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
  candidate = candidate && bluray && bluray->HasOverlayGraphics();
#else
  candidate = false;
#endif
  if (!candidate)
  {
    m_menuOnlyCandidateSince = {};
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_menuOnlyCandidateSince == std::chrono::steady_clock::time_point{})
    m_menuOnlyCandidateSince = now;
  if (now - m_menuOnlyCandidateSince < MENU_ONLY_GRACE)
    return;

  CLog::Log(LOGINFO, "VideoPlayer: disc screen with no playlist is up, starting menu-only playback");

  // A DV disc session requests the DV output engage at disc open (applied at
  // the first DV mode set). With no video the DV core presents nothing - the screen stays black
  // (John Wick 3's resume prompt; shows at once with DV disabled). Release the
  // engage for this screen only: the first DV-output segment re-engages it in
  // OpenStream before its decoder opens, the same path a mixed disc takes.
  // Normal starts never get here (a playlist inside the grace period).
  if (aml_dv_disc_engaged() || aml_dv_disc_engage_pending())
  {
    CLog::Log(LOGINFO, "VideoPlayer: releasing the DV disc engage for the menu-only screen");
    aml_dv_release_disc_engage();
    bluray->SetMenuOnlyNativeGraphics(true);
  }

  m_discMenuOnly = true;
  SetCaching(CACHESTATE_DONE);
  SignalStreamsReady();
}

bool CVideoPlayer::HasVideo() const
{
  return m_HasVideo || m_discMenuOnly;
}

bool CVideoPlayer::HasAudio() const
{
  return m_HasAudio;
}

bool CVideoPlayer::HasRDS() const
{
  return m_CurrentRadioRDS.id >= 0;
}

bool CVideoPlayer::HasID3() const
{
  return m_CurrentAudioID3.id >= 0;
}

bool CVideoPlayer::IsPassthrough() const
{
  return m_VideoPlayerAudio->IsPassthrough();
}

bool CVideoPlayer::CanSeek() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.canseek;
}

void CVideoPlayer::Seek(bool bPlus, bool bLargeStep, bool bChapterOverride)
{
  if (!m_State.canseek)
    return;

  const int64_t time = GetTime();
  const Direction direction = bPlus ? Direction::FORWARD : Direction::BACKWARD;
  const SeekStep step = bLargeStep ? SeekStep::LARGE : SeekStep::NORMAL;

  if (step == SeekStep::LARGE && bChapterOverride)
  {
    std::vector<SeekCandidate> candidates;
    if (auto candidate = GetChapterSeekCandidate(time, direction); candidate.has_value())
      candidates.push_back(std::move(*candidate));
    if (auto candidate = GetBookmarkSeekCandidate(time, direction); candidate.has_value())
      candidates.push_back(std::move(*candidate));

    if (!candidates.empty())
    {
      auto bestCandidateIt = std::ranges::min_element(candidates, std::less<>{},
                                                      [time](const CVideoPlayer::SeekCandidate& c)
                                                      { return std::abs(c.targetTime - time); });
      if (bestCandidateIt != candidates.end())
      {
        bestCandidateIt->action();
        return;
      }
    }
  }

  if (auto candidate = GetTimeOrPercentSeekCandidate(time, direction, step); candidate.has_value())
    candidate->action();
}

void CVideoPlayer::ExecuteTimeSeek(int64_t target, Direction direction, bool accurate)
{
  const int64_t time = GetTime();
  CDVDMsgPlayerSeek::CMode mode;
  mode.time = target;
  mode.backward = direction == Direction::BACKWARD;
  mode.accurate = accurate;
  mode.restore = true;
  mode.trickplay = false;
  mode.sync = true;
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  SynchronizeDemuxer();
  if (target < 0)
    target = 0;
  m_callback.OnPlayBackSeek(target, target - time);
}

int64_t CVideoPlayer::CalcTimeOrPercentSeekTarget(int64_t time,
                                                  int64_t maxTime,
                                                  Direction direction,
                                                  SeekStep step)
{
  const std::shared_ptr<CAdvancedSettings> advancedSettings =
      CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();

  if (advancedSettings == nullptr)
    return time;

  // Calculate a time based jump
  auto fnTimeDelta = [&advancedSettings, step, direction]()
  {
    int64_t delta = 0;
    if (step == SeekStep::LARGE)
      delta = direction == Direction::FORWARD ? advancedSettings->m_videoTimeSeekForwardBig
                                              : advancedSettings->m_videoTimeSeekBackwardBig;
    else
      delta = direction == Direction::FORWARD ? advancedSettings->m_videoTimeSeekForward
                                              : advancedSettings->m_videoTimeSeekBackward;
    return delta * 1000;
  };

  // Calculate a percentage based jump
  auto fnPercentDelta = [&advancedSettings, step, direction, maxTime]()
  {
    int percent = 0;
    if (step == SeekStep::LARGE)
      percent = direction == Direction::FORWARD ? advancedSettings->m_videoPercentSeekForwardBig
                                                : advancedSettings->m_videoPercentSeekBackwardBig;
    else
      percent = direction == Direction::FORWARD ? advancedSettings->m_videoPercentSeekForward
                                                : advancedSettings->m_videoPercentSeekBackward;
    return maxTime * percent / 100;
  };

  int64_t delta = 0;

  if (!advancedSettings->m_videoSmoothPercentToTimeSeeking &&
      advancedSettings->m_videoUseTimeSeeking &&
      maxTime > 2000 * advancedSettings->m_videoTimeSeekForwardBig)
  {
    delta = fnTimeDelta();
  }
  else if (!advancedSettings->m_videoSmoothPercentToTimeSeeking)
  {
    delta = fnPercentDelta();
  }
  else
  {
    const int64_t percentDelta = fnPercentDelta();
    const int64_t timeDelta = fnTimeDelta();

    delta = direction == Direction::FORWARD ? std::min(percentDelta, timeDelta)
                                            : std::max(percentDelta, timeDelta);
  }

  return time + delta;
}

std::optional<CVideoPlayer::SeekCandidate> CVideoPlayer::GetTimeOrPercentSeekCandidate(
    int64_t time, Direction direction, SeekStep step)
{
  const int64_t target =
      CalcTimeOrPercentSeekTarget(time, m_processInfo->GetMaxTime(), direction, step);

  return SeekCandidate{target, [this, target, time, direction]()
                       {
                         if (g_application.CurrentFileItem().IsStack() &&
                             (target > m_processInfo->GetMaxTime() || target < 0))
                         {
                           g_application.SeekTime((target - time) * 0.001 +
                                                  g_application.GetTime());
                           return;
                         }
                         ExecuteTimeSeek(target, direction, false);
                       }};
}

std::optional<CVideoPlayer::SeekCandidate> CVideoPlayer::GetChapterSeekCandidate(
    int64_t time, Direction direction)
{
  const int chapter = GetChapter();

  const bool hasValidChapters = GetChapterCount() > 1 && chapter > 0;
  if (!hasValidChapters)
    return std::nullopt;

  const int targetChapter = direction == Direction::BACKWARD ? GetPreviousChapter() : chapter + 1;
  const std::optional<std::chrono::milliseconds> tsChapter = GetChapterPosMs(targetChapter);

  if (!tsChapter.has_value())
    return std::nullopt;

  const int64_t chapterTime = tsChapter.value().count();
  return SeekCandidate{chapterTime, [this, targetChapter]() { SeekChapter(targetChapter); }};
}

std::optional<CVideoPlayer::SeekCandidate> CVideoPlayer::GetBookmarkSeekCandidate(
    int64_t time, Direction direction)
{
  if (!HasBookmarks())
    return std::nullopt;

  const std::chrono::milliseconds ts{time};
  const std::optional<std::chrono::milliseconds> tsBookmark =
      direction == Direction::BACKWARD ? GetBookmarkPos(GetPreviousBookmark(ts))
                                       : GetBookmarkPos(GetNextBookmark(ts));
  if (!tsBookmark.has_value())
    return std::nullopt;

  const int64_t bookmarkTime = tsBookmark.value().count();
  return SeekCandidate{bookmarkTime, [this, bookmarkTime, direction]()
                       { ExecuteTimeSeek(bookmarkTime, direction, true); }};
}

bool CVideoPlayer::SeekScene(Direction seekDirection)
{
  if (!m_Edl.HasSceneMarker())
    return false;

  /*
   * There is a 5 second grace period applied when seeking for scenes backwards. If there is no
   * grace period applied it is impossible to go backwards past a scene marker.
   */
  auto clock = std::chrono::milliseconds(GetTime());
  if (seekDirection == Direction::BACKWARD && clock > 5s) // 5 seconds
    clock -= 5s;

  const std::optional<std::chrono::milliseconds> sceneMarker =
      m_Edl.GetNextSceneMarker(seekDirection, clock);
  if (sceneMarker)
  {
    /*
     * Seeking is flushed and inaccurate, just like Seek()
     */
    CDVDMsgPlayerSeek::CMode mode;
    mode.time = sceneMarker.value().count();
    mode.backward = seekDirection == Direction::BACKWARD;
    mode.accurate = false;
    mode.restore = false;
    mode.trickplay = false;
    mode.sync = true;

    m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
    SynchronizeDemuxer();
    return true;
  }
  return false;
}

void CVideoPlayer::GetGeneralInfo(std::string& strGeneralInfo)
{
  if (!m_bStop)
  {
    double apts = m_VideoPlayerAudio->GetCurrentPts();
    double vpts = m_VideoPlayerVideo->GetCurrentPts();
    double dDiff = 0;

    if (apts != DVD_NOPTS_VALUE && vpts != DVD_NOPTS_VALUE)
      dDiff = (apts - vpts) / DVD_TIME_BASE;

    std::string strBuf;
    std::unique_lock lock(m_StateSection);
    if (m_State.cache_bytes >= 0)
    {
      strBuf += StringUtils::Format("forward: {} / {:2.0f}% / {:6.3f}s / {:.3f}%",
                                    StringUtils::SizeToString(m_State.cache_bytes),
                                    m_State.cache_level * 100.0, m_State.cache_time,
                                    m_State.cache_offset * 100.0);
    }

    strGeneralInfo = StringUtils::Format("Player: a/v:{: 6.3f}, {}", dDiff, strBuf);
  }
}

void CVideoPlayer::SeekPercentage(float iPercent)
{
  int64_t iTotalTime = m_processInfo->GetMaxTime();

  if (!iTotalTime)
    return;

  SeekTime((int64_t)(iTotalTime * iPercent / 100));
}

float CVideoPlayer::GetPercentage()
{
  int64_t iTotalTime = m_processInfo->GetMaxTime();

  if (!iTotalTime)
    return 0.0f;

  return GetTime() * 100 / (float)iTotalTime;
}

float CVideoPlayer::GetCachePercentage() const
{
  std::unique_lock lock(m_StateSection);
  return (float) (m_State.cache_offset * 100); // NOTE: Percentage returned is relative
}

void CVideoPlayer::SetAVDelay(float fValue)
{
  m_processInfo->GetVideoSettingsLocked().SetAudioDelay(fValue);
  if (m_audioOffsetToAudio)
  {
    m_audioOffsetChangedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    m_requestedAudioOffset = static_cast<double>(fValue) * DVD_TIME_BASE;
  }
  else
    m_renderManager.SetDelay(static_cast<int>(fValue * 1000.0f));
}

float CVideoPlayer::GetAVDelay()
{
  if (m_audioOffsetToAudio)
    return static_cast<float>(m_requestedAudioOffset.load() / DVD_TIME_BASE);
  return static_cast<float>(m_renderManager.GetDelay()) / 1000.0f;
}

void CVideoPlayer::ApplyAudioOffset()
{
  // a slow display shows the picture late: the sound waits for it
  const double requested = m_requestedAudioOffset;
  const double tweak = m_renderManager.GetLatencyTweak();
  const double want = requested - DVD_MSEC_TO_TIME(tweak);
  // 5.V2-4b: what a landing took
  const auto logLanding = [&](const char* site)
  {
    CLog::Log(LOGINFO,
              "VTIMING landing {}: audio offset {:+.3f} ms (requested {:+.3f} ms, tweak {:.3f} ms)",
              site, want * 1000.0 / DVD_TIME_BASE, requested * 1000.0 / DVD_TIME_BASE, tweak);
  };
  if (want == m_landedAudioOffset)
    return;
  // a display reset landed it already
  if (want == m_clock.GetAudioOffset())
  {
    m_landedAudioOffset = want;
    return;
  }
  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  // the +/- keys repeat: take the value they settle on
  if (nowMs - m_audioOffsetChangedMs < 500)
    return;
  // paused, at a trick speed, no audio landed yet, or the display lost (a mode
  // change sets the latency tweak then): the next start lands with it
  if (m_playSpeed != DVD_PLAYSPEED_NORMAL || m_CurrentAudio.id < 0 || !m_CurrentAudio.inited ||
      m_startHeld || m_displayLost ||
      ((m_caching == CACHESTATE_FULL || m_caching == CACHESTATE_INIT) && !m_cachingStall))
  {
    m_clock.SetAudioOffset(want);
    m_landedAudioOffset = want;
    CLog::Log(LOGINFO, "VideoPlayer: audio offset {:+.0f} ms, for the next start",
              want * 1000.0 / DVD_TIME_BASE);
    logLanding("next-start");
    return;
  }
  // a stall or a scheduled lead: after its landing
  if (m_caching != CACHESTATE_DONE || m_clock.IsPaused() ||
      m_streamPlayerSpeed != DVD_PLAYSPEED_NORMAL)
    return;
  CLog::Log(LOGINFO, "VideoPlayer: audio offset {:+.0f} ms, re-landed mid-play",
            want * 1000.0 / DVD_TIME_BASE);
  m_VideoPlayerAudio->SendMessage(
      std::make_shared<CDVDMsgDouble>(CDVDMsg::PLAYER_AUDIO_RELAND, want), 1);
  m_landedAudioOffset = want;
  logLanding("mid-play");
}

void CVideoPlayer::SetSubTitleDelay(float fValue)
{
  m_processInfo->GetVideoSettingsLocked().SetSubtitleDelay(fValue);
  m_VideoPlayerVideo->SetSubtitleDelay(static_cast<double>(-fValue) * DVD_TIME_BASE);
}

float CVideoPlayer::GetSubTitleDelay()
{
  return (float) -m_VideoPlayerVideo->GetSubtitleDelay() / DVD_TIME_BASE;
}

bool CVideoPlayer::GetSubtitleVisible() const
{
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);
    return pStream->IsSubtitleStreamEnabled();
  }

  return m_VideoPlayerVideo->IsSubtitleEnabled();
}

void CVideoPlayer::SetSubtitleVisible(bool bVisible)
{
  m_messenger.Put(
      std::make_shared<CDVDMsgBool>(CDVDMsg::PLAYER_SET_SUBTITLESTREAM_VISIBLE, bVisible));
  m_processInfo->GetVideoSettingsLocked().SetSubtitleVisible(bVisible);
  CVariant data;
  data["player"]["playerid"] = m_item.GetProperty("playlist_type_hint").asInteger32(-1);
  data["property"]["subtitleenabled"] = bVisible;
  CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnPropertyChanged",
                                                     data);
}

void CVideoPlayer::SetEnableStream(CCurrentStream& current, bool isEnabled)
{
  // Guard against stale stream IDs reaching the demuxer after a stream
  // change. CloseStream() calls this method with stream identification coming
  // from m_Current* members that may belong to a previous demuxer session.
  // m_SelectionStreams mirrors the current demuxer's state, so a stale stream
  // from a prior session won't be found and EnableStream() won't be called.
  if (m_pDemuxer && STREAM_SOURCE_MASK(current.source) == STREAM_SOURCE_DEMUX &&
      m_SelectionStreams.Contains(current.type, current.source, current.demuxerId, current.id))
  {
    m_pDemuxer->EnableStream(current.demuxerId, current.id, isEnabled);
  }
}

void CVideoPlayer::SetSubtitleVisibleInternal(bool bVisible)
{
  m_VideoPlayerVideo->EnableSubtitle(bVisible);

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
    std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream)->EnableSubtitleStream(bVisible);

  // NOTE: DV L5 "osdst" subtitle visibility is NOT reported from here - track
  // enablement is not the same as text being on screen (an enabled sub track
  // stays enabled for the whole film, which would keep the bars un-masked/grey
  // the entire time). It is driven per-frame from actual overlay visibility in
  // CRenderManager::Render instead. See CBitstreamConverter::processDoviRpu.

  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();
}

void CVideoPlayer::SetSubtitleVerticalPosition(int value, bool save)
{
  m_processInfo->GetVideoSettingsLocked().SetSubtitleVerticalPosition(value, save);
  m_renderManager.SetSubtitleVerticalPosition(value, save);
}

std::shared_ptr<TextCacheStruct_t> CVideoPlayer::GetTeletextCache()
{
  if (m_CurrentTeletext.id < 0)
    return nullptr;

  return m_VideoPlayerTeletext->GetTeletextCache();
}

bool CVideoPlayer::HasTeletextCache() const
{
  return m_CurrentTeletext.id >= 0;
}

void CVideoPlayer::LoadPage(int p, int sp, unsigned char* buffer)
{
  if (m_CurrentTeletext.id < 0)
      return;

  return m_VideoPlayerTeletext->LoadPage(p, sp, buffer);
}

void CVideoPlayer::SeekTime(int64_t iTime)
{
  int64_t seekOffset = iTime - GetTime();

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = static_cast<double>(iTime);
  mode.backward = true;
  mode.accurate = true;
  mode.trickplay = false;
  mode.sync = true;

  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  SynchronizeDemuxer();
  m_callback.OnPlayBackSeek(iTime, seekOffset);
  m_processInfo->SeekFinished(seekOffset);
}

bool CVideoPlayer::SeekTimeRelative(int64_t iTime)
{
  int64_t abstime = GetTime() + iTime;

  // if the file has EDL cuts we can't rely on m_clock for relative seeks
  // EDL cuts remove time from the original file, hence we might seek to
  // positions too far from the current m_clock position. Seek to absolute
  // time instead
  if (m_Edl.HasCuts())
  {
    SeekTime(abstime);
    return true;
  }

  CDVDMsgPlayerSeek::CMode mode;
  mode.time = (int)iTime;
  mode.relative = true;
  mode.backward = (iTime < 0) ? true : false;
  mode.accurate = false;
  mode.trickplay = false;
  mode.sync = true;

  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
  m_processInfo->SetStateSeeking(true);

  m_callback.OnPlayBackSeek(abstime, iTime);
  m_processInfo->SeekFinished(iTime);
  return true;
}

// return the time in milliseconds
int64_t CVideoPlayer::GetTime() const
{
  std::unique_lock lock(m_StateSection);
  return llrint(m_State.time);
}

void CVideoPlayer::SetSpeed(float speed)
{
  // can't rewind in menu as seeking isn't possible
  // forward is fine
  if (speed < 0 && IsInMenu())
    return;

  if (!CanSeek() && !CanPause())
    return;

  int iSpeed = static_cast<int>(speed * DVD_PLAYSPEED_NORMAL);

  if (!CanSeek())
  {
    if ((iSpeed != DVD_PLAYSPEED_NORMAL) && (iSpeed != DVD_PLAYSPEED_PAUSE))
      return;
  }

  float currentSpeed = m_processInfo->GetNewSpeed();
  m_processInfo->SetNewSpeed(speed);
  if (iSpeed != currentSpeed)
  {
    if (iSpeed == DVD_PLAYSPEED_NORMAL)
      m_callback.OnPlayBackResumed();
    else if (iSpeed == DVD_PLAYSPEED_PAUSE)
      m_callback.OnPlayBackPaused();

    if (iSpeed == DVD_PLAYSPEED_NORMAL)
    {
      float currentTempo = m_processInfo->GetNewTempo();
      if (currentTempo != 1.0f)
      {
        SetTempo(currentTempo);
        return;
      }
    }
    SetPlaySpeed(iSpeed);
  }
}

void CVideoPlayer::SetTempo(float tempo)
{
  tempo = floor(tempo * 100.0f + 0.5f) / 100.0f;
  if (m_processInfo->IsTempoAllowed(tempo))
  {
    int speed = tempo * DVD_PLAYSPEED_NORMAL;
    CDVDMsgPlayerSetSpeed::SpeedParams params = { speed, true };
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSpeed>(params));

    m_processInfo->SetNewTempo(tempo);
  }
}

void CVideoPlayer::FrameAdvance(int frames)
{
  float currentSpeed = m_processInfo->GetNewSpeed();
  if (currentSpeed != DVD_PLAYSPEED_PAUSE)
    return;

  m_messenger.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_FRAME_ADVANCE, frames));
}

bool CVideoPlayer::SupportsTempo() const
{
  return m_State.cantempo;
}

bool CVideoPlayer::OpenStream(CCurrentStream& current, int64_t demuxerId, int iStream, int source, bool reset /*= true*/)
{
  CDemuxStream* stream = NULL;
  CDVDStreamInfo hint;

  CLog::Log(LOGINFO, "Opening stream: {} source: {}", iStream, source);

  if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_DEMUX_SUB)
  {
    int index = m_SelectionStreams.TypeIndexOf(current.type, source, demuxerId, iStream);
    if (index < 0)
      return false;
    const SelectionStream& st = m_SelectionStreams.Get(current.type, index);

    CLog::Log(LOGINFO, "Opening Subtitle file: {}", CURL::GetRedacted(st.filename));
    m_pSubtitleDemuxer.reset();
    const auto demux = m_subtitleDemuxerMap.find(demuxerId);
    if (demux == m_subtitleDemuxerMap.end())
    {
      CLog::Log(LOGINFO, "No demuxer found for file {}", CURL::GetRedacted(st.filename));
      return false;
    }

    m_pSubtitleDemuxer = demux->second;

    double pts = m_VideoPlayerVideo->GetCurrentPts();
    if(pts == DVD_NOPTS_VALUE)
      pts = m_CurrentVideo.dts;
    if(pts == DVD_NOPTS_VALUE)
      pts = 0;
    pts += VideoOffsetPts();
    if (!m_pSubtitleDemuxer->SeekTime((int)(1000.0 * pts / (double)DVD_TIME_BASE)))
      CLog::Log(LOGDEBUG, "{} - failed to start subtitle demuxing from: {:f}", __FUNCTION__, pts);
    stream = m_pSubtitleDemuxer->GetStream(demuxerId, iStream);
    if(!stream || stream->disabled)
      return false;

    m_pSubtitleDemuxer->EnableStream(demuxerId, iStream, true);

    hint.Assign(*stream, true);
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_TEXT)
  {
    int index = m_SelectionStreams.TypeIndexOf(current.type, source, demuxerId, iStream);
    if(index < 0)
      return false;

    hint.Clear();
    hint.filename = m_SelectionStreams.Get(current.type, index).filename;
    hint.fpsscale = m_CurrentVideo.hint.fpsscale;
    hint.fpsrate  = m_CurrentVideo.hint.fpsrate;
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_DEMUX)
  {
    if(!m_pDemuxer)
      return false;

    m_pDemuxer->OpenStream(demuxerId, iStream);

    stream = m_pDemuxer->GetStream(demuxerId, iStream);
    if (!stream || stream->disabled)
      return false;

    hint.Assign(*stream, true);

    if(m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
      hint.filename = "dvd";
    if (hint.fpsrate / hint.fpsscale > 200) {
      hint.fpsrate = 60000;
      hint.fpsscale = 1001;
    }
  }
  else if(STREAM_SOURCE_MASK(source) == STREAM_SOURCE_VIDEOMUX)
  {
    if(!m_pCCDemuxer)
      return false;

    stream = m_pCCDemuxer->GetStream(iStream);
    if(!stream || stream->disabled)
      return false;

    hint.Assign(*stream, false);
  }

  bool res;
  switch(current.type)
  {
    case StreamType::AUDIO:
      res = OpenAudioStream(hint, reset);
      break;
    case StreamType::VIDEO:
    {
      // Resolve the Dolby Vision VS10 output mode from the *real* source type
      // (before it is faked to DV below) and stash it for CAMLCodec. Then fake
      // Dolby Vision when either the legacy SDR2DV/HDR2DV toggle or a non-bypass
      // VS10 selection wants the DV engine engaged.
      unsigned int vs10Mode = aml_vs10_by_hdrtype(hint.hdrType, hint.bitdepth);
#if defined(HAVE_LIBBLURAY)
      // Disc-session DV latch: menu-domain segments of a DV disc that carry no
      // DV stream of their own (FirstPlay bumper, menu loops) are VS10-mapped
      // into the latched DV output instead of dropping the HDMI DV signalling
      // (a ~2s TV resync per drop). Selected titles keep their native format
      // by default; <video><discsessionconformnondv> extends the mapping to
      // NON-menu non-DV titles (extras/featurettes) too - without it such a
      // title trips the DV<->non-DV forced mode switch mid-session, the
      // review's Sony-hostile corner. Default OFF: format-demo discs (S&M)
      // rely on selected titles keeping native HDR10/HLG output.
      if (vs10Mode == DOLBY_VISION_OUTPUT_MODE_BYPASS &&
          hint.hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION &&
          aml_dv_disc_session() && m_pInputBluray)
      {
        const bool menuDomain = m_pInputBluray->IsMenuDomainVideo();
        const bool conformAll = CServiceBroker::GetSettingsComponent()
                                    ->GetAdvancedSettings()
                                    ->m_videoDiscSessionConformNonDV;
        if (menuDomain || conformAll)
        {
          vs10Mode = DOLBY_VISION_OUTPUT_MODE_IPT;
          CLog::Log(LOGDEBUG, "CVideoPlayer::OpenStream - DV disc session: "
                    "{} segment VS10-mapped to DV output",
                    menuDomain ? "menu-domain" : "non-DV title (conform)");
        }
        else
        {
          CLog::Log(LOGDEBUG, "CVideoPlayer::OpenStream - DV disc session: "
                    "non-DV selected title keeps native output "
                    "(discsessionconformnondv=true maps it to DV instead)");
          // Mixed disc: the session VSIF hold blocks the passive DV signal
          // drop this branch used to rely on - actively release the engage
          // so the title's native EOTF signals (else: black screen).
          aml_dv_release_disc_engage();
        }
      }
      // Mixed disc coming back onto a DV-output segment (native DV title, or
      // a segment VS10-mapped into the DV output) after a release: request the
      // re-engage, applied by that segment's DV mode set.
      if (aml_dv_disc_session() && !aml_dv_disc_engaged() &&
          (hint.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION ||
           vs10Mode != DOLBY_VISION_OUTPUT_MODE_BYPASS))
        aml_dv_pre_engage_disc_session();
      // Session mode-hold: while the segment about to open is menu-domain,
      // the resolution chooser keeps the incumbent display mode - no HDMI
      // re-clock for a menu's refresh rate (strict sinks re-train on every
      // re-clock; the feature still takes its one correct switch).
      {
        bool hold = false;
        if (m_pInputBluray &&
            CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_videoDiscSessionModeHold)
          hold = m_pInputBluray->IsMenuDomainVideo();
        aml_set_disc_mode_hold(hold);
      }
#endif
      // HDR10+ -> DV 8.1 dynamic conversion takes precedence over a static VS10
      // (or legacy HDR2DV) HDR10->DV mapping. An HDR10+ source presents as plain
      // HDR10 here (files) or STN-promoted HDR10PLUS (discs), so faking it to DV
      // now would strip its dynamic metadata into the static VS10 engine before
      // the codec can detect HDR10+. Defer: clear the pending and leave the hint
      // native so CDVDVideoCodecAmlogic's 2-stage engage owns it - it converts
      // real HDR10+ (dynamic RPU) and, on a plain-HDR10 miss, re-applies the VS10
      // mapping itself (AddData fallback).
      // A VS10 mapping to a non-DV output (SDR or HDR10) is the user's choice of
      // output and wins over the conversion (user ruling 2026-10-11): the stream
      // then opens exactly as with the conversion off.
      // phase 5 step 5.E1b (log only): the same decision from inputs taken now
      AmlHdrOpenInputs hdrIn;
      hdrIn.hdrType = hint.hdrType;
      hdrIn.vs10Mode = vs10Mode;
      hdrIn.convertByVsEngine = aml_convert_to_dv_by_vs_engine(hint.hdrType);
      hdrIn.deviceSupportsDv = aml_support_dolby_vision();
      hdrIn.displaySupportsDv = aml_display_support_dv();
      hdrIn.hdr10plusConvert = OUTPUT::CSettingsMirror::Get().GetBool(
          CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT);
      const AmlHdrOpenDecision hdrShadow = aml_dv_resolve_stream_open(hdrIn);
      const bool vs10NonDvTarget = vs10Mode == DOLBY_VISION_OUTPUT_MODE_HDR10 ||
                                   vs10Mode == DOLBY_VISION_OUTPUT_MODE_SDR10 ||
                                   vs10Mode == DOLBY_VISION_OUTPUT_MODE_SDR8;
      const bool hdr10plusConvertCandidate =
          (hint.hdrType == StreamHdrType::HDR_TYPE_HDR10 ||
           hint.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS) &&
          !vs10NonDvTarget && aml_support_dolby_vision() && aml_display_support_dv() &&
          OUTPUT::CSettingsMirror::Get().GetBool(
              CSettings::SETTING_COREELEC_AMLOGIC_DV_HDR10PLUS_CONVERT);
      if (hdr10plusConvertCandidate)
        vs10Mode = DOLBY_VISION_OUTPUT_MODE_BYPASS;
      aml_dv_set_vs10_pending(vs10Mode);
      if (!hdr10plusConvertCandidate &&
          (aml_convert_to_dv_by_vs_engine(hint.hdrType) ||
           vs10Mode != DOLBY_VISION_OUTPUT_MODE_BYPASS))
        hint.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
      CLog::Log(LOGINFO, "RESOLVE hdr open shadow: {} type {}/{} vs10 {}/{} (source {})",
                hdrShadow.hdrType == hint.hdrType && hdrShadow.vs10Pending == vs10Mode ? "MATCH"
                                                                                         : "DIFF",
                CStreamDetails::HdrTypeToString(hdrShadow.hdrType),
                CStreamDetails::HdrTypeToString(hint.hdrType), hdrShadow.vs10Pending, vs10Mode,
                CStreamDetails::HdrTypeToString(hdrIn.hdrType));
      res = OpenVideoStream(hint, reset);
      // A new video segment is the only point where the menu-domain
      // low-latency clamp may shrink the queues.
      if (res)
        UpdateMenuDomainQueueDepth(true);
      // Set the m_bFullScreenVideo flag now, before streamsReady, so the
      // renderer's Configure() sees a valid viewport via GetViewWindow().
      // The WINDOW_FULLSCREEN_VIDEO skin activation is deferred to
      // HandlePlaySpeed after streamsReady.
      if (res && m_playerOptions.fullscreen &&
          !CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenVideo())
      {
#if defined(HAS_LIBAMCODEC)
        // 5.V2-6: main sets it before its next FrameMove, which owns the geometry
        if (m_renderLoopHandle)
        {
          if (!m_renderLoopHandle->fullScreenPending.exchange(true))
          {
            // 5.V3-3: a request exists only for a post (the exchange above succeeded)
            const uint64_t seq = CServiceBroker::GetWinSystem()->PostFullScreenVideo(
                m_renderLoopHandle,
                aml_geometrytick_inputs_on() ? &m_renderManager.FullScreenRequest() : nullptr);
            CLog::Log(LOGINFO, "VCONFIGURE fullscreen notice posted gen={}",
                      m_renderLoopHandle->gen);
            if (seq)
              CLog::Log(LOGINFO, "GEOMDIAG fullscreen notice seq {} gen={}", seq,
                        m_renderLoopHandle->gen);
            const SGeometryTickHooks hooks = aml_geometrytick_hooks();
            // test (geometrytick_gfxhold): main's SetFullScreenVideo blocks on the gfx lock
            if (hooks.gfxHoldMs > 0)
              std::thread(
                  [ms = hooks.gfxHoldMs]
                  {
                    std::unique_lock lock(CServiceBroker::GetWinSystem()->GetGfxContext());
                    CLog::Log(LOGWARNING, "GEOMETRYTICK test: gfx lock taken for {} ms", ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                  })
                  .detach();
            // test (geometrytick_fsbeforenotice): the fullscreen window comes up in Process()
            // before DriveRenderLoop takes the notice
            if (hooks.fsBeforeNotice)
            {
              CLog::Log(LOGWARNING, "GEOMETRYTICK test: ACTION_SHOW_GUI posted with the notice");
              CServiceBroker::GetAppMessenger()->PostMsg(
                  TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                  static_cast<void*>(new CAction(ACTION_SHOW_GUI)));
            }
          }
          break;
        }
#endif
        auto& gfx = CServiceBroker::GetWinSystem()->GetGfxContext();
        gfx.SetFullScreenVideo(true);
        const CRect view = gfx.GetViewWindow();
        CLog::Log(LOGDEBUG,
                  "CVideoPlayer::OpenStream: m_bFullScreenVideo set pre-Configure, "
                  "viewport {:.0f}x{:.0f}",
                  view.Width(), view.Height());
      }
      break;
    }
    case StreamType::SUBTITLE:
      // A PGS palette carries no colorimetry of its own; it is authored to
      // match the video stream it accompanies, per BD-ROM Part 3.
      if (hint.codec == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
      {
        hint.colorSpace = m_CurrentVideo.hint.colorSpace;
        hint.colorPrimaries = m_CurrentVideo.hint.colorPrimaries;
        hint.colorTransferCharacteristic = m_CurrentVideo.hint.colorTransferCharacteristic;

        // The video's transfer tag misses PQ-authored titles: Dolby Vision
        // profile 5 leaves it unspecified, and a DV output then runs an
        // untagged PQ palette through the GUI composite, encoding it to PQ a
        // second time (washed, near-white colours). Decide the regime the way
        // the pre-20260909 PG pre-invert did and tag PQ so the HDR overlay path
        // owns it. Disc playback: the playlist's STN dynamic range. Files: the
        // demuxer's pristine hdr_type - NOT m_CurrentVideo.hint.hdrType, which
        // the VS10 engage rewrites to DOLBYVISION even for SDR sources.
        bool pqAuthored = false;
        bool haveDiscRegime = false;
#if defined(HAVE_LIBBLURAY)
        if (std::shared_ptr<CDVDInputStreamBluray> bluray =
                std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
        {
          pqAuthored = bluray->HasPqAuthoredGraphics();
          haveDiscRegime = true;
        }
#endif
        if (!haveDiscRegime && m_pDemuxer && m_CurrentVideo.id >= 0)
        {
          CDemuxStream* videoStream =
              m_pDemuxer->GetStream(m_CurrentVideo.demuxerId, m_CurrentVideo.id);
          if (videoStream && videoStream->type == StreamType::VIDEO)
          {
            const StreamHdrType hdr = static_cast<CDemuxStreamVideo*>(videoStream)->hdr_type;
            pqAuthored = hdr == StreamHdrType::HDR_TYPE_HDR10 ||
                         hdr == StreamHdrType::HDR_TYPE_HDR10PLUS ||
                         hdr == StreamHdrType::HDR_TYPE_DOLBYVISION;
          }
        }
        if (pqAuthored)
          hint.colorTransferCharacteristic = AVCOL_TRC_SMPTE2084;
        CLog::Log(LOGDEBUG, "CVideoPlayer::OpenStream - PGS palette regime: {} ({}, video trc {})",
                  hint.colorTransferCharacteristic == AVCOL_TRC_SMPTE2084 ? "PQ" : "not PQ",
                  haveDiscRegime ? "disc STN" : "video hdr_type",
                  static_cast<int>(m_CurrentVideo.hint.colorTransferCharacteristic));
      }
      res = OpenSubtitleStream(hint);
      break;
    case StreamType::TELETEXT:
      res = OpenTeletextStream(hint);
      break;
    case StreamType::RADIO_RDS:
      res = OpenRadioRDSStream(hint);
      break;
    case StreamType::AUDIO_ID3:
      res = OpenAudioID3Stream(hint);
      break;
    default:
      res = false;
      break;
  }

#if defined(HAS_LIBAMCODEC)
  // test (geometrytick_reopenpending): the video path once more with the notice pending; its
  // exchange finds the flag set and posts nothing
  if (res && current.type == StreamType::VIDEO && m_renderLoopHandle &&
      m_renderLoopHandle->fullScreenPending && aml_geometrytick_hooks().reopenPending &&
      !std::exchange(m_testReopenDone, true))
  {
    CLog::Log(LOGWARNING, "GEOMETRYTICK test: video OpenStream again, notice pending gen={}",
              m_renderLoopHandle->gen);
    return OpenStream(current, demuxerId, iStream, source, reset);
  }
#endif

  if (res)
  {
    int oldId = current.id;
    current.id = iStream;
    current.demuxerId = demuxerId;
    current.source = source;
    current.hint = hint;
    current.stream = (void*)stream;
    current.lastdts = DVD_NOPTS_VALUE;
    if (oldId >= 0 && current.avsync != CCurrentStream::AV_SYNC_FORCE)
      current.avsync = CCurrentStream::AV_SYNC_CHECK;
    if(stream)
      current.changes = stream->changes;
  }
  else
  {
    if(stream)
    {
      /* mark stream as disabled, to disallow further attempts*/
      CLog::Log(LOGWARNING, "{} - Unsupported stream {}. Stream disabled.", __FUNCTION__,
                stream->uniqueId);
      stream->disabled = true;

      CCurrentStream failedStream = current;
      failedStream.id = iStream;
      failedStream.demuxerId = demuxerId;
      failedStream.source = source;
      SetEnableStream(failedStream, false);
    }
  }

  UpdateContentState();
  CServiceBroker::GetDataCacheCore().SignalAudioInfoChange();
  CServiceBroker::GetDataCacheCore().SignalVideoInfoChange();
  CServiceBroker::GetDataCacheCore().SignalSubtitleInfoChange();

  return res;
}

bool CVideoPlayer::OpenAudioStream(CDVDStreamInfo& hint, bool reset)
{
  m_syncStartPtsWait.reset();
  m_syncStartDeferred = false;
  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentAudio.player);
  if(player == nullptr)
    return false;

  // see the matching note in OpenVideoStream (BD menu->title decoder reuse)
  bool reuse = false;
  if (m_bdStreamReuseAudio && m_CurrentAudio.id >= 0)
  {
    reuse = m_CurrentAudio.hint.Equal(hint,
                                      CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
    CLog::Log(LOGINFO, "CVideoPlayer::OpenAudioStream - BD stream reuse {}{}",
              reuse ? "MATCH, reattaching running decoder" : "no format match, normal reopen: ",
              reuse ? "" : m_CurrentAudio.hint.Differences(
                               hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID));
  }
  // one-shot (see OpenVideoStream)
  m_bdStreamReuseAudio = false;

  if (m_closedAudioValid)
  {
    m_closedAudioValid = false;
    const std::string diff = m_closedAudioHint.Differences(
        hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
    CLog::Log(LOGINFO, "CVideoPlayer::OpenAudioStream - join: {}",
              m_closedAudioHint.Equal(hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID)
                  ? "same format as the closed stream"
                  : "differs from the closed stream: " + diff);
  }
  if(!reuse && (m_CurrentAudio.id < 0 ||
     m_CurrentAudio.hint != hint))
  {
    if (!player->OpenStream(hint))
      return false;

    const bool lostAtSend = m_displayLost;
    player->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, lostAtSend), 1);
    TxResendPause(player, lostAtSend, "audio");

    static_cast<IDVDStreamPlayerAudio*>(player)->SetSpeed(m_streamPlayerSpeed);
    m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_STARTING;
    m_CurrentAudio.packets = 0;
  }
  else if (reset)
    player->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESET), 0);

  m_HasAudio = true;

  static_cast<IDVDStreamPlayerAudio*>(player)->SendMessage(
      std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_REQUEST_STATE), 1);

  return true;
}

bool CVideoPlayer::OpenVideoStream(CDVDStreamInfo& hint, bool reset)
{
  m_syncStartPtsWait.reset();
  m_syncStartDeferred = false;
  m_CurrentVideo.starttimePending = false;
  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    /* set aspect ratio as requested by navigator for dvd's */
    float aspect = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream)->GetVideoAspectRatio();
    if (aspect != 0.0f)
    {
      hint.aspect = static_cast<double>(aspect);
      hint.forced_aspect = true;
    }
    hint.dvd = true;
  }

  std::shared_ptr<CDVDInputStream::IMenus> pMenus = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);
#if defined(HAVE_LIBBLURAY)
  // BD: the stills stamp is segment classification and keys on DEMUX-side
  // menu-domain truth, unconditionally - not on IsInMenu(), which (a) BD-J
  // discs hold true across a menu->title start (overlay stays up; stills is
  // not inert downstream: AMLCodec::OpenDecoder excludes stills from the
  // dual-layer MEL stream-mode routing, so a feature stamped as a still
  // opened frame-mode, lost its first GOP and re-grew the m2ts EOS judder)
  // and (b) is now the clock-deferred PRESENTED state, which lags the
  // segment being opened by up to the queue depth. IsMenuDomainVideo()
  // applies the playlist-duration leave-predicate on demux truth.
  if (std::shared_ptr<CDVDInputStreamBluray> bluray =
          std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
    hint.stills = bluray->IsMenuDomainVideo();
  else
#endif
  if (pMenus && pMenus->IsInMenu())
    hint.stills = true;

  // a content start may re-clock a 1000/1001 rate difference; on a navigated disc only a
  // feature-length playlist: short non-menu playlists are bumpers and warnings
  hint.reclockEligible = !hint.stills;
#if defined(HAVE_LIBBLURAY)
  if (std::shared_ptr<CDVDInputStreamBluray> bluray =
          std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream);
      bluray && bluray->IsNavigationMode())
    hint.reclockEligible = hint.reclockEligible && bluray->IsFeatureLengthPlaylist();
#endif

  if (hint.stereo_mode.empty())
  {
#if defined(HAS_LIBAMCODEC)
    // 5.V2-6: P does not reach into the GUI
    if (m_renderLoopHandle)
      hint.stereo_mode = CStereoscopicsManager::DetectStereoModeByPath(m_item.GetDynPath());
    else
#endif
    if (CGUIComponent* gui = CServiceBroker::GetGUI(); gui != nullptr)
    {
      const CStereoscopicsManager &stereoscopicsManager = gui->GetStereoscopicsManager();
      hint.stereo_mode = stereoscopicsManager.DetectStereoModeByString(m_item.GetDynPath());
    }
  }

  if (hint.flags & AV_DISPOSITION_ATTACHED_PIC)
    return false;

  const auto fullScreenRoot = [this]
  {
#if defined(HAS_LIBAMCODEC)
    // 5.V2-6: published by GC's writes, read without the GUI
    if (m_renderLoopHandle)
      return DISPLAYSTATE::FullScreenRoot();
#endif
    return CServiceBroker::GetWinSystem()->GetGfxContext().IsFullScreenRoot();
  };
  // set desired refresh rate
  if (m_CurrentVideo.id < 0 && !m_restartingStreamPlayer && m_playerOptions.fullscreen &&
      fullScreenRoot() && hint.fpsrate != 0 && hint.fpsscale != 0)
  {
    if (OUTPUT::CSettingsMirror::Get().GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) !=
        ADJUST_REFRESHRATE_OFF)
    {
      const double framerate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration(
                                                   (double)DVD_TIME_BASE * hint.fpsscale /
                                                   (hint.fpsrate * (hint.interlaced ? 2 : 1)));

#if defined(HAS_LIBAMCODEC)
      if (m_renderLoopHandle)
        CLog::Log(LOGINFO, "VCONFIGURE refresh-rate trigger raised by the open gen={}",
                  m_renderLoopHandle->gen);
#endif
      m_renderManager.TriggerUpdateResolution(framerate, hint.width, hint.height, hint.stereo_mode);
    }
  }

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentVideo.player);
  if(player == nullptr)
    return false;

  // BD menu->title jump with the stream players kept alive: reattach the
  // running decoder when the format matches ignoring the demuxer/stream id
  // (a new demuxer only changes the ids) and - where it is inert in the
  // decoder - the menu stills flag (differs by definition when leaving a
  // menu). For dual-layer DV (profile 7/4) MEL the stills flag is NOT inert:
  // it selects the decoder's frame-vs-stream dec_mode (AMLCodec::OpenDecoder),
  // so reattaching across a stills change would keep the wrong dec_mode and
  // bring back the m2ts-boundary judder; only FEL (stream mode either way,
  // per the running decoder's RPU parse) may ignore it.
  bool reuse = false;
  if (m_bdStreamReuseVideo && m_CurrentVideo.id >= 0)
  {
    const bool stillsSelectsDecMode =
        (hint.dovi.dv_profile == 7 || hint.dovi.dv_profile == 4) &&
        !(m_processInfo && m_processInfo->GetDoviIsFEL());
    CDVDStreamInfo cmp(hint);
    if (!stillsSelectsDecMode)
      cmp.stills = m_CurrentVideo.hint.stills;
    reuse = m_CurrentVideo.hint.Equal(cmp, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
    CLog::Log(LOGINFO, "CVideoPlayer::OpenVideoStream - BD stream reuse {}{}",
              reuse ? "MATCH, reattaching running decoder" : "no format match, normal reopen: ",
              reuse ? "" : m_CurrentVideo.hint.Differences(
                               cmp, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID));
    // menu -> feature on the running decoder: no open to carry the arm, and the
    // running rate is the feature's
    if (reuse && m_CurrentVideo.hint.stills && hint.reclockEligible)
      m_renderManager.ArmContentReclock(true);
    TxKeepaliveVerdict(reuse);
  }
  // one-shot: each open decision consumes its own flag (streams may be opened
  // lazily per-packet in either order when the disc dictates them, so the
  // flags cannot be cleared on a fixed point in the reopen path)
  m_bdStreamReuseVideo = false;

  if (m_closedVideoValid)
  {
    m_closedVideoValid = false;
    const std::string diff = m_closedVideoHint.Differences(
        hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID);
    CLog::Log(LOGINFO, "CVideoPlayer::OpenVideoStream - join: {}",
              m_closedVideoHint.Equal(hint, CDVDStreamInfo::COMPARE_ALL & ~CDVDStreamInfo::COMPARE_ID)
                  ? "same format as the closed stream"
                  : "differs from the closed stream: " + diff);
  }
  if(!reuse && (m_CurrentVideo.id < 0 ||
     m_CurrentVideo.hint != hint))
  {
    if (hint.codec == AV_CODEC_ID_MPEG2VIDEO || hint.codec == AV_CODEC_ID_H264)
      m_pCCDemuxer.reset();

    const bool freshPlayer = !player->IsInited();
    if (!player->OpenStream(hint))
    {
      aml_drop_kept_frame("video open failed");
      return false;
    }

    // a fresh player's queue is empty: a segment marker queued in the old one
    // may have gone with it (a running one takes STREAMCHANGE in order instead)
    if (freshPlayer)
    {
      m_renderManager.SetIncomingSegmentGen(m_segmentGen);
      if (m_segmentGenPublished != m_segmentGen && m_menuPageWaiting)
        m_menuPageWaitSince = std::chrono::steady_clock::now();
      m_segmentGenPublished = m_segmentGen;
    }

    const bool lostAtSend = m_displayLost;
    player->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, lostAtSend), 1);
    TxResendPause(player, lostAtSend, "video");

    const std::shared_ptr<CDVDInputStream::IExtentionStream>  pExt = std::dynamic_pointer_cast<CDVDInputStream::IExtentionStream>(m_pInputStream);
    if (pExt && !static_cast<IDVDStreamPlayerVideo*>(player)->SupportsExtention())
      pExt->DisableExtention();

    // look for any EDL files
    m_Edl.Clear();
    float fFramesPerSecond = 0.0f;
    if (m_CurrentVideo.hint.fpsscale > 0.0f)
      fFramesPerSecond = static_cast<float>(m_CurrentVideo.hint.fpsrate) / static_cast<float>(m_CurrentVideo.hint.fpsscale);
    m_Edl.ReadEditDecisionLists(m_item, fFramesPerSecond, GetSourceStreamLength());
    CServiceBroker::GetDataCacheCore().SetEditList(m_Edl.GetEditList());
    CServiceBroker::GetDataCacheCore().SetCuts(m_Edl.GetCutMarkers());
    CServiceBroker::GetDataCacheCore().SetSceneMarkers(m_Edl.GetSceneMarkers());

    VECBOOKMARKS bm;
    if (CBookmark::GetBookmarksForFile(m_item.GetDynPath(), bm, {CBookmark::STANDARD}))
    {
      std::vector<std::chrono::milliseconds> pos = CBookmark::BookmarksToPositions(bm);
      SetBookmarks(pos);
    }

    static_cast<IDVDStreamPlayerVideo*>(player)->SetSpeed(m_streamPlayerSpeed);
    m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_STARTING;
    m_CurrentVideo.packets = 0;
  }
  else if (reset)
    player->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_RESET), 0);

  m_HasVideo = true;
  m_discMenuOnly = false;
  // 5.E2-12b D3 (Q4, r1 H-4): a video stream ends the hold (a KEEPALIVE reuse MATCH included)
  if (aml_disc_screen_hold())
  {
    const OUTPUT::COutputWorker::SHeldClear cleared = OUTPUT::COutputWorker::Get().ClearHeld(
        OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f,
        "video open", false);
    if (cleared.mainDropped)
    {
      std::string stereomode;
      m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode, "held clear");
    }
    if (m_txNoVideoHold || m_txNoVideoCarry || m_txNoVideoStep)
      TxNoVideoReset("video open");
    m_txNoVideoCapped = false;
    // 5.E2-12b-2 D4: the gap ends (its picks fold into this title's start; RM re-raised once)
    const uint32_t gapF =
        OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
    OUTPUT::COutputWorker::Get().TakeOwed(gapF);
    const OUTPUT::COutputWorker::SHeldClear gap =
        OUTPUT::COutputWorker::Get().GapClose(gapF, "video open", false);
    if (gap.mainDropped && !cleared.mainDropped)
    {
      std::string stereomode;
      m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode, "gap clear");
    }
    m_txOwedPoint = 0;
    m_txRow3cHeld = false;
  }
#if defined(HAVE_LIBBLURAY)
  // a video stream covers the disc's background plane
  if (std::shared_ptr<CDVDInputStreamBluray> bluray =
          std::dynamic_pointer_cast<CDVDInputStreamBluray>(m_pInputStream))
    bluray->SetBackgroundVisible(false);
#endif

  static_cast<IDVDStreamPlayerVideo*>(player)->SendMessage(
      std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_REQUEST_STATE), 1);

  // open CC demuxer if video is mpeg2
  if ((hint.codec == AV_CODEC_ID_MPEG2VIDEO || hint.codec == AV_CODEC_ID_H264) && !m_pCCDemuxer)
  {
    m_pCCDemuxer = std::make_unique<CDVDDemuxCC>(hint.codec, hint.extradata.GetData(),
                                                 static_cast<int>(hint.extradata.GetSize()));
    m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_VIDEOMUX);
  }

  return true;
}

bool CVideoPlayer::OpenSubtitleStream(const CDVDStreamInfo& hint)
{
  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentSubtitle.player);
  if(player == nullptr)
    return false;

  if(m_CurrentSubtitle.id < 0 ||
     m_CurrentSubtitle.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

void CVideoPlayer::AdaptForcedSubtitles()
{
  SelectionStream ss = m_SelectionStreams.Get(StreamType::SUBTITLE, GetSubtitle());
  if (ss.flags & StreamFlags::FLAG_FORCED)
  {
    SelectionStream as = m_SelectionStreams.Get(StreamType::AUDIO, GetAudioStream());
    bool isVisible = false;
    for (const auto& stream : m_SelectionStreams.Get(StreamType::SUBTITLE))
    {
      if (stream.flags & StreamFlags::FLAG_FORCED && stream.language.Matches(as.language))
      {
        if (OpenStream(m_CurrentSubtitle, stream.demuxerId, stream.id, stream.source))
        {
          isVisible = true;
          break;
        }
      }
    }
    // SetEnableStream only if not visible, when visible OpenStream already implied that stream is enabled
    if (!isVisible)
      SetEnableStream(m_CurrentSubtitle, false);

    SetSubtitleVisibleInternal(isVisible);
  }
}

bool CVideoPlayer::OpenTeletextStream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerTeletext->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentTeletext.player);
  if(player == nullptr)
    return false;

  if(m_CurrentTeletext.id < 0 ||
     m_CurrentTeletext.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::OpenRadioRDSStream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerRadioRDS->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentRadioRDS.player);
  if(player == nullptr)
    return false;

  if(m_CurrentRadioRDS.id < 0 ||
     m_CurrentRadioRDS.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::OpenAudioID3Stream(CDVDStreamInfo& hint)
{
  if (!m_VideoPlayerAudioID3->CheckStream(hint))
    return false;

  IDVDStreamPlayer* player = GetStreamPlayer(m_CurrentAudioID3.player);
  if (player == nullptr)
    return false;

  if (m_CurrentAudioID3.id < 0 || m_CurrentAudioID3.hint != hint)
  {
    if (!player->OpenStream(hint))
      return false;
  }

  return true;
}

bool CVideoPlayer::CloseStream(CCurrentStream& current, bool bWaitForBuffers)
{
  if (&current == &m_CurrentVideo)
    ClearPendingElPackets();

  if (current.id < 0)
    return false;

  CLog::Log(LOGINFO, "Closing stream player {}", current.player);
  // 5.E2-6 (r1 M6, r2 L3): a stop or next file opens the picture gate before V's drain
  if (current.type == StreamType::VIDEO && m_renderManager.OpenTxPictureGate("close"))
    TxRestoreDecoderSpeed("close");

  if(bWaitForBuffers)
    SetCaching(CACHESTATE_DONE);

  //! @todo: CloseStream() primarily closes a stream player. SetEnableStream()
  //! call below should be moved away from this method to the places where
  //! stream enable/disable is actually intended, avoiding stale stream
  //! identification from reaching the demuxer.
  SetEnableStream(current, false);

  IDVDStreamPlayer* player = GetStreamPlayer(current.player);
  if (player)
  {
    if ((current.type == StreamType::AUDIO && current.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
        (current.type == StreamType::VIDEO && current.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
        m_bAbortRequest)
      bWaitForBuffers = false;
    // 5.E2-7 (R5, D6): a draining close on a real loss path session gets P's held predicate
    const bool txHook = bWaitForBuffers && m_txDrainSite && aml_transaction_loss_base() &&
                        !aml_transaction_skip("drainhook") &&
                        (current.type == StreamType::AUDIO || current.type == StreamType::VIDEO);
    if (txHook)
      TxSetDrainHook(current.type, m_txDrainSite);
    player->CloseStream(bWaitForBuffers);
    if (txHook)
      TxSetDrainHook(current.type, nullptr);
  }

  if (m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    if (current.type == StreamType::VIDEO)
    {
      m_closedVideoHint = current.hint;
      m_closedVideoValid = true;
    }
    else if (current.type == StreamType::AUDIO)
    {
      m_closedAudioHint = current.hint;
      m_closedAudioValid = true;
    }
  }

  current.Clear();
  return true;
}

void CVideoPlayer::CheckStreamPlayerAlive(CCurrentStream& current,
                                         IDVDStreamPlayer* player,
                                         int& restarts,
                                         int& deadPolls,
                                         const char* name)
{
  if (current.id < 0 || player == nullptr || !player->IsInited() || player->IsPlayerRunning())
  {
    deadPolls = 0;
    return;
  }

  // A player can also stop ITSELF on purpose: CVideoPlayerVideo answers a codec
  // change it cannot satisfy with PLAYER_ABORT + StopThread() on its own thread,
  // which leaves exactly the shape this function looks for - thread gone, queue
  // still inited. Restarting that races the abort it just asked for.
  //
  // m_bAbortRequest catches it once HandleMessages has drained the message, but
  // the post can land after that ran this iteration, so also require the state
  // to survive one more pass. A genuinely dead thread stays dead; a deliberate
  // stop has its PLAYER_ABORT handled in between.
  if (m_bAbortRequest)
  {
    deadPolls = 0;
    return;
  }
  if (++deadPolls < 2)
    return;
  deadPolls = 0;

  // CloseStream clears current, so the identity has to be taken first.
  const int64_t demuxerId = current.demuxerId;
  const int id = current.id;
  const int source = current.source;

  if (restarts >= MAX_PLAYER_RESTARTS)
  {
    CLog::Log(LOGERROR,
              "CVideoPlayer::CheckStreamPlayerAlive - {} player thread exited {} times, dropping "
              "the stream and continuing without it",
              name, restarts);
    CloseStream(current, false);
    return;
  }

  ++restarts;
  CLog::Log(LOGERROR,
            "CVideoPlayer::CheckStreamPlayerAlive - {} player thread has exited while its stream "
            "is open, reopening ({} of {})",
            name, restarts, MAX_PLAYER_RESTARTS);

  CloseStream(current, false);

  // CloseStream cleared current.id, and OpenStream reads `m_CurrentVideo.id < 0`
  // as "first open of this stream" - which is what arms TriggerUpdateResolution.
  // Left alone, recovering a dead video thread would take a full HDMI re-clock
  // with it, mid-title, up to MAX_PLAYER_RESTARTS times. On this chain that is
  // seconds of black and a DV re-engage; the disc-session mode hold exists
  // precisely to avoid per-segment re-clocks, so a restart must not cause one.
  m_restartingStreamPlayer = true;
  const bool opened = OpenStream(current, demuxerId, id, source, true);
  m_restartingStreamPlayer = false;

  if (!opened)
  {
    CLog::Log(LOGERROR, "CVideoPlayer::CheckStreamPlayerAlive - reopening {} failed, continuing "
                        "without it", name);
    return;
  }

  // The reopened player starts at SYNC_STARTING with an empty queue while the
  // demuxer sits wherever playback had reached, so its first packet is roughly a
  // queue-depth ahead of what is on screen. Every other close/reopen pair in
  // Process() answers that with an accurate seek; without one the survivor eats
  // a multi-second jump against the master clock. Same message, same flags.
  CDVDMsgPlayerSeek::CMode mode;
  mode.time = (int)GetUpdatedTime();
  mode.backward = true;
  mode.accurate = true;
  mode.trickplay = true;
  mode.sync = true;
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSeek>(mode));
}

void CVideoPlayer::FlushBuffers(double pts, bool accurate, bool sync)
{
  m_starveResumeArmed = false;
  // 5.E2-9a (D2): a flush inside a P-decided open re-PRIMEs it (same T); the others end it
  // (E2-8a D5)
  if (TxOpen())
  {
    // 5.E2-10 §3: the superseded start waits for the KEEPALIVE reopen's verdict
    if (m_txSuperseded && m_txFlushCaller && std::string_view(m_txFlushCaller) == "keepalive")
      CLog::Log(LOGINFO, "TXSTART set={} superseded: the keepalive flush keeps it",
                m_txEngagedGen);
    else if (TxKeepsFlush(m_txFlushCaller))
      TxReprime(m_txFlushCaller);
    else
      EndTransactionToLegacy("flush", CRenderManager::TxHow::RERAISE,
                             CRenderManager::TxNeed::JOB_OK);
  }
  else if (m_txEngaged && m_txStarted && !m_txExited && m_clock.IsScheduledStartOpen())
    CLog::Log(LOGINFO, "TXSTART set={} row 7 (lead seek: {}): the re-sync's scheduled start",
              m_txEngagedGen, m_txFlushCaller ? m_txFlushCaller : "flush");
  m_txFlushCaller = nullptr;
  m_renderManager.SetTxGraphicsFrom(0); // 5.E2-9b D3
  // only a user pause keeps the output's audio (design §16.22)
  m_clock.SetHoldFrom(0.0);
  ClearPendingElPackets();
  ResetSegmentEnd();
  // the seams stay: offsets follow the byte position, so a read from anywhere
  // in the title, before a seam or past it, takes the steps it should
  m_seamStreams.clear();
  ClearRecue();
  // a flush or seek leaves no picture for a waiting menu page to go with
  if (m_menuPageWaiting)
  {
    m_overlayContainer.ReleasePendingMenu();
    m_menuPageWaiting = false;
  }
  if (m_startHeld)
    ReleaseHeldStart("flush");
  m_cachingStall = false;
  m_startReleasedClock = DVD_NOPTS_VALUE;
  m_syncStartPtsWait.reset();
  CLog::Log(LOGDEBUG, "CVideoPlayer::FlushBuffers - flushing buffers");

  double startpts;
  if (accurate)
    startpts = pts;
  else
    startpts = DVD_NOPTS_VALUE;

  m_SpeedState.Reset(pts);

  // a defer window from before the flush belongs to a dead sync attempt, and
  // so does a first-picture wait
  m_syncStartDeferred = false;
  m_firstPictureWaitSince = {};
  m_firstPictureWaitExpired = false;

  // and so does an armed seam-step correction: after a flush the timestamps
  // either side of it are unrelated to the boundary that armed it
  m_seamStepPending = false;
  m_seamStepArmedDts = DVD_NOPTS_VALUE;
  m_stillJoinCorrection = DVD_NOPTS_VALUE;
  m_stillJoinOldEnd = false;

#if defined(HAVE_LIBBLURAY)
  // So does an armed-but-uncollected seamless glide. HandleMessages() runs
  // between the iteration that arms one and the iteration that collects it,
  // so a seek can land in that window; the flag would then fire a transition
  // against the freshly re-sought pipeline, undo its re-cache and - being out
  // of sync at that moment - classify as DRAIN and tear it down again.
  if (m_pInputBluray)
    m_pInputBluray->CancelPendingSeamlessTransition();
#endif

  if (sync)
  {
    m_CurrentAudio.inited = false;
    m_CurrentAudio.avsync = CCurrentStream::AV_SYNC_FORCE;
    m_CurrentAudio.starttime = DVD_NOPTS_VALUE;
    m_CurrentAudio.firststarttime = DVD_NOPTS_VALUE;
    m_CurrentVideo.inited = false;
    m_CurrentVideo.avsync = CCurrentStream::AV_SYNC_FORCE;
    m_CurrentVideo.starttime = DVD_NOPTS_VALUE;
    m_CurrentVideo.starttimePending = false;
    m_CurrentSubtitle.inited = false;
    m_CurrentTeletext.inited = false;
    m_CurrentRadioRDS.inited  = false;

    // a video wrap-gap recorded before the flush belongs to a dead timeline,
    // and so does a half-open timeline-restart stamp: the jump it was tracking
    // will never be confirmed now, and leaving the latch set would suppress the
    // stamp on the next genuine boundary
    m_menuWrapVideoGap = 0.0;
    m_timelineRestartStamped = false;
  }

  // stamps taken before the flush reference a dead timeline: snap the
  // presented disc state to the latest demux truth
  ApplyDiscTimelineEvents(true);

  m_CurrentAudio.dts         = DVD_NOPTS_VALUE;
  // a scheduled start lands the audio with the first picture, which is the
  // keyframe before an accurate seek's target: the audio between them plays
  const bool landAudio = m_scheduledStart && m_CurrentVideo.id >= 0 && m_pInputStream &&
                         !m_pInputStream->IsRealtime();
  m_CurrentAudio.startpts    = landAudio ? DVD_NOPTS_VALUE : startpts;
  m_CurrentAudio.packets = 0;

  m_CurrentVideo.dts         = DVD_NOPTS_VALUE;
  m_CurrentVideo.startpts    = startpts;
  m_CurrentVideo.packets = 0;

  m_CurrentSubtitle.dts      = DVD_NOPTS_VALUE;
  m_CurrentSubtitle.startpts = startpts;
  m_CurrentSubtitle.packets = 0;

  m_CurrentTeletext.dts      = DVD_NOPTS_VALUE;
  m_CurrentTeletext.startpts = startpts;
  m_CurrentTeletext.packets = 0;

  m_CurrentRadioRDS.dts      = DVD_NOPTS_VALUE;
  m_CurrentRadioRDS.startpts = startpts;
  m_CurrentRadioRDS.packets = 0;

  m_CurrentAudioID3.dts = DVD_NOPTS_VALUE;
  m_CurrentAudioID3.startpts = startpts;
  m_CurrentAudioID3.packets = 0;

  m_VideoPlayerAudio->Flush(sync);
  // a re-land still queued went with the flush: the next start lands the offset
  m_landedAudioOffset = m_clock.GetAudioOffset();
  m_VideoPlayerVideo->Flush(sync);
  m_VideoPlayerSubtitle->Flush();
  m_VideoPlayerTeletext->Flush();
  m_VideoPlayerRadioRDS->Flush();
  m_VideoPlayerAudioID3->Flush();

  // drop queued pre-seek frames to avoid starving RenderManager's buffer pool so post-seek overlays/subtitles do not get dropped
  if (sync)
    m_renderManager.DiscardBuffer();

  if (m_playSpeed == DVD_PLAYSPEED_NORMAL || m_playSpeed == DVD_PLAYSPEED_PAUSE ||
      m_processInfo->IsTempoAllowed(static_cast<float>(m_playSpeed) / DVD_PLAYSPEED_NORMAL))
  {
    // make sure players are properly flushed, should put them in stalled state
    // A closed player's queue drops the message, so waiting on it only runs
    // out the 1 s timeout (and holds the open player for as long): wait on
    // the players that are open.
    const bool audioInited = m_VideoPlayerAudio->IsInited();
    const bool videoInited = m_VideoPlayerVideo->IsInited();
    const unsigned int sources =
        (audioInited ? SYNCSOURCE_AUDIO : 0) | (videoInited ? SYNCSOURCE_VIDEO : 0);
    if (sources)
    {
      auto msg = std::make_shared<CDVDMsgGeneralSynchronize>(1s, sources);
      if (audioInited)
        m_VideoPlayerAudio->SendMessage(msg, 1);
      if (videoInited)
        m_VideoPlayerVideo->SendMessage(msg, 1);
      msg->Wait(m_bStop, 0);
    }

    // purge any pending PLAYER_STARTED messages
    m_messenger.Flush(CDVDMsg::PLAYER_STARTED);

    // we should now wait for init cache
    SetCaching(CACHESTATE_FLUSH);
    if (sync)
    {
      m_CurrentAudio.syncState = IDVDStreamPlayer::SYNC_STARTING;
      m_CurrentVideo.syncState = IDVDStreamPlayer::SYNC_STARTING;
    }
  }

  m_CurrentVideo.lastdts = DVD_NOPTS_VALUE;
  UpdatePlayState(0);

  m_demuxerSpeed = DVD_PLAYSPEED_NORMAL;
  if (m_pDemuxer)
    m_pDemuxer->SetSpeed(DVD_PLAYSPEED_NORMAL);
}

// since we call ffmpeg functions to decode, this is being called in the same thread as ::Process() is
int CVideoPlayer::OnDiscNavResult(void* pData, int iMessage)
{
  if (!m_pInputStream)
    return 0;

#if defined(HAVE_LIBBLURAY)
  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
  {
    switch (iMessage)
    {
    case BD_EVENT_MENU_OVERLAY:
    {
      const std::shared_ptr<CDVDOverlay>& overlay =
          *static_cast<std::shared_ptr<CDVDOverlay>*>(pData);
      if (overlay && overlay->m_pageWait != CDVDOverlay::PAGE_AT_ONCE)
      {
        // a first display of a preloaded page (6.1): with its segment's first
        // picture; the player thread posts these (the read that produced it)
        overlay->m_segmentGen =
            m_segmentGen + (overlay->m_pageWait == CDVDOverlay::PAGE_NEXT_SEGMENT ? 1 : 0);
        if (!m_menuPageWaiting)
          m_menuPageWaitSince = std::chrono::steady_clock::now();
        m_menuPageWaiting = true;
        CLog::Log(LOGDEBUG, "CVideoPlayer: menu page waits for segment {} (now {}, {})",
                  overlay->m_segmentGen, m_segmentGen,
                  overlay->m_pageWait == CDVDOverlay::PAGE_NEXT_SEGMENT ? "next" : "current");
      }
      m_overlayContainer.ProcessAndAddOverlayIfValid(overlay);
      break;
    }
    case BD_EVENT_MENU:
    {
      const uint32_t menuState = *static_cast<uint32_t*>(pData);
      // Demux-side machinery reacts immediately: a leave-menu must break a
      // still hold NOW - the VM has moved on and the pipeline follows it.
      if (menuState == 0)
      {
        m_dvd.state = DVDSTATE_NORMAL;
        m_dvd.iDVDStillTime = 0ms;
        CLog::Log(LOGDEBUG, "BD_EVENT_MENU - libbluray leave menu (DVDSTATE_NORMAL)");
      }
      // The PRESENTED menu flip is timeline-stamped: it applies when the
      // render clock reaches the demux position where the VM flipped it
      // (docs/bd_timeline_events_design.md). Stamp = last delivered video
      // dts (this callback runs synchronously inside the demux read on the
      // player thread, so that IS the decision position). No packet yet ->
      // no queue between VM and viewer -> apply immediately via NOPTS.
      m_discTimelineEvents.push_back({m_CurrentVideo.dts, menuState, nullptr});
      break;
    }
    case BD_EVENT_PLAYLIST:
    {
      // OSD-visible playlist identity (chapter list / total time): timeline-
      // stamped like the presented menu state, so the OSD flips when the
      // render clock reaches the playlist boundary instead of queue-depth
      // early. Demux machinery reads m_titleInfo directly and is unaffected.
      const auto& ui = *static_cast<std::shared_ptr<const BlurayTitleUiSnapshot>*>(pData);
      m_discTimelineEvents.push_back({m_CurrentVideo.dts, 0, ui});
      break;
    }
    case BD_EVENT_BDJ_PRESENTATION_STAMP:
    {
      if (!pData)
      {
        // the input stream is waiting inside the demux read for the BD-J
        // application: run the timeline so held notifications can release,
        // and a held start, whose release would otherwise wait for this read
        // to return (the application may be waiting for that picture)
        if (m_pInputBluray && m_pInputBluray->BdjDeferStart())
        {
          CheckHeldStart();
          CheckStartGate();
        }
        ApplyDiscTimelineEvents(false, true);
        break;
      }
      // stamped like every disc timeline event: the last delivered video dts
      // is the read position of the batch libbluray just held. Consecutive
      // batches at the same position collapse into the newest sequence.
      const uint32_t seq = *static_cast<const uint32_t*>(pData);
      // a playlist with no video (menu music under BD-J graphics) is timed by
      // its audio
      uint32_t startSeq = 0;
      const bool startPending = m_pInputBluray && m_pInputBluray->BdjStartPending(startSeq);
      const double stamp =
          m_CurrentVideo.id < 0 && startPending ? m_CurrentAudio.dts : m_CurrentVideo.dts;
      // a held start stamped behind the picture: everything read before it is
      // already presented (the reader waited at the end of a playlist), so its
      // first picture is the next segment's. Only the entry holding the start
      // marker waits: one after it reaches the front once the marker is
      // delivered (the start resolved), and its stamp is on the start's
      // timeline or voided by the playlist change's flush, so a segment
      // transition must not wipe it (design 15.57)
      // judged only against the clock of the stamp's own streams; before the
      // start's timeline is committed, the stamp is the start's own read-ahead
      // and the start goes with its schedule: the marker's entry has no stamp
      // (design 15.60)
      const bool awaitSegment = startPending && static_cast<int32_t>(seq - startSeq) >= 0 &&
                                stamp != DVD_NOPTS_VALUE && ClockOnStreams() &&
                                stamp <= m_clock.GetClock();
      const bool voidStartStamp = startPending && stamp != DVD_NOPTS_VALUE && !TimelineCommitted();
      const auto holdsMarker = [&](uint32_t from, uint32_t upto) {
        return startPending && static_cast<int32_t>(startSeq - from) > 0 &&
               static_cast<int32_t>(upto - startSeq) >= 0;
      };
      const auto holdsStart = [&](uint32_t from, uint32_t upto) {
        return awaitSegment && holdsMarker(from, upto);
      };
      uint32_t pushed = m_pInputBluray ? m_pInputBluray->StampAfterSeq() : 0;
      if (holdsStart(pushed, seq))
        CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J start read at the end of what was presented "
                            "(stamp {:.3f}): waits for the next segment", stamp / DVD_TIME_BASE);
      // A release frees everything up to its sequence, so a batch holding
      // marks with their own time is split around each: what was held before
      // it (at the stamp), the mark (at its picture), the rest (at the stamp,
      // behind it). An entry at the stamp never merges into or past a mark.
      const bool endBatch = m_pInputBluray && m_pInputBluray->IsBdjEndBatch(seq);
      const auto push = [&](uint32_t upto, double presentPts) {
        const bool voided = voidStartStamp && holdsMarker(pushed, upto);
        if (presentPts == DVD_NOPTS_VALUE && !endBatch && !voided && !m_discTimelineEvents.empty() &&
            m_discTimelineEvents.back().bdjReleaseSeq != 0 &&
            m_discTimelineEvents.back().presentPts == DVD_NOPTS_VALUE &&
            !m_discTimelineEvents.back().endOfPlaylist && !m_discTimelineEvents.back().markPending &&
            m_discTimelineEvents.back().stampPts == stamp)
        {
          m_discTimelineEvents.back().awaitSegment |=
              holdsStart(m_discTimelineEvents.back().bdjReleaseSeq, upto);
          m_discTimelineEvents.back().bdjReleaseSeq = upto;
          pushed = upto;
          return;
        }
        SDiscTimelineEvent ev{voided ? DVD_NOPTS_VALUE : stamp, 0, nullptr, upto};
        ev.awaitSegment = holdsStart(pushed, upto);
        ev.presentPts = presentPts;
        ev.endOfPlaylist = endBatch && upto == seq;
        m_discTimelineEvents.push_back(ev);
        pushed = upto;
      };
      bool anyPushed = false;
      if (m_pInputBluray && m_pInputBluray->BdjMarkTimes())
      {
        const uint32_t readGen = m_pInputBluray->GetReadTitleGen();
        for (const auto& mark : m_pInputBluray->StampMarkTimes())
        {
          const double present =
              BdjMarkPresentPts(mark.pts45, mark.clipStart, mark.clipEnd, stamp, readGen);
          if (present == DVD_NOPTS_VALUE)
          {
            // its clip's packets have not arrived yet: it waits for them
            // (design 15.55); otherwise the stamp stands
            const CCurrentStream& ref = m_CurrentVideo.id >= 0 ? m_CurrentVideo : m_CurrentAudio;
            const bool notYet = ref.markRefDts == DVD_NOPTS_VALUE ||
                                static_cast<int32_t>(readGen - ref.markRefGen) > 0 ||
                                (ref.markRefGen == readGen && ref.markRefPos < mark.clipStart);
            if (!notYet)
            {
              CLog::Log(LOGDEBUG,
                        "CVideoPlayer: BD-J mark (seq {}) has no picture time here: at its stamp",
                        mark.seq);
              continue;
            }
            if (static_cast<int32_t>(mark.seq - 1 - pushed) > 0 && mark.seq - 1 != 0)
              push(mark.seq - 1, DVD_NOPTS_VALUE);
            SDiscTimelineEvent ev{stamp, 0, nullptr, mark.seq};
            ev.awaitSegment = holdsStart(pushed, mark.seq);
            ev.markPending = true;
            ev.markPts45 = mark.pts45;
            ev.markClipStart = mark.clipStart;
            ev.markClipEnd = mark.clipEnd;
            ev.markGen = readGen;
            ev.markPendingSince = std::chrono::steady_clock::now();
            ev.endOfPlaylist = endBatch && mark.seq == seq;
            m_discTimelineEvents.push_back(ev);
            m_bdjMarksPending = true;
            CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J mark (seq {}) waits for its clip's packets",
                      mark.seq);
            pushed = mark.seq;
            anyPushed = true;
            continue;
          }
          CLog::Log(LOGDEBUG, "CVideoPlayer: BD-J mark (seq {}) at its picture {:.3f} (stamp {:.3f})",
                    mark.seq, present / DVD_TIME_BASE, stamp / DVD_TIME_BASE);
          if (static_cast<int32_t>(mark.seq - 1 - pushed) > 0 && mark.seq - 1 != 0)
            push(mark.seq - 1, DVD_NOPTS_VALUE);
          push(mark.seq, present);
          pushed = mark.seq;
          anyPushed = true;
        }
      }
      if (!anyPushed || pushed != seq)
        push(seq, DVD_NOPTS_VALUE);
      break;
    }
    case BD_EVENT_BDJ_START_BLOCKED:
    {
      // a held BD-J start waits for a stream sync, which runs only in Process
      int64_t ns = 0;
      double startClock = 0.0;
      const bool syncPending =
          (m_CurrentVideo.id >= 0 && m_CurrentVideo.syncState != IDVDStreamPlayer::SYNC_INSYNC) ||
          (m_CurrentAudio.id >= 0 && m_CurrentAudio.syncState != IDVDStreamPlayer::SYNC_INSYNC);
      *static_cast<bool*>(pData) =
          !m_startHeld && !m_clock.GetPendingStart(ns, startClock) && syncPending;
      break;
    }
    case BD_EVENT_PLAYLIST_STOP:
      m_dvd.state = DVDSTATE_NORMAL;
      m_dvd.iDVDStillTime = 0ms;
      m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::GENERAL_FLUSH));
      break;
    case BD_EVENT_AUDIO_STREAM:
      m_dvd.iSelectedAudioStream = *static_cast<int*>(pData);
      break;

    case BD_EVENT_PG_TEXTST_STREAM:
      m_dvd.iSelectedSPUStream = *static_cast<int*>(pData);
      break;
    case BD_EVENT_PG_TEXTST:
    {
      bool enable = (*static_cast<int*>(pData) != 0);
      m_VideoPlayerVideo->EnableSubtitle(enable);
    }
    break;
    case BD_EVENT_STILL_TIME:
    {
      if (m_dvd.state != DVDSTATE_STILL)
      {
        // else notify the player we have received a still frame

        m_dvd.iDVDStillTime = std::chrono::milliseconds(*static_cast<int*>(pData));
        m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();

        if (m_dvd.iDVDStillTime > 0ms)
          m_dvd.iDVDStillTime *= 1000;

        /* adjust for the output delay in the video queue */
        std::chrono::milliseconds time = 0ms;
        if (m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0ms)
        {
          time = std::chrono::milliseconds(
              static_cast<int>(m_VideoPlayerVideo->GetOutputDelay() / (DVD_TIME_BASE / 1000)));
          if (time < 10000ms && time > 0ms)
            m_dvd.iDVDStillTime += time;
        }
        m_dvd.state = DVDSTATE_STILL;
        CLog::Log(LOGDEBUG, "BD_EVENT_STILL_TIME - waiting {} msec, with delay of {} msec",
                  m_dvd.iDVDStillTime.count(), time.count());
      }
    }
    break;
    case BD_EVENT_STILL:
    {
      bool on = static_cast<bool>(*static_cast<int*>(pData));
      if (on && m_dvd.state != DVDSTATE_STILL)
      {
        m_dvd.state = DVDSTATE_STILL;
        m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();
        m_dvd.iDVDStillTime = 0ms;
        CLog::Log(LOGDEBUG, "CDVDPlayer::OnDVDNavResult - libbluray DVDSTATE_STILL start");
      }
      else if (!on && m_dvd.state == DVDSTATE_STILL)
      {
        m_dvd.state = DVDSTATE_NORMAL;
        m_dvd.iDVDStillStartTime = {};
        m_dvd.iDVDStillTime = 0ms;
        CLog::Log(LOGDEBUG, "CDVDPlayer::OnDVDNavResult - libbluray DVDSTATE_STILL end");
      }
    }
    break;
    case BD_EVENT_MENU_ERROR:
    {
      m_dvd.state = DVDSTATE_NORMAL;
      CLog::Log(LOGDEBUG, "CVideoPlayer::OnDiscNavResult - libbluray menu not supported (DVDSTATE_NORMAL)");
      CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnBlurayMenuError");
    }
    break;
    case BD_EVENT_ENC_ERROR:
    {
      m_dvd.state = DVDSTATE_NORMAL;
      CLog::Log(LOGDEBUG, "CVideoPlayer::OnDiscNavResult - libbluray the disc/file is encrypted and can't be played (DVDSTATE_NORMAL)");
      CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player,
                                                         "OnBlurayEncryptedError");
    }
    break;
    case BD_EVENT_DISCONTINUITY:
      CLog::Log(LOGDEBUG,
                "CVideoPlayer::OnDiscNavResult - libbluray discontinuity detected (DEMUXER_RESET)");
      m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::DEMUXER_RESET));
      break;
    default:
      break;
    }

    return 0;
  }
#endif

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
  {
    std::shared_ptr<CDVDInputStreamNavigator> pStream = std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);

    switch (iMessage)
    {
    case DVDNAV_STILL_FRAME:
      {
        //CLog::Log(LOGDEBUG, "DVDNAV_STILL_FRAME");

        dvdnav_still_event_t *still_event = static_cast<dvdnav_still_event_t*>(pData);
        // should wait the specified time here while we let the player running
        // after that call dvdnav_still_skip(m_dvdnav);

        if (m_dvd.state != DVDSTATE_STILL)
        {
          // else notify the player we have received a still frame

          if(still_event->length < 0xff)
            m_dvd.iDVDStillTime = std::chrono::seconds(still_event->length);
          else
            m_dvd.iDVDStillTime = 0ms;

          m_dvd.iDVDStillStartTime = std::chrono::steady_clock::now();

          /* adjust for the output delay in the video queue */
          std::chrono::milliseconds time = 0ms;
          if (m_CurrentVideo.stream && m_dvd.iDVDStillTime > 0ms)
          {
            time = std::chrono::milliseconds(
                static_cast<int>(m_VideoPlayerVideo->GetOutputDelay() / (DVD_TIME_BASE / 1000)));
            if (time < 10000ms && time > 0ms)
              m_dvd.iDVDStillTime += time;
          }
          m_dvd.state = DVDSTATE_STILL;
          CLog::Log(LOGDEBUG, "DVDNAV_STILL_FRAME - waiting {} sec, with delay of {} msec",
                    still_event->length, time.count());
        }
        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_SPU_CLUT_CHANGE:
      {
        m_VideoPlayerSubtitle->SendMessage(
            std::make_shared<CDVDMsgSubtitleClutChange>((uint8_t*)pData));
      }
      break;
    case DVDNAV_SPU_STREAM_CHANGE:
      {
        dvdnav_spu_stream_change_event_t* event = static_cast<dvdnav_spu_stream_change_event_t*>(pData);

        int iStream = event->physical_wide;
        bool visible = !(iStream & 0x80);

        SetSubtitleVisibleInternal(visible);

        if (iStream >= 0)
          m_dvd.iSelectedSPUStream = (iStream & ~0x80);
        else
          m_dvd.iSelectedSPUStream = -1;

        m_CurrentSubtitle.stream = NULL;
      }
      break;
    case DVDNAV_AUDIO_STREAM_CHANGE:
      {
        dvdnav_audio_stream_change_event_t* event = static_cast<dvdnav_audio_stream_change_event_t*>(pData);
        // Tell system what audiostream should be opened by default
        m_dvd.iSelectedAudioStream = event->physical;
        m_CurrentAudio.stream = NULL;
      }
      break;
    case DVDNAV_HIGHLIGHT:
      {
        //dvdnav_highlight_event_t* pInfo = (dvdnav_highlight_event_t*)pData;
        int iButton = pStream->GetCurrentButton();
        CLog::Log(LOGDEBUG, "DVDNAV_HIGHLIGHT: Highlight button {}", iButton);
        m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_NORMAL);
      }
      break;
    case DVDNAV_VTS_CHANGE:
      {
        //dvdnav_vts_change_event_t* vts_change_event = (dvdnav_vts_change_event_t*)pData;
        CLog::Log(LOGDEBUG, "DVDNAV_VTS_CHANGE");

        //Make sure we clear all the old overlays here, or else old forced items are left.
        m_overlayContainer.Clear();

        //Force an aspect ratio that is set in the dvdheaders if available
        m_CurrentVideo.hint.aspect = static_cast<double>(pStream->GetVideoAspectRatio());
        if( m_VideoPlayerVideo->IsInited() )
          m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgDouble>(
              CDVDMsg::VIDEO_SET_ASPECT, m_CurrentVideo.hint.aspect));

        m_SelectionStreams.Clear(StreamType::NONE, STREAM_SOURCE_NAV);
        m_SelectionStreams.Update(m_pInputStream, m_pDemuxer.get());
        UpdateContent();

        return NAVRESULT_HOLD;
      }
      break;
    case DVDNAV_CELL_CHANGE:
      {
        //dvdnav_cell_change_event_t* cell_change_event = (dvdnav_cell_change_event_t*)pData;
        CLog::Log(LOGDEBUG, "DVDNAV_CELL_CHANGE");

        if (m_dvd.state != DVDSTATE_STILL)
          m_dvd.state = DVDSTATE_NORMAL;
      }
      break;
    case DVDNAV_NAV_PACKET:
      {
          //pci_t* pci = (pci_t*)pData;

          // this should be possible to use to make sure we get
          // seamless transitions over these boundaries
          // if we remember the old vobunits boundaries
          // when a packet comes out of demuxer that has
          // pts values outside that boundary, it belongs
          // to the new vobunit, which has new timestamps
          UpdatePlayState(0);
      }
      break;
    case DVDNAV_HOP_CHANNEL:
      {
        // This event is issued whenever a non-seamless operation has been executed.
        // Applications with fifos should drop the fifos content to speed up responsiveness.
        CLog::Log(LOGDEBUG, "DVDNAV_HOP_CHANNEL");
        if(m_dvd.state == DVDSTATE_SEEK)
          m_dvd.state = DVDSTATE_NORMAL;
        else
        {
          bool sync = !IsInMenuInternal();
          m_txFlushCaller = "DVDNAV hop";
          FlushBuffers(DVD_NOPTS_VALUE, false, sync);
          m_dvd.syncClock = true;
          m_dvd.state = DVDSTATE_NORMAL;
          if (m_pDemuxer)
            m_pDemuxer->Flush();
        }

        return NAVRESULT_ERROR;
      }
      break;
    case DVDNAV_STOP:
      {
        CLog::Log(LOGDEBUG, "DVDNAV_STOP");
        m_dvd.state = DVDSTATE_NORMAL;
      }
      break;
    case DVDNAV_ERROR:
      {
        CLog::Log(LOGDEBUG, "DVDNAV_ERROR");
        m_dvd.state = DVDSTATE_NORMAL;
        CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player,
                                                           "OnPlaybackFailed");
      }
      break;
    default:
    {}
      break;
    }
  }
  return NAVRESULT_NOP;
}

void CVideoPlayer::GetVideoResolution(unsigned int &width, unsigned int &height)
{
  RESOLUTION_INFO res = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();
  width = res.iWidth;
  height = res.iHeight;
}

bool CVideoPlayer::OnAction(const CAction &action)
{
#define THREAD_ACTION(action) \
  do \
  { \
    if (!IsCurrentThread()) \
    { \
      m_messenger.Put( \
          std::make_shared<CDVDMsgType<CAction>>(CDVDMsg::GENERAL_GUI_ACTION, action)); \
      return true; \
    } \
  } while (false)

  std::shared_ptr<CDVDInputStream::IMenus> pMenus = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);
  if (pMenus)
  {
    if (m_dvd.state == DVDSTATE_STILL && m_dvd.iDVDStillTime != 0ms &&
        pMenus->GetTotalButtons() == 0)
    {
      switch(action.GetID())
      {
        case ACTION_NEXT_ITEM:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_UP:
        case ACTION_SELECT_ITEM:
          {
            THREAD_ACTION(action);
            /* this will force us out of the stillframe */
            CLog::Log(LOGDEBUG, "{} - User asked to exit stillframe", __FUNCTION__);
            m_dvd.iDVDStillStartTime = {};
            m_dvd.iDVDStillTime = 1ms;
          }
          return true;
      }
    }


    switch (action.GetID())
    {
/* this code is disabled to allow switching playlist items (dvdimage "stacks") */
#if 0
    case ACTION_PREV_ITEM:  // SKIP-:
      {
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed prev");
        pMenus->OnPrevious();
        m_processInfo->SeekFinished(0);
        return true;
      }
      break;
    case ACTION_NEXT_ITEM:  // SKIP+:
      {
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed next");
        pMenus->OnNext();
        m_processInfo->SeekFinished(0);
        return true;
      }
      break;
#endif
    case ACTION_SHOW_VIDEOMENU:   // start button
      {
        THREAD_ACTION(action);
        // Optional menu type carried in the action name ("popup" | "top")
        // from PlayerControl(ShowVideoMenu(popup|top)); empty = auto (legacy).
        CDVDInputStream::IMenus::MenuCall mtype = CDVDInputStream::IMenus::MenuCall::Auto;
        const std::string& menuArg = action.GetName();
        if (menuArg == "popup")
          mtype = CDVDInputStream::IMenus::MenuCall::Popup;
        else if (menuArg == "top")
          mtype = CDVDInputStream::IMenus::MenuCall::Top;
        CLog::LogF(LOGDEBUG, "Trying to go to the menu ({})",
                   menuArg.empty() ? "auto" : menuArg.c_str());
        if (pMenus->OnMenu(mtype))
        {
          if (UserSpeed() == DVD_PLAYSPEED_PAUSE)
          {
            SetPlaySpeed(DVD_PLAYSPEED_NORMAL);
            m_callback.OnPlayBackResumed();
          }

          // Let everyone know that we've gone to the menu
          CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnMenu");
        }
        return true;
      }
      break;
    }

    if (pMenus->IsInMenu())
    {
      switch (action.GetID())
      {
      case ACTION_NEXT_ITEM:
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed next in menu, stream will decide");
        if (pMenus->CanSeek() && GetChapterCount() > 0 && GetChapter() < GetChapterCount())
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetChapter() + 1));
        else
          pMenus->OnNext();

        m_processInfo->SeekFinished(0);
        return true;
      case ACTION_PREV_ITEM:
        THREAD_ACTION(action);
        CLog::Log(LOGDEBUG, " - pushed prev in menu, stream will decide");
        if (pMenus->CanSeek() && GetChapterCount() > 0 && GetChapter() > 0)
          m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetPreviousChapter()));
        else
          pMenus->OnPrevious();

        m_processInfo->SeekFinished(0);
        return true;
      case ACTION_PREVIOUS_MENU:
      case ACTION_NAV_BACK:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - menu back");
          pMenus->OnBack();
        }
        break;
      case ACTION_MOVE_LEFT:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move left");
          pMenus->OnLeft();
        }
        break;
      case ACTION_MOVE_RIGHT:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move right");
          pMenus->OnRight();
        }
        break;
      case ACTION_MOVE_UP:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move up");
          pMenus->OnUp();
        }
        break;
      case ACTION_MOVE_DOWN:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - move down");
          pMenus->OnDown();
        }
        break;

      case ACTION_MOUSE_MOVE:
      case ACTION_MOUSE_LEFT_CLICK:
        {
          CRect rs, rd, rv;
          m_renderManager.GetVideoRect(rs, rd, rv);
          CPoint pt(action.GetAmount(), action.GetAmount(1));
          if (!rd.PtInRect(pt))
            return false; // out of bounds
          THREAD_ACTION(action);
          // convert to video coords...
          pt -= CPoint(rd.x1, rd.y1);
          pt.x *= rs.Width() / rd.Width();
          pt.y *= rs.Height() / rd.Height();
          pt += CPoint(rs.x1, rs.y1);
          if (action.GetID() == ACTION_MOUSE_LEFT_CLICK)
          {
            if (pMenus->OnMouseClick(pt))
              return true;
            else
            {
              CServiceBroker::GetAppMessenger()->PostMsg(
                  TMSG_GUI_ACTION, WINDOW_INVALID, -1,
                  static_cast<void*>(new CAction(ACTION_TRIGGER_OSD)));
              return false;
            }
          }
          return pMenus->OnMouseMove(pt);
        }
        break;
      case ACTION_SELECT_ITEM:
        {
          THREAD_ACTION(action);
          CLog::Log(LOGDEBUG, " - button select");
          // show button pushed overlay
          if(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD))
            m_VideoPlayerSubtitle->UpdateOverlayInfo(std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream), LIBDVDNAV_BUTTON_CLICKED);

          pMenus->ActivateButton();
        }
        break;
      case REMOTE_0:
      case REMOTE_1:
      case REMOTE_2:
      case REMOTE_3:
      case REMOTE_4:
      case REMOTE_5:
      case REMOTE_6:
      case REMOTE_7:
      case REMOTE_8:
      case REMOTE_9:
        {
          THREAD_ACTION(action);
          // Offset from key codes back to button number
          int button = action.GetID() - REMOTE_0;
          CLog::Log(LOGDEBUG, " - button pressed {}", button);
          pMenus->SelectButton(button);
        }
       break;
      default:
        return false;
        break;
      }
      return true; // message is handled
    }
  }

  pMenus.reset();

  switch (action.GetID())
  {
    case ACTION_NEXT_ITEM:
      if (GetChapter() > 0 && GetChapter() < GetChapterCount())
      {
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetChapter() + 1));
        m_processInfo->SeekFinished(0);
        return true;
      }
      else if (SeekScene(Direction::FORWARD))
        return true;
      else
        break;
    case ACTION_PREV_ITEM:
      if (GetChapter() > 0)
      {
        m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(GetPreviousChapter()));
        m_processInfo->SeekFinished(0);
        return true;
      }
      else if (SeekScene(Direction::BACKWARD))
        return true;
      else
        break;
    case ACTION_TOGGLE_COMMSKIP:
      m_SkipCommercials = !m_SkipCommercials;
      CServiceBroker::GetAnnouncementManager()->Announce(
          ANNOUNCEMENT::Player, "OnToggleSkipCommercials", CVariant{m_SkipCommercials});
      break;

    case ACTION_PLAYER_DEBUG:
      m_renderManager.ToggleDebug();
      break;
    case ACTION_PLAYER_DEBUG_VIDEO:
      m_renderManager.ToggleDebugVideo();
      break;

    case ACTION_PLAYER_PROCESS_INFO:
      CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnProcessInfo");
      return true;

    // Dolby Vision VS10 engine: switch the output mode live during playback.
    // "Original" reverts to follow-source (native DV or native SDR/HDR as-is).
    // The resolution update then lets the window system re-decide the HDMI wire
    // for the new output through the same display lost/reset protocol a mode
    // change uses, so playback pauses and re-anchors across the re-clock.
    // 5.G-6: with P's path published on for this file TxMidVs10Press posts the press to P and
    // returns, so the direct write and its trigger below are never reached on the path. Kept for
    // midplaytx_off, path-off sessions, another file's press and a press P gives back
    // (TxMidVs10ToMain re-enters here after the path, S4)
    case ACTION_VS10_ORIGINAL:
      if (TxMidVs10Press(action.GetID(), DOLBY_VISION_OUTPUT_MODE_BYPASS))
        return true;
      if (aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_BYPASS))
        TriggerVs10Resolution();
      return true;
    case ACTION_VS10_SDR:
      if (TxMidVs10Press(action.GetID(), DOLBY_VISION_OUTPUT_MODE_SDR10))
        return true;
      if (aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_SDR10))
        TriggerVs10Resolution();
      return true;
    case ACTION_VS10_HDR10:
      if (TxMidVs10Press(action.GetID(), DOLBY_VISION_OUTPUT_MODE_HDR10))
        return true;
      if (aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_HDR10))
        TriggerVs10Resolution();
      return true;
    case ACTION_VS10_DV:
      if (TxMidVs10Press(action.GetID(), DOLBY_VISION_OUTPUT_MODE_IPT))
        return true;
      if (aml_dv_set_vs10_mode(DOLBY_VISION_OUTPUT_MODE_IPT))
        TriggerVs10Resolution();
      return true;
  }

  // return false to inform the caller we didn't handle the message
  return false;
}

bool CVideoPlayer::IsInMenuInternal() const
{
  std::shared_ptr<CDVDInputStream::IMenus> pStream = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);
  if (pStream)
  {
    if (m_dvd.state == DVDSTATE_STILL)
      return true;
    else
      return pStream->IsInMenu();
  }
  return false;
}


bool CVideoPlayer::IsInMenu() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.isInMenu;
}

MenuType CVideoPlayer::GetSupportedMenuType() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.menuType;
}

std::string CVideoPlayer::GetPlayerState()
{
  std::unique_lock lock(m_StateSection);
  return m_State.player_state;
}

bool CVideoPlayer::SetPlayerState(const std::string& state)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetState>(state));
  return true;
}

int CVideoPlayer::GetChapterCount() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.chapters.size();
}

int CVideoPlayer::GetChapter() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.chapter;
}

void CVideoPlayer::GetChapterName(std::string& strChapterName, int chapterIdx) const
{
  std::unique_lock lock(m_StateSection);
  if (chapterIdx == -1 && m_State.chapter > 0 && m_State.chapter <= (int) m_State.chapters.size())
    strChapterName = m_State.chapters[m_State.chapter - 1].first;
  else if (chapterIdx > 0 && chapterIdx <= (int) m_State.chapters.size())
    strChapterName = m_State.chapters[chapterIdx - 1].first;
}

int CVideoPlayer::SeekChapter(int iChapter)
{
  if (GetChapter() > 0)
  {
    if (iChapter < 0)
      iChapter = 0;
    if (iChapter > GetChapterCount())
      return 0;

    // Seek to the chapter.
    m_messenger.Put(std::make_shared<CDVDMsgPlayerSeekChapter>(iChapter));
    SynchronizeDemuxer();
  }

  return 0;
}

int64_t CVideoPlayer::GetChapterPos(int chapterIdx) const
{
  std::unique_lock lock(m_StateSection);
  if (chapterIdx > 0 && chapterIdx <= static_cast<int>(m_State.chapters.size()))
    return std::chrono::round<std::chrono::seconds>(m_State.chapters[chapterIdx - 1].second)
        .count();

  return -1;
}

std::optional<std::chrono::milliseconds> CVideoPlayer::GetChapterPosMs(int chapterIdx) const
{
  std::unique_lock lock(m_StateSection);
  if (chapterIdx > 0 && chapterIdx <= static_cast<int>(m_State.chapters.size()))
    return m_State.chapters[chapterIdx - 1].second;

  return std::nullopt;
}

int CVideoPlayer::ToRawChapter(int visibleChapter) const
{
  std::unique_lock lock(m_StateSection);
  if (visibleChapter > 0 && visibleChapter <= static_cast<int>(m_State.rawChapters.size()))
    return m_State.rawChapters[visibleChapter - 1];

  return visibleChapter;
}

int CVideoPlayer::GetPreviousChapter()
{
  // 5-second grace period from chapter start to skip backwards to previous chapter
  // Afterwards skip to start of current chapter.
  const int chapter = GetChapter();

  if (chapter > 0 && (GetTime() < (GetChapterPos(chapter) + 5) * 1000))
    return chapter - 1;
  else
    return chapter;
}

bool CVideoPlayer::HasBookmarks() const
{
  std::unique_lock lock(m_StateSection);
  return !m_State.m_bookmarks.empty();
}

std::vector<std::chrono::milliseconds> CVideoPlayer::GetBookmarks() const
{
  std::unique_lock lock(m_StateSection);
  return m_State.m_bookmarks;
}

void CVideoPlayer::SetBookmarks(const std::vector<std::chrono::milliseconds>& bookmarks)
{
  std::unique_lock lock(m_StateSection);
  m_State.m_bookmarks = bookmarks;
  CServiceBroker::GetDataCacheCore().SetBookmarks(bookmarks);
}

int CVideoPlayer::GetPreviousBookmark(std::chrono::milliseconds ts)
{
  std::unique_lock lock(m_StateSection);
  std::vector<std::chrono::milliseconds> bookmarks = m_State.m_bookmarks;

  if (bookmarks.empty())
    return -1;

  // Add grace period of 5 seconds to make it easier to skip backwards through bookmarks
  //! @todo reduce to 2 seconds if seek accuracy can be improved
  const std::chrono::milliseconds adjusted_ts = ts - 5s;

  size_t idx{0};
  while (idx < bookmarks.size() && bookmarks[idx] < adjusted_ts)
    ++idx;

  return idx > 0 ? static_cast<int>(idx) - 1 : -1;
}

int CVideoPlayer::GetNextBookmark(std::chrono::milliseconds ts)
{
  std::unique_lock lock(m_StateSection);
  std::vector<std::chrono::milliseconds> bookmarks = m_State.m_bookmarks;

  if (bookmarks.empty())
    return -1;

  size_t idx{0};
  while (idx < bookmarks.size() && bookmarks[idx] <= ts)
    ++idx;

  return idx < bookmarks.size() ? idx : -1;
}

std::optional<std::chrono::milliseconds> CVideoPlayer::GetBookmarkPos(int idx)
{
  std::unique_lock lock(m_StateSection);
  if (idx >= 0 && static_cast<size_t>(idx) < m_State.m_bookmarks.size())
    return m_State.m_bookmarks[idx];
  else
    return std::nullopt;
}

void CVideoPlayer::AddSubtitle(const std::string& strSubPath)
{
  m_messenger.Put(
      std::make_shared<CDVDMsgType<std::string>>(CDVDMsg::SUBTITLE_ADDFILE, strSubPath));
}

bool CVideoPlayer::IsCaching() const
{
  std::unique_lock lock(m_StateSection);
  return !m_State.isInMenu && m_State.caching;
}

int CVideoPlayer::GetCacheLevel() const
{
  std::unique_lock lock(m_StateSection);
  return (int)(m_State.cache_level * 100);
}

double CVideoPlayer::GetQueueTime()
{
  int a = m_VideoPlayerAudio->GetLevel();
  int v = m_processInfo->GetLevelVQ();
  return std::max(a, v) * m_messageQueueTimeSize * 1000.0 / 100.0;
}

int CVideoPlayer::AddSubtitleFile(const std::string& filename, const std::string& subfilename)
{
  std::string ext = URIUtils::GetExtension(filename);
  std::string vobsubfile = subfilename;
  if (ext == ".idx" || ext == ".sup")
  {
    std::shared_ptr<CDVDDemux> pDemux;
    if (ext == ".idx")
    {
      if (vobsubfile.empty())
      {
        // find corresponding .sub (e.g. in case of manually selected .idx sub)
        vobsubfile = CUtil::GetVobSubSubFromIdx(filename);
        if (vobsubfile.empty())
          return -1;
      }

      auto pDemuxVobsub = std::make_shared<CDVDDemuxVobsub>();
      if (!pDemuxVobsub->Open(filename, STREAM_SOURCE_NONE, vobsubfile))
        return -1;

      m_SelectionStreams.Update(nullptr, pDemuxVobsub.get(), vobsubfile);
      pDemux = pDemuxVobsub;
    }
    else // .sup file
    {
      CFileItem item(filename, false);
      std::shared_ptr<CDVDInputStream> pInput;
      pInput = CDVDFactoryInputStream::CreateInputStream(nullptr, item);
      if (!pInput || !pInput->Open())
        return -1;

      auto pDemuxFFmpeg = std::make_shared<CDVDDemuxFFmpeg>();
      if (!pDemuxFFmpeg->Open(pInput, false))
        return -1;

      m_SelectionStreams.Update(nullptr, pDemuxFFmpeg.get(), filename);
      pDemux = pDemuxFFmpeg;
    }

    ExternalStreamInfo info =
        CUtil::GetExternalStreamDetailsFromFilename(m_item.GetDynPath(), filename);

    for (auto sub : pDemux->GetStreams())
    {
      if (sub->type != StreamType::SUBTITLE)
        continue;

      int index = m_SelectionStreams.TypeIndexOf(
          StreamType::SUBTITLE, m_SelectionStreams.Source(STREAM_SOURCE_DEMUX_SUB, filename),
          sub->demuxerId, sub->uniqueId);
      SelectionStream& stream = m_SelectionStreams.Get(StreamType::SUBTITLE, index);

      if (stream.name.empty())
        stream.name = info.name;

      if (stream.language.IsEmpty())
        stream.language = info.language;

      if (static_cast<StreamFlags>(info.flag) != StreamFlags::FLAG_NONE)
        stream.flags = static_cast<StreamFlags>(info.flag);
    }

    UpdateContent();
    // the demuxer id is unique
    m_subtitleDemuxerMap[pDemux->GetDemuxerId()] = pDemux;
    return m_SelectionStreams.TypeIndexOf(
        StreamType::SUBTITLE, m_SelectionStreams.Source(STREAM_SOURCE_DEMUX_SUB, filename),
        pDemux->GetDemuxerId(), 0);
  }

  if(ext == ".sub")
  {
    // if this looks like vobsub file (i.e. .idx found), add it as such
    std::string vobsubidx = CUtil::GetVobSubIdxFromSub(filename);
    if (!vobsubidx.empty())
      return AddSubtitleFile(vobsubidx, filename);
  }

  SelectionStream s;
  s.source   = m_SelectionStreams.Source(STREAM_SOURCE_TEXT, filename);
  s.type = StreamType::SUBTITLE;
  s.id       = 0;
  s.filename = filename;
  ExternalStreamInfo info = CUtil::GetExternalStreamDetailsFromFilename(m_item.GetDynPath(), filename);
  s.name = info.name;
  s.language = info.language;
  if (static_cast<StreamFlags>(info.flag) != StreamFlags::FLAG_NONE)
    s.flags = static_cast<StreamFlags>(info.flag);

  m_SelectionStreams.Update(s);
  UpdateContent();
  return m_SelectionStreams.TypeIndexOf(StreamType::SUBTITLE, s.source, s.demuxerId, s.id);
}

namespace
{
int CalculateCurrentChapter(
    std::chrono::milliseconds currentTime,
    const std::vector<std::pair<std::string, std::chrono::milliseconds>>& chapters)
{
  // Chapters are assumed to be sorted in increasing timestamp order
  if (chapters.empty() || currentTime < chapters[0].second)
    return 0;

  const std::size_t end = chapters.size() - 1;

  for (std::size_t i = 0; i < end; ++i)
  {
    if (currentTime >= chapters[i].second && currentTime < chapters[i + 1].second)
      return i + 1;
  }
  if (currentTime >= chapters[end].second)
    return static_cast<int>(end + 1);

  return 0;
}

// A chapter is fully cut (i.e. not reachable/visible to the user) if its whole
// [start, end) span, on the original (pre-EDL) timeline, is covered by the union of one or
// more contiguous/overlapping EDL CUTs.
bool IsChapterFullyCut(const CEdl& edl,
                       std::chrono::milliseconds chapterStart,
                       std::chrono::milliseconds chapterEnd)
{
  if (!edl.HasCuts())
    return false;

  // GetRawEditList() is sorted ascending by start
  std::chrono::milliseconds covered{chapterStart};
  for (const EDL::Edit& edit : edl.GetRawEditList())
  {
    if (edit.action != EDL::Action::CUT || edit.end <= covered)
      continue;
    if (edit.start > covered)
      break; // gap between cuts - some visible content remains

    covered = edit.end;
    if (covered >= chapterEnd)
      return true;
  }
  return false;
}

// Translate a raw demuxer/inputstream chapter number to the corresponding 1-based visible
// chapter number using a just-built rawChapters map. Returns 0 if rawChapter has no visible
// counterpart (e.g. it was filtered out as fully cut).
int ToVisibleChapter(const std::vector<int>& rawChapters, int rawChapter)
{
  const auto it = std::ranges::find(rawChapters, rawChapter);
  if (it != rawChapters.end())
    return static_cast<int>(std::distance(rawChapters.begin(), it)) + 1;

  return 0;
}
} // namespace

void CVideoPlayer::UpdatePlayState(double timeout)
{
  if (m_State.timestamp != 0 &&
      m_State.timestamp + DVD_MSEC_TO_TIME(timeout) > m_clock.GetAbsoluteClock())
    return;

  SPlayerState state(m_State);

  state.dts = DVD_NOPTS_VALUE;
  if (m_CurrentVideo.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.dts;
  else if (m_CurrentAudio.dts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.dts;
  else if (m_CurrentVideo.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentVideo.startpts;
  else if (m_CurrentAudio.startpts != DVD_NOPTS_VALUE)
    state.dts = m_CurrentAudio.startpts;

  state.startTime = 0;
  state.timeMin = 0;

  std::shared_ptr<CDVDInputStream::IMenus> pMenu = std::dynamic_pointer_cast<CDVDInputStream::IMenus>(m_pInputStream);

  bool chapterNbEnabled{false};

  if (m_pDemuxer)
  {
    const std::chrono::milliseconds rawStreamLength{m_pDemuxer->GetStreamLength()};

    state.chapters.clear();
    state.rawChapters.clear();
    if (const int chapterCount = m_pDemuxer->GetChapterCount(); chapterCount > 0)
    {
      for (int i = 0, ie = chapterCount; i < ie; ++i)
      {
        const std::chrono::milliseconds rawStart{m_pDemuxer->GetChapterPos(i + 1)};
        // GetStreamLength() returns 0 when the duration is unknown (e.g. live/network
        // streams)
        const std::chrono::milliseconds rawEnd{
            i + 1 < ie ? m_pDemuxer->GetChapterPos(i + 2)
                       : (rawStreamLength > rawStart ? rawStreamLength
                                                     : std::chrono::milliseconds::max())};
        if (IsChapterFullyCut(m_Edl, rawStart, rawEnd))
          continue;

        auto& p = state.chapters.emplace_back(std::string{}, m_Edl.GetTimeWithoutCuts(rawStart));
        m_pDemuxer->GetChapterName(p.first, i + 1);
        state.rawChapters.emplace_back(i + 1);
      }
    }

    if (IsInMenuInternal() && pMenu && !pMenu->CanSeek())
    {
      state.chapter = 0;
    }
    else
    {
      state.chapter = ToVisibleChapter(state.rawChapters, m_pDemuxer->GetChapter());
      chapterNbEnabled = true;
    }

    state.time = DVD_TIME_TO_MSEC(m_clock.GetClock());
    // Let the clock catch up after seek
    if (m_CurrentVideo.startpts != DVD_NOPTS_VALUE &&
        state.time < DVD_TIME_TO_MSEC(m_CurrentVideo.startpts))
      state.time = DVD_TIME_TO_MSEC(m_CurrentVideo.startpts);

    state.timeMax = static_cast<double>(rawStreamLength.count());
  }

  state.canpause = false;
  state.canseek = false;
  state.cantempo = false;
  state.isInMenu = false;
  state.menuType = MenuType::NONE;

  if (m_pInputStream)
  {
    CDVDInputStream::ITimes* pTimes = m_pInputStream->GetITimes();
    CDVDInputStream::IDisplayTime* pDisplayTime = m_pInputStream->GetIDisplayTime();

    CDVDInputStream::ITimes::Times times;
    const bool haveTimes = pTimes && pTimes->GetTimes(times);

    // Raw (pre-EDL) upper bound of the stream, needed below to tell whether the last
    // chapter is fully contained within a trailing EDL cut.
    std::chrono::milliseconds rawStreamEnd{std::chrono::milliseconds::max()};
    if (haveTimes)
      rawStreamEnd = std::chrono::milliseconds(DVD_TIME_TO_MSEC(times.ptsEnd - times.ptsStart));
    else if (pDisplayTime && pDisplayTime->GetTotalTime() > 0)
      rawStreamEnd = std::chrono::milliseconds(pDisplayTime->GetTotalTime());

    CDVDInputStream::IChapter* pChapter = m_pInputStream->GetIChapter();
    if (pChapter)
    {
      state.chapters.clear();
      state.rawChapters.clear();
      if (const int chapterCount = pChapter->GetChapterCount(); chapterCount > 0)
      {
        for (int i = 0, ie = chapterCount; i < ie; ++i)
        {
          const std::chrono::milliseconds rawStart{pChapter->GetChapterPos(i + 1)};
          const std::chrono::milliseconds rawEnd{i + 1 < ie ? pChapter->GetChapterPos(i + 2)
                                                            : rawStreamEnd};
          if (IsChapterFullyCut(m_Edl, rawStart, rawEnd))
            continue;

          auto& p = state.chapters.emplace_back(std::string{}, m_Edl.GetTimeWithoutCuts(rawStart));
          pChapter->GetChapterName(p.first, i + 1);
          state.rawChapters.emplace_back(i + 1);
        }
      }

      if (IsInMenuInternal() && pMenu && !pMenu->CanSeek())
      {
        state.chapter = 0;
      }
      else
      {
        state.chapter = ToVisibleChapter(state.rawChapters, pChapter->GetChapter());
        chapterNbEnabled = true;
      }
    }

    if (haveTimes)
    {
      state.startTime = times.startTime;
      state.time = (m_clock.GetClock() - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.timeMax = (times.ptsEnd - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.timeMin = (times.ptsBegin - times.ptsStart) * 1000 / DVD_TIME_BASE;
      state.time_offset = -times.ptsStart;
    }
    else if (pDisplayTime && pDisplayTime->GetTotalTime() > 0)
    {
      if (state.dts != DVD_NOPTS_VALUE)
      {
        int dispTime = 0;
        if (m_CurrentVideo.id >= 0 && m_CurrentVideo.dispTime)
          dispTime = m_CurrentVideo.dispTime;
        else if (m_CurrentAudio.dispTime)
          dispTime = m_CurrentAudio.dispTime;

        state.time_offset = DVD_MSEC_TO_TIME(dispTime) - state.dts;
      }
      state.time += state.time_offset * 1000 / DVD_TIME_BASE;
      state.timeMax = pDisplayTime->GetTotalTime();
    }
    else
    {
      state.time_offset = 0;
    }

    if (pMenu)
    {
      if (!pMenu->GetState(state.player_state))
        state.player_state = "";

      if (m_dvd.state == DVDSTATE_STILL)
      {
        const auto now = std::chrono::steady_clock::now();
        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_dvd.iDVDStillStartTime);
        state.time = duration.count();
        state.timeMax = m_dvd.iDVDStillTime.count();
        state.isInMenu = true;
      }
      else if (IsInMenuInternal())
      {
        // BD: state.time already carries the dispTime-offset-corrected
        // (presentation-proximate) value from the branch above; overwriting
        // it with the demux-side bd_tell position made the menu OSD clock
        // run up to a queue depth ahead of the picture (review A13). Keep
        // the raw input-stream time only as fallback (DVD path, or no
        // usable dts/duration).
        if (!(m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && pDisplayTime &&
              pDisplayTime->GetTotalTime() > 0 && state.dts != DVD_NOPTS_VALUE))
          state.time = pDisplayTime->GetTime();
        state.isInMenu = true;
        if (!pMenu->CanSeek())
          state.time_offset = 0;
      }
      state.menuType = pMenu->GetSupportedMenuType();
    }

    state.canpause = m_pInputStream->CanPause();

    bool realtime = m_pInputStream->IsRealtime();
    state.cantempo = CanTempo() && !realtime;

    m_processInfo->SetStateRealtime(realtime);
  }

  if (m_Edl.HasCuts())
  {
    state.time = static_cast<double>(
        m_Edl.GetTimeWithoutCuts(std::chrono::milliseconds(std::lround(state.time))).count());
    state.timeMax = state.timeMax - static_cast<double>(m_Edl.GetTotalCutTime().count());
  }

  // Attempt to calculate the current chapter from the currently known playing position
  // The current chapter provided by the demuxer/inputstream is ahead by a cache duration most of the time.
  if (chapterNbEnabled)
  {
    const std::chrono::milliseconds currentTime{llrint(state.time)};
    const int playPosChapter = CalculateCurrentChapter(currentTime, state.chapters);

    // Successfully calculated a current chapter from the play position?
    // Overwrite the current chapter reported by the demuxer/inputstream
    if (playPosChapter > 0)
      state.chapter = playPosChapter;
  }

  // Convert to second resolution used outside of VideoPlayer for chapter positions
  std::vector<std::pair<std::string, int64_t>> chapters;
  chapters.reserve(state.chapters.size());
  for (const auto& chapter : state.chapters)
  {
    chapters.emplace_back(chapter.first,
                          std::chrono::round<std::chrono::seconds>(chapter.second).count());
  }

  CServiceBroker::GetDataCacheCore().SetChapters(chapters);

  if (m_caching > CACHESTATE_DONE && m_caching < CACHESTATE_PLAY)
    state.caching = true;
  else
    state.caching = false;

  double queueTime = GetQueueTime();
  CacheInfo cache = GetCachingTimes();

  if (cache.valid)
  {
    state.cache_level = std::max(0.0, std::min(1.0, cache.level));
    state.cache_offset = cache.offset;
    state.cache_time = cache.time;
  }
  else
  {
    state.cache_level = std::min(1.0, queueTime / (m_messageQueueTimeSize * 1000.0));
    state.cache_offset = queueTime / state.timeMax;
    state.cache_time = queueTime / 1000.0;
  }

  XFILE::SCacheStatus status;
  if (m_pInputStream && m_pInputStream->GetCacheStatus(&status))
  {
    state.cache_bytes = status.forward;
    if(state.timeMax)
      state.cache_bytes += m_pInputStream->GetLength() * (int64_t)(queueTime / state.timeMax);
  }
  else
    state.cache_bytes = 0;

  state.timestamp = m_clock.GetAbsoluteClock();

  if (state.timeMax <= 0)
  {
    state.timeMax = state.time;
    state.timeMin = state.time;
  }
  if (state.timeMin == state.timeMax)
  {
    state.canseek = false;
    state.cantempo = false;
  }
  else
  {
    state.canseek = true;
    state.canpause = true;
  }

  m_processInfo->SetPlayTimes(state.startTime, state.time, state.timeMin, state.timeMax);

  std::unique_lock lock(m_StateSection);
  m_State = state;
}

bool CVideoPlayer::CanTempo()
{
  return OUTPUT::CSettingsMirror::Get().GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
}

int64_t CVideoPlayer::GetUpdatedTime()
{
  UpdatePlayState(0);
  return llrint(m_State.time);
}

void CVideoPlayer::SetDynamicRangeCompression(long drc)
{
  m_processInfo->GetVideoSettingsLocked().SetVolumeAmplification(static_cast<float>(drc) / 100);
  m_VideoPlayerAudio->SetDynamicRangeCompression(drc);
}

CVideoSettings CVideoPlayer::GetVideoSettings() const
{
  return m_processInfo->GetVideoSettings();
}

void CVideoPlayer::SetVideoSettings(CVideoSettings& settings)
{
  m_processInfo->SetVideoSettings(settings);
  m_renderManager.SetVideoSettings(settings);
  if (!m_audioOffsetToAudio)
    m_renderManager.SetDelay(static_cast<int>(settings.m_AudioDelay * 1000.0f));
  else if (static_cast<double>(settings.m_AudioDelay) * DVD_TIME_BASE != m_requestedAudioOffset)
  {
    m_audioOffsetChangedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    m_requestedAudioOffset = static_cast<double>(settings.m_AudioDelay) * DVD_TIME_BASE;
  }
  m_renderManager.SetSubtitleVerticalPosition(settings.m_subtitleVerticalPosition,
                                              settings.m_subtitleVerticalPositionSave);
  m_VideoPlayerVideo->EnableSubtitle(settings.m_SubtitleOn);
  m_VideoPlayerVideo->SetSubtitleDelay(static_cast<int>(-settings.m_SubtitleDelay * DVD_TIME_BASE));
}

void CVideoPlayer::FrameMove()
{
  if (m_renderLoopDead)
    CLog::Log(LOGERROR, "RENDERLOOP FrameMove on a dead player");
  m_renderManager.FrameMove();
}

void CVideoPlayer::Render(bool clear, uint32_t alpha, bool gui)
{
  m_renderManager.Render(clear, 0, alpha, gui);
}

void CVideoPlayer::FlushRenderer(bool overlaysOnly)
{
  m_renderManager.Flush(true, true, overlaysOnly);
}

void CVideoPlayer::SetRenderViewMode(int mode, float zoom, float par, float shift, bool stretch)
{
  m_processInfo->GetVideoSettingsLocked().SetViewMode(mode, zoom, par, shift, stretch);
  m_renderManager.SetVideoSettings(m_processInfo->GetVideoSettings());
  m_renderManager.SetViewMode(mode);
}

float CVideoPlayer::GetRenderAspectRatio() const
{
  return m_renderManager.GetAspectRatio();
}

void CVideoPlayer::GetRects(CRect& source, CRect& dest, CRect& view) const
{
  m_renderManager.GetVideoRect(source, dest, view);
}

unsigned int CVideoPlayer::GetOrientation() const
{
  return m_renderManager.GetOrientation();
}

void CVideoPlayer::TriggerUpdateResolution()
{
  std::string stereomode;
  m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode);
}

void CVideoPlayer::TriggerVs10Resolution()
{
  // E2-8b D3 (code review A-M2): the VS10 mode is a W input, so after the post its notice is
  // never `same`; before it (and with main not shut out) it is today's parameterless trigger
  std::string stereomode;
  m_renderManager.TriggerUpdateResolution(0, 0, 0, stereomode, "vs10");
}

bool CVideoPlayer::IsRenderingVideo() const
{
  return m_renderManager.IsConfigured();
}

bool CVideoPlayer::HasVisibleOverlay() const
{
  return m_renderManager.HasVisibleOverlay();
}

bool CVideoPlayer::IsLiveStream() const
{
  if (!m_processInfo)
    return false;
  return m_processInfo->IsRealtimeStream();
}

bool CVideoPlayer::IsStreaming() const
{
  if (!m_processInfo)
    return false;
  return m_processInfo->IsStreaming();
}

bool CVideoPlayer::EvaluateIsStreaming() const
{
  if (m_pInputStream && m_pInputStream->IsStreaming())
    return true;

  if (m_pDemuxer && m_pDemuxer->IsStreaming())
    return true;

  if (!m_item.GetProperty(STREAM_PROPERTY_INPUTSTREAM).empty())
    return true;

  const std::string& mime = m_item.GetMimeType();
  if (StringUtils::EqualsNoCase(mime, "application/vnd.apple.mpegurl") ||
      StringUtils::EqualsNoCase(mime, "vnd.apple.mpegurl") ||
      StringUtils::EqualsNoCase(mime, "application/x-mpegURL") ||
      StringUtils::EqualsNoCase(mime, "application/dash+xml") ||
      StringUtils::EqualsNoCase(mime, "application/vnd.ms-sstr+xml"))
  {
    return true;
  }

  // Dyn path extensions
  if (m_item.IsType(".m3u8") || m_item.IsType(".mpd") || m_item.IsType(".ism") ||
      m_item.IsType(".isml"))
    return true;

  // Smoothstreaming urls may look like https://server/path/subpath.ism/manifest?foo=bar
  const std::string filename = m_item.GetDynURL().GetFileName();
  if (StringUtils::EndsWithNoCase(filename, ".ism/manifest") ||
      StringUtils::EndsWithNoCase(filename, ".isml/manifest"))
    return true;

  return false;
}

bool CVideoPlayer::Supports(EINTERLACEMETHOD method) const
{
  if (!m_processInfo)
    return false;
  return m_processInfo->Supports(method);
}

EINTERLACEMETHOD CVideoPlayer::GetDeinterlacingMethodDefault() const
{
  if (!m_processInfo)
    return EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE;
  return m_processInfo->GetDeinterlacingMethodDefault();
}

bool CVideoPlayer::Supports(ESCALINGMETHOD method) const
{
  return m_renderManager.Supports(method);
}

bool CVideoPlayer::Supports(ERENDERFEATURE feature) const
{
#ifndef HAVE_LIBPOSTPROC
  if (feature == ERENDERFEATURE::RENDERFEATURE_POSTPROCESS)
    return false;
#endif
  return m_renderManager.Supports(feature);
}

void CVideoPlayer::VideoParamsChange()
{
  m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
}

void CVideoPlayer::GetDebugInfo(std::string &audio, std::string &video, std::string &general)
{
  audio = m_VideoPlayerAudio->GetPlayerInfo();
  video = m_VideoPlayerVideo->GetPlayerInfo();
  GetGeneralInfo(general);
}

void CVideoPlayer::UpdateClockSync(bool enabled)
{
  m_processInfo->SetRenderClockSync(enabled);
}

void CVideoPlayer::UpdateRenderInfo(CRenderInfo &info)
{
  m_processInfo->UpdateRenderInfo(info);
}

void CVideoPlayer::UpdateRenderBuffers(int queued, int discard, int free)
{
  m_processInfo->UpdateRenderBuffers(queued, discard, free);
}

void CVideoPlayer::UpdateGuiRender(bool gui)
{
  m_processInfo->SetGuiRender(gui);
}

void CVideoPlayer::UpdateVideoRender(bool video)
{
  m_processInfo->SetVideoRender(video);
}

// IDispResource interface
void CVideoPlayer::OnLostDisplay()
{
  // one hold however often the loss is announced (a disconnect, then the
  // reconnect's mode set)
  if (m_displayLost)
  {
    // 5.E2-7 (D2b): a walk meeting a real freeze whose word it would strand re-freezes it by CAS
    // steps (no body: the freeze is still in force)
    if (m_txFreezeReal.load())
      TxRefreeze();
    return;
  }
  // 5.E2-11 D1.2: a loss inside an open start before START takes no FREEZE body; decided on the
  // word W just wrote and the open job, as W's row 9 (no P member written)
  if (aml_transaction_loss_in_start())
  {
    const uint32_t ownT = m_txWordT.load();
    const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
    using WordState = OUTPUT::COutputWorker::WordState;
    const auto job = ownT && word.t == ownT && word.spanOpen ? DISPLAYSTATE::TxOpenJob() : nullptr;
    if (job && job->T == word.t &&
        (word.state == WordState::STARTING ||
         (word.state == WordState::NONE && !job->started.load())))
    {
      // HEAD's line once per loss (a Restore's own walk in the span is not one)
      auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
      const bool spanWalk = winSystem && winSystem->InSpanWalk();
      if (spanWalk)
        CLog::Log(LOGINFO, "VideoPlayer: OnLostDisplay received");
      CLog::Log(LOGINFO, "TXLOSS row 9 (no body) T={} e={} word {}{}", word.t, word.e,
                word.state == WordState::STARTING ? "STARTING" : "NONE",
                spanWalk ? "" : " (a mode set's walk)");
      return;
    }
  }
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (winSystem && winSystem->IsLostSpanOpen() && !m_holdOnDisplayLoss)
  {
    CLog::Log(LOGINFO, "VideoPlayer: display lost, playback runs on (no video, audio not on HDMI)");
    return;
  }
  CLog::Log(LOGINFO, "VideoPlayer: OnLostDisplay received");
  // 5.E2-3: the shadow word reads FREEZING parked while the body runs; on P it is the newcomer
  // freeze (HoldIfDisplayLost). T read once; 0 after the stop (E2-3 review M2, L4)
  const uint32_t wordT = aml_transaction_shadow_on() ? m_txWordT.load() : 0;
  const bool newcomer = wordT && IsCurrentThread();
  // the audio heard with the picture on screen: an output kept open through the mode change
  // gives back what it has not played from there. Read before the mark below, so the offset
  // it adds is the one in force when the loss came (E2-2 code review L1)
  const bool audioInited = m_VideoPlayerAudio->IsInited();
  double holdFrom = DVD_NOPTS_VALUE;
  if (audioInited)
  {
    double maxStep = 0.0;
    if (!(m_displayResumeOnGrid && m_renderManager.GetResumePts(holdFrom, maxStep)))
      holdFrom = DVD_NOPTS_VALUE;
    else
      holdFrom += m_clock.GetAudioOffset();
  }
  // 5.E2-7 (D1): real when the session's predicate holds and this freeze's word step can apply
  // (NONE, or the span's claim, for this player's T); stored before the mark, which P reads first
  bool txReal = false;
  const char* txLegacyWhy = nullptr;
  if (wordT && aml_transaction_loss_real())
  {
    const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
    using WordState = OUTPUT::COutputWorker::WordState;
    // code review L8: the audio engine's keepsink as it is now, not only as the latch computed it
    IAE* ae = CServiceBroker::GetActiveAE();
    txReal = word.t == wordT && ae && ae->KeepsOutputAcrossDisplayReset() &&
             (word.state == WordState::NONE || (word.state == WordState::FREEZING && !word.sub));
  }
  if (txReal)
  {
    TxRecordVsync();
    // 5.E2-7b (D3 (a)): VPA's own rule (VPA's PLAYER_DISPLAY_LOST): the picture's pts only for a
    // stream player at 1x; m_streamPlayerSpeed is the speed VPA was last sent
    m_txLossHoldFrom = audioInited && holdFrom != DVD_NOPTS_VALUE &&
                               m_streamPlayerSpeed == DVD_PLAYSPEED_NORMAL
                           ? holdFrom
                           : 0.0;
    m_txLossOffset = m_clock.GetAudioOffset();
    m_txLossHoldBefore = m_clock.GetHoldFrom();
    m_txLossPicture = audioInited && holdFrom != DVD_NOPTS_VALUE ? holdFrom : 0.0;
    m_txBodyDone = false;
    m_txHeldAsPause = false;
    m_txFreezeAtNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    m_txResumeDelayLogged = false;
    m_txFreezeReal.store(true, std::memory_order_release);
  }
  // 5.E2-2: the mark first, before the stream players are held and the clock pauses, so no P
  // reader (CheckHeldStart, CN's gate, the play press, the caching end, the offset landing)
  // schedules a resume or lands an offset inside the freeze; m_lossState goes LOST below, and
  // ConvertLostDisplayToPause retries its CAS on LOST (docs/phase5_e2_plan.md E2-2)
  m_displayLost = true;
  // after the mark, off the window E2-2 narrowed (E2-3 review L3)
  if (wordT && !OUTPUT::COutputWorker::Get().WordFreezeBody(
                   wordT, newcomer, ToWordHead(WordHeadNow(LossState::NONE))) &&
      txReal)
  {
    // the word moved between the read and the CAS: HEAD's freeze and reset for this one
    txReal = false;
    m_txFreezeReal = false;
    txLegacyWhy = "the body's word step did not apply";
  }
  if (audioInited)
  {
    m_VideoPlayerAudio->SendMessage(
        std::make_shared<CDVDMsgDouble>(CDVDMsg::PLAYER_DISPLAY_LOST, holdFrom), 1);
  }
  if (m_VideoPlayerVideo->IsInited())
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, true), 1);
  // before the clock pauses, so the renderer never takes this for a plain pause
  m_renderManager.SetDisplayLost(true);
  m_clock.Pause(true);
  m_lossState = LossState::LOST;
  m_discTime.SetDisplayLost(true);
  if (wordT)
    OUTPUT::COutputWorker::Get().WordFrozen(wordT, newcomer, ToWordHead(WordHeadNow(LossState::LOST)));
  if (!txReal && aml_transaction_on())
  {
    if (!txLegacyWhy)
      txLegacyWhy = !wordT                         ? "no word"
                    : !aml_transaction_loss_base() ? aml_transaction_loss_reason()
                    : aml_transaction_skip("resume") ? "transaction_skip resume"
                                                     : "word not NONE";
    // 5.E2-8a (D6): a loss between CN's opening and START meets the start's own STARTING
    if (wordT && std::string_view(txLegacyWhy) == "word not NONE")
    {
      const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
      if (word.t == wordT && word.state == OUTPUT::COutputWorker::WordState::STARTING)
        txLegacyWhy = "word STARTING, before E2-11";
    }
    CLog::Log(LOGINFO, "TXLOSS legacy ({}) T={}", txLegacyWhy, wordT);
  }
  {
    // gate (§18.24): the next frame is never behind the held clock
    double front = DVD_NOPTS_VALUE;
    double step = 0.0;
    const bool out = m_renderManager.GetResumePts(front, step);
    if (out)
      CLog::Log(LOGINFO, "VideoPlayer: display lost, next frame {:+.1f} ms from the clock",
                (front - m_clock.GetClock()) * 1000.0 / DVD_TIME_BASE);
    m_lostMaxStep = out ? step : 0.0;
    m_lostFront = out ? front : DVD_NOPTS_VALUE;
  }
  if (txReal)
  {
    const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
    CLog::Log(LOGINFO, "TXLOSS real ({}) T={} e={}", newcomer ? "newcomer" : "body", wordT, word.e);
    // 5.E2-11 D5: from W-visible facts only (the started job, or the published T0)
    {
      struct timespec mono = {};
      clock_gettime(CLOCK_MONOTONIC, &mono);
      const int64_t nowNs = static_cast<int64_t>(mono.tv_sec) * 1000000000 + mono.tv_nsec;
      const int64_t t0 = m_txLeadT0Ns.load();
      const auto job = DISPLAYSTATE::TxOpenJob();
      if ((job && job->T == wordT && job->started.load()) || t0 > nowNs)
        CLog::Log(LOGINFO, "TXLOSS in the lead (T0 in {} ms)",
                  t0 > nowNs ? (t0 - nowNs) / 1000000 : 0);
    }
    // r3 N3: the frozen checks are armed only now, the body done under the guard
    m_txBodyDone.store(true, std::memory_order_release);
  }
  // Causes https://github.com/xbmc/xbmc/issues/15447
  // FlushRenderer();
}

void CVideoPlayer::OnResetDisplay()
{
  if (!m_displayLost)
    return;
  // 5.E2-7 (D2d): a real freeze's reset is the word's CAS and P's wake; P runs RESUME
  if (m_txFreezeReal.load())
  {
    TxResetReal();
    return;
  }

  // a conversion to a pause on the player thread finishes first
  LossState state = LossState::LOST;
  if (!m_lossState.compare_exchange_strong(state, LossState::RETURNING))
  {
    for (int i = 0; state == LossState::CONVERTING && i < 1000; i++)
    {
      CThread::Sleep(1ms);
      state = m_lossState.load();
    }
    if (state == LossState::CONVERTING)
      CLog::Log(LOGERROR, "VideoPlayer: display back while the pause conversion still runs");
    m_lossState = LossState::RETURNING;
  }
  // 5.E2-3: T read once; 0 after the stop (E2-3 review M2, L4). The row comes from HEAD's state
  // after the spin (review M1)
  const uint32_t wordT = aml_transaction_shadow_on() ? m_txWordT.load() : 0;
  if (wordT)
    OUTPUT::COutputWorker::Get().WordReturning(
        wordT, static_cast<OUTPUT::COutputWorker::WordState>(WordMirror(state)),
        ToWordHead(WordHeadNow(LossState::RETURNING)));
  const bool heldAsPause = state == LossState::PAUSED;

  CLog::Log(LOGINFO, "VideoPlayer: OnResetDisplay received");
  ClearPlayerFreeze(heldAsPause);
  m_lossState = LossState::NONE;
  if (wordT)
    OUTPUT::COutputWorker::Get().WordReturned(wordT, ToWordHead(WordHeadNow(LossState::NONE)));
}

// 5.E2-7 (C2): the reset's body, moved out of OnResetDisplay unchanged (docs/phase5_e2_7_impl.md
// D4 step 3); OnResetDisplay calls it as before
void CVideoPlayer::ClearPlayerFreeze(bool heldAsPause)
{
  // resume like a user resume: from the picture on screen, at a vblank, the
  // audio (its sink just reopened) landing there instead of joining a clock
  // already running; the players resume after, so their first audio finds
  // the schedule
  constexpr double RESUME_LEAD = 0.35;
  const double held = m_clock.GetClock();
  double pts = DVD_NOPTS_VALUE;
  double maxStep = 0.0;
  int64_t startNs = 0;
  double startClock = 0.0;
  // a mode change sets the display latency tweak: this landing lands it
  if (m_audioOffsetToAudio)
  {
    const double requested = m_requestedAudioOffset;
    const double tweak = m_renderManager.GetLatencyTweak();
    const double landed = requested - DVD_MSEC_TO_TIME(tweak);
    m_clock.SetAudioOffset(landed);
    // 5.V2-4b: what the reset landed
    CLog::Log(LOGINFO,
              "VTIMING landing reset: audio offset {:+.3f} ms (requested {:+.3f} ms, tweak {:.3f} "
              "ms)",
              landed * 1000.0 / DVD_TIME_BASE, requested * 1000.0 / DVD_TIME_BASE, tweak);
  }
  int64_t discRunNs = 0;
  // 5.B2-5c: the clock held at the park, the frames out were released up to the lost mode's
  // latency and vsync past it; the new mode's can be shorter. Only up to the frame out then
  double lostStep = 0.0;
  double lostFront = DVD_NOPTS_VALUE;
  if (OUTPUT::LostFirstOn())
  {
    lostStep = m_lostMaxStep.exchange(0.0);
    lostFront = m_lostFront.exchange(DVD_NOPTS_VALUE);
  }
  const bool resumable = m_displayResumeOnGrid && m_renderManager.GetResumePts(pts, maxStep);
  double stepLimit = maxStep;
  if (resumable && lostStep > maxStep && lostFront != DVD_NOPTS_VALUE && pts <= lostFront)
  {
    stepLimit = lostStep;
    CLog::Log(LOGINFO,
              "VideoPlayer: display back, the lost mode's step limit {:.1f} ms (the new mode's "
              "{:.1f} ms, step {:.1f} ms)",
              lostStep * 1000.0 / DVD_TIME_BASE, maxStep * 1000.0 / DVD_TIME_BASE,
              (pts - held) * 1000.0 / DVD_TIME_BASE);
  }
  if (resumable && m_clock.UnpauseAt(pts, stepLimit, RESUME_LEAD, startNs, startClock))
  {
    m_displayResumeScheduled = true;
    discRunNs = startNs;
    CLog::Log(LOGINFO,
              "VideoPlayer: display back, resume scheduled: clock {:.3f} -> {:.3f} at {} ns",
              held / DVD_TIME_BASE, startClock / DVD_TIME_BASE, startNs);
  }
  else
  {
    if (heldAsPause)
      CLog::Log(LOGINFO, "VideoPlayer: display back, held as a pause (clock {:.3f})",
                held / DVD_TIME_BASE);
    else if (m_displayResumeOnGrid && pts == DVD_NOPTS_VALUE)
      CLog::Log(LOGINFO, "VideoPlayer: display back, nothing presented yet (clock {:.3f})",
                held / DVD_TIME_BASE);
    else if (m_displayResumeOnGrid)
      CLog::Log(LOGINFO, "VideoPlayer: display back, resume not scheduled (clock {:.3f}, next "
                "frame {:.3f})", held / DVD_TIME_BASE, pts / DVD_TIME_BASE);
    m_clock.Pause(false);
  }
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
  m_displayLost = false;
  m_discTime.SetDisplayLost(false, discRunNs);
  m_renderManager.SetDisplayLost(false);
  m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
  m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
  // a mode switch (e.g. Dolby Vision engaging mid-menu) leaves a still disc
  // menu black: no new video frame is presented, so the retained overlay is
  // never re-composed - repost it from the player thread (Process loop)
  m_repostDiscOverlays = true;
}

// 5.E2-7: the real loss path (docs/phase5_e2_7_impl.md D2-D4, D7, r3 amendments)
bool CVideoPlayer::TxRefreeze()
{
  const uint32_t wordT = m_txWordT.load();
  if (!wordT)
    return false;
  using WordState = OUTPUT::COutputWorker::WordState;
  const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
  // r3 L1: only where the walk would strand the word; elsewhere HEAD's silent return
  if (word.t != wordT ||
      !((word.state == WordState::FREEZING && !word.sub) ||
        (word.state == WordState::RETURNING && !word.spanOpen) ||
        word.state == WordState::RESET_OWED))
    return false;
  // r2 M2: HEAD's loss line, which p5f_check pairs with each unplug
  CLog::Log(LOGINFO, "VideoPlayer: OnLostDisplay received");
  CLog::Log(LOGINFO, "TXLOSS re-freeze ({}) T={} e={}",
            OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)), wordT, word.e);
  m_txBodyDone = false;
  WordState from = WordState::NONE;
  const bool lost = OUTPUT::COutputWorker::Get().WordRefreeze(wordT, from);
  if (lost)
    m_lossState = LossState::LOST;
  // r3 N2: RESUME waits for the setup after this freeze's own reset
  TxRecordVsync();
  m_txBodyDone.store(true, std::memory_order_release);
  return lost;
}

void CVideoPlayer::TxRecordVsync()
{
  const SAmlVsyncResetState vsync = AmlVsyncResetState();
  m_txVsyncRecord = vsync.resetReq;
  m_txSetupRecord = vsync.setups;
  m_txVsyncWasRegistered = vsync.registered;
  m_txVsyncGen.fetch_add(1, std::memory_order_release);
}

void CVideoPlayer::TxResetReal()
{
  const uint32_t wordT = m_txWordT.load();
  using WordState = OUTPUT::COutputWorker::WordState;
  const auto now = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
  if (wordT && now.t == wordT &&
      (now.state == WordState::RETURNING || now.state == WordState::RESET_OWED))
  {
    // code review L1: a second reset while RESUME is owed. HEAD returns silently here (its first
    // reset already cleared the mark): no "received" line; the word's skip is logged explained
    WordState from = WordState::NONE;
    const OUTPUT::COutputWorker::SWordHead head{"a second reset while RESUME is owed", true};
    OUTPUT::COutputWorker::Get().WordReturnReal(wordT, from, &head);
  }
  else
  {
    CLog::Log(LOGINFO, "VideoPlayer: OnResetDisplay received");
    if (wordT)
    {
      WordState from = WordState::NONE;
      const WordState to = OUTPUT::COutputWorker::Get().WordReturnReal(wordT, from);
      if (to == WordState::RETURNING)
      {
        // code review L5: latched, a re-freeze of a converted freeze stays held as a pause
        if (from == WordState::PAUSED)
          m_txHeldAsPause = true;
        m_lossState = LossState::RETURNING;
      }
    }
  }
  // a wake only: RESUME reads the word under the guard (r2 M4)
  m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RETURNED), 1);
}

void CVideoPlayer::TxResetPassState()
{
  m_txConvertStep = 0;
  m_txResumeWaiting = false;
  m_txResumeWaitLogged = false;
  m_txOtherWordTiming = false;
  m_txClockErrLogged = false;
  m_txSpanRefusedLogged = false;
  m_txSkipLogged = 0;
  m_txStateLogged = -1;
  m_txResumeClockErrLogged = false;
}

void CVideoPlayer::TxFreezeEnded()
{
  m_txFreezeReal = false;
  m_txBodyDone = false;
  m_txHeldAsPause = false;
  m_txConvertStep = 0;
  m_txResumeWaiting = false;
  m_txResumeWaitLogged = false;
  m_txOtherWordTiming = false;
  m_txClockErrLogged = false;
  m_txSpanRefusedLogged = false;
  m_txSkipLogged = 0;
  m_txStateLogged = -1;
  m_txResumeClockErrLogged = false;
  if (aml_transaction_inbox() && (m_txInboxTook || !m_txInboxHeldLogged.empty()))
    CLog::Log(LOGINFO, "TXLOSS inbox summary: took {}, held {}", m_txInboxTook,
              m_txInboxHeldLogged.size());
  m_txInboxTook = 0;
  m_txInboxHeldLogged.clear();
  m_txInboxFrontLogged = false;
  m_txInboxPauseTaken = false;
  m_txInboxSpeedLoss = 0;
}

void CVideoPlayer::TxClearAtEnd(const char* why)
{
  // code review L6: a conversion whose step 2 is still owed submits its callback first (HEAD
  // fires it in the one pass); P only, before the guard
  if (m_txFreezeReal.load() && m_txConvertStep == 1)
    TxConvertStep2();
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  const auto clear = [this, why]
  {
    if (!m_displayLost || !m_txFreezeReal.load())
      return;
    // the clock's part (i) at a stop or next file (draft42:107): released, disc time from 0; the
    // fixed part's stream messages so no stream stays display-held (errata E7 as proposed)
    m_clock.Pause(false);
    m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, false), 1);
    m_displayLost = false;
    m_discTime.SetDisplayLost(false);
    m_renderManager.SetDisplayLost(false);
    m_VideoPlayerAudio->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
    m_VideoPlayerVideo->SendMessage(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_DISPLAY_RESET), 1);
    m_lossState = LossState::NONE;
    TxFreezeEnded();
    CLog::Log(LOGINFO, "TXLOSS clear ({})", why);
  };
  if (winSystem)
    winSystem->RunUnderPlayerGuard(clear, false);
  else
    clear();
}

void CVideoPlayer::TxConvertStep2()
{
  m_txConvertStep = 0;
  const uint32_t ownT = m_txWordT.load();
  using WordState = OUTPUT::COutputWorker::WordState;
  const auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
  if (ownT && word.t == ownT && word.state == WordState::CONVERTING)
  {
    if (OUTPUT::COutputWorker::Get().WordPaused(ownT, ToWordHead(WordHeadNow(LossState::PAUSED))))
      m_lossState = LossState::PAUSED;
  }
  else if (ownT && word.t == ownT && word.state == WordState::RESET_OWED)
  {
    if (OUTPUT::COutputWorker::Get().WordResetOwedReturning(ownT))
    {
      m_txHeldAsPause = true;
      m_lossState = LossState::RETURNING;
    }
  }
  else
    CLog::Log(LOGINFO, "TXLOSS conversion step 2 on {} (its effects only)",
              OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)));
  // code review L5: the conversion ran: the RESUME after it is held as a pause, whatever the word
  m_txHeldAsPause = true;
  // r3 N1: HEAD's effects whatever the word (HEAD submits them even after a failed CAS)
  m_discTime.SetDisplayLost(false);
  ConvertToPauseCallbacks();
}

bool CVideoPlayer::TxResume(const char* site, bool tryGuard)
{
  auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
  if (!winSystem)
    return false;
  const uint32_t ownT = m_txWordT.load();
  const auto now = std::chrono::steady_clock::now();
  // code review M3: the wait belongs to the record it started under; a re-freeze (a new record,
  // in the loop or inside a drain) restarts it
  const uint64_t gen = m_txVsyncGen.load(std::memory_order_acquire);
  if (!m_txResumeWaiting || gen != m_txResumeGen)
  {
    m_txResumeWaiting = true;
    m_txResumeWaitLogged = false;
    m_txResumeWaitSince = now;
    m_txResumeGen = gen;
  }
  // step 1: two vblanks after the vsync's re-setup for this freeze's reset, read under the guard
  // so the reset's fan-out (which bumps the vsync's count) has finished
  SAmlVsyncResetState vsync;
  if (!winSystem->RunUnderPlayerGuard([&vsync] { vsync = AmlVsyncResetState(); }, tryGuard))
    return false;
  const auto waited =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_txResumeWaitSince);
  const std::chrono::milliseconds hold{aml_transaction_hooks().resumeWaitMs};
  if (waited < hold)
    return false;
  const uint64_t record = m_txVsyncRecord.load();
  // a reset this vsync took: wait for the Setup answering it. None taken: no vblank clock only if
  // none was set up at the freeze either; a vsync that stopped during the loss (set up at the
  // freeze, not now) is waited for until a newer Setup has run (code review L7)
  const bool wasRegistered = m_txVsyncWasRegistered.load();
  const bool noClock = vsync.resetReq == record && !vsync.registered && !wasRegistered;
  bool ready;
  if (noClock)
    ready = true;
  else if (vsync.resetReq != record)
    ready = vsync.registered && vsync.setupFor == vsync.resetReq && vsync.vblanks >= 2;
  else // the same Setup running through (as HEAD), or a newer one after a stop: two vblanks of it
    ready = vsync.registered && vsync.vblanks >= 2;
  if (!ready && waited - hold < std::chrono::milliseconds(200))
    return false;
  if (!m_txResumeWaitLogged)
  {
    m_txResumeWaitLogged = true;
    // fix verification N-L2: on a timeline-clock session the reference clock counts the
    // timekeeper's ticks (CVideoSyncTimeline, GLES GetVideoSync): CVideoSyncAML is not set up, so
    // no vsync reset exists to wait for; the cause is logged so the gate can tell it apart
    if (noClock)
      CLog::Log(LOGINFO, "RESUME no vblank clock ({})",
                PRESENTATION::TimelineClockActive().load()
                    ? "timeline clock: the reference clock counts the timekeeper's ticks"
                    : "no vsync set up");
    else if (!ready)
      CLog::Log(LOGWARNING, "RESUME vblank bound (setup {}/{}, vblanks {})", vsync.setupFor,
                vsync.resetReq, vsync.vblanks);
    else
      CLog::Log(LOGINFO, "RESUME wait {} ms (setup {}/{}, vblanks {}, setups {} since {})",
                waited.count(), vsync.setupFor, vsync.resetReq, vsync.vblanks, vsync.setups,
                m_txSetupRecord.load());
  }
  // steps 2-5 under the guard: the re-read, the clear, the CAS to NONE
  bool done = false;
  bool regen = false;
  uint64_t skipped = 0;
  uint32_t eDone = 0;
  int speed = DVD_PLAYSPEED_NORMAL;
  const bool pauseTaken = m_txInboxPauseTaken;
  const bool ran = winSystem->RunUnderPlayerGuard(
      [&]
      {
        // code review M3: a re-freeze and a new reset between step 1 and here: wait again
        if (m_txVsyncGen.load(std::memory_order_acquire) != gen)
        {
          regen = true;
          return;
        }
        const uint64_t value = OUTPUT::COutputWorker::Get().LoadWord();
        const auto word = OUTPUT::COutputWorker::UnpackWord(value);
        if (!ownT || word.t != ownT ||
            word.state != OUTPUT::COutputWorker::WordState::RETURNING || word.spanOpen)
        {
          skipped = value;
          return;
        }
        // code review L9: D3's clock check at RESUME's step 2 as well
        if (!m_clock.IsPaused() && !m_txResumeClockErrLogged)
        {
          m_txResumeClockErrLogged = true;
          CLog::Log(LOGERROR, "TXLOSS clock running while frozen (at RESUME, word {})",
                    OUTPUT::COutputWorker::WordText(value));
        }
        speed = m_clock.GetResumeSpeed(m_playSpeed);
        ClearPlayerFreeze(m_txHeldAsPause.load() &&
                          m_clock.GetResumeSpeed(m_playSpeed) == DVD_PLAYSPEED_PAUSE);
        m_lossState = LossState::NONE;
        std::string lostTo;
        if (!OUTPUT::COutputWorker::Get().WordResumeDone(ownT, value, lostTo))
          CLog::Log(LOGERROR, "RESUME CAS lost to {}", lostTo);
        m_messenger.Flush(CDVDMsg::PLAYER_DISPLAY_RETURNED);
        eDone = word.e;
        TxFreezeEnded();
        done = true;
      },
      tryGuard);
  if (!ran)
    return false;
  if (regen)
  {
    CLog::Log(LOGINFO, "RESUME wait restarted: a re-freeze between its steps");
    m_txResumeWaiting = false;
    return false;
  }
  if (skipped)
  {
    if (skipped != m_txSkipLogged)
    {
      m_txSkipLogged = skipped;
      CLog::Log(LOGINFO, "RESUME skipped (word {})", OUTPUT::COutputWorker::WordText(skipped));
    }
    m_txResumeWaiting = false;
    return false;
  }
  if (done)
    CLog::Log(LOGINFO, "RESUME done T={} e={} -> NONE ({}; speed {})", ownT, eDone, site, speed);
  // 5.E2-7b (D6): the loss's cut used the offset in force then
  if (done && pauseTaken && speed == DVD_PLAYSPEED_PAUSE &&
      m_clock.GetAudioOffset() != m_txLossOffset.load())
    CLog::Log(LOGINFO, "RESUME held, offset moved {:+.0f} -> {:+.0f} ms",
              m_txLossOffset.load() * 1000.0 / DVD_TIME_BASE,
              m_clock.GetAudioOffset() * 1000.0 / DVD_TIME_BASE);
  return done;
}

void CVideoPlayer::TxSetDrainHook(StreamType type, const char* site)
{
  std::function<EDrainHeld()> hook;
  if (site && std::string_view(site) == "OnExit")
  {
    // the file plays out in P's loop: only the last pictures can be held here; a real freeze
    // ends the held wait at once (no RESUME); the exit's clear follows the closes
    hook = [this]
    {
      if (m_displayLost && m_txFreezeReal.load())
      {
        if (!m_txExitHookLogged)
        {
          m_txExitHookLogged = true;
          CLog::Log(LOGINFO, "OnExit hook ended held wait");
        }
        return EDrainHeld::END;
      }
      return m_clock.IsPaused() ? EDrainHeld::HELD : EDrainHeld::NOT_HELD;
    };
  }
  else if (site)
  {
    // the segment closes, OpenDefaultStreams, CheckBetterStream: RESUME runs in the wait, the
    // guard by try-lock, never a blocking wait here (errata E5)
    const std::string where = std::string("drain:") + site;
    // gate hook transaction_resumedelay (E2-12b-2 A4, r2 M-3): read once; keyed on this freeze;
    // only the drain side's RESUME is withheld (P's loop RESUME never); inert at 0
    const int resumeDelayMs = aml_transaction_hooks().resumeDelayMs;
    hook = [this, where, resumeDelayMs]
    {
      if (resumeDelayMs > 0 && m_displayLost && m_txFreezeReal.load())
      {
        const int64_t at = m_txFreezeAtNs.load();
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        if (at && now - at < static_cast<int64_t>(resumeDelayMs) * 1000000)
        {
          if (!m_txResumeDelayLogged.exchange(true))
            CLog::Log(LOGWARNING, "TXTEST resumedelay {}: the drain-side RESUME withheld", resumeDelayMs);
          return EDrainHeld::NOT_HELD;
        }
      }
      if (m_displayLost && m_txFreezeReal.load())
      {
        const uint32_t ownT = m_txWordT.load();
        const auto word =
            OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
        if (ownT && word.t == ownT &&
            word.state == OUTPUT::COutputWorker::WordState::RETURNING && !word.spanOpen)
          TxResume(where.c_str(), true);
      }
      return m_clock.IsPaused() ? EDrainHeld::HELD : EDrainHeld::NOT_HELD;
    };
  }
  if (type == StreamType::AUDIO)
  {
    if (auto* audio = dynamic_cast<CVideoPlayerAudio*>(m_VideoPlayerAudio.get()))
      audio->SetDrainHeldHook(std::move(hook));
  }
  else if (auto* video = dynamic_cast<CVideoPlayerVideo*>(m_VideoPlayerVideo.get()))
    video->SetDrainHeldHook(std::move(hook));
}

void CVideoPlayer::TxResendPause(IDVDStreamPlayer* player, bool lostAtSend, const char* stream)
{
  // 5.E2-7 (D8, plan E2-2's L2): W's FREEZE body can land between the mark's read and the send,
  // its GENERAL_PAUSE(true) then overtaken by this GENERAL_PAUSE(false). On the real path only P
  // clears the mark and W sets it before its sends, so a re-read after the send sees it
  // code review L2: only on a real freeze, where only P clears the mark (a legacy freeze's reset on
  // W clears it too, and could land between the re-read and the resend)
  if (lostAtSend || !aml_transaction_loss_real() || aml_transaction_skip("l2") || !m_displayLost ||
      !m_txFreezeReal.load())
    return;
  player->SendMessage(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_PAUSE, true), 1);
  CLog::Log(LOGINFO, "L2 resend GENERAL_PAUSE(true) ({}): the display was lost during the open",
            stream);
}

bool CVideoPlayer::TxSpeedWaits()
{
  int speed = DVD_PLAYSPEED_NORMAL;
  CDVDMsg::Message front = CDVDMsg::NONE;
  // with no audio stream nothing lands, and nothing reports
  if (aml_transaction_inbox() && m_CurrentAudio.id >= 0 && m_clock.IsScheduledStartOpen())
  {
    std::shared_ptr<CDVDMsg> msg;
    const auto peek = [&speed](CDVDMsg& m)
    {
      if (m.IsType(CDVDMsg::PLAYER_SETSPEED))
        speed = static_cast<CDVDMsgPlayerSetSpeed&>(m).GetSpeed();
      return false;
    };
    m_messenger.GetFrontIf(msg, peek, [](CDVDMsg&) { return false; }, front);
  }
  const bool wait = front == CDVDMsg::PLAYER_SETSPEED && speed != DVD_PLAYSPEED_PAUSE &&
                    speed != DVD_PLAYSPEED_NORMAL;
  const auto now = std::chrono::steady_clock::now();
  if (wait && !m_txSpeedWaiting)
  {
    m_txSpeedWaiting = true;
    m_txSpeedWaitSince = now;
    CLog::Log(LOGINFO, "SPEED waits for the scheduled start: {}", speed);
  }
  else if (!wait && m_txSpeedWaiting)
  {
    m_txSpeedWaiting = false;
    CLog::Log(LOGINFO, "SPEED wait over after {} ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(now - m_txSpeedWaitSince)
                  .count());
  }
  return wait;
}

bool CVideoPlayer::TxInbox() const
{
  return m_txFreezeReal.load() && aml_transaction_inbox();
}

namespace
{
std::string TxMsgName(CDVDMsg::Message type)
{
  switch (type)
  {
    case CDVDMsg::PLAYER_DISPLAY_RETURNED:
      return "PLAYER_DISPLAY_RETURNED";
    case CDVDMsg::PLAYER_AVCHANGE:
      return "PLAYER_AVCHANGE";
    case CDVDMsg::PLAYER_TX_REPLY:
      return "PLAYER_TX_REPLY";
    case CDVDMsg::PLAYER_REPORT_STATE:
      return "PLAYER_REPORT_STATE";
    case CDVDMsg::PLAYER_SETSPEED:
      return "PLAYER_SETSPEED";
    case CDVDMsg::GENERAL_GUI_ACTION:
      return "GENERAL_GUI_ACTION";
    case CDVDMsg::PLAYER_STARTED:
      return "PLAYER_STARTED";
    case CDVDMsg::PLAYER_OPENFILE:
      return "PLAYER_OPENFILE";
    case CDVDMsg::PLAYER_SEEK:
      return "PLAYER_SEEK";
    case CDVDMsg::PLAYER_SEEK_CHAPTER:
      return "PLAYER_SEEK_CHAPTER";
    case CDVDMsg::GENERAL_FLUSH:
      return "GENERAL_FLUSH";
    case CDVDMsg::GENERAL_SYNCHRONIZE:
      return "GENERAL_SYNCHRONIZE";
    case CDVDMsg::DEMUXER_RESET:
      return "DEMUXER_RESET";
    case CDVDMsg::PLAYER_SET_STATE:
      return "PLAYER_SET_STATE";
    default:
      return "type " + std::to_string(static_cast<int>(type));
  }
}
} // namespace

const char* CVideoPlayer::TxInboxGuiHold(const CAction& action)
{
  if (!m_pInputStream || !m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY))
    return m_pInputStream && m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) ? "DVD" : "other";
#if defined(HAVE_LIBBLURAY)
  const int id = action.GetID();
  if (id == ACTION_SHOW_VIDEOMENU || id == ACTION_PREVIOUS_MENU || id == ACTION_NAV_BACK)
    return "menu call";
  auto* bd = static_cast<CDVDInputStreamBluray*>(m_pInputStream.get());
  if (!bd->IsHdmvMenuTitle())
    return "BD-J";
  if (m_dvd.state == DVDSTATE_STILL && m_dvd.iDVDStillTime != 0ms)
    return "still";
  if (!bd->IsInMenu())
    return "other";
  switch (id)
  {
    case ACTION_MOVE_LEFT:
    case ACTION_MOVE_RIGHT:
    case ACTION_MOVE_UP:
    case ACTION_MOVE_DOWN:
    case ACTION_SELECT_ITEM:
    case ACTION_NEXT_ITEM:
    case ACTION_PREV_ITEM:
      return nullptr;
    default:
      return id >= REMOTE_0 && id <= REMOTE_9 ? nullptr : "other";
  }
#else
  return "other";
#endif
}

void CVideoPlayer::TxInboxPass()
{
  // D2: a take never overtakes; the passed-over messages run after RESUME, at HEAD's moment
  const auto start = std::chrono::steady_clock::now();
  for (int n = 0; n < 8 && !m_bAbortRequest && std::chrono::steady_clock::now() - start < 20ms;
       ++n)
  {
    const bool listFront = !m_txInboxFrontLogged;
    std::string front;
    const char* why = nullptr;
    const auto pass = [listFront, &front](CDVDMsg& m)
    {
      // 5.E2-8b: a Start job's reply runs after RESUME (CN waits while lost), never holding the
      // inbox's takes behind it
      const bool passed = m.IsType(CDVDMsg::PLAYER_DISPLAY_RETURNED) ||
                          m.IsType(CDVDMsg::PLAYER_AVCHANGE) ||
                          m.IsType(CDVDMsg::PLAYER_REPORT_STATE) ||
                          m.IsType(CDVDMsg::PLAYER_TX_REPLY);
      if (passed && listFront)
        front += (front.empty() ? "" : ",") + TxMsgName(m.GetMessageType());
      return passed;
    };
    const auto accept = [this, &why](CDVDMsg& m)
    {
      if (m.IsType(CDVDMsg::PLAYER_SETSPEED))
      {
        // the reset's resume is scheduled only at normal speed (CDVDClock::UnpauseAt): a tempo
        // or a seek speed taken here would return unscheduled; it runs after RESUME instead
        const int speed = static_cast<CDVDMsgPlayerSetSpeed&>(m).GetSpeed();
        if (speed == DVD_PLAYSPEED_PAUSE || speed == DVD_PLAYSPEED_NORMAL)
          return true;
        why = "not pause or play";
        return false;
      }
      if (!m.IsType(CDVDMsg::GENERAL_GUI_ACTION))
        return false;
      why = TxInboxGuiHold(static_cast<CDVDMsgType<CAction>&>(m).m_value);
      return why == nullptr;
    };
    std::shared_ptr<CDVDMsg> msg;
    CDVDMsg::Message frontType = CDVDMsg::NONE;
    const bool took = m_messenger.GetFrontIf(msg, accept, pass, frontType) == MSGQ_OK;
    const CDVDMsg::Message first = took ? msg->GetMessageType() : frontType;
    if (listFront && first != CDVDMsg::NONE)
      front += (front.empty() ? "" : ",") + TxMsgName(first);
    if (listFront && !front.empty())
    {
      m_txInboxFrontLogged = true;
      CLog::Log(LOGINFO, "TXLOSS inbox front {}", front);
    }
    if (!took)
    {
      if (frontType != CDVDMsg::NONE)
      {
        std::string held = TxMsgName(frontType);
        if (why)
          held += std::string(" (") + why + ")";
        if (std::find(m_txInboxHeldLogged.begin(), m_txInboxHeldLogged.end(), held) ==
            m_txInboxHeldLogged.end())
        {
          m_txInboxHeldLogged.push_back(held);
          CLog::Log(LOGINFO, "TXLOSS inbox held behind {}", held);
        }
      }
      return;
    }
    ++m_txInboxTook;
    m_txInboxTaking = true;
    if (msg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      const int speed = std::static_pointer_cast<CDVDMsgPlayerSetSpeed>(msg)->GetSpeed();
      // gate E29B-1 item 4: a speed change taken in this loss stands over its 15 s conversion
      if (speed != UserSpeed())
      {
        m_txInboxSpeedLoss = m_txVsyncGen.load(std::memory_order_acquire);
        CLog::Log(LOGINFO, "TXLOSS speed {} taken in this loss: no 15 s pause conversion in this loss",
                  speed);
      }
      HandleSetSpeed(msg);
      m_txInboxTaking = false;
      if (speed == DVD_PLAYSPEED_PAUSE)
        m_txInboxPauseTaken = true;
      CLog::Log(LOGINFO,
                "TXLOSS inbox took SETSPEED {} (word {}; resumes at {}; holdFrom {} {:.3f} ms)",
                speed, OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::Get().LoadWord()),
                m_clock.GetResumeSpeed(m_playSpeed), m_txInboxHoldKept ? "kept" : "set",
                m_txInboxHoldValue / 1000.0);
    }
    else
    {
      const CAction action = std::static_pointer_cast<CDVDMsgType<CAction>>(msg)->m_value;
      OnAction(action);
      m_txInboxTaking = false;
      CLog::Log(LOGINFO, "TXLOSS inbox took GUI_ACTION {} (word {})", action.GetID(),
                OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::Get().LoadWord()));
    }
  }
}

void CVideoPlayer::TxFrozenPass()
{
  const auto now = std::chrono::steady_clock::now();
  // as HEAD's sleep branch: nor can a menu page or a boundary start progress while frozen
  if (m_menuPageWaiting)
    m_menuPageWaitSince = now;
  if (m_boundaryStartWaitSince != std::chrono::steady_clock::time_point{})
    m_boundaryStartWaitSince = now;
  using WordState = OUTPUT::COutputWorker::WordState;
  const uint32_t ownT = m_txWordT.load();
  // r3 N1: step 2 follows step 1 on the next pass, whatever the word
  if (m_txConvertStep == 1)
    TxConvertStep2();
  auto word = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
  const bool own = ownT && word.t == ownT;
  const bool frozenWord = own && (word.state == WordState::LOST ||
                                  word.state == WordState::CONVERTING ||
                                  word.state == WordState::PAUSED ||
                                  word.state == WordState::RESET_OWED ||
                                  word.state == WordState::RETURNING);
  if (own && static_cast<int>(word.state) != m_txStateLogged)
  {
    m_txStateLogged = static_cast<int>(word.state);
    CLog::Log(LOGINFO, "TXLOSS state {}",
              OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)));
  }
  // r3 N3: another word only counts if it persists to the 1 s fallback (never inside a body)
  // code review L4, fix verification N-M1: FREEZING claimed is W's walk in progress (its fan-out
  // can outlast 1 s), never "another word"; FREEZING parked only once the body is done
  if (!own ||
      !(frozenWord ||
        (word.state == WordState::FREEZING && (!word.sub || !m_txBodyDone.load()))))
  {
    if (!m_txOtherWordTiming)
    {
      m_txOtherWordTiming = true;
      m_txOtherWordSince = now;
    }
    else if (now - m_txOtherWordSince >= std::chrono::seconds(1))
    {
      CLog::Log(LOGERROR, "TXLOSS frozen under {} (T={}) for 1 s: cleared as HEAD's reset would",
                OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)), ownT);
      if (m_txConvertStep == 1)
        TxConvertStep2(); // code review L6
      auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
      bool refrozen = false;
      const auto fallback = [this, ownT, winSystem, &refrozen]
      {
        if (!m_displayLost || !m_txFreezeReal.load())
          return;
        // fix verification N-M1: never clear while the display is lost or the word is this
        // player's frozen state again (a re-freeze landed while the fallback waited for the guard)
        const auto w = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
        const bool ownFrozen =
            ownT && w.t == ownT && w.state != WordState::NONE &&
            w.state != WordState::STARTING && w.state != WordState::LEADING;
        if (ownFrozen || (winSystem && winSystem->IsLostSpanOpen()))
        {
          refrozen = true;
          return;
        }
        ClearPlayerFreeze(m_txHeldAsPause.load() &&
                          m_clock.GetResumeSpeed(m_playSpeed) == DVD_PLAYSPEED_PAUSE);
        m_lossState = LossState::NONE;
        TxFreezeEnded();
        CLog::Log(LOGINFO, "TXLOSS clear (fallback)");
      };
      if (winSystem)
        winSystem->RunUnderPlayerGuard(fallback, false);
      if (refrozen)
      {
        m_txOtherWordTiming = false;
        CLog::Log(LOGINFO, "TXLOSS fallback skipped: frozen again or the display still lost");
      }
      return;
    }
  }
  else
    m_txOtherWordTiming = false;
  // r3 N3 / r2 M1: the clock stays held while frozen (checked only once the body is done)
  if (frozenWord && m_txBodyDone.load(std::memory_order_acquire) && !m_txClockErrLogged &&
      !m_clock.IsPaused())
  {
    m_txClockErrLogged = true;
    CLog::Log(LOGERROR, "TXLOSS clock running while frozen (word {})",
              OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)));
  }
  // 5.E2-7b (D1): the inbox before the conversion and RESUME read the speed
  if (frozenWord && m_txConvertStep == 0 && m_txBodyDone.load(std::memory_order_acquire) &&
      TxInbox())
    TxInboxPass();
  if (own && word.state == WordState::LOST && word.spanOpen && m_txConvertStep == 0)
  {
    // step 1 of the conversion, gated by its word CAS (r3 L6)
    auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem());
    if (winSystem && winSystem->DisconnectedForMs() >= 15000 &&
        UserSpeed() != DVD_PLAYSPEED_PAUSE && CanPause() &&
        m_txInboxSpeedLoss != m_txVsyncGen.load(std::memory_order_acquire) &&
        OUTPUT::COutputWorker::Get().WordConverting(ownT,
                                                    ToWordHead(WordHeadNow(LossState::LOST))))
    {
      m_lossState = LossState::CONVERTING;
      CLog::Log(LOGINFO, "VideoPlayer: display lost for {} ms, now an ordinary pause",
                winSystem->DisconnectedForMs());
      ConvertToPauseSpeeds();
      m_txConvertStep = 1;
    }
  }
  if (!(own && word.state == WordState::RETURNING))
    m_txResumeWaiting = false; // a re-freeze restarts RESUME's wait at its next return
  else
  {
    if (word.spanOpen)
    {
      if (!m_txSpanRefusedLogged)
      {
        m_txSpanRefusedLogged = true;
        CLog::Log(LOGERROR, "RESUME refused: the span is open (word {})",
                  OUTPUT::COutputWorker::WordText(OUTPUT::COutputWorker::PackWord(word)));
      }
    }
    else if (TxResume("loop", false))
      return;
  }
  // the inbox: the wake, and a next file only in a span (D3, r3 L2); the rest stays queued for
  // HandleMessages after RESUME, HEAD's order
  const bool takeOpen = own && word.spanOpen &&
                        (word.state == WordState::LOST || word.state == WordState::CONVERTING ||
                         word.state == WordState::PAUSED);
  std::shared_ptr<CDVDMsg> msg;
  if (takeOpen && m_messenger.GetPacketCount(CDVDMsg::PLAYER_OPENFILE) > 0)
  {
    // code review L10, fix verification N-L3: the span re-read under the guard and the next file
    // taken in the same hold (guard -> queue, W's own order), so a next file never leaves the
    // queue unless it runs: the other messages keep their order around it
    if (auto* winSystem = dynamic_cast<CWinSystemAmlogic*>(CServiceBroker::GetWinSystem()))
      winSystem->RunUnderPlayerGuard(
          [this, &msg, ownT]
          {
            const auto w =
                OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord());
            if (w.t == ownT && w.spanOpen &&
                (w.state == WordState::LOST || w.state == WordState::CONVERTING ||
                 w.state == WordState::PAUSED))
              m_messenger.GetFirstOf(
                  msg, [](CDVDMsg& m) { return m.IsType(CDVDMsg::PLAYER_OPENFILE); },
                  std::chrono::milliseconds(0));
          },
          false);
  }
  if (!msg)
  {
    const auto timeout = own && word.state == WordState::RETURNING
                             ? std::chrono::milliseconds(5)
                             : std::chrono::milliseconds(50);
    if (m_messenger.GetFirstOf(
            msg, [](CDVDMsg& m) { return m.IsType(CDVDMsg::PLAYER_DISPLAY_RETURNED); },
            timeout) == MSGQ_OK)
      CLog::Log(LOGINFO, "TXLOSS inbox took Returned");
    return;
  }
  // newest wins, as HEAD's HandleMessages (an older next file is skipped)
  if (m_messenger.GetPacketCount(CDVDMsg::PLAYER_OPENFILE) > 0)
  {
    CLog::Log(LOGINFO, "TXLOSS inbox took OPENFILE: a newer one is queued, this one dropped");
    return;
  }
  CLog::Log(LOGINFO, "TXLOSS inbox took OPENFILE");
  // fix verification N-L1: an owed conversion step 2 (OnPlayBackPaused) before the next file's
  // callbacks, as HEAD's one-pass conversion
  if (m_txConvertStep == 1)
    TxConvertStep2();
  // row 28: the ABORT CAS first, then HEAD's next file with the clear before Prepare (D7)
  if (const uint32_t wordT = m_txWordT.exchange(0))
  {
    m_txStopF = OUTPUT::COutputWorker::UnpackWord(OUTPUT::COutputWorker::Get().LoadWord()).f;
    OUTPUT::COutputWorker::Get().WordStop(wordT);
  }
  m_txClearBeforePrepare = true;
  HandleOpenFile(msg);
}

void CVideoPlayer::UpdateFileItemStreamDetails(CFileItem& item, UpdateStreamDetails update)
{
  if (update == UpdateStreamDetails::UPDATE_IF_FLAGGED)
  {
    if (!m_updateStreamDetails)
      return;

    // For blurays
    item.SetProperty("update_stream_details", true);

    m_updateStreamDetails = false;
  }

  CLog::Log(LOGDEBUG, "CVideoPlayer: updating file item stream details with available streams");

  VideoStreamInfo videoInfo;
  AudioStreamInfo audioInfo;
  SubtitleStreamInfo subtitleInfo;
  CVideoInfoTag* info = item.GetVideoInfoTag();
  GetVideoStreamInfo(CURRENT_STREAM, videoInfo);
  info->m_streamDetails.SetStreams(videoInfo, m_processInfo->GetMaxTime() / 1000, audioInfo,
                                   subtitleInfo, CStreamDetail::MEDIA);

  //grab all the audio and subtitle info and save it

  for (int i = 0; i < GetAudioStreamCount(); i++)
  {
    GetAudioStreamInfo(i, audioInfo);
    info->m_streamDetails.AddStream(new CStreamDetailAudio(audioInfo, CStreamDetail::MEDIA));
  }

  for (int i = 0; i < GetSubtitleCount(); i++)
  {
    GetSubtitleStreamInfo(i, subtitleInfo);
    info->m_streamDetails.AddStream(new CStreamDetailSubtitle(subtitleInfo, CStreamDetail::MEDIA));
  }
}

//------------------------------------------------------------------------------
// content related methods
//------------------------------------------------------------------------------

void CVideoPlayer::UpdateContent()
{
  std::unique_lock lock(m_content.m_section);
  m_content.m_selectionStreams = m_SelectionStreams;
  m_content.m_programs = m_programs;
}

void CVideoPlayer::UpdateContentState()
{
  std::unique_lock lock(m_content.m_section);

  m_content.m_videoIndex = m_SelectionStreams.TypeIndexOf(
      StreamType::VIDEO, m_CurrentVideo.source, m_CurrentVideo.demuxerId, m_CurrentVideo.id);
  m_content.m_audioIndex = m_SelectionStreams.TypeIndexOf(
      StreamType::AUDIO, m_CurrentAudio.source, m_CurrentAudio.demuxerId, m_CurrentAudio.id);
  m_content.m_subtitleIndex =
      m_SelectionStreams.TypeIndexOf(StreamType::SUBTITLE, m_CurrentSubtitle.source,
                                     m_CurrentSubtitle.demuxerId, m_CurrentSubtitle.id);

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_DVD) && m_content.m_videoIndex == -1 &&
      m_content.m_audioIndex == -1)
  {
    std::shared_ptr<CDVDInputStreamNavigator> nav =
          std::static_pointer_cast<CDVDInputStreamNavigator>(m_pInputStream);

    m_content.m_videoIndex = m_SelectionStreams.TypeIndexOf(StreamType::VIDEO, STREAM_SOURCE_NAV,
                                                            -1, nav->GetActiveAngle());
    m_content.m_audioIndex = m_SelectionStreams.TypeIndexOf(StreamType::AUDIO, STREAM_SOURCE_NAV,
                                                            -1, nav->GetActiveAudioStream());

    // only update the subtitle index in libdvdnav if the subtitle is provided by the dvd itself,
    // i.e. for external subtitles the index is always greater than the subtitlecount in dvdnav
    if (m_content.m_subtitleIndex < nav->GetSubTitleStreamCount())
    {
      m_content.m_subtitleIndex = m_SelectionStreams.TypeIndexOf(
          StreamType::SUBTITLE, STREAM_SOURCE_NAV, -1, nav->GetActiveSubtitleStream());
    }
  }

  if (m_pInputStream->IsStreamType(DVDSTREAM_TYPE_BLURAY) && m_State.menuType == MenuType::NATIVE)
  {
    // Update settings with changes made in bluray menu
    CVideoSettings settings{m_processInfo->GetVideoSettings()};
    settings.m_AudioStream = m_content.m_audioIndex;
    settings.m_SubtitleStream = m_content.m_subtitleIndex;
    m_processInfo->SetVideoSettings(settings);
  }
}

void CVideoPlayer::GetVideoStreamInfo(int streamId, VideoStreamInfo& info) const
{
  std::unique_lock lock(m_content.m_section);

  if (streamId == CURRENT_STREAM)
    streamId = m_content.m_videoIndex;

  if (streamId < 0 || streamId > GetVideoStreamCount() - 1)
  {
    info.valid = false;
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(StreamType::VIDEO, streamId);
  if (!s.language.IsEmpty())
    info.language = s.language;

  if (!s.name.empty())
    info.name = s.name;

  m_renderManager.GetVideoRect(info.SrcRect, info.DestRect, info.VideoRect);

  info.valid = true;
  info.bitrate = s.bitrate;
  info.width = s.width;
  info.height = s.height;
  info.codecName = s.codec;
  info.videoAspectRatio = s.aspect_ratio;
  info.stereoMode = s.stereo_mode;
  info.flags = s.flags;
  info.hdrType = s.hdrType;

  // handle fallback on broken Dolby Vision path
  if (info.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    if (!aml_dolby_vision_enabled())
    {
      const CHDRCapabilities caps = CServiceBroker::GetWinSystem()->GetDisplayHDRCapabilities();
      info.hdrType = StreamHdrType::HDR_TYPE_HDR10;

      if (!caps.SupportsHDR10() || s.dovi.dv_profile == 4)
        info.hdrType = StreamHdrType::HDR_TYPE_NONE;
    }
  }

  if (info.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    if (info.hdrDetail.length() == 0)
      info.hdrDetail =
          s.dovi.dv_profile == 0 ? "" : std::to_string(static_cast<int>(s.dovi.dv_profile));
    // distinguish HDR10 from HLG base
    if (s.dovi.dv_profile == 8)
    {
      info.hdrDetail += ".";
      info.hdrDetail += std::to_string(static_cast<int>(s.dovi.dv_bl_signal_compatibility_id));
      if (s.dovi.dv_bl_signal_compatibility_id == 4)
        info.hdrTypeAlt = StreamHdrType::HDR_TYPE_HLG;
    }
    else if (s.dovi.dv_profile == 7)
    {
      info.hdrDetail += m_processInfo->GetDoviIsFEL() ? "FEL" : "MEL";
    }
  }
  else if (info.hdrType == StreamHdrType::HDR_TYPE_HDR10)
  {
    if (m_processInfo->GetIsHdr10Plus())
      info.hdrType = StreamHdrType::HDR_TYPE_HDR10PLUS;
  }
  else
  {
    info.hdrDetail = "";
    info.hdrTypeAlt = StreamHdrType::HDR_TYPE_NONE;
  }
  info.fpsRate = s.fpsRate;
  info.fpsScale = s.fpsScale;
}

int CVideoPlayer::GetVideoStreamCount() const
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(StreamType::VIDEO);
}

int CVideoPlayer::GetVideoStream() const
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_videoIndex;
}

void CVideoPlayer::SetVideoStream(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetVideoStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetVideoStream(iStream);
  SynchronizeDemuxer();
  NotifyVideoUpdate();
}

void CVideoPlayer::GetAudioStreamInfo(int index, AudioStreamInfo& info) const
{
  std::unique_lock lock(m_content.m_section);

  if (index == CURRENT_STREAM)
    index = m_content.m_audioIndex;

  if (index < 0 || index > GetAudioStreamCount() - 1)
  {
    info = AudioStreamInfo{};
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(StreamType::AUDIO, index);
  info.language = s.language;
  info.name = s.name;

  if (s.type == StreamType::NONE)
    info.name += " (Invalid)";

  info.valid = true;
  info.bitrate = s.bitrate;
  info.channels = s.channels;
  info.codecName = s.codec;
  info.codecDesc = s.codecDesc;
  info.flags = s.flags;
}

int CVideoPlayer::GetAudioStreamCount() const
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(StreamType::AUDIO);
}

int CVideoPlayer::GetAudioStream()
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_audioIndex;
}

void CVideoPlayer::SetAudioStream(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetAudioStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetAudioStream(iStream);
  SynchronizeDemuxer();
  NotifyAudioUpdate();
}

void CVideoPlayer::GetSubtitleStreamInfo(int index, SubtitleStreamInfo& info) const
{
  std::unique_lock lock(m_content.m_section);

  if (index == CURRENT_STREAM)
    index = m_content.m_subtitleIndex;

  if (index < 0 || index > GetSubtitleCount() - 1)
  {
    info = SubtitleStreamInfo{};
    return;
  }

  const SelectionStream& s = m_content.m_selectionStreams.Get(StreamType::SUBTITLE, index);
  info.name = s.name;

  if (s.type == StreamType::NONE)
    info.name += "(Invalid)";

  info.valid = true;
  info.language = s.language;
  info.codecName = s.codec;
  info.codecDesc = s.codecDesc;
  info.flags = s.flags;
  info.isExternal = STREAM_SOURCE_MASK(s.source) == STREAM_SOURCE_DEMUX_SUB ||
                    STREAM_SOURCE_MASK(s.source) == STREAM_SOURCE_TEXT;
}

void CVideoPlayer::SetSubtitle(int iStream)
{
  m_messenger.Put(std::make_shared<CDVDMsgPlayerSetSubtitleStream>(iStream));
  m_processInfo->GetVideoSettingsLocked().SetSubtitleStream(iStream);
  NotifySubtitleUpdate(SubtitleChange::FLAG_STREAMINFO_CHANGE);
}

int CVideoPlayer::GetSubtitleCount() const
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_selectionStreams.CountType(StreamType::SUBTITLE);
}

int CVideoPlayer::GetSubtitle()
{
  std::unique_lock lock(m_content.m_section);
  return m_content.m_subtitleIndex;
}

int CVideoPlayer::GetPrograms(std::vector<ProgramInfo>& programs)
{
  std::unique_lock lock(m_content.m_section);
  programs = m_programs;
  return programs.size();
}

void CVideoPlayer::SetProgram(int progId)
{
  m_messenger.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SET_PROGRAM, progId));
}

int CVideoPlayer::GetProgramsCount() const
{
  std::unique_lock lock(m_content.m_section);
  return m_programs.size();
}

void CVideoPlayer::SetUpdateStreamDetails()
{
  m_messenger.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_SET_UPDATE_STREAM_DETAILS));
}

void CVideoPlayer::NotifySubtitleUpdate(int flags)
{
  CVariant data;
  data["player"]["playerid"] = m_item.GetProperty("playlist_type_hint").asInteger32(-1);
  if ((flags & SubtitleChange::FLAG_STATUS_CHANGE) != 0)
  {
    data["property"]["subtitleenabled"] = m_processInfo->GetVideoSettings().m_SubtitleOn;
  }
  if ((flags & SubtitleChange::FLAG_STREAMINFO_CHANGE) != 0)
  {
    const int stream = m_processInfo->GetVideoSettings().m_SubtitleStream;
    SubtitleStreamInfo info;
    GetSubtitleStreamInfo(stream, info);
    if (!info.valid)
    {
      // Only proceed if we're also sending status change
      if ((flags & SubtitleChange::FLAG_STATUS_CHANGE) == 0)
        return;
    }
    else
    {
      // Only add stream info if valid
      CVariant contentEntry(CVariant::VariantTypeObject);
      contentEntry["index"] = stream;
      contentEntry["codec"] = info.codecDesc;
      contentEntry["isdefault"] = (info.flags & StreamFlags::FLAG_DEFAULT) != 0;
      contentEntry["isforced"] = (info.flags & StreamFlags::FLAG_FORCED) != 0;
      contentEntry["isimpaired"] = (info.flags & StreamFlags::FLAG_VISUAL_IMPAIRED) != 0;
      contentEntry["language"] = info.language.AsBcp47();
      contentEntry["name"] = info.name;
      data["property"]["currentsubtitle"] = contentEntry;
    }
  }
  CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnPropertyChanged",
                                                     data);
}

void CVideoPlayer::NotifyAudioUpdate()
{
  AudioStreamInfo info;
  const int stream = m_processInfo->GetVideoSettings().m_AudioStream;
  GetAudioStreamInfo(stream, info);
  if (!info.valid)
    return;
  CVariant data;
  data["player"]["playerid"] = m_item.GetProperty("playlist_type_hint").asInteger32(-1);
  CVariant contentEntry(CVariant::VariantTypeObject);
  contentEntry["index"] = stream;
  contentEntry["bitrate"] = info.bitrate;
  contentEntry["channels"] = info.channels;
  contentEntry["codec"] = info.codecDesc;
  contentEntry["isdefault"] = (info.flags & StreamFlags::FLAG_DEFAULT) != 0;
  contentEntry["isimpaired"] = (info.flags & StreamFlags::FLAG_HEARING_IMPAIRED) != 0;
  contentEntry["isoriginal"] = (info.flags & StreamFlags::FLAG_ORIGINAL) != 0;
  contentEntry["language"] = info.language.AsBcp47();
  contentEntry["name"] = info.name;
  data["property"]["currentaudiostream"] = contentEntry;
  CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnPropertyChanged",
                                                     data);
}

void CVideoPlayer::NotifyVideoUpdate()
{
  VideoStreamInfo info;
  const int stream = m_processInfo->GetVideoSettings().m_VideoStream;
  GetVideoStreamInfo(stream, info);
  if (!info.valid)
    return;
  CVariant data;
  data["player"]["playerid"] = m_item.GetProperty("playlist_type_hint").asInteger32(-1);
  CVariant contentEntry(CVariant::VariantTypeObject);
  contentEntry["index"] = stream;
  contentEntry["codec"] = info.codecName;
  contentEntry["height"] = info.height;
  contentEntry["width"] = info.width;
  contentEntry["language"] = info.language.AsBcp47();
  contentEntry["name"] = info.name;
  data["property"]["currentvideostream"] = contentEntry;
  CServiceBroker::GetAnnouncementManager()->Announce(ANNOUNCEMENT::Player, "OnPropertyChanged",
                                                     data);
}
