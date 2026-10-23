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

#include <map>
#include <mutex>
#include <string>

// Phase 5 step 5.B1-2: the settings that link and DV writes and the video-side threads read,
// mirrored outside CSettings.
// A setting callback runs under that setting's lock, and from 5.B1-3 a callback may wait for a
// W job; W reading the setting through CSettings would then wait for the callback. The mirror
// takes each change in OnSettingChanging, which Kodi calls again with the old value when a
// change is refused, so it never keeps a value that did not stick.
// special://profile/settingsmirror_check compares every read with CSettings and logs a DIFF.

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
  CSettingsMirror() = default;
  void Store(const std::shared_ptr<const CSetting>& setting);
  SettingsContainer Load();

  mutable std::mutex m_mutex;
  std::map<std::string, int> m_values; // a bool as 0/1
  bool m_started = false;
  bool m_check = false;
};

} // namespace OUTPUT
