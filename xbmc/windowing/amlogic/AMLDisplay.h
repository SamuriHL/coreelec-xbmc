/*
 *  Copyright (C) 2025 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <atomic>
#include <memory>
#include <optional>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rendering/RenderSystemTypes.h"
#include "threads/CriticalSection.h"
#include "utils/HDRCapabilities.h"
#include "windowing/Resolution.h"
#include "windowing/amlogic/DisplayState.h"

struct drm_fb
{
  uint32_t fb_id;
  uint32_t format;
  uint64_t surfaceEpoch; //!< 5.B2-4: the UI epoch of the surface it was made for
};

//! 5.B2 M5 (SB3 probe, log only): the RmFBs of the GUI surface's fbs at its EGL destroy
namespace SB3PROBE
{
//! special://profile/m5probe_on, read once (default off)
bool On();
struct RmFbResult
{
  int count;
  int64_t firstNs; //!< steady clock
  int64_t firstUs; //!< the first RmFB's duration
  uint32_t firstFb;
};
void ArmRmFb();
RmFbResult TakeRmFb();
} // namespace SB3PROBE

class CAMLGBMUtils
{
public:
  CAMLGBMUtils(int fd);
  struct gbm_device *GetDevice() const { return m_device.get(); }
  struct gbm_surface *GetSurface() const { return m_surface.get(); }
  //! testFailure: outputdrive_commitfail may fail this call (test only); surfaceEpoch:
  //! the UI epoch the surface's fbs carry (5.B2-4)
  bool CreateSurface(int width,
                     int height,
                     uint32_t format,
                     bool testFailure = false,
                     std::optional<uint64_t> surfaceEpoch = std::nullopt);
  uint32_t GetFBId() { return m_drm_fb->fb_id; }
  bool LockFrontBuffer(int fd);
  // real_player: the GUI plane buffers the presentation coordinator commits.
  // Locked and released on the render thread; fb_id stays valid while locked.
  struct gbm_bo* LockFront(int fd, uint32_t& fb_id);
  //! 5.B2-4, with the epoch C returned bo with: released only into its own surface,
  //! and the bo is not read unless that is the current one
  bool Release(struct gbm_bo* bo, std::optional<uint64_t> epoch = std::nullopt);
private:
  struct drm_fb* GetFBFromBo(int fd, struct gbm_bo* bo);

  struct GbmDeviceDeleter
  {
    void operator()(struct gbm_device* d) const noexcept
    {
      if (d)
        gbm_device_destroy(d);
    }
  };

  struct GbmSurfaceDeleter
  {
    void operator()(struct gbm_surface* s) const noexcept
    {
      if (s)
        gbm_surface_destroy(s);
    }
  };

  struct CGBMSurfaceBuffer
  {
    CGBMSurfaceBuffer(struct gbm_surface* surface)
      : m_surface(surface), m_bo(gbm_surface_lock_front_buffer(surface))
    {
    }
    ~CGBMSurfaceBuffer()
    {
      if (m_surface && m_bo)
        gbm_surface_release_buffer(m_surface, m_bo);
    }
    struct gbm_bo* Get() const { return m_bo; }
  private:
    struct gbm_surface* m_surface{nullptr};
    struct gbm_bo* m_bo{nullptr};
  };

  uint32_t m_format;
  uint64_t m_surfaceEpoch{0};
  bool m_epochOn{false}; //!< a surface was made with an epoch (5.B2-4)
  std::unique_ptr<struct gbm_device, GbmDeviceDeleter> m_device{nullptr};
  std::unique_ptr<struct gbm_surface, GbmSurfaceDeleter> m_surface{nullptr};
  std::unique_ptr<CGBMSurfaceBuffer> m_buffer{nullptr};
  struct drm_fb* m_drm_fb{nullptr};
};

class CAMLDRMUtils
{
public:
  CAMLDRMUtils();
  virtual ~CAMLDRMUtils();

  int aml_get_drmDevice_handle() const { return m_fd; }
  uint32_t aml_get_drmDevice_crtc_id() const { return m_crtc ? m_crtc->crtc_id : 0; }
  void aml_init_drmDevice();
  bool aml_drmDevice_vsync();
  std::string aml_get_drmDevice_mode();
  std::string aml_get_drmDevice_modes();
  //! frac_reclock: a content start, which re-clocks a 1000/1001 rate difference at the
  //! live mode string
  bool aml_set_drmDevice_mode(const RESOLUTION_INFO &res, std::string mode,
    const RenderStereoMode stereo_mode, std::string framebuffer_name, bool force_mode_switch,
    bool hotplug_mode_switch, bool frac_reclock = false);
  int aml_get_drmProperty(std::string name,
                          unsigned int obj_type,
                          void* data = nullptr,
                          int* data_len = nullptr);
  void aml_set_drmProperty(std::string name, unsigned int obj_type, unsigned int value);
  void aml_set_drmProperty(std::string name, unsigned int obj_type, std::string value);
  int aml_get_drmDevice_modes_count(drmModeConnection *connection);
  //! 5.B3-1 (log only): the connector's encoder_id, the CRTC's id and its mode name as the last
  //! re-init read them; 0, 0, "" and false without a connector or a CRTC
  void aml_get_drmDevice_link(uint32_t& encoder_id,
                              uint32_t& crtc_id,
                              std::string& mode,
                              bool& mode_valid);
  std::string aml_get_drmDevice_preferred_mode();
  //! frac_reclock: commit a FRAC_RATE_POLICY change at the live mode string; held_notice:
  //! a set kept at the live mode posts its timing
  bool aml_set_drmDevice_active(std::string mode, int fractional_rate,
    const RenderStereoMode stereo_mode, bool active, bool frac_reclock = false,
    bool held_notice = true);
  bool aml_get_drmDevice_connected() const { return m_connection == DRM_MODE_CONNECTED; }
  //! the last mode set kept the live mode (same mode string): no commit, the frac policy unchanged
  bool aml_get_drmDevice_mode_held() const { return m_modeHeld; }
  //! a hotplug set commits even with no display (more than the dummy mode)
  bool aml_get_drmDevice_hotplug_commits();
  //! the last mode set returned without a commit: no display and not a hotplug set
  bool aml_get_drmDevice_set_not_connected() const { return m_setNotConnected; }
  bool aml_output_wire_stale();
  void FlipPage(uint32_t fb_id);
  //! the GUI plane flip without fences; nullptr when there is nothing to flip to
  drmModeAtomicReqPtr BuildFlipRequest(uint32_t fb_id);
  //! 5.B2-4: BuildFlipRequest under m_drmSection, with the source size it sets
  drmModeAtomicReqPtr BuildFlipRequestLocked(uint32_t fb_id, int& src_w, int& src_h);
  //! 5.B2-5b (S1'): X1's GUI plane off (FB, CRTC and both rects 0) under m_drmSection;
  //! nullptr without a plane or CRTC
  drmModeAtomicReqPtr BuildUiPlaneOffRequestLocked();
  // real_player graphics plane (osd1, under the GUI plane)
  bool HasOverlayPlane();
  //! the graphics plane showing fb_id (a src_w x src_h buffer) over the whole
  //! screen, under the GUI plane (GUI flips keep that order until it is
  //! switched off)
  drmModeAtomicReqPtr BuildOverlayRequest(uint32_t fb_id, int src_w, int src_h);
  //! the graphics plane off; a commit that only disables a plane is refused,
  //! so it carries the GUI plane's current fb (0: none)
  drmModeAtomicReqPtr BuildOverlayOffRequest(uint32_t primary_fb_id);

  void SetInFenceFd(int fd) { if (m_inFenceFd != -1) close(m_inFenceFd); m_inFenceFd = fd; }
  int TakeOutFenceFd()
  {
    int fd{-1};
    return std::exchange(m_outFenceFd, fd);
  }
private:
  void CleanAndClose();
  void aml_init_drmDevice_display();
  void aml_set_framebuffer_resolution(unsigned int width,
                                      unsigned int height,
                                      std::string framebuffer_name);
  int aml_get_drmDevice();
  int get_drmProp(unsigned int id,
                  std::string name,
                  unsigned int obj_type,
                  void* data = nullptr,
                  int* data_len = nullptr);
  //! commit_frac: the re-train also takes fractional_rate; false when nothing was committed
  bool apply_dv_wire_format(bool commit_frac = false, int fractional_rate = 0);
  bool leaving_dv_wire() const;
  //! tells the timekeeper the timing a mode set gives the display
  static void PostModeNotice(const drmModeModeInfo& mode, bool fractional);
  void set_drmProp(unsigned int id, std::string name,
    unsigned int obj_type, unsigned int value, drmModeAtomicReqPtr req);
  bool SupportsFormat(drmModePlane *plane, uint32_t format);
  void LogUiPlaneBeforeCommit();
  int m_fd{-1};
  // The link may carry a DV tunnel format: TV-led (RGB/444 8-bit) or
  // player-led/LLDV (YUV422 12-bit), until it is handed back to the kernel on
  // the way out - see leaving_dv_wire().
  bool m_wireDvTunnel{false};
  bool m_modeHeld{false};
  bool m_setNotConnected{false};
  bool m_fracCommitted{false}; //!< the last aml_set_drmDevice_active re-clocked the live mode
  int m_width;
  int m_height;
  int m_ScreenWidth;
  int m_ScreenHeight;
  CCriticalSection m_drmSection;

  drmModeResPtr m_resources{nullptr};
  drmModeConnectorPtr m_connector{nullptr};
  drmModeConnection m_connection{DRM_MODE_DISCONNECTED};
  drmModeEncoderPtr m_encoder{nullptr};
  drmModeCrtcPtr m_crtc{nullptr};
  drmModeCrtcPtr m_orig_crtc{nullptr};
  drmModePlanePtr m_plane{nullptr};
  drmModePlanePtr m_overlayPlane{nullptr};
  std::atomic<bool> m_overlayActive{false};

  int m_inFenceFd{-1};
  int m_outFenceFd{-1};
};

class CAMLDisplay
{
public:
  CAMLDisplay();

  void aml_refresh_display_caps();
  int aml_get_Device_handle() const { return m_amlDRMUtils->aml_get_drmDevice_handle(); }
  uint32_t aml_get_Device_crtc_id() const { return m_amlDRMUtils->aml_get_drmDevice_crtc_id(); }
  void aml_init_drmDevice() { m_amlDRMUtils->aml_init_drmDevice(); }
  bool aml_drmDevice_vsync() { return m_amlDRMUtils->aml_drmDevice_vsync(); };
  bool aml_get_display_connected() const { return m_amlDRMUtils->aml_get_drmDevice_connected(); }
  bool aml_set_not_connected() const
  {
    return m_amlDRMUtils->aml_get_drmDevice_set_not_connected();
  }
  bool aml_hotplug_set_commits() { return m_amlDRMUtils->aml_get_drmDevice_hotplug_commits(); }
  //! 5.V1-2: the refresh the display runs after a successful set of res; held: the set kept the
  //! live mode, whose frac policy may differ from res's
  float aml_committed_refresh(const RESOLUTION_INFO& res, bool& held) const;
  bool set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
    const RenderStereoMode stereo_mode, bool force_mode_switch, bool hotplug_mode_switch,
    bool frac_reclock = false);
  void handle_display_stereo_mode(const RenderStereoMode stereo_mode);
  bool set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
    bool force_mode_switch, bool hotplug_mode_switch, bool frac_reclock = false);
  int aml_get_display_modes_count(drmModeConnection *connection) const
    { return m_amlDRMUtils->aml_get_drmDevice_modes_count(connection); }
  void aml_get_display_link(uint32_t& encoder_id,
                            uint32_t& crtc_id,
                            std::string& mode,
                            bool& mode_valid) const
  {
    m_amlDRMUtils->aml_get_drmDevice_link(encoder_id, crtc_id, mode, mode_valid);
  }
  std::string aml_get_preferred_mode();
  //! nativeGui: DISABLEGUISCALING as the caller read it (5.B2-5a's W, which reads no CSettings)
  bool aml_get_native_resolution(RESOLUTION_INFO* res,
                                 std::optional<bool> nativeGui = std::nullopt);
  bool aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions);
  int aml_get_drmProperty(std::string name,
                          unsigned int obj_type,
                          void* data = nullptr,
                          int* data_len = nullptr) const
  {
    return m_amlDRMUtils->aml_get_drmProperty(name, obj_type, data, data_len);
  }
  void aml_set_drmProperty(std::string name, unsigned int obj_type, unsigned int value)
    { m_amlDRMUtils->aml_set_drmProperty(name, obj_type, value); }
  void aml_set_drmProperty(std::string name, unsigned int obj_type, std::string value)
    { m_amlDRMUtils->aml_set_drmProperty(name, obj_type, value); }
  void FlipPage(uint32_t fb_id) { m_amlDRMUtils->FlipPage(fb_id); }
  drmModeAtomicReqPtr BuildFlipRequest(uint32_t fb_id)
  {
    return m_amlDRMUtils->BuildFlipRequest(fb_id);
  }
  drmModeAtomicReqPtr BuildFlipRequestLocked(uint32_t fb_id, int& src_w, int& src_h)
  {
    return m_amlDRMUtils->BuildFlipRequestLocked(fb_id, src_w, src_h);
  }
  drmModeAtomicReqPtr BuildUiPlaneOffRequestLocked()
  {
    return m_amlDRMUtils->BuildUiPlaneOffRequestLocked();
  }
  bool HasOverlayPlane() { return m_amlDRMUtils->HasOverlayPlane(); }
  drmModeAtomicReqPtr BuildOverlayRequest(uint32_t fb_id, int src_w, int src_h)
  {
    return m_amlDRMUtils->BuildOverlayRequest(fb_id, src_w, src_h);
  }
  drmModeAtomicReqPtr BuildOverlayOffRequest(uint32_t primary_fb_id)
  {
    return m_amlDRMUtils->BuildOverlayOffRequest(primary_fb_id);
  }
  bool aml_output_wire_stale() { return m_amlDRMUtils->aml_output_wire_stale(); }
  bool aml_set_drmDevice_active(bool active) const
    { return m_amlDRMUtils->aml_set_drmDevice_active(
      m_amlDRMUtils->aml_get_drmDevice_mode(),
      m_amlDRMUtils->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR),
      m_stereo_mode, active); }
  void SetInFenceFd(int fd) { m_amlDRMUtils->SetInFenceFd(fd); }
  int TakeOutFenceFd() const { return m_amlDRMUtils->TakeOutFenceFd(); }
  bool GetHotPlug() { bool ret = m_bHotPlug; m_bHotPlug = false; return ret; }
  //! 5.B2-5a: GetHotPlug without the clear
  bool PeekHotPlug() const { return m_bHotPlug; }
  void SetHotPlug() { m_bHotPlug = true; }

  CHDRCapabilities GetHDRCaps() const
  {
    const CHDRCapabilities caps = m_hdr_caps;
    DISPLAYSTATE::ShadowHdrCaps(caps);
    return caps;
  }
  bool aml_display_support_dv() const
  {
    const bool dv = m_hdr_caps.SupportsDolbyVision() != DolbyVisionFormat::DOLBYVISION_TYPE_NONE;
    DISPLAYSTATE::ShadowDv(dv);
    return dv;
  }
  bool aml_display_support_3d() const
  {
    const bool support = m_support_3d;
    DISPLAYSTATE::Shadow3d(support);
    return support;
  }
private:
  std::unique_ptr<CAMLDRMUtils> m_amlDRMUtils;
  bool aml_mode_to_resolution(const char* mode,
                              RESOLUTION_INFO* res,
                              std::optional<bool> nativeGuiIn = std::nullopt);
  bool m_bHotPlug = false;
  RenderStereoMode m_stereo_mode;
  CHDRCapabilities m_hdr_caps;
  bool m_is_widescreen = false;
  bool m_support_3d = false;
};
