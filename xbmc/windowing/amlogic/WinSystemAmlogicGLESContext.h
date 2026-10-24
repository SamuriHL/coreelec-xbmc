/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/EGLUtils.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/FrameBufferObject.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

class CPresentationCoordinator;
class CGraphicsPlaneAML;
class CTimekeeper;
struct gbm_bo;

class CGuiCompositeShaderGLES;

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

struct SDriveShadow;
struct SDriveJob;

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext();
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  //! 5.B2-5b: a reply main gave up on is applied here when it comes (r1 L11)
  bool MessagePump() override;

  // Implementation of CRenderSystemBase via CRenderSystemGLES
  bool InitRenderSystem() override;
  bool DestroyRenderSystem() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(const RenderStereoMode mode) const override;
  void PresentRender(bool rendered, bool videoLayer) override;

  // GUI compositing for HDR/DV: render the GUI into an sRGB FBO and transform
  // it to BT.2020 PQ once, post-blend, instead of per-primitive in the GUI
  // shaders (which blends anti-aliased edges in PQ space). See CRendererAML.
  bool SetGuiCompositing(int colorTransfer) override;
  bool BeginRender() override;
  bool BeginGuiComposite(bool guiWillRender) override;
  void EndGuiComposite() override;
  void CompositeGui() override;
  bool BeginHdrOverlayRender() override;
  void EndHdrOverlayRender(bool drawn) override;
  //! True only while HDR overlays really go through the HDR FBO and the
  //! composite. When that FBO cannot be created or bound they are drawn onto
  //! the back buffer instead, and the overlay textures must then carry the
  //! limited-range encode themselves.
  bool HdrOverlaysComposited() const override
  {
    return m_guiCompositing && !m_hdrFboUnavailable;
  }
  bool IsHdrComposite() const override { return m_guiCompositing; }
  bool HdrGraphicsOnPlane() const override;
  void PresentHdrGraphics(const SHdrGraphics& graphics) override;
  void ClearOverlayPlane() override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override {};

private:
  // real_player: the presentation coordinator commits the GUI plane
  //! true when it waited for the frame's commit, which paces the loop
  bool SubmitGuiFrame();
  void ReleaseReturnedGuiBuffers();
  void DetachGuiSurface();

  std::unique_ptr<CEGLContextUtils> m_pGLContext;
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;

  // GUI HDR/DV compositing state (see the SetGuiCompositing group above).
  bool m_guiCompositing{false};
  CFrameBufferObject m_guiFbo;
  int m_guiFboWidth{0};
  int m_guiFboHeight{0};
  // True when the GUI FBO is empty (no draws this frame); CompositeGui skips composite when true.
  bool m_guiFboClean{false};
  // Whether the GUI render pass will run this frame; set by BeginGuiComposite.
  bool m_guiWillRender{true};
  // HDR overlays (PQ PGS, disc menus) drawn off-screen by the video pass and
  // output as-is by CompositeGui under the transformed GUI. Drawing them straight
  // onto the back buffer instead stalled the GPU for ~4s at a BD-J menu clip
  // change (Superman UHD), freezing the menu's slide-in animation.
  CFrameBufferObject m_hdrFbo;
  int m_hdrFboWidth{0};
  int m_hdrFboHeight{0};
  bool m_hdrFboHasContent{false};
  bool m_hdrFboUnavailable{false};
  // GUI frames still to repaint after CreateNewWindow replaced the GBM surface. More than one,
  // because FlipPage cannot report a failed commit and swaps can fail around a mode switch.
  int m_guiRepaintFrames{0};
  // Transfer function the LUTs were built for, and the GUI reference white
  // (PQ-normalized) baked into them - kept so a live guipeakluminance change can
  // rebuild the PQ LUT without waiting for the next stream start.
  int m_guiCompositeTransfer{0};
  float m_guiCompositePeak{-1.0f};
  std::unique_ptr<CGuiCompositeShaderGLES> m_compositeShader;
  //! the KODI_LIMITED_RANGE define the kept program was compiled with
  bool m_compositeShaderLimited{false};
  bool BuildCompositeShader();

  //! false when there is no CRTC yet or the timekeeper isn't wanted
  bool StartTimekeeper();
  bool m_tickOwner{false};
  bool m_timekeeperShadow{false};
  bool m_timelineClock{false};
  // declared first so it outlives the coordinator, which polls its tick fd
  std::unique_ptr<CTimekeeper> m_timekeeper;
  std::unique_ptr<CPresentationCoordinator> m_coordinator;
  std::unique_ptr<CGraphicsPlaneAML> m_graphicsPlane;
  bool m_hdrGraphicsThisFrame{false}; //!< the video pass handed them over this GUI frame
  bool m_hdrGraphicsShown{false}; //!< the plane was last given something to show
  //! GUI buffers locked for the coordinator, not yet released
  int m_guiLocked{0};
  //! 5.B2-1 log only: m_guiLocked at the last surface detach; the set whose count is logged
  //! after its repaint frames
  int m_guiDiscarded{0};
  uint64_t m_guiLockedLogSet{0};
  std::vector<gbm_bo*> m_guiReturned;
  std::vector<uint64_t> m_guiReturnedEpochs;
  //! 5.B2-4: the UI epoch of the GUI surface (0: off, or the boot set's surface)
  uint64_t m_surfaceEpoch{0};
  int m_guiLockCapStops{0};
  //! 5.B2-5a: main's request for W's shadow; seeds W's backups from main's at the first one
  std::shared_ptr<SDriveShadow> BeginDriveShadow(const RESOLUTION_INFO& res,
                                                 bool fullScreen,
                                                 RenderStereoMode stereo,
                                                 StreamHdrType hdrType,
                                                 bool nativeGUI);
  //! W's backups from main's after a set
  void NoteDriveShadowBackups();
  //! the shadow of a set that ran main's mode set job, logged when the set is done
  std::shared_ptr<SDriveShadow> m_driveShadow;

  // 5.B2-5b: W runs main's mode sets (design §4.2, §4.5, §4.6; plan rule S1')
  //! main: the request by value, W's job, the bounded wait with the gfx lock released, the reply
  bool SetFullScreenOnW(bool fullScreen, RESOLUTION_INFO& res);
  //! W: today's CreateNewWindow sequence, in today's order, from the request alone
  void RunModeSetJob(SDriveJob& job);
  //! main: the reply's state into main's members, then the OutputChanged handler; false when
  //! the set (or its handler) failed
  bool ApplyDriveReply(SDriveJob& job);
  //! main's OutputChanged handler for a committed set: S1''s release and asserts, the EGL
  //! destroy, the new surface, the delay 0 reset, presentation ready
  bool RebuildGuiSurface(const SDriveJob& job);
  //! the display reset at delay 0 (CreateNewWindow, and 5.B2-5b's handler), armed for the set's
  //! lost generation (5.B3-7a: under OUTPUT::OrderingOn re-checked inside the lock, or dropped)
  void ResetDisplayNow(uint64_t lostGen);
  // 5.B2-6: SETTLE, the delayed display reset on W's clock (design §4.3, §4.4)
  struct SSettle
  {
    uint64_t gen{0}; //!< every arm and supersede bumps it; a check of an older one returns
    bool armed{false};
    uint64_t request{0}; //!< the reset waits for main's handler of this request
    uint64_t ledgerSet{0};
    uint64_t lostGen{0}; //!< 5.B3-7a: the set's lost generation, re-checked at the reset
    std::chrono::steady_clock::time_point modeSetAt{};
    std::chrono::milliseconds delay{0};
    bool lockWait{false}; //!< Q6: a film start at delay 0 waits for the sink lock
    int lockedReads{0};
    bool unreadableLogged{false};
  };
  //! W only
  SSettle m_settle;
  //! main: the request whose handler reached the reset (its new surface is up)
  std::atomic<uint64_t> m_settleHandled{0};
  //! main: the request whose handler failed (its SETTLE is dropped, Q9)
  std::atomic<uint64_t> m_settleFailed{0};
  //! W: ends the armed SETTLE unreset
  void SettleCancel(const char* why);
  //! main: a committed set whose handler failed: the Q9 ERROR, its SETTLE dropped on W
  void SettleHandlerFailed(const SDriveJob& job);
  //! W, at the park of a set: an armed SETTLE ends unreset; its owed lost stay in the ledger
  //! and go to this set's reset (union)
  void SettleSupersede(const SDriveJob& job);
  //! W, after the job: SETTLE armed for a committed set at delay > 0 or a film start at delay 0;
  //! a failed set's owed lost logged (Q9: no reset until a set succeeds)
  void SettleAfterJob(SDriveJob& job);
  //! W: one check of SETTLE generation gen, re-posted until the reset or a supersede
  void SettleCheck(uint64_t gen);
  //! the request main applied last, and the latest it posted (only its late reply is applied)
  uint64_t m_driveApplied{0};
  uint64_t m_drivePosted{0};
  //! the OUTPUTDRIVE set line and B2-1's ledger lines of a set, from the job's record
  void ReportDriveSet(
      const SDriveJob& job, bool created, int64_t waitMs, int64_t handlerMs, bool late);

  // 5.B3-7b: the hotplug on W (design §6; plan §4a O1, O2)
  void LatchRestoreInputs() override;
  void RunRestoreOnW(SHotplugReply& reply,
                     const RESOLUTION_INFO& res,
                     std::optional<int> dvLed) override;
  void ApplyHotplugReplies() override;
  //! how main applies a reply's Restore (O2)
  enum class RestoreForm
  {
    ADOPT, //!< its epoch is C's current one: GC's set adopts the reply (M9)
    BEHIND, //!< a newer set main applied rebuilt the surface: GC's row re-mapped by value
    AHEAD, //!< a newer W set's reply, still queued, rebuilds: the table and caps only
    AHEAD_OF_FAILED, //!< a newer set failed after its teardown: GC to the Restore's mode
    NOT_TORN_DOWN, //!< the Restore failed before its teardown: GC's row re-mapped by value
  };
  static const char* RestoreFormName(RestoreForm form);
  RestoreForm HotplugRestoreForm(const SDriveJob& job) const;
  //! main: a torn-down reply of a set whose epoch is not C's current one (a W set since) only
  //! updates main's backups, not-ready, and is logged; true then (O2, under the hotplug on W)
  bool SupersededOnW(const SDriveJob& job);
  void ApplyHotplugReply(SHotplugReply& reply);
  //! GC's row, read before a table step, on the new table by value (no set); false: not on it
  bool RemapGcRow(const SHotplugReply& reply, const RESOLUTION_INFO& before);
  //! main: a reply W's hand-off gives SetFullScreenOnW, for the adopt's GC set (one shot)
  std::shared_ptr<SDriveJob> m_adoptJob;
  //! main: the UI epoch of the last applied FAILED reply that had torn down (O2)
  uint64_t m_lastFailedEpoch{0};
  //! main: the reply whose deferral was logged
  uint64_t m_deferLogged{0};
  // W's Restore inputs, fixed at the hotplug's start on W (LatchRestoreInputs)
  std::string m_wFbName;
  uint32_t m_wUiFormat{0};
  CPresentationCoordinator* m_wCoordinator{nullptr};
  CGraphicsPlaneAML* m_wGraphicsPlane{nullptr};

  // 5.B3-7b D6 (log only; main's, no atomics): a presentation-gate span under the hotplug on W
  // (the "RENDERGATE span" line) and the first drawn frames after it ("RENDERGATE after").
  // Under hotplugworker_off / skip "B3-7b" m_wHotplug stays false and none of it runs.
  struct SRenderGateProbe
  {
    using Clock = std::chrono::steady_clock;
    bool open{false}; //!< a span: the first skipped present until the first that passes
    bool hotplug{false}; //!< the span began on IsHotplugPending (else on !IsPresentationReady)
    Clock::time_point start{};
    Clock::time_point lastSkip{};
    int64_t maxGapUs{0}; //!< the longest time between two skipped presents
    int drawn{0}; //!< skipped presents whose frame drew (hasRendered, after a BeginRender)
    int unbegun{0}; //!< skipped rendered presents with no BeginRender (ClearOverlayPlane's)
    int skipped{0};
    Clock::time_point applyAt{}; //!< main's last hotplug reply apply (MessagePump)
    uint64_t applyReply{0};
    Clock::time_point beginAt{}; //!< this frame's BeginRender, while tracking
    bool begun{false}; //!< a BeginRender since the last present (under m_wHotplug)
    Clock::time_point openedAt{}; //!< the span's end
    // the drawn presents from the span end: [0] is the present that opens the gate when it drew
    // (the span line says), else the first drawn after it (-1: none tracked)
    static constexpr int AFTER = 3;
    int after{-1};
    int undrawn{0}; //!< presents without a drawn frame among them
    int64_t atMs[AFTER]{}; //!< the present's entry, from the apply (from the span end without one)
    bool begunAt[AFTER]{}; //!< the present had a BeginRender
    int64_t drawUs[AFTER]{}; //!< BeginRender to the present's entry (-1: no BeginRender)
    int64_t swapUs[AFTER]{}; //!< TrySwapBuffers (-1: no swap, e.g. GUI_LOCK_CAP)
    int64_t lockUs[AFTER]{}; //!< SubmitGuiFrame's LockFront (-1: none)
    int64_t buildUs[AFTER]{}; //!< its BuildFlipRequest (-1: no buffer)
    const char* submit[AFTER]{}; //!< ok, refused (SM4), lock-cap, no-buffer, no-request, legacy
    int64_t waitUs[AFTER]{}; //!< SubmitGuiFrame's WaitTaken (-1: none)
    bool taken[AFTER]{}; //!< WaitTaken saw C take it within its 50 ms
    int64_t presentUs[AFTER]{}; //!< the present section of PresentRender
    int64_t curSwapUs{-1};
    int64_t curWaitUs{-1};
    int64_t curLockUs{-1};
    int64_t curBuildUs{-1};
    const char* curSubmit{"-"};
    bool curTaken{false};
    bool Tracking() const { return open || after >= 0; }
  } m_renderGate;
  void RenderGateSkip(bool rendered, bool hotplugPending);
  void RenderGateOpen(bool rendered, SRenderGateProbe::Clock::time_point now);
  void RenderGateAfter(bool rendered,
                       SRenderGateProbe::Clock::time_point entry,
                       SRenderGateProbe::Clock::time_point done);
  void RenderGateFlush(const char* why);
  //! 5.B3-7b D6 (fix): this span's "RENDERGATE glFlush" line is logged (main, under m_wHotplug)
  bool m_gateFlushLogged{false};
};

}
}
}
