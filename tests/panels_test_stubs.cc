// Test-only definitions of the file-scope state that shell_panels.cc borrows
// from main.cc. In the real binary these live in main.cc (promoted from
// static to external linkage during the Phase-2 globals-promotion). Here they
// are inert defaults + no-op persistence helpers so the panel builders can be
// linked and constructed without the full shell/XBus/Vulkan process.

#include "shell_panels.h"

// ── Promoted globals ─────────────────────────────────────────────────────────
XBusClient* g_shell_xbus;
bool g_dnd;
uint32_t g_accent_idx;
std::vector<std::pair<std::string,std::string>> g_recent_files;
std::array<std::string, kNumClipSlots> g_clip_slots;
std::atomic<uint32_t> g_idle_ticks;
std::atomic<uint32_t> g_idle_lock_secs;
int g_current_ws;
std::string g_wifi_ssid;
bool g_dnd_sched_on;
int  g_dnd_sched_start;
int  g_dnd_sched_end;
bool g_ns_sched_on;
int  g_ns_sched_start;
int  g_ns_sched_end;
bool g_focus_active;
std::time_t g_focus_start;
int g_theme_preset;
bool g_wp_rotate_on;
int  g_wp_rotate_interval;
int  g_wp_rotate_countdown;
CountdownTimer g_countdown;
std::mutex g_countdown_mtx;
bool g_bat_saver;
uint32_t g_bat_saver_prev_lock;
int  g_keymap_idx;
std::array<uint32_t, 24> g_app_hourly;
bool g_session_enabled;
std::vector<std::string> g_session_execs;
std::string g_scratch_note;
std::vector<PomoLogEntry> g_pomo_log;
std::mutex g_pomo_log_mtx;
std::atomic<int>  g_pomo_phase;
std::atomic<int>  g_pomo_secs;
std::atomic<bool> g_pomo_running;
std::atomic<int>  g_pomo_cycles;

// ── Persistence / launch helpers (no-ops in tests) ───────────────────────────
void saveClipSlots() noexcept {}
void saveDndSched() noexcept {}
void saveNsSched() noexcept {}
void saveThemePreset() noexcept {}
void saveWpRotate() noexcept {}
void saveKeymap() noexcept {}
void saveAppHeatmap() noexcept {}
void saveSession() noexcept {}
void saveScratchNote() noexcept {}
void savePomoLog() noexcept {}
std::string signalBars(int32_t) noexcept { return {}; }
void launchApp(const std::string&, const std::string&) noexcept {}
