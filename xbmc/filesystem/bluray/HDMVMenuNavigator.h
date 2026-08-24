/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <map>
#include <vector>

class CURL;

namespace XFILE
{
struct PlaylistInformation;

/*!
 * \brief Asks a disc's own HDMV menu which playlists are the episodes, and in
 * what order.
 *
 * Statically parses index.bdmv and MovieObject.bdmv, executes the disc's HDMV
 * navigation programs on a small register-machine interpreter (semantics per
 * libbluray hdmv_vm.c / mobj_parse.c - no code copied), finds the menu the
 * disc parks on, structurally parses that menu's Interactive Graphics
 * composition (pages / button-overlap-groups / buttons and their navigation
 * commands - layout per libbluray ig_decode.c), then presses every button to
 * learn which playlist each menu entry starts. The menu's own statement of
 * the episode list replaces duration-guessing in the simplified-menu episode
 * matching whenever it is available.
 *
 * The approach and several hard-won rules (out-of-mux IG carried by
 * sub-paths, register state must be the state in force while the menu plays,
 * numeric-select ordering with neighbour-chain fallback, PLAY-ALL buttons
 * classified by reaching two or more episode-length playlists) come from the
 * KeyDB Tools episode-identification pipeline; the binary formats were
 * re-derived from libbluray's parsers.
 *
 * Pure metadata extraction: no rendering, no OCR, no playback side effects.
 * BD-J discs are out of scope (they have no HDMV programs to run) - the
 * caller falls back to the duration heuristics.
 */
class CHDMVMenuNavigator
{
public:
  struct MenuStatedEpisodes
  {
    //! Episode playlists in the order the disc's menu states them
    std::vector<unsigned int> episodePlaylists;
    //! Playlists reached by buttons that play two or more episode-length
    //! playlists (PLAY ALL buttons) - never episode entries themselves
    std::vector<unsigned int> playAllPlaylists;
    //! True when a menu was found and stated at least two episode playlists
    bool valid{false};
  };

  /*!
   * \param url bluray:// url of the disc (host = disc root)
   * \param playlists all playlists on the disc with their durations (from the
   *        directory scan) - used to validate button targets and to tell
   *        episode-length playlists from menu loops and trailers
   * \param minEpisodeDuration playlists at least this long count as episodes
   */
  static MenuStatedEpisodes GetMenuStatedEpisodes(
      const CURL& url,
      const std::map<unsigned int, PlaylistInformation>& playlists,
      std::chrono::milliseconds minEpisodeDuration);
};
} // namespace XFILE
