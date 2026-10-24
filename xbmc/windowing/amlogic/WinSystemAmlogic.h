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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <gbm.h>

class IDispResource;

namespace KODI::WINDOWING::AML
{
struct SDriveJob;
}

//! 5.B3-7b (O1): the one reply of a W hotplug job, applied by main's MessagePump
struct SHotplugReply
{
  enum class Kind
  {
    ECHO,
    INVALID_READ,
    WAIT_TIMEOUT,
    THROW,
    LOSS,
    LOSS_CAPSFAIL,
    RESTORE,
    REASSERT,
    RETRY_NOOP,
    RETRY_RESTORE,
  };
  static const char* KindName(Kind kind);

  uint64_t replyId{0};
  uint64_t upTo{0}; //!< the receipts this reply answers
  Kind kind{Kind::THROW};
  //! the caps step ran: main sets the visibilities and the DV_LED value from these
  bool hasCaps{false};
  bool deviceDv{false};
  bool sinkDv{false};
  bool hdrDisplay{false};
  std::optional<int> dvLed; //!< the DV_LED clamp result (a DV sink), the value W's Restore used
  //! a multi-mode list: the probe's entries for main's table, by value, and the one RES_DESKTOP
  //! takes (RefreshResolutions' live-mode rule; -1 keeps it)
  bool hasTable{false};
  std::vector<RESOLUTION_INFO> table;
  int liveIndex{-1};
  //! the Restore's mode set, run on W; main adopts it or maps by value (O2)
  std::shared_ptr<KODI::WINDOWING::AML::SDriveJob> restoreJob;
  //! the Restore consumes W's recorded video mode (its row, or the retry's committed row), run
  //! without a throw
  bool videoMode{false};
  // W's own facts, for its job line
  HpdRead read;
  int modes{-1};
  int64_t parkMs{-1};
  bool parkRetried{false};
};

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
  //! 5.B3-7b (O1): with the hotplug on W, a receipt not yet answered by a popped reply
  bool IsHotplugPending() const
  {
    return m_hotplugPending.load() ||
           (m_wHotplug.load() && m_poppedReceipt.load() < m_receiptSeq.load());
  }
  bool IsPresentationReady() const { return m_presentationReady; }
  void SetPresentationReady(bool ready) { m_presentationReady = ready; }

  EGLDisplay m_nativeDisplay;

  RenderStereoMode m_stereo_mode;

  //! main writes it; 5.B3-7b's P5-9 timer on W reads it
  std::atomic_bool m_delayDispReset;
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
  //! 5.B3-7a (O3, OUTPUT::OrderingOn only), m_resourceSection held: true if the reset armed for
  //! lostGen may fan out (DisplayResetAllowed() and no lost fan-out since); else the reset is
  //! dropped and logged, and its lost calls stay owed to the next reset (B2-6's union)
  bool ResetStillArmedLocked(uint64_t lostGen, const char* reset);
  //! 5.B3-7a (O3): bumped by every lost fan-out (the span's, a set's LostWalk) under
  //! OUTPUT::OrderingOn; 0 under off. Read and written under m_resourceSection
  uint64_t m_lostGen{0};
  //! main: the lost generation WSA CreateNewWindow's walk armed (its delay 0 reset, its poll)
  uint64_t m_windowLostGen{0};
  //! main: the lost generation the poll reset (m_delayDispReset) is armed for: the walk of the
  //! last set it covers (a set at delay 0 leaves an armed poll to reset for it, as today)
  uint64_t m_pollLostGen{0};
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
  //! CreateNewWindow's lost walk; 5.B2-5b runs it on W in main's mode set job, main waiting.
  //! Returns the lost generation it armed (5.B3-7a; 0 under off)
  uint64_t LostWalk();
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

  // 5.B3-7b: the hotplug on W (OUTPUT::HotplugOnW, live from main's first pump after W's start)
  //! W's Restore inputs the GLES context fixes at that pump (fbName, uiFormat, the coordinator and
  //! graphics plane pointers, W's backups)
  virtual void LatchRestoreInputs() {}
  //! W: the Restore's mode set, run in the hotplug job's park; its job goes in the reply
  virtual void RunRestoreOnW(SHotplugReply& reply,
                             const RESOLUTION_INFO& res,
                             std::optional<int> dvLed)
  {
  }
  //! main, MessagePump: the queued replies, unless an apply or a GC set is on main's stack
  virtual void ApplyHotplugReplies() {}
  //! main: a reply's table into CDisplaySettings, and RES_DESKTOP's entry published for W
  //! (throwInside: the applythrow hook's "table")
  void ApplyHotplugTable(const SHotplugReply& reply, bool throwInside);
  //! main: a reply's setting visibilities and its DV_LED value
  void ApplyHotplugCaps(const SHotplugReply& reply);
  //! main: the first entry from RES_DESKTOP with this mode string, refresh (0.01 Hz) and flags
  //! (HotplugEvent's restore match); -1 when none
  static int FindResolution(const std::string& strId, float refresh, uint32_t flags);
  //! main: one W retry job now, whatever the span (the recovery of a dropped Restore)
  //! committed: the mode the dropped Restore committed (DisplayState's), which the retry
  //! re-commits when W's table has it (row 1 otherwise)
  void PostHotplugRetry(const DISPLAYSTATE::ModeFacts& committed);
  std::atomic_bool m_wHotplug{false};
  std::atomic<uint64_t> m_receiptSeq{0};
  std::atomic<uint64_t> m_poppedReceipt{0};
  std::atomic<uint64_t> m_appliedReceipt{0};
  //! main: the last reply with videoMode that main applied with GC on a row of the new table, or
  //! dropped with no retry to re-target it; W keeps its video mode until then (D5)
  std::atomic<uint64_t> m_videoModeApplied{0};
  std::mutex m_replyMutex;
  std::deque<SHotplugReply> m_replies;
  //! main: a reply is popped and being applied (scope guarded; P5-9's timer on W reads it)
  std::atomic_bool m_applyingReply{false};
  //! main: its own mode set request is on W or being applied (W's hotplug jobs and P5-9's timer
  //! wait while it is)
  std::atomic_int m_mainRequests{0};
  //! main: set with a request, cleared at the first pump after the last one (GuiInputs then
  //! carry its result); W's hotplug jobs and P5-9's timer wait while it is set
  std::atomic_bool m_guiInputsStale{false};
  //! W counts a reply main gave up on, main uncounts it when it takes it (P5-9's timer waits)
  std::atomic_int m_lateReplies{0};
  //! main: hotplug tables applied (a late reply posted before one maps its resolution by value)
  uint64_t m_hotplugTables{0};

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

  // 5.B3-7b: the hotplug on W
  //! main, the first pump after W's start under OUTPUT::HotplugOnW: W's inputs, then the switch
  void StartHotplugOnW();
  //! a receipt (the FD thread, or main handing over its flag): one W job per collapse (O1)
  void ReceiveHotplug(bool fromFd);
  //! W: one hotplug job (retry: P5-9's, on HEAD's predicate; no receipts taken, no echo test)
  //! recover: a dropped Restore's committed mode, the retry's target (the throw recovery only)
  void RunHotplugJob(bool retry, std::optional<DISPLAYSTATE::ModeFacts> recover = std::nullopt);
  void HotplugJob(SHotplugReply& reply,
                  bool retry,
                  const std::optional<DISPLAYSTATE::ModeFacts>& recover);
  //! W: P5-9's timer, armed at a span's opening
  void ArmHotplugRetry();
  void PostHotplugRetryTick(uint64_t gen, std::chrono::milliseconds after);
  void HotplugRetryTick(uint64_t gen);
  //! main: RES_DESKTOP's entry, for W's Restore target when a reconnect's live mode is not in its
  //! table (RefreshResolutions keeps the slot then)
  void PublishMainSlot();
  //! main: hotplugworker_setafter's settings resolution change, when due
  void TestSetAfter();
  std::atomic_bool m_jobQueued{false};
  std::atomic<int64_t> m_setAfterDueMs{0};
  std::mutex m_slotMutex;
  RESOLUTION_INFO m_mainSlot;
  // W only
  uint64_t m_wUpTo{0};
  uint64_t m_wReplies{0};
  unsigned int m_wLastAct{0}; //!< the hpd_cb_seq of W's last act
  bool m_wDisconnectedRead{false}; //!< hpd_state source: the last read was disconnected
  uint64_t m_wDisconnectedAt{0}; //!< the receipts counted just after that read
  std::optional<DISPLAYSTATE::ModeFacts> m_wVideoMode; //!< HotplugEvent's m_videoModeAtHotplug
  uint64_t m_wVideoModeReply{0}; //!< the last reply whose Restore targets it (0: none)
  bool m_wHas2160{false}; //!< W's last table
  bool m_wDeferred{false}; //!< the queued job waits for main's set (logged once)
  uint64_t m_wRetryGen{0};
  std::chrono::milliseconds m_wRetryDelay{1000};

  bool m_lostAtDisconnect{false};
  std::atomic_bool m_lostOpen{false};
  std::atomic<int64_t> m_disconnectedSinceMs{0};
  XbmcThreads::EndTime<> m_lostRetry;
  std::chrono::milliseconds m_lostRetryDelay{1000};
  void HotplugEvent();
  void RefreshDisplayCapabilities();
  //! RefreshDisplayCapabilities' DV rows (5.B3-7b: main's reply handler sets them too)
  void SetDvVisibility(bool device_dv, bool sink_dv);
  //! the DV_LED value a sink with dv_cap takes (RefreshDisplayCapabilities' clamp; W decides it
  //! for its Restore from 5.B3-7b)
  static int DvLedClamp(int value, int dvCap);
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
