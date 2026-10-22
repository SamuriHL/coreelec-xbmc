/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLDisplay.h"
#include "DynamicDll.h"

#include "platform/linux/input/LibInputHandler.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "settings/lib/ISettingCallback.h"
#include "threads/CriticalSection.h"
#include "windowing/WinSystem.h"
#include "threads/SystemClock.h"
#include "system_egl.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include "windowing/Resolution.h"

#include <atomic>
#include <functional>
#include <optional>
#include <gbm.h>

class IDispResource;

class CWinSystemAmlogic : public CWinSystemBase, public ISettingCallback
{
public:
  CWinSystemAmlogic();
  virtual ~CWinSystemAmlogic();

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  // ISettingCallback: live-apply the Dolby Vision VSVDB max-luminance override
  // when the display-peak/override settings change during DV playback.
  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;

  bool DestroyWindow() override;
  bool MessagePump() override;
  void UpdateResolutions() override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
  float GetGuiSdrPeakLuminance() const override;
  HDR_STATUS GetOSHDRStatus() override;
  float GetFrameLatencyAdjustment() override;

  virtual void Register(IDispResource *resource);
  virtual void Unregister(IDispResource *resource);

  static void SettingOptionsComponentsFiller(const std::shared_ptr<const CSetting>& setting,
                                             std::vector<IntegerSettingOption>& list,
                                             int& current);

  void MonitorStart();
  void MonitorStop();

  CAMLDisplay* GetAmlDisplay() const { return m_amlDisplay.get(); }

  // DRM/KMS handles for the hardware-vsync reference clock (CVideoSyncAML).
  // CE22 runs the display on DRM (mesondrmfb), so the vsync source is the DRM
  // vblank sequence, not the legacy OSD-fb ioctl.
  int GetDRMDeviceFd() const { return m_amlDisplay ? m_amlDisplay->aml_get_Device_handle() : -1; }
  //! 5.B3-4: under hotplugworker the published DisplayState's (DISPLAYSTATE::LinkState), else
  //! today's live read of m_crtc / m_connection
  uint32_t GetDRMCrtcId() const;
  bool GetDRMConnected() const;
  //! 5.B3-4: the vblank wait, gated on GetDRMConnected's source
  bool DrmVsync();

  //! design §18.24 (P5-8): a lost display, held from the disconnect until a
  //! reset with the display connected again
  bool IsLostSpanOpen() const { return m_lostOpen.load(); }
  //! how long the output has been disconnected (0 while connected)
  int64_t DisconnectedForMs() const;
  //! calls the resource's OnLostDisplay() when a span is open; true if it did
  bool HoldIfDisplayLost(IDispResource* resource);
protected:
  //! no display reset inside an open span while the output is disconnected
  bool DisplayResetAllowed() const;
  //! the reset was fanned out with the display back
  void CloseLostSpan();
  std::string m_framebuffer_name;
  bool IsHotplugPending() const { return m_hotplugPending.load(); }
  bool IsPresentationReady() const { return m_presentationReady; }
  void SetPresentationReady(bool ready) { m_presentationReady = ready; }

  EGLDisplay m_nativeDisplay;

  RenderStereoMode m_stereo_mode;

  bool m_delayDispReset;
  XbmcThreads::EndTime<> m_dispResetTimer;
  //! real_player: the display reset follows the later of the refresh-change
  //! delay and the driven sink's own lock after a mode set
  bool SinkLockedAfterModeSet();
  std::chrono::steady_clock::time_point m_modeSetAt{};
  std::chrono::milliseconds m_resetDelay{0};
  XbmcThreads::EndTime<> m_sinkLockPoll;
  int m_sinkLockedReads{0};

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;
  //! 5.B2-1 LEDGER lines (design §4.3, log only): each lost and reset call, by the mode set
  //! or lost span it belongs to; called under m_resourceSection, except LedgerSetFailed
  void LedgerBegin(const char* kind);
  void LedgerLost(IDispResource* resource);
  void LedgerReset(IDispResource* resource, std::chrono::milliseconds delay);
  void LedgerResetDone();
  void LedgerSetFailed();
  std::atomic<uint64_t> m_ledgerSet{0};
  std::atomic_int m_ledgerLost{0};
  int m_ledgerReset{0};
  std::atomic_bool m_ledgerFailed{false};
  std::chrono::steady_clock::time_point m_ledgerAt{};
  std::unique_ptr<CLibInputHandler> m_libinput;
  bool m_force_mode_switch;
  bool m_hotplug_mode_switch{false};
  //! the set may re-clock a 1000/1001 rate difference at the live mode string
  bool m_frac_reclock{false};
  bool m_outputWorkerStarted{false};
  //! 5.B2-4's switch (surface epoch, C's cleared UI buffer): OUTPUT::OutputDriveOn() and no
  //! outputdrive_skip "B2-4", read once; read only after the boot set
  static bool SurfaceEpochOn();
  //! 5.B2-5b's switch: OUTPUT::OutputDriveOn(), no outputdrive_skip "B2-5b", and B2-3 and
  //! B2-4 on; read once, at W's start (OUTPUT::ModeSetsOnW() from then on)
  static bool ModeSetsOnWSwitch();
  //! 5.B2-5c's switch: under 5.B2-5b, the lost walk right after the park; no outputdrive_skip
  //! "B2-5c" and no outputdrive_order "today" (both: B2-5b's order). Read once, at W's start
  static bool LostFirstSwitch();
  //! hotplugworker_reinit "<period ms> <seconds>" (B3-4's gate): an injected re-init on W
  //! (park, aml_init_drmDevice, caps publish) every period, from W's start; hotplugworker only
  void StartTestReinit();
  //! CreateNewWindow's lost walk; 5.B2-5b runs it on W in main's mode set job, main waiting
  void LostWalk();
  //! 5.B2-5b: CreateNewWindow's arming of the delayed reset, on main from W's reply (r1 M9),
  //! counted from W's commit
  void ArmDelayedReset(int delay, std::chrono::steady_clock::time_point modeSetAt);
  //! 5.B2-5a: W's shadow of this set's decision, run in the mode set job before the commit,
  //! then given the commit's result (set by the GLES context for one set)
  std::function<void()> m_shadowBefore;
  std::function<void(bool committed, bool notConnected)> m_shadowAfter;
  //! main writes it; 5.B2-6's SETTLE reads it on W
  std::atomic_bool m_presentationReady{false};
  //! 5.B2-6: W's SETTLE is armed (W publishes it; main's lost-span retry reads it, r1 M9)
  std::atomic_bool m_resetPending{false};
  bool m_nativeGUI;
  static std::unique_ptr<CAMLDisplay> m_amlDisplay;
  std::unique_ptr<CAMLGBMUtils> m_amlGBMUtils{nullptr};
  std::unique_ptr<KODI::UTILS::EGL::CEGLFence> m_eglFence{nullptr};
private:
  struct callback_data
  {
    struct udev_monitor* udevMonitor;
    CWinSystemAmlogic* object;
  };

  void RefreshResolutions();
  //! 5.B3-6 (OUTPUT::DetectShadowOn): at a receipt (the FD thread), one W job per collapse that
  //! classifies the event as W will from B3-7b, and only logs it ("HOTPLUG shadow")
  void PostDetectShadow();
  void RunDetectShadow(std::chrono::steady_clock::time_point posted);
  std::atomic_bool m_shadowQueued{false};
  std::atomic<uint64_t> m_shadowReceipts{0};
  // W only
  unsigned int m_shadowLastAct{0}; //!< the hpd_cb_seq of the shadow's last act
  bool m_shadowSpan{false}; //!< the shadow's own span: opened by its loss, closed by its restore
  bool m_shadowDisconnectedRead{false}; //!< hpd_state source: the last read was disconnected
  uint64_t m_shadowDisconnectedAt{0}; //!< the receipts counted just after that read
  //! 5.B3-1 (log only): the init's "startup link" line and today's per-event read line;
  //! modes < 0: the event's re-init threw (connection and modes logged "-")
  void LogStartupLink();
  void LogHotplugRead(drmModeConnection connection, int modes);
  void OpenLostSpan();

  bool m_lostAtDisconnect{false};
  std::atomic_bool m_lostOpen{false};
  std::atomic<int64_t> m_disconnectedSinceMs{0};
  XbmcThreads::EndTime<> m_lostRetry;
  std::chrono::milliseconds m_lostRetryDelay{1000};
  void HotplugEvent();
  void RefreshDisplayCapabilities();
  //! 5.V1-2: W publishes the caps to DisplayState; main waits
  void PublishDisplayCaps();
  static void FDEventCallback(int id, int fd, short revents, void *data);

  int m_fdMonitorId;

  struct udev *m_udev;
  struct callback_data m_callback_data;
  std::atomic<bool> m_hotplugPending{false};
  //! the playing video's mode at the first hotplug of a playback
  std::optional<RESOLUTION_INFO> m_videoModeAtHotplug;
};
