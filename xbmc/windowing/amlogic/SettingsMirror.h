/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "settings/lib/ISettingCallback.h"
#include "settings/lib/ISettingsHandler.h"
#include "settings/SettingsBase.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Phase 5 step 5.B1-2: the settings that link and DV writes and the video-side threads read,
// mirrored outside CSettings.
// A setting callback runs under that setting's lock, and from 5.B1-3 a callback may wait for a
// W job; W reading the setting through CSettings would then wait for the callback. The mirror
// takes each change in OnSettingChanging, which Kodi calls again with the old value when a
// change is refused, so it never keeps a value that did not stick.
// special://profile/settingsmirror_check compares every read with CSettings and logs a DIFF.
// 5H-G1 Q-C (user 2026-11-26 "move to lock free settings mirror"): reads take no lock. The ID
// table is fixed at construction; each value is an atomic (a string an atomic shared_ptr), so a
// reader never waits on main's change or a profile load. The settings P, V and A read that are
// not output settings are mirrored too (subtitle, locale, network, cache, audio). A read of an ID
// not in the table falls back to CSettings and marks a MAINCOUPLING "SettingsMirror miss" site.

namespace OUTPUT
{

class CSettingsMirror : public ISettingCallback, public ISettingsHandler
{
public:
  static CSettingsMirror& Get();

  //! main thread, with the settings loaded
  void Start();
  void Stop();

  bool GetBool(const std::string& id) const;
  int GetInt(const std::string& id) const;
  //! 5H-G1 Q-C: CSettings' own type rules (a string or number read of another type is ""/0)
  std::string GetString(const std::string& id) const;
  double GetNumber(const std::string& id) const;
  //! 5.E2-10 9d: the value is the mirror's (a setting not mirrored is read from CSettings)
  bool IsMirrored(const std::string& id) const;

  bool OnSettingChanging(const std::shared_ptr<const CSetting>& setting) override;
  void OnSettingsLoaded() override;

  //! 5.B3-7b: this thread's reads of one setting take value until the scope ends (W's Restore
  //! takes the DV_LED clamp it decided, which main writes only when it applies the reply)
  class CScopedValue
  {
  public:
    CScopedValue(const std::string& id, int value);
    ~CScopedValue();
    CScopedValue(const CScopedValue&) = delete;
    CScopedValue& operator=(const CScopedValue&) = delete;
  };

private:
  struct SSlot
  {
    std::string id;
    std::atomic<bool> present{false}; //!< found at the last load
    std::atomic<int> type{0}; //!< SettingType
    std::atomic<int> intValue{0}; //!< a bool as 0/1
    std::atomic<double> numberValue{0.0};
    std::atomic<std::shared_ptr<const std::string>> stringValue;
  };

  CSettingsMirror();
  void Store(const std::shared_ptr<const CSetting>& setting);
  void StoreSlot(SSlot& slot, const CSetting& setting);
  SettingsContainer Load();
  //! nullptr: not in the table, not yet started, or not found at the load
  const SSlot* Find(const std::string& id) const;
  void Check(const std::string& id, const SSlot& slot) const;

  std::mutex m_mutex; //!< Start/Stop only, never a read
  std::vector<std::unique_ptr<SSlot>> m_slots; //!< fixed at construction
  std::atomic<bool> m_started{false};
  std::atomic<bool> m_check{false};
};

} // namespace OUTPUT
