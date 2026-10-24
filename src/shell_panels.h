#pragma once
// ── xshell panel builders (Phase-2 relocation out of main.cc) ────────────────
// Home of the ShellContext cross-section state (carved out of the enormous
// main()) and the per-panel buildXPanel(ctx) builders that populate it. main.cc
// keeps reference-aliases with the original local names so the far-apart card
// assembly, broadcast dispatcher, and refresh timer are untouched.
//
// NOTE: this is an internal single-application header — it deliberately mirrors
// main.cc's `using namespace` convention so the ShellContext members and the
// builder bodies can name widgets/IPC types unqualified.

#include "xkit/widget.h"
#include "xkit/clipboard.h"
#include "xkit/xbus_client.h"
#include "xshell/app_discovery.h"
#include "shared/protocol.h"
#include "shell_helpers.h"
#include <functional>
#include <memory>
#include <array>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <ctime>
#include <utility>
#include <cstdint>
#include <iterator>
#include <set>

using namespace fbx::xkit;
using namespace fbx::xshell;
using namespace fbx::shared;

// Process-global XBus client (defined in main.cc). Panel builders call through it.
extern XBusClient* g_shell_xbus;

// Named clipboard slot count — shared by ShellContext (cs_preview_lbls) and the
// clipboard-slot persistence in main.cc.
inline constexpr int kNumClipSlots = 5;

// ── Promoted file-scope state (Phase-2 globals-promotion) ────────────────────
// These were `static` in main.cc; promoted to external linkage so the relocated
// panel builders in shell_panels.cc can reference them. Definitions remain in
// main.cc (variables + helper bodies); trivial value-types live here so both TUs
// share one definition.
struct KbLayout { const char* code; const char* label; };
inline constexpr KbLayout kLayouts[] = {
    {"us", "US"}, {"fr", "FR"}, {"de", "DE"}, {"es", "ES"}, {"jp", "JP"}
};
inline constexpr int kNumLayouts = static_cast<int>(std::size(kLayouts));
inline constexpr int kPomoWork = 25 * 60;
struct CountdownTimer {
    std::string  label;
    std::time_t  end_at{0};
    bool         active{false};
};
struct PomoLogEntry {
    std::time_t ts;        // Unix timestamp of completion
    int         phase;     // 0=work, 1=short break, 2=long break
    int         duration;  // seconds
};

extern bool g_dnd;
extern uint32_t g_accent_idx;
extern std::vector<std::pair<std::string,std::string>> g_recent_files;
extern std::array<std::string, kNumClipSlots> g_clip_slots;
extern std::atomic<uint32_t> g_idle_ticks;
extern std::atomic<uint32_t> g_idle_lock_secs;
extern int g_current_ws;
extern std::string g_wifi_ssid;
extern bool g_dnd_sched_on;
extern int  g_dnd_sched_start;
extern int  g_dnd_sched_end;
extern bool g_ns_sched_on;
extern int  g_ns_sched_start;
extern int  g_ns_sched_end;
extern bool g_focus_active;
extern std::time_t g_focus_start;
extern int g_theme_preset;
extern bool g_wp_rotate_on;
extern int  g_wp_rotate_interval;
extern int  g_wp_rotate_countdown;
extern CountdownTimer g_countdown;
extern std::mutex g_countdown_mtx;
extern bool g_bat_saver;
extern uint32_t g_bat_saver_prev_lock;
extern int  g_keymap_idx;
extern std::array<uint32_t, 24> g_app_hourly;
extern bool g_session_enabled;
extern std::vector<std::string> g_session_execs;
extern std::string g_scratch_note;
extern std::vector<PomoLogEntry> g_pomo_log;
extern std::mutex g_pomo_log_mtx;
extern std::atomic<int>  g_pomo_phase;
extern std::atomic<int>  g_pomo_secs;
extern std::atomic<bool> g_pomo_running;
extern std::atomic<int>  g_pomo_cycles;

void saveClipSlots() noexcept;
void saveDndSched() noexcept;
void saveNsSched() noexcept;
void saveThemePreset() noexcept;
void saveWpRotate() noexcept;
void saveKeymap() noexcept;
void saveAppHeatmap() noexcept;
void saveSession() noexcept;
void saveScratchNote() noexcept;
void savePomoLog() noexcept;
std::string signalBars(int32_t dbm) noexcept;
void launchApp(const std::string& exec_path, const std::string& file_path = "") noexcept;

// Shared cross-section state carved out of the enormous main() so panel builders
// (below) can populate it and the far-apart card assembly, broadcast dispatcher,
// and refresh timer reference it. Grows one panel cluster at a time. main() keeps
// reference-aliases with the original local names, so downstream code — including
// the big broadcast/timer capture lists — is untouched by each extraction.
struct ShellContext {
    // Game Mode
    std::shared_ptr<TextWidget>   gm_status_lbl;
    std::shared_ptr<TextWidget>   gm_app_lbl;
    std::shared_ptr<ButtonWidget> gm_enable_btn;
    std::shared_ptr<ButtonWidget> gm_disable_btn;
    std::shared_ptr<FlexRow>      gm_btn_row;
    std::function<void(const GameModeState&)> update_gm_ui;

    // Camera & Microphone privacy
    std::shared_ptr<TextWidget>   cam_status_lbl, mic_status_lbl;
    std::shared_ptr<ButtonWidget> cam_block_btn, cam_allow_btn, mic_block_btn, mic_allow_btn;
    std::shared_ptr<FlexRow>      cam_row, mic_row;
    std::function<void(const PrivacyStatus&)> update_privacy_ui;

    // Wake Lock
    std::shared_ptr<bool>         wl_active;
    std::shared_ptr<uint32_t>     wl_dur_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> wl_dur_btns;
    std::shared_ptr<SwitchWidget> wl_switch;
    std::shared_ptr<TextWidget>   wl_status_lbl;
    std::function<void(const WakeLockState&)> update_wl_ui;

    // Remote Login (SSH daemon)
    std::shared_ptr<SwitchWidget> rl_switch;
    std::shared_ptr<TextWidget>   rl_info_lbl;
    std::function<void(const RemoteLoginState&)> update_rl_ui;

    // Language / Locale
    std::shared_ptr<std::string>  locale_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> locale_btns;
    std::shared_ptr<TextWidget>   locale_sub_lbl;
    std::function<void(const LocaleState&)> update_locale_ui;

    // Accessibility (Reader / Contrast / Magnify quick toggles)
    std::shared_ptr<bool>         a11y_reader_on, a11y_contrast_on, a11y_magnify_on;
    std::shared_ptr<ButtonWidget> a11y_reader_btn, a11y_contrast_btn, a11y_magnify_btn;
    std::function<void(const A11yState&)> update_a11y_ui_b;

    // Proxy Settings
    std::shared_ptr<uint32_t>     proxy_mode_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> proxy_mode_btns;
    std::shared_ptr<TextWidget>   proxy_info_lbl;
    std::function<void(const ProxyConfig&)> update_proxy_ui;

    // DNS Configuration
    std::shared_ptr<uint32_t>     dns_mode_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> dns_mode_btns;
    std::shared_ptr<TextWidget>   dns_resolver_lbl;
    std::function<void(const DnsConfig&)> update_dns_ui;

    // Update Channel
    std::shared_ptr<uint32_t>     upd_ch_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 2>> upd_ch_btns;
    std::shared_ptr<TextWidget>   upd_ch_badge_lbl;
    std::shared_ptr<FlexRow>      upd_ch_row;
    std::function<void(const UpdateChannelState&)> update_upd_ch_ui;

    // Time Zone
    std::shared_ptr<int>          tz_idx;
    std::shared_ptr<TextWidget>   tz_name_lbl, tz_disp_lbl, tz_time_lbl;
    std::shared_ptr<ButtonWidget> tz_prev_btn, tz_next_btn;
    std::function<void(const TimezoneState&)> update_tz_ui;

    // Package Signing
    std::shared_ptr<SwitchWidget> pkgsign_req_switch, pkgsign_unk_switch;
    std::shared_ptr<PkgSigningPolicy> pkgsign_cur;
    std::function<void(const PkgSigningPolicy&)> update_pkgsign_ui;

    // Battery Health
    std::shared_ptr<SwitchWidget> bhealth_opt_sw;
    std::shared_ptr<TextWidget>   bhealth_status_lbl, bhealth_info_lbl;
    std::function<void(const BatteryHealthState&)> update_bhealth_ui;

    // Screen Lock
    std::shared_ptr<SwitchWidget> slk_enabled_sw, slk_require_sw, slk_sleep_sw;
    std::shared_ptr<int>          slk_timeout_cur;
    std::shared_ptr<TextWidget>   slk_status_lbl;
    std::shared_ptr<std::vector<std::shared_ptr<ButtonWidget>>> slk_timeout_btns;
    std::shared_ptr<ButtonWidget> slk_lock_btn;
    std::function<void()>         slk_hl;
    std::function<void(const ScreenLockPolicy&)> update_slk_ui;

    // Notif Schedule
    std::shared_ptr<SwitchWidget>    nsch_enabled_sw, nsch_calls_sw, nsch_alarms_sw, nsch_weekend_sw;
    std::shared_ptr<TextInputWidget> nsch_start_h_input, nsch_start_m_input,
                                     nsch_end_h_input, nsch_end_m_input;
    std::shared_ptr<TextWidget>      nsch_status_lbl;
    std::shared_ptr<ButtonWidget>    nsch_apply_btn;
    std::function<void(const NotifSchedule&)> update_nsch_ui;

    // Auto-lock toggle
    std::shared_ptr<SwitchWidget> qs_autolock_switch;
    std::shared_ptr<FlexRow>      qs_autolock_row;

    // Battery Saver toggle
    std::shared_ptr<SwitchWidget> bat_saver_switch;
    std::shared_ptr<TextWidget>   bat_saver_lbl;
    std::shared_ptr<FlexRow>      bat_saver_row;

    // Keyboard Backlight
    std::shared_ptr<SwitchWidget> kb_bl_switch;
    std::shared_ptr<TextWidget>   kb_bl_val;
    std::shared_ptr<SliderWidget> kb_bl_slider;
    std::shared_ptr<FlexRow>      kb_bl_row;
    std::function<void(const KeyboardBacklightState&)> update_kb_bl_ui;

    // Presentation Mode + Power Profile switcher
    std::shared_ptr<SwitchWidget> pres_switch;
    std::shared_ptr<TextWidget>   pres_status_lbl;
    std::function<void(const PresentationState&)> update_pres_ui;
    std::shared_ptr<TextWidget>   pp_status_lbl;
    std::shared_ptr<FlexRow>      pp_row1, pp_row2;
    std::function<void(const PowerProfileState&)> update_pp_ui;

    // Night Shift slider
    std::shared_ptr<SwitchWidget> qs_ns_switch;
    std::shared_ptr<TextWidget>   qs_ns_val;
    std::shared_ptr<SliderWidget> qs_ns_slider;
    std::shared_ptr<FlexRow>      qs_ns_hdr_row, qs_ns_slider_row;
    std::function<void()>         send_ns_config;

    // Screen Saver
    std::shared_ptr<bool>         ss_on;
    std::shared_ptr<SwitchWidget> ss_switch;
    std::shared_ptr<uint32_t>     ss_timeout_cur, ss_mode_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> ss_timeout_btns;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> ss_mode_btns;
    std::function<void(uint32_t, uint32_t)> update_ss_ui;

    // Dock Layout + Dev Overlay presets
    std::shared_ptr<uint32_t>     dock_pos_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> dock_pos_btns;
    std::shared_ptr<SwitchWidget> dock_autohide;
    std::function<void(const DockSettings&)> update_dock_ui;
    std::shared_ptr<uint32_t>     dev_overlay_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> dev_overlay_btns;
    std::shared_ptr<FlexRow>      dev_overlay_row;
    std::function<void(const DevOverlayState&)> update_dev_overlay_ui;

    // XWayland
    std::shared_ptr<bool>         xwl_running;
    std::shared_ptr<SwitchWidget> xwl_switch;
    std::shared_ptr<TextWidget>   xwl_info_lbl;
    std::function<void(const XWaylandStatus&)> update_xwl_ui;

    // Session restore
    std::shared_ptr<std::vector<std::string>> live_titles;
    std::shared_ptr<TextWidget>   sess_hdr_lbl, sess_count_lbl;
    std::shared_ptr<SwitchWidget> sess_switch;
    std::shared_ptr<FlexRow>      sess_sw_row, sess_save_row;

    // Display Settings overlay
    std::shared_ptr<FlexColumn>       disp_col;
    std::shared_ptr<ContainerWidget>  disp_overlay;
    std::shared_ptr<ButtonWidget>     disp_btn;

    // Pomodoro Focus Timer + Wallpaper rotation controls
    std::shared_ptr<TextWidget>   pomo_phase_lbl, pomo_cd_lbl, pomo_tbar_w, pomo_start_lbl;
    std::shared_ptr<FlexRow>      pomo_hdr_row, pomo_btn_row;
    std::shared_ptr<SwitchWidget> wp_rotate_switch;
    std::shared_ptr<TextWidget>   wp_folder_lbl, wp_interval_lbl;
    std::shared_ptr<ButtonWidget> wp_interval_dec, wp_interval_inc;
    std::function<void()>         update_wp_interval_lbl;

    // Focus Mode
    std::shared_ptr<TextWidget>   focus_status_lbl;
    std::shared_ptr<ButtonWidget> focus_end_btn;
    std::shared_ptr<FlexRow>      focus_dur_row, focus_cat_row;
    std::function<void(const FocusModeState&)> update_focus_ui;

    // Keyboard layout switcher
    std::shared_ptr<TextWidget>   kb_active_lbl;
    std::shared_ptr<FlexRow>      kb_layout_row;

    // Updates button (taskbar)
    std::shared_ptr<ButtonWidget> updates_btn;
    std::function<void(uint32_t)> update_upd_btn;

    // Battery indicator (taskbar)
    std::shared_ptr<ButtonWidget> bat_btn;
    std::shared_ptr<std::function<void(const PowerState&)>> update_battery;

    // Notification badge button (taskbar)
    std::shared_ptr<int>              notif_unread;
    std::shared_ptr<TextWidget>       notif_badge_lbl;
    std::shared_ptr<ContainerWidget>  notif_badge_box;
    std::shared_ptr<ButtonWidget>     notif_tbar_btn;

    // OSD overlay (volume / brightness)
    std::shared_ptr<ContainerWidget>  osd_overlay;
    std::shared_ptr<std::function<void(const std::string&, int)>> show_osd;

    // System tray icon row
    std::shared_ptr<FlexRow>          tray_row;
    std::shared_ptr<std::function<void(uint32_t)>> load_tray;

    // Idle auto-lock cycle selector (action center)
    std::shared_ptr<ButtonWidget>     idle_lock_btn;

    // Contact quick-search (action center)
    std::shared_ptr<TextWidget>       contact_hdr_lbl;
    std::shared_ptr<TextInputWidget>  contact_search_input;
    std::shared_ptr<FlexColumn>       contact_results_col;
    std::shared_ptr<std::function<void(const std::string&)>> do_contact_search;

    // Quick Notes mini-grid (action center)
    std::shared_ptr<FlexColumn>       notes_mini_col;
    std::shared_ptr<FlexRow>          notes_mini_hdr_row;
    std::shared_ptr<std::function<void(const std::vector<NoteEntry>&)>> rebuild_notes_mini;

    // Clipboard history mini-panel (action center)
    std::shared_ptr<FlexRow>          clip_ac_hdr_row;
    std::shared_ptr<FlexColumn>       clip_ac_col;
    std::shared_ptr<std::function<void()>> rebuild_clip_ac;

    // Startup apps section (action center)
    std::shared_ptr<TextWidget>       startup_apps_hdr_lbl;
    std::shared_ptr<FlexColumn>       startup_apps_col;
    std::shared_ptr<std::function<void()>> rebuild_startup_apps;

    // Scratch pad section (action center)
    std::shared_ptr<TextInputWidget>  scratch_input;
    std::shared_ptr<FlexRow>          scratch_hdr_row;

    // System resource sparklines section (action center)
    std::shared_ptr<TextWidget>       spark_hdr_lbl;
    std::shared_ptr<TextWidget>       spark_cpu_lbl;
    std::shared_ptr<TextWidget>       spark_ram_lbl;

    // License & Support section (action center)
    std::shared_ptr<TextWidget>       lic_hdr_lbl;
    std::shared_ptr<TextWidget>       lic_key_lbl;
    std::shared_ptr<TextWidget>       lic_tier_lbl;
    std::shared_ptr<TextWidget>       lic_expiry_lbl;
    std::shared_ptr<TextWidget>       lic_seats_lbl;
    std::shared_ptr<TextWidget>       lic_support_lbl;
    std::shared_ptr<ButtonWidget>     lic_copy_btn;
    std::function<void(const LicenseState&)> update_lic_ui;

    // Memory Pressure monitor (action center)
    std::shared_ptr<TextWidget>       mem_hdr_lbl;
    std::shared_ptr<TextWidget>       mem_level_lbl;
    std::shared_ptr<TextWidget>       mem_ram_lbl;
    std::shared_ptr<TextWidget>       mem_swap_lbl;
    std::shared_ptr<TextWidget>       mem_faults_lbl;
    std::shared_ptr<FlexRow>          mem_bar_row;
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 10>> mem_bar_blocks;
    std::function<void(const MemPressureState&)> update_mem_ui;

    // Frame Timing monitor (action center)
    std::shared_ptr<TextWidget>       ft_hdr_lbl;
    std::shared_ptr<TextWidget>       ft_avg_lbl;
    std::shared_ptr<TextWidget>       ft_p99_lbl;
    std::shared_ptr<TextWidget>       ft_jank_lbl;
    std::shared_ptr<TextWidget>       ft_gpu_lbl;
    std::shared_ptr<TextWidget>       ft_vsync_lbl;
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 16>> ft_bars;
    std::shared_ptr<FlexRow>          ft_bar_row;
    std::function<void(const FramePerfState&)> update_ft_ui;

    // System Information monitor (action center)
    std::shared_ptr<TextWidget>       si_hdr_lbl;
    std::shared_ptr<FlexColumn>       si_col;
    std::shared_ptr<TextWidget>       si_hostname_lbl;
    std::shared_ptr<TextWidget>       si_os_lbl;
    std::shared_ptr<TextWidget>       si_cpu_lbl;
    std::shared_ptr<TextWidget>       si_cores_lbl;
    std::shared_ptr<TextWidget>       si_ram_lbl;
    std::shared_ptr<TextWidget>       si_gpu_lbl;
    std::shared_ptr<TextWidget>       si_uptime_lbl;
    std::shared_ptr<TextWidget>       si_serial_lbl;
    std::function<void(const SysInfoState&)> update_sysinfo_ui;

    // GPU Statistics monitor (action center) — only the updater escapes; the
    // labels stay local to the builder (kept alive by the closure's captures).
    std::function<void(const GpuStats&)> update_gpu_ui_b;

    // GPU Monitor (quick-settings card)
    std::shared_ptr<TextWidget>       gpu_name_lbl;
    std::shared_ptr<TextWidget>       gpu_util_lbl;
    std::shared_ptr<TextWidget>       gpu_vram_lbl;
    std::shared_ptr<TextWidget>       gpu_temp_lbl;
    std::shared_ptr<TextWidget>       gpu_fan_lbl;
    std::function<void(const GpuStatsDetail&)> update_gpu_ui;

    // Night Light (compositor colour-temperature control)
    std::shared_ptr<SwitchWidget>     nl_switch;
    std::shared_ptr<TextWidget>       nl_temp_val;
    std::shared_ptr<SliderWidget>     nl_slider;
    std::shared_ptr<SwitchWidget>     nl_sched_switch;
    std::shared_ptr<TextInputWidget>  nl_from_input;
    std::shared_ptr<TextInputWidget>  nl_to_input;
    std::shared_ptr<FlexRow>          nl_hdr_row;
    std::shared_ptr<FlexRow>          nl_slider_row;
    std::shared_ptr<FlexRow>          nl_sched_hdr;
    std::shared_ptr<FlexRow>          nl_sched_times;
    std::function<void(const NightLightState&)> update_nl_ui;

    // Gesture Config (quick-settings card)
    std::shared_ptr<GestureConfig>    gest_cfg;
    std::shared_ptr<SwitchWidget>     gest_switch;
    std::shared_ptr<SliderWidget>     gest_swipe3_sl;
    std::shared_ptr<SliderWidget>     gest_pinch_sl;
    std::shared_ptr<SwitchWidget>     gest_tap_sw;
    std::shared_ptr<SwitchWidget>     gest_nscroll_sw;
    std::shared_ptr<SwitchWidget>     gest_2fscroll_sw;
    std::function<void(const GestureConfig&)> update_gest_ui;

    // Stylus input (quick-settings card)
    std::shared_ptr<SwitchWidget>     stylus_palm_switch;
    std::shared_ptr<bool>             stylus_connected;
    std::shared_ptr<uint32_t>         stylus_pressure_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> stylus_pressure_btns;
    std::shared_ptr<TextWidget>       stylus_conn_lbl;
    std::shared_ptr<FlexRow>          stylus_pressure_row;
    std::function<void(const StylusState&)> update_stylus_ui;

    // Color Profile presets (quick-settings card)
    std::shared_ptr<uint32_t>         color_prof_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> color_prof_btns;
    std::shared_ptr<FlexRow>          color_prof_row;
    std::function<void(const ColorProfileState&)> update_color_prof_ui;

    // Cursor Size presets (quick-settings card)
    std::shared_ptr<uint32_t>         cursor_size_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> cursor_size_btns;
    std::shared_ptr<FlexRow>          cursor_size_row;
    std::function<void(const CursorSizeState&)> update_cursor_size_ui;

    // Display Scaling presets (quick-settings card)
    std::shared_ptr<uint32_t>         disp_scale_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> disp_scale_btns;
    std::shared_ptr<FlexRow>          disp_scale_row;
    std::function<void(const DisplayScaleState&)> update_disp_scale_ui;

    // Display Refresh Rate presets (quick-settings card)
    std::shared_ptr<uint32_t>         rrate_cur;
    std::shared_ptr<uint32_t>         rrate_mask;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> rrate_btns;
    std::shared_ptr<FlexRow>          rrate_row;
    std::function<void(const RefreshRateState&)> update_rrate_ui;

    // Text Size presets (quick-settings card) — text_size_row stays a main()
    // local (assembled far from the creation block) referencing text_size_btns.
    std::shared_ptr<uint32_t>         text_size_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> text_size_btns;
    std::function<void(const TextSizeState&)> update_text_size_ui;

    // Auto-Brightness toggle (quick-settings card)
    std::shared_ptr<SwitchWidget>     auto_br_switch;
    std::shared_ptr<TextWidget>       auto_br_lux_lbl;
    std::shared_ptr<FlexRow>          auto_br_hdr_row;
    std::function<void(const AutoBrightnessState&)> update_auto_br_ui;

    // Screen Timeout quick-selector (quick-settings card)
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> stout_btns;
    std::shared_ptr<uint32_t>         stout_cur;
    std::shared_ptr<FlexRow>          stout_row;
    std::function<void(const ScreenTimeoutState&)> update_stout_ui;

    // Screen Rotation Lock (quick-settings card)
    std::shared_ptr<SwitchWidget>     rot_lock_switch;
    std::shared_ptr<TextWidget>       rot_orient_lbl;
    std::function<void(const RotationLockState&)> update_rot_ui;

    // Microphone Mute (quick-settings card)
    std::shared_ptr<SwitchWidget>     mic_mute_switch;
    std::shared_ptr<TextWidget>       mic_mute_status;
    std::function<void(const MicMuteState&)> update_mic_mute_ui;

    // Admin Unlock (shown only when the screen is locked)
    std::shared_ptr<bool>             unlock_locked;
    std::shared_ptr<FlexRow>          unlock_inputs_row;
    std::shared_ptr<TextWidget>       unlock_status_lbl;

    // Thermal Zones (paginated Fetcher list) — the TzFetcher machinery stays
    // builder-local; these four handles hit the card/broadcast/timer sites.
    std::shared_ptr<TextWidget>       tz_count_lbl;
    std::shared_ptr<FlexColumn>       tz_col;
    std::shared_ptr<TextWidget>       tz_status_lbl;
    std::shared_ptr<std::function<void()>> rebuild_tz;

    // Location Services app list (paginated Fetcher) — a latent panel; only the
    // updater + rebuild pointer escape (widgets stay builder-local).
    std::function<void(const LocationState&)> update_loc_policy;
    std::shared_ptr<std::function<void()>> rebuild_loc;

    // Camera Devices (paginated Fetcher list) — CamFetcher machinery stays local.
    std::shared_ptr<TextWidget>       cam_dev_count_lbl;
    std::shared_ptr<FlexColumn>       cam_dev_col;
    std::shared_ptr<std::function<void()>> rebuild_cam_dev;

    // Multi-Seat (paginated Fetcher list) — SeatFetcher machinery stays local.
    std::shared_ptr<TextWidget>       seat_count_lbl;
    std::shared_ptr<FlexColumn>       seat_col;
    std::shared_ptr<std::function<void()>> rebuild_seats;

    // Crash Reporter (paginated Fetcher list) — CrashFetcher machinery stays local.
    std::shared_ptr<TextWidget>       crash_hdr_lbl;
    std::shared_ptr<FlexColumn>       crash_col;
    std::shared_ptr<ButtonWidget>     crash_clear_btn;
    std::shared_ptr<std::function<void()>> rebuild_crash;

    // Security Events (paginated Fetcher list) — SecFetcher + the refresh/clear
    // buttons stay builder-local (kept alive by sec_btn_row).
    std::shared_ptr<TextWidget>       sec_hdr_lbl;
    std::shared_ptr<FlexColumn>       sec_col;
    std::shared_ptr<FlexRow>          sec_btn_row;
    std::shared_ptr<std::function<void()>> rebuild_sec;

    // Cron Jobs (paginated Fetcher list) — add_entry closure stays local.
    std::shared_ptr<TextWidget>       cron_hdr_lbl;
    std::shared_ptr<FlexColumn>       cron_col;
    std::shared_ptr<ButtonWidget>     cron_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_cron;

    // Mounted Volumes (paginated Fetcher list) — inline fetch closure stays local.
    std::shared_ptr<TextWidget>       vol_hdr_lbl;
    std::shared_ptr<FlexColumn>       vol_col;
    std::shared_ptr<std::function<void()>> rebuild_vol;

    // Startup Items (paginated Fetcher list) — per-row builder stays in fetch closure.
    std::shared_ptr<TextWidget>       startup_hdr_lbl;
    std::shared_ptr<FlexColumn>       startup_col;
    std::shared_ptr<ButtonWidget>     startup_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_startup;

    // Open Network Connections (paginated Fetcher list) — inline fetch closure stays local.
    std::shared_ptr<TextWidget>       netconn_hdr_lbl;
    std::shared_ptr<FlexColumn>       netconn_col;
    std::shared_ptr<ButtonWidget>     netconn_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_netconn;

    // SSH Keys (paginated Fetcher list) — per-key row builder stays in fetch closure.
    std::shared_ptr<TextWidget>       sshkey_hdr_lbl;
    std::shared_ptr<TextWidget>       sshkey_count_lbl;
    std::shared_ptr<TextWidget>       sshkey_known_lbl;
    std::shared_ptr<FlexColumn>       sshkey_col;
    std::shared_ptr<std::function<void()>> rebuild_sshkey;

    // Environment Variables (paginated Fetcher list) — the Set-Var form inputs +
    // button stay builder-local, held alive by the escaping envvar_form_row.
    std::shared_ptr<TextWidget>       envvar_hdr_lbl;
    std::shared_ptr<TextWidget>       envvar_count_lbl;
    std::shared_ptr<FlexColumn>       envvar_col;
    std::shared_ptr<FlexRow>          envvar_form_row;
    std::shared_ptr<std::function<void()>> rebuild_envvar;

    // System Logs (paginated Fetcher list) — the level filter, 3 filter buttons +
    // highlight helper stay builder-local, held alive by the escaping filter_row.
    std::shared_ptr<TextWidget>       syslog_hdr_lbl;
    std::shared_ptr<FlexColumn>       syslog_col;
    std::shared_ptr<FlexRow>          syslog_filter_row;
    std::shared_ptr<ButtonWidget>     syslog_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_syslog;

    // USB Devices (paginated Fetcher list) — inline fetch closure stays local.
    std::shared_ptr<TextWidget>       usb_hdr_lbl;
    std::shared_ptr<TextWidget>       usb_count_lbl;
    std::shared_ptr<FlexColumn>       usb_col;
    std::shared_ptr<ButtonWidget>     usb_rescan_btn;
    std::shared_ptr<std::function<void()>> rebuild_usb;

    // Login Sessions (paginated Fetcher list) — per-session row builder stays local.
    std::shared_ptr<TextWidget>       lsess_hdr_lbl;
    std::shared_ptr<FlexColumn>       lsess_col;
    std::shared_ptr<ButtonWidget>     lsess_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_lsess;

    // Battery Drain (paginated Fetcher list) — add_entry row builder stays local.
    std::shared_ptr<TextWidget>       bdrain_hdr_lbl;
    std::shared_ptr<FlexColumn>       bdrain_col;
    std::shared_ptr<ButtonWidget>     bdrain_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_bdrain;

    // File Sync (paginated Fetcher list) — local FsFetcher struct stays in rebuild.
    std::shared_ptr<TextWidget>       fs_hdr_lbl;
    std::shared_ptr<TextWidget>       fs_count_lbl;
    std::shared_ptr<FlexColumn>       fs_col;
    std::shared_ptr<std::function<void()>> rebuild_file_sync;

    // Audio Routing (paginated Fetcher list) — local AudioRouteFetcher struct stays.
    std::shared_ptr<TextWidget>       ar_hdr_lbl;
    std::shared_ptr<TextWidget>       ar_count_lbl;
    std::shared_ptr<FlexColumn>       audio_route_col;
    std::shared_ptr<std::function<void()>> rebuild_audio_routes;

    // Network Shares (paginated Fetcher list) — local ShareFetcher struct stays.
    std::shared_ptr<TextWidget>       ns_hdr_lbl;
    std::shared_ptr<TextWidget>       ns_count_lbl;
    std::shared_ptr<FlexColumn>       share_col;
    std::shared_ptr<std::function<void()>> rebuild_shares;

    // Printer Queue (paginated Fetcher list) — local PrintFetcher struct stays; the
    // print_hdr_lbl is latent (never added to a card) so it stays builder-local too.
    std::shared_ptr<TextWidget>       print_count_lbl;
    std::shared_ptr<FlexColumn>       print_col;
    std::shared_ptr<std::function<void()>> rebuild_print_queue;

    // Package Updates (paginated Fetcher list) — local PkgFetcher struct stays.
    std::shared_ptr<TextWidget>       pkg_upd_hdr_lbl;
    std::shared_ptr<TextWidget>       pkg_upd_count_lbl;
    std::shared_ptr<FlexColumn>       pkg_upd_col;
    std::shared_ptr<ButtonWidget>     pkg_upd_all_btn;
    std::shared_ptr<ButtonWidget>     pkg_upd_check_btn;
    std::shared_ptr<std::function<void()>> rebuild_pkg_upd;

    // ZFS Pool Health (paginated Fetcher list) — local ZfsFetcher struct stays.
    std::shared_ptr<TextWidget>       zfs_hdr_lbl;
    std::shared_ptr<TextWidget>       zfs_count_lbl;
    std::shared_ptr<FlexColumn>       zfs_col;
    std::shared_ptr<std::function<void()>> rebuild_zfs;

    // Active Jails (paginated Fetcher list) — local JailFetcher struct stays.
    std::shared_ptr<TextWidget>       jail_hdr_lbl;
    std::shared_ptr<TextWidget>       jail_count_lbl;
    std::shared_ptr<FlexColumn>       jail_col;
    std::shared_ptr<ButtonWidget>     jail_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_jails;

    // Boot Environments (paginated Fetcher list) — local BeFetcher struct stays.
    std::shared_ptr<TextWidget>       be_hdr_lbl;
    std::shared_ptr<TextWidget>       be_count_lbl;
    std::shared_ptr<FlexColumn>       be_col;
    std::shared_ptr<ButtonWidget>     be_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_be;

    // Containers (paginated Fetcher list) — local ContainerFetcher struct stays.
    std::shared_ptr<TextWidget>       ct_hdr_lbl;
    std::shared_ptr<TextWidget>       ct_count_lbl;
    std::shared_ptr<FlexColumn>       ct_col;
    std::shared_ptr<std::function<void()>> rebuild_containers;

    // Virtual Machines (paginated Fetcher list) — local VmFetcher struct stays.
    std::shared_ptr<TextWidget>       vm_hdr_lbl;
    std::shared_ptr<TextWidget>       vm_count_lbl;
    std::shared_ptr<FlexColumn>       vm_col;
    std::shared_ptr<std::function<void()>> rebuild_vms;

    // Kernel Modules (paginated Fetcher list) — local KmodFetcher struct stays.
    std::shared_ptr<TextWidget>       kmod_hdr_lbl;
    std::shared_ptr<TextWidget>       kmod_count_lbl;
    std::shared_ptr<FlexColumn>       kmod_col;
    std::shared_ptr<ButtonWidget>     kmod_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_kmod;

    // Screenshot & Recordings (paginated Fetcher list) — local CapFetcher struct stays.
    std::shared_ptr<TextWidget>       cap_hdr_lbl;
    std::shared_ptr<TextWidget>       cap_count_lbl;
    std::shared_ptr<FlexColumn>       cap_col;
    std::shared_ptr<ButtonWidget>     cap_screenshot_btn;
    std::shared_ptr<std::function<void()>> rebuild_captures;

    // Keychain (paginated Fetcher list) — local KcFetcher struct stays.
    std::shared_ptr<TextWidget>       kc_hdr_lbl;
    std::shared_ptr<TextWidget>       kc_count_lbl;
    std::shared_ptr<FlexColumn>       kc_col;
    std::shared_ptr<ButtonWidget>     kc_lock_btn;
    std::shared_ptr<std::function<void()>> rebuild_keychain;

    // System Services (paginated Fetcher list) — local SvcFetcher struct stays.
    std::shared_ptr<TextWidget>       svc_hdr_lbl;
    std::shared_ptr<FlexColumn>       svc_col;
    std::shared_ptr<ButtonWidget>     svc_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_svc;

    // Calendar quick-add — the "+ Add" button stays builder-local, held alive by
    // the escaping cal_add_row.
    std::shared_ptr<TextWidget>       cal_add_hdr_lbl;
    std::shared_ptr<TextInputWidget>  cal_add_input;
    std::shared_ptr<TextWidget>       cal_add_status;
    std::shared_ptr<FlexRow>          cal_add_row;

    // 3-Day Weather Forecast — per-card label arrays; forecast_rebuild is assigned
    // later (in OnConnect), the builder only creates the empty shared_ptr.
    std::shared_ptr<TextWidget>                                 forecast_hdr_lbl;
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 3>> forecast_day_lbls;
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 3>> forecast_icon_lbls;
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 3>> forecast_temp_lbls;
    std::shared_ptr<std::function<void()>>                      forecast_rebuild;
    std::shared_ptr<FlexRow>                                    forecast_row;

    // Quick countdown timer — the Start/Stop buttons stay builder-local, held by
    // the escaping timer_btn_row.
    std::shared_ptr<TextWidget>       timer_hdr_lbl;
    std::shared_ptr<TextInputWidget>  timer_input;
    std::shared_ptr<TextWidget>       timer_cd_lbl;
    std::shared_ptr<TextInputWidget>  timer_label_input;
    std::shared_ptr<TextWidget>       timer_status_lbl;
    std::shared_ptr<FlexRow>          timer_btn_row;

    // Pomodoro session log — header label + Clear button stay builder-local, held
    // by the escaping pomo_log_hdr_row; rebuild_pomo_log body is assigned later.
    std::shared_ptr<FlexColumn>       pomo_log_col;
    std::shared_ptr<std::function<void()>> rebuild_pomo_log;
    std::shared_ptr<FlexRow>          pomo_log_hdr_row;

    // App usage heatmap — header label + Reset button stay builder-local, held by
    // the escaping heat_hdr_row; heat_bar_lbl also fed by g_heat_update_fn (OnConnect).
    std::shared_ptr<TextWidget>       heat_bar_lbl;
    std::shared_ptr<TextWidget>       heat_hr_lbl;
    std::shared_ptr<FlexRow>          heat_hdr_row;

    // Screenshot & Recording — Full/Delay buttons stay builder-local, held by the
    // escaping ss_btn_row; the rest are driven by RecordingChanged/reply handlers.
    std::shared_ptr<TextWidget>       ss_hdr_lbl;
    std::shared_ptr<TextWidget>       ss_status_lbl;
    std::shared_ptr<std::string>      ss_last_path;
    std::shared_ptr<ButtonWidget>     ss_open_btn;
    std::shared_ptr<bool>             ss_recording;
    std::shared_ptr<ButtonWidget>     rec_toggle_btn;
    std::shared_ptr<TextWidget>       rec_status_lbl;
    std::shared_ptr<FlexRow>          ss_btn_row;
    std::shared_ptr<FlexRow>          ss_last_row;

    // Storage quick-view (Fetcher list) — local PoolFetcher (+ fmtBytes/fmtBar
    // helper closures) stays inside the rebuild.
    std::shared_ptr<TextWidget>       stor_hdr_lbl;
    std::shared_ptr<FlexColumn>       stor_col;
    std::shared_ptr<std::function<void()>> rebuild_storage;

    // Disk I/O monitor (Fetcher list) — local DioFetcher stays inside the rebuild.
    std::shared_ptr<TextWidget>       diskio_hdr_lbl;
    std::shared_ptr<FlexColumn>       diskio_col;
    std::shared_ptr<ButtonWidget>     diskio_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_diskio;

    // SMART Disk Health (Fetcher list) — local SmartFetcher stays inside the rebuild.
    std::shared_ptr<TextWidget>       smart_hdr_lbl;
    std::shared_ptr<FlexColumn>       smart_col;
    std::shared_ptr<ButtonWidget>     smart_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_smart;

    // Disk Usage (Fetcher list) — local DuFetcher stays inside the rebuild.
    std::shared_ptr<TextWidget>       du_hdr_lbl;
    std::shared_ptr<TextWidget>       du_status_lbl;
    std::shared_ptr<FlexColumn>       du_col;
    std::shared_ptr<ButtonWidget>     du_scan_btn;
    std::shared_ptr<std::function<void()>> rebuild_disk_usage;

    // Archive — the extract/create form inputs + buttons stay builder-local, held
    // by the escaping extract/create rows.
    std::shared_ptr<TextWidget>       arch_hdr_lbl;
    std::shared_ptr<TextWidget>       arch_status_lbl;
    std::shared_ptr<FlexRow>          arch_extract_row;
    std::shared_ptr<FlexRow>          arch_create_row;

    // Print Queue (Fetcher list) — local PrintFetcher stays inside the rebuild.
    std::shared_ptr<TextWidget>       print_hdr_lbl_b;
    std::shared_ptr<FlexColumn>       print_col_b;
    std::shared_ptr<ButtonWidget>     print_refresh_btn;
    std::shared_ptr<std::function<void()>> rebuild_print;

    // USB Device Monitor (Fetcher list) — LATENT panel: only rebuild_usb_b escapes
    // (broadcast/timer). The header/col/refresh-btn stay builder-local (never
    // added to a card; the col is kept alive by the rebuild closure).
    std::shared_ptr<std::function<void()>> rebuild_usb_b;

    // Bookmarks (Fetcher list) — local BmFetcher stays inside the rebuild.
    std::shared_ptr<TextWidget>       bm_hdr_lbl;
    std::shared_ptr<TextWidget>       bm_count_lbl;
    std::shared_ptr<FlexColumn>       bm_col;
    std::shared_ptr<ButtonWidget>     bm_clear_btn;
    std::shared_ptr<std::function<void()>> rebuild_bm;

    // Active Transfers (outer-scope-fetcher list) — the XfrFetcher struct + xfrf
    // shared_ptr stay builder-local, kept alive by the rebuild_xfr closure.
    std::shared_ptr<TextWidget>       xfr_hdr_lbl;
    std::shared_ptr<TextWidget>       xfr_count_lbl;
    std::shared_ptr<FlexColumn>       xfr_col;
    std::shared_ptr<std::function<void()>> rebuild_xfr;

    // Game Controllers (outer-scope-fetcher list) — the GcFetcher struct + gcf
    // shared_ptr stay builder-local, kept alive by the rebuild_gc closure.
    std::shared_ptr<TextWidget>       gc_hdr_lbl;
    std::shared_ptr<TextWidget>       gc_count_lbl;
    std::shared_ptr<FlexColumn>       gc_col;
    std::shared_ptr<std::function<void()>> rebuild_gc;

    // Saved Wi-Fi Networks (outer-scope-fetcher list) — the SwifiFetcher struct +
    // swf shared_ptr stay builder-local, kept alive by the rebuild_swifi closure.
    std::shared_ptr<TextWidget>       swifi_hdr_lbl;
    std::shared_ptr<TextWidget>       swifi_count_lbl;
    std::shared_ptr<FlexColumn>       swifi_col;
    std::shared_ptr<std::function<void()>> rebuild_swifi;

    // Security Keys (outer-scope-fetcher list) — the SeckeyFetcher struct + skf
    // shared_ptr stay builder-local, kept alive by the rebuild_seckey closure.
    std::shared_ptr<TextWidget>       seckey_hdr_lbl;
    std::shared_ptr<TextWidget>       seckey_count_lbl;
    std::shared_ptr<FlexColumn>       seckey_col;
    std::shared_ptr<std::function<void()>> rebuild_seckey;
    std::shared_ptr<TextWidget>       seckey_status_lbl;
    std::shared_ptr<ButtonWidget>     seckey_register_btn;

    // Text Replacement (outer-scope-fetcher list + add form) — the TxtrepFetcher
    // struct + trf stay builder-local; the add-form widgets are carded so escape.
    std::shared_ptr<TextWidget>       txtrep_hdr_lbl;
    std::shared_ptr<TextWidget>       txtrep_count_lbl;
    std::shared_ptr<FlexColumn>       txtrep_col;
    std::shared_ptr<std::function<void()>> rebuild_txtrep;
    std::shared_ptr<TextInputWidget>  txtrep_shortcut_input;
    std::shared_ptr<TextInputWidget>  txtrep_phrase_input;
    std::shared_ptr<TextWidget>       txtrep_add_status_lbl;
    std::shared_ptr<ButtonWidget>     txtrep_add_btn;

    // App Time Limits (outer-scope-fetcher list + add form) — the AppLimitFetcher
    // struct + alf stay builder-local; the add-form widgets are carded so escape.
    std::shared_ptr<TextWidget>       applimit_hdr_lbl;
    std::shared_ptr<TextWidget>       applimit_count_lbl;
    std::shared_ptr<FlexColumn>       applimit_col;
    std::shared_ptr<std::function<void()>> rebuild_applimit;
    std::shared_ptr<TextInputWidget>  applimit_name_input;
    std::shared_ptr<TextInputWidget>  applimit_min_input;
    std::shared_ptr<TextWidget>       applimit_add_status_lbl;
    std::shared_ptr<ButtonWidget>     applimit_add_btn;

    // Notification Sounds (outer-scope-fetcher list) — the NotifSoundFetcher struct
    // + nsf stay builder-local, kept alive by the rebuild_notifsnd closure.
    std::shared_ptr<TextWidget>       notifsnd_hdr_lbl;
    std::shared_ptr<TextWidget>       notifsnd_count_lbl;
    std::shared_ptr<FlexColumn>       notifsnd_col;
    std::shared_ptr<std::function<void()>> rebuild_notifsnd;

    // Login Items (outer-scope-fetcher list) — the LoginItemFetcher struct + lif
    // stay builder-local, kept alive by the rebuild_litem closure.
    std::shared_ptr<TextWidget>       litem_hdr_lbl;
    std::shared_ptr<TextWidget>       litem_count_lbl;
    std::shared_ptr<FlexColumn>       litem_col;
    std::shared_ptr<std::function<void()>> rebuild_litem;

    // Trusted Devices (outer-scope-fetcher list) — the TrustedDevFetcher struct +
    // tdf stay builder-local, kept alive by the rebuild_tdev closure.
    std::shared_ptr<TextWidget>       tdev_hdr_lbl;
    std::shared_ptr<TextWidget>       tdev_count_lbl;
    std::shared_ptr<FlexColumn>       tdev_col;
    std::shared_ptr<std::function<void()>> rebuild_tdev;

    // Keyboard Shortcuts (outer-scope-fetcher list) — the KeyShortcutFetcher struct
    // + ksf stay builder-local, kept alive by the rebuild_kbsc closure.
    std::shared_ptr<TextWidget>       kbsc_hdr_lbl;
    std::shared_ptr<TextWidget>       kbsc_count_lbl;
    std::shared_ptr<FlexColumn>       kbsc_col;
    std::shared_ptr<std::function<void()>> rebuild_kbsc;

    // Default Apps (outer-scope-fetcher list) — the DefaultAppFetcher struct + daf
    // stay builder-local, kept alive by the rebuild_dapp closure.
    std::shared_ptr<TextWidget>       dapp_hdr_lbl;
    std::shared_ptr<TextWidget>       dapp_count_lbl;
    std::shared_ptr<FlexColumn>       dapp_col;
    std::shared_ptr<std::function<void()>> rebuild_dapp;

    // Reminders (outer-scope-fetcher list) — the RemFetcher struct + remf stay
    // builder-local, kept alive by the rebuild_rem closure.
    std::shared_ptr<TextWidget>       rem_hdr_lbl;
    std::shared_ptr<TextWidget>       rem_count_lbl;
    std::shared_ptr<FlexColumn>       rem_col;
    std::shared_ptr<std::function<void()>> rebuild_rem;

    // Scheduled Backups (outer-scope-fetcher list) — the BkpFetcher struct + bkpf
    // stay builder-local, kept alive by the rebuild_bkp closure.
    std::shared_ptr<TextWidget>       bkp_hdr_lbl;
    std::shared_ptr<TextWidget>       bkp_count_lbl;
    std::shared_ptr<FlexColumn>       bkp_col;
    std::shared_ptr<std::function<void()>> rebuild_bkp;

    // TOTP Authenticator (outer-scope-fetcher list) — the TotpFetcher struct + tpf
    // stay builder-local, kept alive by the rebuild_totp closure.
    std::shared_ptr<TextWidget>       totp_hdr_lbl;
    std::shared_ptr<TextWidget>       totp_count_lbl;
    std::shared_ptr<FlexColumn>       totp_col;
    std::shared_ptr<std::function<void()>> rebuild_totp;

    // Bluetooth Audio (outer-scope-fetcher list) — the BtAudioFetcher struct + baf
    // stay builder-local, kept alive by the rebuild_btaudio closure.
    std::shared_ptr<TextWidget>       btaudio_hdr_lbl;
    std::shared_ptr<TextWidget>       btaudio_count_lbl;
    std::shared_ptr<FlexColumn>       btaudio_col;
    std::shared_ptr<std::function<void()>> rebuild_btaudio;

    // App Sandbox (outer-scope-fetcher list) — the SandboxFetcher struct + sbf stay
    // builder-local, kept alive by the rebuild_sandbox closure.
    std::shared_ptr<TextWidget>       sandbox_hdr_lbl;
    std::shared_ptr<TextWidget>       sandbox_count_lbl;
    std::shared_ptr<FlexColumn>       sandbox_col;
    std::shared_ptr<std::function<void()>> rebuild_sandbox;

    // USB Devices (detail, outer-scope-fetcher list) — the UsbDevFetcher struct +
    // udf stay builder-local, kept alive by the rebuild_usbdev closure.
    std::shared_ptr<TextWidget>       usbdev_hdr_lbl;
    std::shared_ptr<TextWidget>       usbdev_count_lbl;
    std::shared_ptr<FlexColumn>       usbdev_col;
    std::shared_ptr<TextWidget>       usbdev_status_lbl;
    std::shared_ptr<std::function<void()>> rebuild_usbdev;

    // Network Shares (mount) — the NsFetcher + all mount-form widgets stay
    // builder-local (form widgets held by ns_form_col; nspf kept alive by the
    // rebuild_ns closure via a strong capture).
    std::shared_ptr<TextWidget>       nsm_hdr_lbl;
    std::shared_ptr<TextWidget>       nsm_count_lbl;
    std::shared_ptr<FlexColumn>       ns_col;
    std::shared_ptr<FlexColumn>       ns_form_col;
    std::shared_ptr<TextWidget>       ns_status_lbl;
    std::shared_ptr<ButtonWidget>     ns_toggle_btn;
    std::shared_ptr<std::function<void()>> rebuild_ns;

    // System Log (in-rebuild fetcher) — the level filter buttons array escapes
    // (carded); the SlogFetcher + slog_filter_hl closure stay builder-local.
    std::shared_ptr<TextWidget>       slog_hdr_lbl;
    std::shared_ptr<FlexColumn>       slog_col;
    std::shared_ptr<uint32_t>         slog_level_mask;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> slog_filter_btns;
    std::shared_ptr<std::function<void()>> rebuild_slog;

    // Remote Desktop (in-rebuild fetcher + connect form) — the connect-form widgets
    // stay builder-local (held by rdp_form_col); rdp_status_lbl also escapes.
    std::shared_ptr<TextWidget>       rdp_hdr_lbl;
    std::shared_ptr<TextWidget>       rdp_count_lbl;
    std::shared_ptr<FlexColumn>       rdp_col;
    std::shared_ptr<FlexColumn>       rdp_form_col;
    std::shared_ptr<TextWidget>       rdp_status_lbl;
    std::shared_ptr<ButtonWidget>     rdp_toggle_btn;
    std::shared_ptr<std::function<void()>> rebuild_rdp;

    // Input Devices (in-rebuild fetcher) — the IndevFetcher stays builder-local.
    std::shared_ptr<TextWidget>       indev_hdr_lbl;
    std::shared_ptr<TextWidget>       indev_count_lbl;
    std::shared_ptr<FlexColumn>       indev_col;
    std::shared_ptr<std::function<void()>> rebuild_indev;

    // Clipboard History (in-rebuild fetcher) — the ClipFetcher stays builder-local.
    std::shared_ptr<TextWidget>       clip_hdr_lbl;
    std::shared_ptr<TextWidget>       clip_count_lbl;
    std::shared_ptr<FlexColumn>       clip_col;
    std::shared_ptr<ButtonWidget>     clip_clear_btn;
    std::shared_ptr<std::function<void()>> rebuild_clip;

    // Screenshot Gallery (in-rebuild fetcher) — the ScapFetcher stays builder-local.
    std::shared_ptr<TextWidget>       scap_hdr_lbl;
    std::shared_ptr<TextWidget>       scap_count_lbl;
    std::shared_ptr<FlexColumn>       scap_col;
    std::shared_ptr<ButtonWidget>     scap_btn;
    std::shared_ptr<std::function<void()>> rebuild_scap;

    // Upcoming Events (in-rebuild fetcher) — the range filter buttons array escapes
    // (carded); the UevFetcher + uev_range_hl closure stay builder-local.
    std::shared_ptr<TextWidget>       uev_hdr_lbl;
    std::shared_ptr<TextWidget>       uev_count_lbl;
    std::shared_ptr<FlexColumn>       uev_col;
    std::shared_ptr<uint32_t>         uev_range_cur;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 3>> uev_range_btns;
    std::shared_ptr<std::function<void()>> rebuild_uev;

    // Recent Files (iterates g_recent_files; no fetcher) — header/col + rebuild.
    std::shared_ptr<TextWidget>       rf_hdr_lbl;
    std::shared_ptr<FlexColumn>       rf_col;
    std::shared_ptr<std::function<void()>> rebuild_rf;

    // Named Clipboard Slots — per-slot Save/Copy buttons stay builder-local (held
    // by cs_col rows); the preview-label array escapes (OnConnect prefills it).
    std::shared_ptr<TextWidget>       cs_hdr_lbl;
    std::shared_ptr<FlexColumn>       cs_col;
    std::array<std::shared_ptr<TextWidget>, kNumClipSlots> cs_preview_lbls;

    // AC stub sections (App Volume / Screen Time / DND-priority) — each is just a
    // header + column + empty rebuild_X shared_ptr; the rebuild bodies are wired
    // later in OnConnect via the aliases.
    std::shared_ptr<TextWidget>       audio_vol_hdr_lbl;
    std::shared_ptr<FlexColumn>       audio_vol_col;
    std::shared_ptr<std::function<void()>> rebuild_audio_vol;
    std::shared_ptr<TextWidget>       screen_time_hdr_lbl;
    std::shared_ptr<FlexColumn>       screen_time_col;
    std::shared_ptr<std::function<void()>> rebuild_screen_time;
    std::shared_ptr<TextWidget>       dnd_exc_hdr_lbl;
    std::shared_ptr<FlexColumn>       dnd_exc_col;
    std::shared_ptr<std::function<void()>> rebuild_dnd_exc;

    // Location Services (QS toggle) — switch + count/last labels + update_loc_ui.
    std::shared_ptr<SwitchWidget>     loc_switch;
    std::shared_ptr<TextWidget>       loc_count_lbl;
    std::shared_ptr<TextWidget>       loc_last_lbl;
    std::function<void(const LocationState&)> update_loc_ui;

    // Audio Output sink selector — name label + pills row + rebuild_aout (the
    // per-sink pill buttons are built inside the rebuild).
    std::shared_ptr<TextWidget>       aout_name_lbl;
    std::shared_ptr<FlexRow>          aout_pills_row;
    std::shared_ptr<std::function<void()>> rebuild_aout;

    // NTP / Time Sync toggle — switch + status/server labels + update_ntp_ui.
    std::shared_ptr<SwitchWidget>     ntp_switch;
    std::shared_ptr<TextWidget>       ntp_status_lbl;
    std::shared_ptr<TextWidget>       ntp_server_lbl;
    std::function<void(const NtpState&)> update_ntp_ui;

    // Mouse & Pointer — speed slider/label + natural/accel/tap switches + status +
    // update_ptr_ui; the ptr_speed_cur state and send_ptr helper stay builder-local.
    std::shared_ptr<TextWidget>       ptr_speed_lbl;
    std::shared_ptr<SliderWidget>     ptr_speed_slider;
    std::shared_ptr<SwitchWidget>     ptr_natural_sw;
    std::shared_ptr<SwitchWidget>     ptr_accel_sw;
    std::shared_ptr<SwitchWidget>     ptr_tap_sw;
    std::shared_ptr<TextWidget>       ptr_status_lbl;
    std::function<void(const PointerConfig&)> update_ptr_ui;

    // Network + Wi-Fi (status widgets + scan list + inline connect panel + paged
    // scan loader). The Wi-Fi connect form widgets stay builder-local (held by
    // wifi_connect_col / captured by do_scan_page).
    std::shared_ptr<TextWidget>       qp_conn_w;
    std::shared_ptr<TextWidget>       qp_ip_w;
    std::shared_ptr<TextWidget>       qp_signal_w;
    std::shared_ptr<TextWidget>       qp_bw_w;
    std::shared_ptr<TextWidget>       net_rx_spark_lbl;
    std::shared_ptr<TextWidget>       net_tx_spark_lbl;
    std::shared_ptr<FlexColumn>       qp_net_list;
    std::shared_ptr<FlexColumn>       wifi_connect_col;
    std::shared_ptr<std::function<void(uint32_t)>> do_scan_page;
    std::shared_ptr<ButtonWidget>     qp_disc_btn;

    // Bluetooth device panel (device list + scan + adapter on/off) — the paged
    // PageFetcher stays builder-local (inside rebuild_bt_devices).
    std::shared_ptr<FlexColumn>       bt_devices_col;
    std::shared_ptr<TextWidget>       bt_status_lbl;
    std::shared_ptr<std::function<void()>> rebuild_bt_devices;
    std::shared_ptr<ButtonWidget>     bt_scan_btn;
    std::shared_ptr<TextWidget>       bt_state_lbl;
    std::shared_ptr<ButtonWidget>     bt_enable_btn;
    std::shared_ptr<ButtonWidget>     bt_disable_btn;
    std::shared_ptr<FlexRow>          bt_toggle_row;
    std::function<void(const BluetoothState&)> update_bt_ui;

    // VPN quick-connect — status/info labels + iface input + connect/disconnect
    // buttons (in vpn_btn_row) + update_vpn_ui.
    std::shared_ptr<TextWidget>       vpn_status_lbl;
    std::shared_ptr<TextWidget>       vpn_info_lbl;
    std::shared_ptr<TextInputWidget>  vpn_iface_input;
    std::shared_ptr<ButtonWidget>     vpn_conn_btn;
    std::shared_ptr<ButtonWidget>     vpn_disc_btn;
    std::shared_ptr<FlexRow>          vpn_btn_row;
    std::function<void(const VpnStatus&)> update_vpn_ui;

    // Wi-Fi Hotspot config form (SSID/pass inputs + Apply). hs_ssid_input is a ctx
    // field (program lifetime) because the Hotspot enable/disable tile's
    // extra_update captures it by reference; hs_pass_input/hs_apply_btn stay
    // builder-local (held by hs_cfg_row1).
    std::shared_ptr<TextInputWidget>  hs_ssid_input;
    std::shared_ptr<FlexRow>          hs_cfg_row1;
    std::shared_ptr<TextInputWidget>  hs_cfg_row2;

    // Screen Cast — status/elapsed labels + receiver list + scan/stop buttons +
    // ctrl row + rebuild_cast_receivers + update_cast_ui; the CastFetcher stays
    // builder-local (inside the rebuild).
    std::shared_ptr<TextWidget>       cast_status_lbl;
    std::shared_ptr<TextWidget>       cast_elapsed_lbl;
    std::shared_ptr<FlexColumn>       cast_receiver_col;
    std::shared_ptr<ButtonWidget>     cast_scan_btn;
    std::shared_ptr<ButtonWidget>     cast_stop_btn;
    std::shared_ptr<FlexRow>          cast_ctrl_row;
    std::shared_ptr<std::function<void()>> rebuild_cast_receivers;
    std::function<void(const CastState&)> update_cast_ui;

    // Remote Desktop (QS) password form — pass input + Set button, held by
    // rd_pass_row (the only escaping handle). The enable/disable tile + rd_btn_row
    // stay inline in main().
    std::shared_ptr<FlexRow>          rd_pass_row;

    // Media playback controls — title/artist/progress labels + playing flag +
    // play button + button row + update_mp_ui; prev/next buttons stay builder-local
    // (held by mp_btn_row).
    std::shared_ptr<TextWidget>       mp_title_lbl;
    std::shared_ptr<TextWidget>       mp_artist_lbl;
    std::shared_ptr<TextWidget>       mp_progress_lbl;
    std::shared_ptr<bool>             mp_playing;
    std::shared_ptr<ButtonWidget>     mp_play_btn;
    std::shared_ptr<FlexRow>          mp_btn_row;
    std::function<void(const MediaPlayerState&)> update_mp_ui;

    // Color Filter — enable switch + 4-way type button row + monochrome switch +
    // intensity slider/label (+ rows) + update_cf_ui. kCfTypeLabels stays a
    // builder-local static.
    std::shared_ptr<SwitchWidget>     cf_switch;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> cf_type_btns;
    std::shared_ptr<FlexRow>          cf_type_row;
    std::shared_ptr<SwitchWidget>     cf_mono_switch;
    std::shared_ptr<TextWidget>       cf_intensity_val;
    std::shared_ptr<SliderWidget>     cf_intensity_slider;
    std::shared_ptr<FlexRow>          cf_intensity_row;
    std::function<void(const ColorFilterState&)> update_cf_ui;

    // Audio EQ — status label + 5 band sliders/value-labels + preset pill buttons +
    // update_eq_ui; eq_gains/eq_preset_cur state + eq_hl_preset/send_eq helpers stay
    // builder-local. (kEqBandNames stays a main()-local static — used at the card
    // site; kEqPresets/kEqPresetGains move into the builder.)
    std::shared_ptr<TextWidget>       eq_status_lbl;
    std::array<std::shared_ptr<SliderWidget>, 5> eq_sliders;
    std::array<std::shared_ptr<TextWidget>,   5> eq_val_lbls;
    std::shared_ptr<std::vector<std::shared_ptr<ButtonWidget>>> eq_preset_btns;
    std::function<void(const AudioEqConfig&)> update_eq_ui;

    // Power menu (Sleep / Restart / Shut Down / Lock) — the confirm row + 4 action
    // buttons escape; the confirm label, pending-opcode, Yes/Cancel buttons, and
    // hide_confirm/make_power_btn helpers stay builder-local.
    std::shared_ptr<FlexRow>          power_confirm_row;
    std::shared_ptr<ButtonWidget>     power_sleep_btn;
    std::shared_ptr<ButtonWidget>     power_restart_btn;
    std::shared_ptr<ButtonWidget>     power_shutdown_btn;
    std::shared_ptr<ButtonWidget>     power_lock_btn;

    // DND Schedule UI — switch + from/to hour labels + header/times rows + countdown
    // label; the hour dec/inc buttons + make_hour_* helpers stay builder-local (held
    // by dnd_sched_times). Uses file-scope g_dnd_sched_* globals + saveDndSched.
    std::shared_ptr<SwitchWidget>     dnd_sched_switch;
    std::shared_ptr<TextWidget>       dnd_from_lbl;
    std::shared_ptr<TextWidget>       dnd_to_lbl;
    std::shared_ptr<FlexRow>          dnd_sched_hdr;
    std::shared_ptr<FlexRow>          dnd_sched_times;
    std::shared_ptr<TextWidget>       dnd_countdown_lbl;

    // Night Shift Schedule UI — same hour-stepper shape as DND schedule; steppers +
    // make_ns_hour_* helpers stay builder-local (held by ns_sched_times). Uses
    // file-scope g_ns_sched_* globals + saveNsSched.
    std::shared_ptr<SwitchWidget>     ns_sched_switch;
    std::shared_ptr<TextWidget>       ns_from_lbl;
    std::shared_ptr<TextWidget>       ns_to_lbl;
    std::shared_ptr<FlexRow>          ns_sched_hdr;
    std::shared_ptr<FlexRow>          ns_sched_times;

    // Per-workspace wallpaper strip — 4 WS thumbnail buttons + highlight fn +
    // header/row; ws_wp_open_fn's body is assigned later in OnConnect (builder
    // creates the empty shared_ptr). Uses the file-scope g_current_ws global.
    std::shared_ptr<std::function<void(int)>> ws_wp_open_fn;
    std::shared_ptr<std::array<std::shared_ptr<ButtonWidget>, 4>> ws_wp_btns;
    std::shared_ptr<std::function<void()>> ws_wp_update_hl;
    std::shared_ptr<TextWidget>       ws_wp_hdr_lbl;
    std::shared_ptr<FlexRow>          ws_wp_row;

    // Airplane Mode (cross-coupled: builder takes qs_wifi_switch + qs_bt_switch by
    // ref) — switch + update_ap_ui escape.
    std::shared_ptr<SwitchWidget>     ap_switch;
    std::function<void(const AirplaneModeState&)> update_ap_ui;

    // Focus-mode toggle (cross-coupled: builder takes qs_dnd_switch by ref) — the
    // focus_btn escapes; focus_lbl stays builder-local (held by the button).
    std::shared_ptr<ButtonWidget>     focus_btn;

    // Color-theme presets (cross-coupled: builder takes qs_dark_switch by ref) —
    // label array + selected index + apply_tp fn + preset row escape; kPresets
    // stays a builder-local static.
    std::shared_ptr<std::array<std::shared_ptr<TextWidget>, 5>> tp_labels;
    std::shared_ptr<int>              tp_idx;
    std::function<void(int)>          apply_tp;
    std::shared_ptr<FlexRow>          theme_presets_row;
};

// ── Builder prototypes (definitions relocated to shell_panels.cc) ─────────────
void buildCronJobsPanel(ShellContext& ctx);
void buildMountedVolumesPanel(ShellContext& ctx);
void buildStartupItemsPanel(ShellContext& ctx);
void buildNetConnsPanel(ShellContext& ctx);
void buildSshKeysPanel(ShellContext& ctx);
void buildEnvVarsPanel(ShellContext& ctx);
void buildSysLogPanel(ShellContext& ctx);
void buildUsbDevicesPanel(ShellContext& ctx);
void buildLoginSessionsPanel(ShellContext& ctx);
void buildBatteryDrainPanel(ShellContext& ctx);
void buildFileSyncPanel(ShellContext& ctx);
void buildAudioRoutingPanel(ShellContext& ctx);
void buildNetworkSharesPanel(ShellContext& ctx);
void buildPrinterQueuePanel(ShellContext& ctx);
void buildPackageUpdatesPanel(ShellContext& ctx);
void buildZfsPoolsPanel(ShellContext& ctx);
void buildActiveJailsPanel(ShellContext& ctx);
void buildBootEnvPanel(ShellContext& ctx);
void buildContainersPanel(ShellContext& ctx);
void buildVirtualMachinesPanel(ShellContext& ctx);
void buildKernelModulesPanel(ShellContext& ctx);
void buildCapturesPanel(ShellContext& ctx);
void buildKeychainPanel(ShellContext& ctx);
void buildSystemServicesPanel(ShellContext& ctx);
void buildStoragePanel(ShellContext& ctx);
void buildDiskIoPanel(ShellContext& ctx);
void buildSmartHealthPanel(ShellContext& ctx);
void buildDiskUsagePanel(ShellContext& ctx);
void buildArchivePanel(ShellContext& ctx);
void buildPrintSpoolerPanel(ShellContext& ctx);
void buildUsbMonitorPanel(ShellContext& ctx);
void buildBookmarksPanel(ShellContext& ctx);
void buildActiveTransfersPanel(ShellContext& ctx);
void buildGameControllersPanel(ShellContext& ctx);
void buildSavedWifiPanel(ShellContext& ctx);
void buildSecurityKeysPanel(ShellContext& ctx);
void buildTextReplacementPanel(ShellContext& ctx);
void buildAppTimeLimitsPanel(ShellContext& ctx);
void buildNotifSoundsPanel(ShellContext& ctx);
void buildLoginItemsPanel(ShellContext& ctx);
void buildTrustedDevicesPanel(ShellContext& ctx);
void buildKeyboardShortcutsPanel(ShellContext& ctx);
void buildDefaultAppsPanel(ShellContext& ctx);
void buildRemindersPanel(ShellContext& ctx);
void buildScheduledBackupsPanel(ShellContext& ctx);
void buildTotpPanel(ShellContext& ctx);
void buildBtAudioPanel(ShellContext& ctx);
void buildAppSandboxPanel(ShellContext& ctx);
void buildUsbDeviceDetailPanel(ShellContext& ctx);
void buildNetworkSharesMountPanel(ShellContext& ctx);
void buildSystemLogPanel(ShellContext& ctx);
void buildRemoteDesktopPanel(ShellContext& ctx);
void buildInputDevicesPanel(ShellContext& ctx);
void buildClipboardHistoryPanel(ShellContext& ctx);
void buildScreenshotGalleryPanel(ShellContext& ctx);
void buildUpcomingEventsPanel(ShellContext& ctx);
void buildAcStubSections(ShellContext& ctx);
void buildLocationServicesTogglePanel(ShellContext& ctx);
void buildAudioOutputPanel(ShellContext& ctx);
void buildNtpTogglePanel(ShellContext& ctx);
void buildMousePointerPanel(ShellContext& ctx);
void buildBluetoothDevicePanel(ShellContext& ctx);
void buildVpnPanel(ShellContext& ctx);
void buildHotspotConfigPanel(ShellContext& ctx);
void buildScreenCastPanel(ShellContext& ctx);
void buildRemoteDesktopPassForm(ShellContext& ctx);
void buildMediaPlaybackPanel(ShellContext& ctx);
void buildColorFilterPanel(ShellContext& ctx);
void buildAudioEqPanel(ShellContext& ctx);
void buildPowerMenuPanel(ShellContext& ctx);
void buildGameModePanel(ShellContext& ctx);
void buildPrivacyPanel(ShellContext& ctx);
void buildWakeLockPanel(ShellContext& ctx);
void buildRemoteLoginPanel(ShellContext& ctx);
void buildLocalePanel(ShellContext& ctx);
void buildA11yQuickPanel(ShellContext& ctx);
void buildProxyPanel(ShellContext& ctx);
void buildDnsConfigPanel(ShellContext& ctx);
void buildUpdateChannelPanel(ShellContext& ctx);
void buildTimeZonePanel(ShellContext& ctx);
void buildPackageSigningPanel(ShellContext& ctx);
void buildBatteryHealthPanel(ShellContext& ctx);
void buildScreenLockPanel(ShellContext& ctx);
void buildNotifSchedulePanel(ShellContext& ctx);
void buildAutoLockPanel(ShellContext& ctx);
void buildKeyboardBacklightPanel(ShellContext& ctx);
void buildPowerProfilePanel(ShellContext& ctx);
void buildNightShiftPanel(ShellContext& ctx);
void buildScreenSaverPanel(ShellContext& ctx);
void buildDockLayoutPanel(ShellContext& ctx);
void buildXWaylandPanel(ShellContext& ctx);
void buildDisplaySettingsPanel(ShellContext& ctx);
void buildFocusModePanel(ShellContext& ctx);
void buildBatteryIndicatorPanel(ShellContext& ctx);
void buildNotifBadgePanel(ShellContext& ctx);
void buildOsdOverlayPanel(ShellContext& ctx);
void buildSystemTrayPanel(ShellContext& ctx);
void buildContactSearchPanel(ShellContext& ctx);
void buildNotesMiniPanel(ShellContext& ctx);
void buildClipboardMiniPanel(ShellContext& ctx);
void buildSparklinesPanel(ShellContext& ctx);
void buildLicensePanel(ShellContext& ctx);
void buildMemPressurePanel(ShellContext& ctx);
void buildFrameTimingPanel(ShellContext& ctx);
void buildSysInfoPanel(ShellContext& ctx);
void buildGpuStatsPanel(ShellContext& ctx);
void buildGpuMonitorPanel(ShellContext& ctx);
void buildNightLightPanel(ShellContext& ctx);
void buildGestureConfigPanel(ShellContext& ctx);
void buildStylusPanel(ShellContext& ctx);
void buildColorProfilePanel(ShellContext& ctx);
void buildCursorSizePanel(ShellContext& ctx);
void buildDisplayScalePanel(ShellContext& ctx);
void buildRefreshRatePanel(ShellContext& ctx);
void buildTextSizePanel(ShellContext& ctx);
void buildAutoBrightnessPanel(ShellContext& ctx);
void buildScreenTimeoutPanel(ShellContext& ctx);
void buildRotationLockPanel(ShellContext& ctx);
void buildMicMutePanel(ShellContext& ctx);
void buildAdminUnlockPanel(ShellContext& ctx);
void buildThermalZonesPanel(ShellContext& ctx);
void buildLocationServicesPanel(ShellContext& ctx);
void buildCameraDevicesPanel(ShellContext& ctx);
void buildMultiSeatPanel(ShellContext& ctx);
void buildCrashReporterPanel(ShellContext& ctx);
void buildSecurityEventsPanel(ShellContext& ctx);
void buildCalendarQuickAddPanel(ShellContext& ctx);
void buildBatterySaverPanel(ShellContext& ctx);
void buildPomodoroPanel(ShellContext& ctx);
void buildUpdatesButtonPanel(ShellContext& ctx, const std::vector<AppMetadata>& apps);
void buildIdleLockPanel(ShellContext& ctx);
void buildStartupAppsPanel(ShellContext& ctx);
void buildScratchPadPanel(ShellContext& ctx);
void buildWeatherForecastPanel(ShellContext& ctx);
void buildQuickTimerPanel(ShellContext& ctx);
void buildPomodoroLogPanel(ShellContext& ctx);
void buildAppHeatmapPanel(ShellContext& ctx);
void buildScreenshotPanel(ShellContext& ctx);
void buildRecentFilesPanel(ShellContext& ctx);
void buildClipboardSlotsPanel(ShellContext& ctx);
void buildNetworkWifiPanel(ShellContext& ctx);
void buildDndSchedulePanel(ShellContext& ctx);
void buildNightShiftSchedulePanel(ShellContext& ctx);
void buildWorkspaceWallpaperPanel(ShellContext& ctx);
void buildAirplaneModePanel(ShellContext& ctx, const std::shared_ptr<SwitchWidget>& qs_wifi_switch, const std::shared_ptr<SwitchWidget>& qs_bt_switch);
void buildFocusToggleButton(ShellContext& ctx, const std::shared_ptr<SwitchWidget>& qs_dnd_switch);
void buildColorThemePanel(ShellContext& ctx, const std::shared_ptr<SwitchWidget>& qs_dark_switch);
void buildSessionRestorePanel(ShellContext& ctx, const std::vector<AppMetadata>& apps);
void buildKeyboardLayoutPanel(ShellContext& ctx);
