/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDOverlayContainer.h"

#include "DVDCodecs/Overlay/DVDOverlay.h"
#include "DVDInputStreams/DVDInputStreamNavigator.h"
#include "utils/log.h"
#if defined(HAS_LIBAMCODEC)
#include "windowing/amlogic/GraphicsTick.h"
#endif

#include <chrono>

#include <algorithm>
#include <memory>
#include <mutex>

CDVDOverlayContainer::~CDVDOverlayContainer()
{
  {
    // 5.G2 GT-5 (review rA B1, rB M4): a dying container never registers or posts again, so
    // the Clear below cannot put it back into the registry after ContainerGone
    std::unique_lock lock(*this);
    m_destroying = true;
  }
  Clear();
#if defined(HAS_LIBAMCODEC)
  // G calls back only through the registry, which this waits for; last, after every change
  GRAPHICSTICK::ContainerGone(this);
#endif
}

void CDVDOverlayContainer::ProcessAndAddOverlayIfValid(const std::shared_ptr<CDVDOverlay>& pOverlay)
{
  std::unique_lock lock(*this);

  // a disc menu composition group (non-flushable) REPLACES the previous menu
  // composition outright: disc navigation posts the complete current
  // composition on every change (and an empty group when the menu plane
  // closes), so any earlier menu group is stale - without this, a menu
  // change leaves the old menu's graphics on screen forever (non-flushable
  // overlays are exempt from the stop-markup below and from stream flushes)
  if (!pOverlay->IsOverlayContainerFlushable())
  {
    // A page that waits for its segment's first picture (6.1) replaces only
    // the pages still waiting; the current page stays up until then. A page
    // posted while one waits (highlight, animation, a repost) waits with it,
    // so nothing overtakes the page it follows.
    unsigned int pendingGen = 0;
    for (const auto& o : m_overlays)
      if (!o->IsOverlayContainerFlushable() && o->m_segmentGen > m_releasedSegment)
        pendingGen = std::max(pendingGen, o->m_segmentGen);
    if (pOverlay->m_segmentGen <= m_releasedSegment && pendingGen > 0)
      pOverlay->m_segmentGen = pendingGen;
    const bool waits = pOverlay->m_segmentGen > m_releasedSegment;

    for (auto it = m_overlays.begin(); it != m_overlays.end();)
    {
      if (!(*it)->IsOverlayContainerFlushable() &&
          (!waits || (*it)->m_segmentGen > m_releasedSegment))
        it = m_overlays.erase(it);
      else
        ++it;
    }
  }

  // markup any non ending overlays, to finish
  // when this new one starts, there can be
  // multiple overlays queued at same start
  // point so only stop them when we get a
  // new startpoint
  for(int i = m_overlays.size();i>0;)
  {
    i--;

    // disc menu overlays (non-flushable) are lifecycle-managed by the input
    // stream; an unrelated overlay (e.g. a subtitle from the same stream)
    // starting must not end them
    if (!m_overlays[i]->IsOverlayContainerFlushable())
      continue;

    if(m_overlays[i]->iPTSStopTime)
    {
      if(!m_overlays[i]->replace)
        break;
      if(m_overlays[i]->iPTSStopTime <= pOverlay->iPTSStartTime)
        break;
    }

    if (m_overlays[i]->iPTSStartTime != pOverlay->iPTSStartTime)
      m_overlays[i]->iPTSStopTime = pOverlay->iPTSStartTime;
  }

  m_overlays.emplace_back(pOverlay);
  if (!pOverlay->IsOverlayContainerFlushable())
    NoteUntimedChange();
}

VecOverlays* CDVDOverlayContainer::GetOverlays()
{
  return &m_overlays;
}

VecOverlays::iterator CDVDOverlayContainer::Remove(VecOverlays::iterator itOverlay)
{
  std::unique_lock lock(*this);
  return m_overlays.erase(itOverlay);
}

void CDVDOverlayContainer::CleanUp(double pts)
{
  std::unique_lock lock(*this);

  auto it = m_overlays.begin();
  while (it != m_overlays.end())
  {
    const std::shared_ptr<CDVDOverlay>& pOverlay = *it;

    // never delete forced overlays, they are used in menu's
    // clear takes care of removing them
    // also if stoptime = 0, it means the next subtitles will use its starttime as the stoptime
    // which means we cannot delete overlays with stoptime 0
    if (!pOverlay->bForced && pOverlay->iPTSStopTime <= pts && pOverlay->iPTSStopTime != 0)
    {
      if (!pOverlay->IsOverlayContainerFlushable())
        NoteUntimedChange();
      //CLog::Log(LOGDEBUG,"CDVDOverlay::CleanUp, removing {}", (int)(pts / 1000));
      //CLog::Log(LOGDEBUG,"CDVDOverlay::CleanUp, remove, start : {}, stop : {}", (int)(pOverlay->iPTSStartTime / 1000), (int)(pOverlay->iPTSStopTime / 1000));
      it = Remove(it);
      continue;
    }
    else if (pOverlay->bForced)
    {
      //Check for newer replacements
      auto it2 = it;
      bool bNewer = false;
      while (!bNewer && ++it2 != m_overlays.end())
      {
        const std::shared_ptr<CDVDOverlay>& pOverlay2 = *it2;
        // There can be multiple overlays queued at same start point.
        // Skip them to find a new start point.
        if (pOverlay2->bForced && pOverlay2->iPTSStartTime <= pts)
          bNewer = true;
      }

      if (bNewer)
      {
        if (!pOverlay->IsOverlayContainerFlushable())
          NoteUntimedChange();
        it = Remove(it);
        continue;
      }
    }
    ++it;
  }

}

void CDVDOverlayContainer::Flush()
{
  std::unique_lock lock(*this);

  // Flush only the overlays marked as flushable
  m_overlays.erase(std::remove_if(m_overlays.begin(), m_overlays.end(),
                                  [](const std::shared_ptr<CDVDOverlay>& ov) {
                                    return ov->IsOverlayContainerFlushable();
                                  }),
                   m_overlays.end());
}

void CDVDOverlayContainer::Clear()
{
  std::unique_lock lock(*this);
  const bool menu = std::any_of(m_overlays.begin(), m_overlays.end(),
                                [](const std::shared_ptr<CDVDOverlay>& o)
                                { return !o->IsOverlayContainerFlushable(); });
  m_overlays.clear();
  if (menu)
    NoteUntimedChange();
}

size_t CDVDOverlayContainer::GetSize()
{
  std::unique_lock lock(*this);
  return m_overlays.size();
}

bool CDVDOverlayContainer::ContainsOverlayType(DVDOverlayType type)
{
  bool result = false;

  std::unique_lock lock(*this);

  auto it = m_overlays.begin();
  while (!result && it != m_overlays.end())
  {
    if ((*it)->IsOverlayType(type)) result = true;
    ++it;
  }

  return result;
}

std::shared_ptr<CDVDOverlay> CDVDOverlayContainer::GetPresentLatestOverlay()
{
  std::unique_lock lock(*this);
  return PresentLatestLocked();
}

std::shared_ptr<CDVDOverlay> CDVDOverlayContainer::PresentLatestLocked()
{
  for (auto it = m_overlays.rbegin(); it != m_overlays.rend(); ++it)
  {
    if ((*it)->m_presentLatest && !(*it)->IsOverlayContainerFlushable() &&
        (*it)->m_segmentGen <= m_releasedSegment)
      return *it;
  }
  return nullptr;
}

std::shared_ptr<CDVDOverlay> CDVDOverlayContainer::GetWaitingPageOverlay(unsigned int& gen)
{
  std::unique_lock lock(*this);
  return WaitingPageLocked(gen);
}

std::shared_ptr<CDVDOverlay> CDVDOverlayContainer::WaitingPageLocked(unsigned int& gen)
{
  gen = 0;
  // pages posted while one waits wait with it (ProcessAndAddOverlayIfValid): the newest is the
  // page the segment's first picture shows
  for (auto it = m_overlays.rbegin(); it != m_overlays.rend(); ++it)
  {
    if ((*it)->m_presentLatest && !(*it)->IsOverlayContainerFlushable() &&
        (*it)->m_segmentGen > m_releasedSegment)
    {
      gen = (*it)->m_segmentGen;
      return *it;
    }
  }
  return nullptr;
}

uint64_t CDVDOverlayContainer::UntimedChanges()
{
  std::unique_lock lock(*this);
  return m_untimedChanges;
}

void CDVDOverlayContainer::NoteUntimedChange()
{
  m_untimedChanges++;
#if defined(HAS_LIBAMCODEC)
  if (m_destroying)
    return;
  GRAPHICSTICK::RegisterContainer(this);
  if (!GRAPHICSTICK::UntimedHook())
    return;
  // the snapshot is the overlays' references, taken under this lock: an image a posted state
  // holds is never changed after the post (BD replaces a cut or re-paletted image by a new one)
  const auto images = [](const std::shared_ptr<CDVDOverlay>& group)
  {
    std::vector<std::shared_ptr<CDVDOverlay>> out;
    if (group && group->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
      for (const auto& o : static_cast<CDVDOverlayGroup&>(*group).m_overlays)
        if (o)
          out.push_back(o);
    return out;
  };
  GRAPHICSTICK::SUntimedState state;
  state.change = m_untimedChanges;
  state.postNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
  const std::shared_ptr<CDVDOverlay> current = PresentLatestLocked();
  state.current = images(current);
  state.currentGen = current ? current->m_segmentGen : 0;
  state.keepAliveTick = current ? current->m_keepAliveTick : 0;
  state.waiting = images(WaitingPageLocked(state.waitingGen));
  state.releasedSegment = m_releasedSegment;
  GRAPHICSTICK::PostUntimed(std::move(state));
#endif
}

void CDVDOverlayContainer::PostUntimedSnapshot()
{
  std::unique_lock lock(*this);
  NoteUntimedChange();
}

void CDVDOverlayContainer::NotePresentedSegment(unsigned int gen)
{
  std::unique_lock lock(*this);
  if (gen <= m_releasedSegment)
    return;
  bool released = false;
  for (const auto& o : m_overlays)
    if (!o->IsOverlayContainerFlushable() && o->m_segmentGen > m_releasedSegment &&
        o->m_segmentGen <= gen)
    {
      CLog::Log(LOGDEBUG, "CDVDOverlayContainer - menu page of segment {} shown with its picture",
                o->m_segmentGen);
      released = true;
      break;
    }
  m_releasedSegment = gen;
  if (released)
    NoteUntimedChange();
}

void CDVDOverlayContainer::ReleasePendingMenu()
{
  std::unique_lock lock(*this);
  const unsigned int was = m_releasedSegment;
  for (const auto& o : m_overlays)
    m_releasedSegment = std::max(m_releasedSegment, o->m_segmentGen);
  if (m_releasedSegment != was)
    NoteUntimedChange();
}

bool CDVDOverlayContainer::HasPendingMenu()
{
  std::unique_lock lock(*this);
  for (const auto& o : m_overlays)
    if (!o->IsOverlayContainerFlushable() && o->m_segmentGen > m_releasedSegment)
      return true;
  return false;
}

bool CDVDOverlayContainer::HasDrawableOverlay()
{
  std::unique_lock lock(*this);

  auto isDrawable = [](const CDVDOverlay& o)
  { return o.IsOverlayType(DVDOVERLAY_TYPE_IMAGE) || o.IsOverlayType(DVDOVERLAY_TYPE_SPU); };

  for (const std::shared_ptr<CDVDOverlay>& ov : m_overlays)
  {
    if (!ov)
      continue;
    if (isDrawable(*ov))
      return true;
    // One level of recursion only (groups never nest here): a PGS subtitle and a
    // BD menu composition both reach the container as a GROUP whose members are
    // the IMAGE overlays - a top-level-only check would report a live subtitle as
    // "no drawable" and let the renderer blank it.
    if (ov->IsOverlayType(DVDOVERLAY_TYPE_GROUP))
      for (const std::shared_ptr<CDVDOverlay>& child :
           static_cast<const CDVDOverlayGroup&>(*ov).m_overlays)
        if (child && isDrawable(*child))
          return true;
  }
  return false;
}

/*
 * iAction should be LIBDVDNAV_BUTTON_NORMAL or LIBDVDNAV_BUTTON_CLICKED
 */
void CDVDOverlayContainer::UpdateOverlayInfo(
    const std::shared_ptr<CDVDInputStreamNavigator>& pStream, CDVDDemuxSPU* pSpu, int iAction)
{
  std::unique_lock lock(*this);

  pStream->CheckButtons();

  //Update any forced overlays.
  for(VecOverlays::iterator it = m_overlays.begin(); it != m_overlays.end(); ++it )
  {
    if ((*it)->IsOverlayType(DVDOVERLAY_TYPE_SPU))
    {
      auto pOverlaySpu = std::static_pointer_cast<CDVDOverlaySpu>(*it);

      // make sure its a forced (menu) overlay
      // set menu spu color and alpha data if there is a valid menu overlay
      if (pOverlaySpu->bForced)
      {
        if (pOverlaySpu.use_count() > 1)
        {
          pOverlaySpu = std::make_shared<CDVDOverlaySpu>(*pOverlaySpu);
          (*it) = pOverlaySpu;
        }

        if (pStream->GetCurrentButtonInfo(*pOverlaySpu, pSpu, iAction))
        {
          pOverlaySpu->m_textureid = 0;
        }

      }
    }
  }
}
