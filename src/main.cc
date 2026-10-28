#include "xkit/widget.h"
#include "xkit/animation.h"
#include "xkit/application.h"
#include "xkit/audio_player.h"
#include "xkit/clipboard.h"
#include "xshell/app_discovery.h"
#include "xrender/font_manager.h"
#include "shared/logger.h"
#include "shared/user_env.h"
#include "shared/autostart.h"
#include "shell_helpers.h"
#include "shell_query.h"    // MathParser, quickMathEval, searchFiles (carved out)
#include "shell_panels.h"   // ShellContext + buildXPanel() builders (Phase 2)
#include <functional>
#include <iostream>
#include <vector>
#include <set>
#include <array>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <thread>
#include <chrono>
#include <ctime>
#include <format>
#include <cstring>
#include <filesystem>
#include <future>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <sys/sysctl.h>

extern "C" char** environ;  // POSIX doesn't guarantee a header declares it

using namespace fbx::xkit;
using namespace fbx::xshell;
using namespace fbx::shared;

XBusClient* g_shell_xbus = nullptr;  // non-static: shared with shell_panels.cc (declared extern in shell_panels.h)

// Do Not Disturb — checked before showing notification cards
bool g_dnd = false;

// Current theme state — kept in sync with ThemeService so toggles don't clobber accent
uint32_t g_accent_idx = 0;

// Recent files — prepended on each file launch, max 20 entries {display_name, full_path}
std::vector<std::pair<std::string,std::string>> g_recent_files;

static std::string recentFilesPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-recent-files"
                : "/tmp/.freebsdx-recent-files";
}
static void loadRecentFiles() noexcept {
    std::ifstream f(recentFilesPath());
    if (!f.is_open()) return;
    g_recent_files.clear();
    std::string line;
    while (std::getline(f, line) && g_recent_files.size() < 20) {
        auto sep = line.find('|');
        if (sep == std::string::npos || sep == 0) continue;
        g_recent_files.emplace_back(line.substr(0, sep), line.substr(sep + 1));
    }
}
static void saveRecentFiles() noexcept {
    std::ofstream f(recentFilesPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& [name, path] : g_recent_files)
        f << name << '|' << path << '\n';
}

// Named clipboard slots (A–E) — 5 persistent text slots
// kNumClipSlots now lives in shell_panels.h (inline constexpr, shared with ShellContext)
std::array<std::string, kNumClipSlots> g_clip_slots;

static std::string clipSlotsPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-clip-slots"
                : "/tmp/.freebsdx-clip-slots";
}
static void loadClipSlots() noexcept {
    std::ifstream f(clipSlotsPath());
    if (!f.is_open()) return;
    std::string line;
    for (int i = 0; i < kNumClipSlots && std::getline(f, line); ++i)
        g_clip_slots[i] = line;
}
void saveClipSlots() noexcept {
    std::ofstream f(clipSlotsPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& s : g_clip_slots) f << s << '\n';
}

// Recent search queries — saved/loaded to disk, max 10 entries
static std::vector<std::string> g_recent_queries;

static std::string recentQueriesPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-recent-queries"
                : "/tmp/.freebsdx-recent-queries";
}
static void loadRecentQueries() noexcept {
    std::ifstream f(recentQueriesPath());
    if (!f.is_open()) return;
    g_recent_queries.clear();
    std::string line;
    while (std::getline(f, line) && g_recent_queries.size() < 10)
        if (!line.empty()) g_recent_queries.push_back(line);
}
static void saveRecentQueries() noexcept {
    std::ofstream f(recentQueriesPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& q : g_recent_queries) f << q << '\n';
}
static void pushRecentQuery(const std::string& q) noexcept {
    if (q.empty()) return;
    g_recent_queries.erase(
        std::remove(g_recent_queries.begin(), g_recent_queries.end(), q),
        g_recent_queries.end());
    g_recent_queries.insert(g_recent_queries.begin(), q);
    if (g_recent_queries.size() > 10) g_recent_queries.resize(10);
    saveRecentQueries();
}

// Last warned battery percentage — avoids repeat notifications at same threshold
static std::atomic<int> g_last_batt_warned{100};

// Idle auto-lock — seconds since last user input; lock when >= g_idle_lock_secs
std::atomic<uint32_t> g_idle_ticks{0};
std::atomic<uint32_t> g_idle_lock_secs{300}; // 0 = never; default 5 min

// Current workspace index — updated on WorkspaceChanged broadcasts
int g_current_ws = 0;

// Current connected Wi-Fi SSID — updated on NetworkChanged / GetStatus; used to
// highlight the active network in the scan list.
std::string g_wifi_ssid;

// ── DND Schedule ─────────────────────────────────────────────────────────────
bool g_dnd_sched_on    = false;
int  g_dnd_sched_start = 22; // hour to begin DND (default 10 PM)
int  g_dnd_sched_end   = 7;  // hour to end DND   (default  7 AM)

static std::string dndSchedPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-dnd-sched"
                : "/tmp/.freebsdx-dnd-sched";
}
static void loadDndSched() noexcept {
    std::ifstream f(dndSchedPath());
    if (!f.is_open()) return;
    std::string line;
    auto readInt = [&](int& out) {
        if (!std::getline(f, line)) return;
        try { out = std::stoi(line); } catch (...) {}
    };
    int enabled = 0;
    readInt(enabled); g_dnd_sched_on = (enabled != 0);
    readInt(g_dnd_sched_start);
    readInt(g_dnd_sched_end);
}
void saveDndSched() noexcept {
    std::ofstream f(dndSchedPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << (g_dnd_sched_on ? 1 : 0) << '\n'
      << g_dnd_sched_start << '\n'
      << g_dnd_sched_end   << '\n';
}
// Returns true if the given hour falls inside the DND schedule window.
static bool dndSchedActive(int hour) noexcept {
    if (!g_dnd_sched_on) return false;
    if (g_dnd_sched_start < g_dnd_sched_end)
        return hour >= g_dnd_sched_start && hour < g_dnd_sched_end;
    return hour >= g_dnd_sched_start || hour < g_dnd_sched_end; // wraps midnight
}

// ── Night Shift Schedule ──────────────────────────────────────────────────────
bool g_ns_sched_on       = false;
int  g_ns_sched_start    = 20; // hour to enable  (default 8 PM)
int  g_ns_sched_end      =  7; // hour to disable (default 7 AM)
static int  g_ns_sched_intensity = 50; // % warmth applied when schedule enables NS

static std::string nsSchedPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-ns-sched"
                : "/tmp/.freebsdx-ns-sched";
}
static void loadNsSched() noexcept {
    std::ifstream f(nsSchedPath());
    if (!f.is_open()) return;
    std::string line;
    auto readInt = [&](int& out) {
        if (!std::getline(f, line)) return;
        try { out = std::stoi(line); } catch (...) {}
    };
    int enabled = 0;
    readInt(enabled); g_ns_sched_on = (enabled != 0);
    readInt(g_ns_sched_start);
    readInt(g_ns_sched_end);
    readInt(g_ns_sched_intensity);
}
void saveNsSched() noexcept {
    std::ofstream f(nsSchedPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << (g_ns_sched_on ? 1 : 0) << '\n'
      << g_ns_sched_start        << '\n'
      << g_ns_sched_end          << '\n'
      << g_ns_sched_intensity    << '\n';
}
static bool nsSchedActive(int hour) noexcept {
    if (!g_ns_sched_on) return false;
    if (g_ns_sched_start < g_ns_sched_end)
        return hour >= g_ns_sched_start && hour < g_ns_sched_end;
    return hour >= g_ns_sched_start || hour < g_ns_sched_end; // wraps midnight
}

// ── App child-process crash tracking ─────────────────────────────────────────
static std::mutex               g_child_pids_mtx;
static std::map<pid_t, std::string> g_child_pids; // pid → display name (capped at 64)

// ── Startup apps ──────────────────────────────────────────────────────────────
static std::set<std::string> g_startup_apps; // app identifiers to auto-launch at login

static std::string startupAppsPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-startup-apps"
                : "/tmp/.freebsdx-startup-apps";
}
static void loadStartupApps() noexcept {
    std::ifstream f(startupAppsPath());
    if (!f.is_open()) return;
    g_startup_apps.clear();
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) g_startup_apps.insert(line);
}
static void saveStartupApps() noexcept {
    std::ofstream f(startupAppsPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& id : g_startup_apps) f << id << '\n';
}

// ── Disk space alerts ─────────────────────────────────────────────────────────
// Tracks ZFS pool names currently above 90% usage so we don't repeat the warn
static std::set<std::string> g_warned_disks;

// ── Focus mode ────────────────────────────────────────────────────────────────
bool        g_focus_active = false;
std::time_t g_focus_start  = 0;

// ── Color theme presets ───────────────────────────────────────────────────────
int g_theme_preset = -1; // -1 = custom; 0-4 = one of kPresets[]

static std::string themePresetPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-theme-preset"
                : "/tmp/.freebsdx-theme-preset";
}
static void loadThemePreset() noexcept {
    std::ifstream f(themePresetPath());
    if (!f.is_open()) return;
    std::string line;
    if (std::getline(f, line)) {
        try { g_theme_preset = std::stoi(line); } catch (...) {}
    }
}
void saveThemePreset() noexcept {
    std::ofstream f(themePresetPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << g_theme_preset << '\n';
}

// ── Calendar event reminders ─────────────────────────────────────────────────
static std::set<uint32_t> g_notified_event_ids;
static int                g_notif_day = -1; // last day reminders were sent; reset set at midnight

// ── DND priority apps (exceptions) ───────────────────────────────────────────
static std::set<std::string> g_dnd_exceptions; // app names allowed through DND

static std::string dndExceptionsPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-dnd-exceptions"
                : "/tmp/.freebsdx-dnd-exceptions";
}
static void loadDndExceptions() noexcept {
    std::ifstream f(dndExceptionsPath());
    if (!f.is_open()) return;
    g_dnd_exceptions.clear();
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) g_dnd_exceptions.insert(line);
}
static void saveDndExceptions() noexcept {
    std::ofstream f(dndExceptionsPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& s : g_dnd_exceptions) f << s << '\n';
}

// ── Screen time tracking ──────────────────────────────────────────────────────
static std::mutex g_usage_mtx;
static std::map<pid_t, std::time_t>    g_child_launch_times; // pid → launch wall-clock
static std::map<std::string, uint32_t> g_app_usage_secs;     // name → total seconds today
static int g_usage_day = -1; // day of month; reset map at midnight

static std::string fmtUsage(uint32_t secs) noexcept {
    if (secs == 0) return "0s";
    if (secs < 60) return std::format("{}s", secs);
    uint32_t m = secs / 60, s = secs % 60;
    if (m < 60) return std::format("{}m {}s", m, s);
    return std::format("{}h {}m", m / 60, m % 60);
}

// ── DND missed-notification digest ───────────────────────────────────────────
static std::atomic<uint32_t>           g_dnd_suppressed_count{0};
static std::mutex                      g_dnd_digest_mtx;
static std::map<std::string, uint32_t> g_dnd_suppressed_apps; // app → suppressed count
static bool                            g_dnd_was_on = false;  // state at last 60-tick check

// ── Wallpaper rotation ───────────────────────────────────────────────────────
bool                     g_wp_rotate_on       = false;
int                      g_wp_rotate_interval = 1800; // seconds (default 30 min)
static std::string              g_wp_rotate_folder;
static std::vector<std::string> g_wp_files;
static size_t                   g_wp_rotate_idx      = 0;
int                      g_wp_rotate_countdown = 0;   // ticks until next rotation

static std::string wpRotatePath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-wp-rotate"
                : "/tmp/.freebsdx-wp-rotate";
}
static void loadWpRotate() noexcept {
    std::ifstream f(wpRotatePath());
    if (!f.is_open()) return;
    std::string line;
    auto readInt = [&](int& v) { if (std::getline(f, line)) try { v=std::stoi(line); } catch (...) {} };
    int on = 0; readInt(on); g_wp_rotate_on = (on != 0);
    readInt(g_wp_rotate_interval);
    if (std::getline(f, line)) g_wp_rotate_folder = line;
    int idx = 0; readInt(idx); g_wp_rotate_idx = static_cast<size_t>(idx);
}
void saveWpRotate() noexcept {
    std::ofstream f(wpRotatePath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << (g_wp_rotate_on ? 1 : 0) << '\n'
      << g_wp_rotate_interval << '\n'
      << g_wp_rotate_folder   << '\n'
      << static_cast<int>(g_wp_rotate_idx) << '\n';
}
static void scanWpFolder() noexcept {
    g_wp_files.clear();
    if (g_wp_rotate_folder.empty()) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(g_wp_rotate_folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        auto ext = entry.path().extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
            g_wp_files.push_back(entry.path().string());
    }
    std::sort(g_wp_files.begin(), g_wp_files.end());
    if (g_wp_rotate_idx >= g_wp_files.size()) g_wp_rotate_idx = 0;
}

// ── Quick countdown timer ─────────────────────────────────────────────────────

CountdownTimer g_countdown;
std::mutex     g_countdown_mtx;

// parseTimerInput, fmtCountdown — defined in shell_helpers.h

// ── Volume preset ────────────────────────────────────────────────────────────
static int g_vol_preset = -1; // index into {25,50,75,100}; -1 = none active

static std::string volPresetPath() noexcept {
    const char* h = getenv("HOME");
    return std::string(h ? h : "/root") + "/.freebsdx-vol-preset";
}
static void loadVolPreset() noexcept {
    std::ifstream f(volPresetPath());
    if (!f) return;
    int v = -1; f >> v;
    if (v >= -1 && v <= 3) g_vol_preset = v;
}
static void saveVolPreset() noexcept {
    std::ofstream f(volPresetPath(), std::ios::trunc);
    if (f) f << g_vol_preset << '\n';
}

// ── Battery saver mode ───────────────────────────────────────────────────────
bool     g_bat_saver           = false;
uint32_t g_bat_saver_prev_lock = 300;  // lock timeout before saver engaged

// ── Keyboard layout switcher ──────────────────────────────────────────────────

int  g_keymap_idx = 0;

static std::string keymapPath() noexcept {
    const char* h = getenv("HOME");
    return std::string(h ? h : "/root") + "/.freebsdx-keymap";
}
static void loadKeymap() noexcept {
    std::ifstream f(keymapPath());
    if (!f) return;
    int v = 0; f >> v;
    if (v >= 0 && v < kNumLayouts) g_keymap_idx = v;
}
void saveKeymap() noexcept {
    std::ofstream f(keymapPath(), std::ios::trunc);
    if (f) f << g_keymap_idx << '\n';
}

// ── DND countdown ─────────────────────────────────────────────────────────────
// Returns a human-readable string for when DND will end, or "" if DND inactive.
// dndCountdownStr() (pure inner formatter) lives in shell_helpers.h.
static std::string dndCountdownText() noexcept {
    if (!g_dnd_sched_on || !g_dnd) return "";
    std::time_t t = std::time(nullptr);
    struct tm lt{};
    localtime_r(&t, &lt);
    int cur_min = lt.tm_hour * 60 + lt.tm_min;
    int end_min = g_dnd_sched_end * 60;
    int diff    = end_min - cur_min;
    if (diff <= 0) diff += 24 * 60; // wraps past midnight
    return dndCountdownStr(diff);
}

// ── App usage heatmap ─────────────────────────────────────────────────────────
std::array<uint32_t, 24> g_app_hourly{};
static std::function<void()>    g_heat_update_fn; // wired to UI label after construction

static std::string appHeatmapPath() noexcept {
    const char* h = getenv("HOME");
    return std::string(h ? h : "/root") + "/.freebsdx-app-heatmap";
}
static void loadAppHeatmap() noexcept {
    std::ifstream f(appHeatmapPath());
    if (!f) return;
    for (int i = 0; i < 24; ++i) { uint32_t v = 0; f >> v; g_app_hourly[i] = v; }
}
void saveAppHeatmap() noexcept {
    std::ofstream f(appHeatmapPath(), std::ios::trunc);
    if (!f) return;
    for (int i = 0; i < 24; ++i) f << g_app_hourly[i] << (i < 23 ? ' ' : '\n');
}
static void recordAppLaunch() noexcept {
    std::time_t t = std::time(nullptr);
    struct tm lt{};
    localtime_r(&t, &lt);
    ++g_app_hourly[static_cast<size_t>(lt.tm_hour)];
    saveAppHeatmap();
    if (g_heat_update_fn) g_heat_update_fn();
}
// fmtHeatmap — defined in shell_helpers.h

// ── Workspace session restore ─────────────────────────────────────────────────
bool                     g_session_enabled = false;
std::vector<std::string> g_session_execs;
static bool                     g_session_restored = false;

static std::string sessionPath() noexcept {
    const char* h = getenv("HOME");
    return std::string(h ? h : "/root") + "/.freebsdx-session";
}
static void loadSession() noexcept {
    std::ifstream f(sessionPath());
    if (!f) return;
    std::string line;
    if (std::getline(f, line)) g_session_enabled = (line == "1");
    g_session_execs.clear();
    while (std::getline(f, line))
        if (!line.empty()) g_session_execs.push_back(line);
}
void saveSession() noexcept {
    std::ofstream f(sessionPath(), std::ios::trunc);
    if (!f) return;
    f << (g_session_enabled ? "1" : "0") << '\n';
    for (const auto& e : g_session_execs) f << e << '\n';
}

// parseCalInput — defined in shell_helpers.h

// ── Hotkey remapper ───────────────────────────────────────────────────────────
// Maps shell action id (InputEventType value) → {keycode, modifiers}
static std::map<uint32_t, std::pair<uint32_t,uint32_t>> g_hotkey_overrides;
static std::atomic<int> g_hotkey_capture_action{-1}; // -1 = idle; ≥0 = capturing for action

static std::string hotkeyPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-hotkeys"
                : "/tmp/.freebsdx-hotkeys";
}
// Format: action_id|keycode|modifiers  (one per line)
static void loadHotkeys() noexcept {
    std::ifstream f(hotkeyPath());
    if (!f.is_open()) return;
    g_hotkey_overrides.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        size_t a = line.find('|'), b = line.find('|', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        try {
            uint32_t act = static_cast<uint32_t>(std::stoul(line.substr(0, a)));
            uint32_t kc  = static_cast<uint32_t>(std::stoul(line.substr(a+1, b-a-1)));
            uint32_t mod = static_cast<uint32_t>(std::stoul(line.substr(b+1)));
            g_hotkey_overrides[act] = {kc, mod};
        } catch (...) {}
    }
}
static void saveHotkeys() noexcept {
    std::ofstream f(hotkeyPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& [act, km] : g_hotkey_overrides)
        f << act << '|' << km.first << '|' << km.second << '\n';
}
// Returns "Super+N" style display string for a binding
static std::string fmtHotkey(uint32_t kc, uint32_t mods) noexcept {
    std::string s;
    if (mods & 8u) s += "Super+";
    if (mods & 4u) s += "Alt+";
    if (mods & 2u) s += "Ctrl+";
    if (mods & 1u) s += "Shift+";
    static const std::pair<uint32_t,const char*> kMap[] = {
        {57,"Space"},{49,"N"},{47,"V"},{25,"P"},{53,"/"},{30,"A"},
        {31,"S"},{32,"D"},{18,"E"},{19,"R"},{20,"T"},{33,"F"},{34,"G"},
        {2,"1"},{3,"2"},{4,"3"},{5,"4"},{6,"5"},{7,"6"},{8,"7"},{9,"8"},{10,"9"},{11,"0"},
        {0, nullptr}
    };
    for (int i = 0; kMap[i].second; ++i)
        if (kMap[i].first == kc) { s += kMap[i].second; return s; }
    s += "Key" + std::to_string(kc);
    return s;
}

// ── Window rules ──────────────────────────────────────────────────────────────
struct WinRule {
    std::string title_match;          // substring matched (case-insensitive) against window title
    bool        maximize{false};
    bool        always_on_top{false};
    int         workspace{-1};        // -1 = don't assign; 0-based workspace index
};
static std::vector<WinRule>  g_win_rules;
static std::set<uint32_t>    g_rule_applied_ids; // surface_ids already processed — no re-apply

static std::string winRulesPath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-win-rules"
                : "/tmp/.freebsdx-win-rules";
}
// Format per line: title_match|maximize|always_on_top|workspace
static void loadWinRules() noexcept {
    std::ifstream f(winRulesPath());
    if (!f.is_open()) return;
    g_win_rules.clear();
    std::string line;
    auto field = [](const std::string& s, int idx) -> std::string {
        int cur = 0;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '|') {
                if (cur == idx) return s.substr(start, i - start);
                ++cur; start = i + 1;
            }
        }
        return {};
    };
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        WinRule r;
        r.title_match   = field(line, 0);
        r.maximize       = field(line, 1) == "1";
        r.always_on_top  = field(line, 2) == "1";
        try { r.workspace = std::stoi(field(line, 3)); } catch (...) { r.workspace = -1; }
        if (!r.title_match.empty()) g_win_rules.push_back(std::move(r));
    }
}
static void saveWinRules() noexcept {
    std::ofstream f(winRulesPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (const auto& r : g_win_rules)
        f << r.title_match << '|'
          << (r.maximize ? 1 : 0) << '|'
          << (r.always_on_top ? 1 : 0) << '|'
          << r.workspace << '\n';
}

// ── Notification snooze ───────────────────────────────────────────────────────
struct SnoozedNotif {
    uint32_t    id;
    std::string title;
    std::string message;
    std::string app_name;
    std::time_t wake_at{0};
};
static std::mutex                g_snooze_mtx;
static std::vector<SnoozedNotif> g_snoozed;

// ── Quick notes scratch pad ───────────────────────────────────────────────────
std::string g_scratch_note;

static std::string scratchNotePath() noexcept {
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.freebsdx-scratch"
                : "/tmp/.freebsdx-scratch";
}
static void loadScratchNote() noexcept {
    std::ifstream f(scratchNotePath());
    if (!f.is_open()) return;
    g_scratch_note.assign(std::istreambuf_iterator<char>(f),
                          std::istreambuf_iterator<char>());
    while (!g_scratch_note.empty() && g_scratch_note.back() == '\n')
        g_scratch_note.pop_back();
}
void saveScratchNote() noexcept {
    std::ofstream f(scratchNotePath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << g_scratch_note << '\n';
}

// ── System resource sparklines ────────────────────────────────────────────────
static constexpr int kSparkLen = 10;
static std::array<int, kSparkLen> g_cpu_history{};
static std::array<int, kSparkLen> g_ram_history{};
static int      g_spark_idx     = 0;    // rolling write index
static uint64_t g_prev_cp[5]   = {};   // kern.cp_time snapshot (user,nice,sys,intr,idle)
static bool     g_prev_cp_valid = false;

static int sampleCpuPct() noexcept {
    uint64_t cp[5]{};
    size_t sz = sizeof(cp);
    if (sysctlbyname("kern.cp_time", cp, &sz, nullptr, 0) != 0) return -1;
    if (!g_prev_cp_valid) {
        std::memcpy(g_prev_cp, cp, sizeof(cp));
        g_prev_cp_valid = true;
        return 0;
    }
    uint64_t idle_d  = cp[4] - g_prev_cp[4];
    uint64_t total_d = 0;
    for (int i = 0; i < 5; ++i) total_d += cp[i] - g_prev_cp[i];
    std::memcpy(g_prev_cp, cp, sizeof(cp));
    if (total_d == 0) return 0;
    return std::clamp(static_cast<int>(100 - idle_d * 100 / total_d), 0, 100);
}
static int sampleRamPct() noexcept {
    uint32_t total = 0, free_p = 0;
    size_t sz = sizeof(uint32_t);
    sysctlbyname("vm.stats.vm.v_page_count", &total, &sz, nullptr, 0);
    sz = sizeof(uint32_t);
    sysctlbyname("vm.stats.vm.v_free_count", &free_p, &sz, nullptr, 0);
    if (total == 0) return 0;
    return std::clamp(static_cast<int>((total - free_p) * 100u / total), 0, 100);
}
static std::string fmtSparkBar(const std::array<int, kSparkLen>& h, int tail) noexcept {
    static const char* kBars[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    std::string s;
    s.reserve(kSparkLen * 3); // each bar glyph is up to 3 UTF-8 bytes
    for (int i = 0; i < kSparkLen; ++i)
        s += kBars[h[(tail + i) % kSparkLen] * 7 / 100];
    return s;
}

// ── Network speed sparklines ─────────────────────────────────────────────────
// kNetSparkLen and fmtNetSpark — defined in shell_helpers.h
static std::array<uint64_t, kNetSparkLen> g_net_rx_hist{};
static std::array<uint64_t, kNetSparkLen> g_net_tx_hist{};
static int g_net_spark_idx = 0;

// ── Pomodoro session log ───────────────────────────────────────────────────────

std::vector<PomoLogEntry> g_pomo_log;
std::mutex                g_pomo_log_mtx;

static std::string pomoLogPath() noexcept {
    const char* h = getenv("HOME");
    return std::string(h ? h : "/root") + "/.freebsdx-pomo-log";
}
static void loadPomoLog() noexcept {
    std::ifstream f(pomoLogPath());
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_pomo_log_mtx);
    g_pomo_log.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        PomoLogEntry e{};
        std::istringstream ss(line);
        long long ts = 0; int ph = 0, dur = 0;
        if (ss >> ts >> ph >> dur) {
            e.ts = static_cast<std::time_t>(ts);
            e.phase = ph; e.duration = dur;
            g_pomo_log.push_back(e);
        }
    }
}
void savePomoLog() noexcept {
    std::ofstream f(pomoLogPath(), std::ios::trunc);
    if (!f) return;
    std::lock_guard<std::mutex> lk(g_pomo_log_mtx);
    for (const auto& e : g_pomo_log)
        f << static_cast<long long>(e.ts) << ' ' << e.phase << ' ' << e.duration << '\n';
}

// ── Network data usage ────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_data_today_bytes{0};
static int                   g_data_day = -1;
static int                   g_data_warned_gb = 0; // bitmask: bit0=1GB, bit1=5GB

// ── Pomodoro / Focus Timer ────────────────────────────────────────────────────
static constexpr int kPomoShort =  5 * 60;
static constexpr int kPomoLong  = 15 * 60;

std::atomic<int>  g_pomo_phase{0};   // 0=work, 1=short break, 2=long break
std::atomic<int>  g_pomo_secs{kPomoWork};
std::atomic<bool> g_pomo_running{false};
std::atomic<int>  g_pomo_cycles{0};  // completed work sessions

static std::string pomoFmt(int secs) noexcept {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", secs / 60, secs % 60);
    return buf;
}

// ── Virtual Keyboard ─────────────────────────────────────────────────────────
// Linux KEY_* codes
static constexpr uint32_t K_GRAVE=41, K_MINUS=12, K_EQUAL=13, K_BACKSPACE=14;
static constexpr uint32_t K_Q=16,K_W=17,K_E=18,K_R=19,K_T=20,K_Y=21,K_U=22,K_I=23,K_O=24,K_P=25;
static constexpr uint32_t K_LBRACK=26,K_RBRACK=27,K_ENTER=28,K_BACKSLASH=43;
static constexpr uint32_t K_A=30,K_S=31,K_D=32,K_F=33,K_G=34,K_H=35,K_J=36,K_K=37,K_L=38;
static constexpr uint32_t K_SEMI=39,K_APOS=40;
static constexpr uint32_t K_Z=44,K_X=45,K_C=46,K_V=47,K_B=48,K_N=49,K_M=50;
static constexpr uint32_t K_COMMA=51,K_DOT=52,K_SLASH=53,K_SPACE=57;
static constexpr uint32_t K_1=2,K_2=3,K_3=4,K_4=5,K_5=6,K_6=7,K_7=8,K_8=9,K_9=10,K_0=11;
// modifier bits (protocol.h: bit0=Shift, bit1=Ctrl, bit2=Alt, bit3=CapsLock)
static constexpr uint32_t MOD_SHIFT = 1u;

// Virtual keyboard layer state (toggle flags)
static bool vkb_shift = false;
static bool vkb_caps  = false;
static bool vkb_sym   = false;

// Signal bars from dBm: ≥ -50 = full, -50..-65 = good, -65..-80 = medium, < -80 = weak
std::string signalBars(int32_t dbm) noexcept {
    if (dbm >= 0) return "░░░░"; // not connected / unknown
    if (dbm > -50) return "▁▃▅▇";
    if (dbm > -65) return "▁▃▅░";
    if (dbm > -80) return "▁▃░░";
    return "▁░░░";
}

static std::string fmtBps(uint64_t bps) noexcept {
    if (bps >= 1024 * 1024)
        return std::format("{:.1f}MB/s", static_cast<double>(bps) / (1024 * 1024));
    if (bps >= 1024)
        return std::format("{:.1f}KB/s", static_cast<double>(bps) / 1024);
    return std::format("{}B/s", bps);
}

// Bundle executable → manifest identifier, filled at app discovery. Passed to
// the app as XAPP_ID so it announces itself to the compositor (per-app
// permissions are keyed by it).
static std::map<std::string, std::string> g_app_ids;

// Environment for anything the shell launches: ours + the user's overrides
// (Settings → Environment) + the two session vars and `session_extra`
// ("KEY=value"), which always win. XAPP_ID is never inherited.
static std::vector<std::string> childEnv(const std::vector<std::string>& session_extra = {}) {
    std::vector<std::string> env_strs;
    auto overridden = [](std::string_view kv, std::string_view key) {
        return kv.size() > key.size() && kv.substr(0, key.size()) == key && kv[key.size()] == '=';
    };
    std::vector<std::string> keys{"WAYLAND_DISPLAY", "XBUS_SOCKET", "XAPP_ID"};
    std::vector<std::string> extra{"WAYLAND_DISPLAY=wayland-0",
                                   "XBUS_SOCKET=/tmp/freebsdx.sock"};
    for (const auto& kv : session_extra) {
        keys.push_back(kv.substr(0, kv.find('=')));
        extra.push_back(kv);
    }
    if (const char* home = std::getenv("HOME")) {
        std::ifstream f(fbx::shared::userenv::filePath(home));
        std::stringstream body;
        body << f.rdbuf();
        for (auto& v : fbx::shared::userenv::parse(body.str())) {
            if (v.key == "WAYLAND_DISPLAY" || v.key == "XBUS_SOCKET" || v.key == "XAPP_ID") continue;
            keys.push_back(v.key);
            extra.push_back(v.key + "=" + v.value);
        }
    }
    for (char** e = environ; e && *e; ++e) {
        bool skip = false;
        for (const auto& k : keys) skip = skip || overridden(*e, k);
        if (!skip) env_strs.emplace_back(*e);
    }
    env_strs.insert(env_strs.end(), extra.begin(), extra.end());
    return env_strs;
}

// fork + execve `args` with childEnv(). argv/envp are built before fork: the
// child of a threaded process may only call async-signal-safe functions, so no
// setenv/malloc after fork. Returns the pid, or -1.
static pid_t spawnChild(std::vector<std::string> args,
                        const std::vector<std::string>& session_extra = {}) {
    std::vector<std::string> env_strs = childEnv(session_extra);
    std::vector<char*> envp, argv;
    for (auto& e : env_strs) envp.push_back(e.data());
    envp.push_back(nullptr);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == 0) {
        // Close every FD above stderr so the child starts with a clean table.
        // The app will open its own XBus socket and Vulkan device from scratch.
        closefrom(STDERR_FILENO + 1);
        execve(argv[0], argv.data(), envp.data());
        static const char kMsg[] = "[xshell] execve failed\n";
        (void)!write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
        _exit(1);
    }
    return pid;
}

void launchApp(const std::string& exec_path,
                      const std::string& file_path) noexcept {
    LOG_INFO("Launching: {}{}", exec_path, file_path.empty() ? "" : " --open " + file_path);
    recordAppLaunch();
    std::vector<std::string> args{exec_path};
    if (!file_path.empty()) { args.push_back("--open"); args.push_back(file_path); }
    std::vector<std::string> session_extra;
    if (auto it = g_app_ids.find(exec_path); it != g_app_ids.end())
        session_extra.push_back("XAPP_ID=" + it->second);
    pid_t pid = spawnChild(std::move(args), session_extra);
    if (pid > 0) {
        // Parent: track child for crash detection (cap at 64 to avoid unbounded growth)
        std::string display = std::filesystem::path(exec_path).filename().string();
        std::lock_guard<std::mutex> lk(g_child_pids_mtx);
        if (g_child_pids.size() < 64) {
            g_child_pids.emplace(pid, std::move(display));
            g_child_launch_times.emplace(pid, std::time(nullptr));
        }
    }
}

// Launch the user's enabled XDG autostart entries (~/.config/autostart, managed
// by Settings → Startup Items) once per compositor session: a marker newer
// than the compositor's socket means this session already ran them, so an
// xshell restart doesn't launch everything twice. Not tracked for crash
// notifications — autostart helpers often exit on purpose.
static void runAutostart() {
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string marker = std::string("/tmp/freebsdx-autostart.") + std::to_string(getuid());
    struct stat sock_st{}, mark_st{};
    if (::stat("/tmp/freebsdx.sock", &sock_st) == 0 && ::lstat(marker.c_str(), &mark_st) == 0 &&
        S_ISREG(mark_st.st_mode) && mark_st.st_uid == getuid() &&
        mark_st.st_mtime >= sock_st.st_mtime) {
        LOG_INFO("Autostart: already ran this session");
        return;
    }
    int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) { ::futimens(fd, nullptr); ::close(fd); }

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& de : std::filesystem::directory_iterator(fbx::shared::autostart::dirPath(home), ec))
        if (fbx::shared::autostart::isEntryFile(de.path().filename().string()) && de.is_regular_file(ec))
            files.push_back(de.path());
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream f(path);
        std::stringstream body;
        body << f.rdbuf();
        fbx::shared::autostart::Entry e;
        if (!fbx::shared::autostart::parse(body.str(), e) || !fbx::shared::autostart::shouldLaunch(e))
            continue;
        // A bundle app (Settings → Login Items): launch it like the Launchpad
        // does, so it gets its current binary and its XAPP_ID.
        if (!e.app_id.empty()) {
            auto it = std::find_if(g_app_ids.begin(), g_app_ids.end(),
                                   [&](const auto& kv) { return kv.second == e.app_id; });
            if (it != g_app_ids.end()) {
                LOG_INFO("Autostart: {} (app {})", e.name, e.app_id);
                launchApp(it->first);
                continue;
            }
            LOG_WARN("Autostart: app {} isn't installed; trying its Exec", e.app_id);
        }
        std::string cmd = fbx::shared::autostart::commandLine(e.exec);
        if (cmd.empty()) continue;
        LOG_INFO("Autostart: {} ({})", e.name.empty() ? path.filename().string() : e.name, cmd);
        spawnChild({"/bin/sh", "-c", cmd});
    }
}

struct SettingsKw {
    const char* keyword;
    const char* label;
};
static const SettingsKw kSettingsKeywords[] = {
    {"display",       "Display Settings"},
    {"brightness",    "Brightness"},
    {"resolution",    "Display Resolution"},
    {"scale",         "HiDPI / Scaling"},
    {"hdr",           "HDR Output"},
    {"vrr",           "Variable Refresh Rate"},
    {"night",         "Night Shift"},
    {"dark",          "Dark Mode"},
    {"theme",         "Appearance & Theme"},
    {"accent",        "Accent Color"},
    {"wallpaper",     "Wallpaper"},
    {"font",          "Fonts"},
    {"wifi",          "Wi-Fi"},
    {"network",       "Network"},
    {"ethernet",      "Ethernet"},
    {"vpn",           "VPN"},
    {"bluetooth",     "Bluetooth"},
    {"volume",        "Sound & Volume"},
    {"audio",         "Sound & Volume"},
    {"microphone",    "Microphone"},
    {"speaker",       "Speaker Output"},
    {"battery",       "Power & Battery"},
    {"sleep",         "Sleep Settings"},
    {"power",         "Power Management"},
    {"backlight",     "Screen Brightness"},
    {"keyboard",      "Keyboard"},
    {"mouse",         "Mouse & Trackpad"},
    {"touchscreen",   "Touchscreen"},
    {"gamepad",       "Gamepad / Controller"},
    {"lock",          "Lock Screen"},
    {"password",      "Login & Password"},
    {"user",          "User Account"},
    {"language",      "Language & Region"},
    {"accessibility", "Accessibility"},
    {"screen reader", "Screen Reader"},
    {"ime",           "Input Method"},
    {"print",         "Printers & Scanning"},
    {"update",        "Software Updates"},
    {"remote",        "Remote Desktop"},
    {"rdp",           "Remote Desktop"},
    {"vnc",           "Remote Desktop"},
    {"camera",        "Camera"},
    {"screenshot",    "Screenshot"},
    {"storage",       "Storage & ZFS"},
    {"zfs",           "ZFS Snapshots"},
    {"sandbox",       "App Sandbox"},
    {"security",      "Security & Privacy"},
    {"notification",  "Notifications"},
    {"dnd",           "Do Not Disturb"},
    {"seat",          "Multi-Seat"},
    {"profile",       "Power Profile"},
    {nullptr, nullptr}
};

// Returns non-empty string with result if expr is a simple arithmetic expression,
// empty if it can't be evaluated (don't attempt complex parsing — just strtod + +-*/).
// MathParser / quickMathEval / searchFiles were carved out to shell_query.h.

struct NotifEntry {
    uint32_t                     id{0};
    std::string                  title;
    std::string                  message;
    std::string                  time_str;   // "HH:MM" at arrival
    std::shared_ptr<FlexColumn>  card;       // the card widget
    bool                         dismissed{false};
};

// ── Shell context (main() decomposition) ─────────────────────────────────────


// ── Data-driven "switch tile" (main() decomposition — prototype) ─────────────
// A large family of quick-settings tiles share one exact shape: a SwitchWidget
// bound to a service's SetState (payload = a Request with a uint32 enabled_), a
// transient "Saved ✓" status label, a detail line derived from the broadcast
// State, and an update_X_ui(State) the dispatcher calls. Capture that shape once
// so each tile is a few lines (State/Request types, service, opcode, detail
// formatter) instead of the ~22-line inline block. Type-safe: State/Request are
// template params, so the returned updater keeps the exact typed signature the
// existing dispatcher calls — downstream code is untouched (alias trick, as with
// the panel builders).
template <class State>
struct SwitchTileHandles {
    std::shared_ptr<SwitchWidget>     sw;
    std::shared_ptr<TextWidget>       detail_lbl;
    std::shared_ptr<TextWidget>       status_lbl;
    std::function<void(const State&)> update;
};

template <class State, class Request>
static SwitchTileHandles<State>
buildSwitchTile(const char* service, uint32_t set_opcode,
                std::function<std::string(const State&)> detail) {
    SwitchTileHandles<State> h;
    h.sw = std::make_shared<SwitchWidget>(false);
    h.detail_lbl = std::make_shared<TextWidget>("", 10.0f);
    h.detail_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    h.status_lbl = std::make_shared<TextWidget>("", 10.0f);
    h.status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});

    auto        sw         = h.sw;
    auto        status_lbl = h.status_lbl;
    std::string svc        = service;
    h.sw->setOnChanged([sw, status_lbl, svc, set_opcode](bool) {
        if (!g_shell_xbus) return;
        Request req{};
        req.enabled_ = sw->value() ? 1u : 0u;
        g_shell_xbus->Call(svc.c_str(), set_opcode,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [status_lbl](const XBusMessage&) { status_lbl->setText("Saved \xe2\x9c\x93"); });
    });

    auto detail_lbl = h.detail_lbl;
    h.update = [sw, detail_lbl, detail](const State& st) {
        sw->setValue(st.enabled_ != 0);
        detail_lbl->setText(detail(st));
    };
    return h;
}

// The other large tile family: a "state-label toggle" — a SwitchWidget plus one
// status label that shows on/off text (and on/off colours) reflecting the live
// state, with NO transient "Saved ✓" (the Call passes a null callback). Both the
// onChanged handler and the update_X_ui share the same text/colour logic. The
// single uint32 request field is addressed by pointer-to-member so this covers
// both `enabled_`-style and differently-named fields (e.g. ScrollDir's natural_).
// Pass equal on/off colours for tiles that only change the text.
template <class State>
struct ToggleTileHandles {
    std::shared_ptr<SwitchWidget>     sw;
    std::shared_ptr<TextWidget>       status_lbl;
    std::function<void(const State&)> update;
};

template <class State, class Request>
static ToggleTileHandles<State>
buildToggleTile(const char* service, uint32_t set_opcode, bool initial,
                uint32_t Request::* field, std::function<bool(const State&)> get,
                const char* on_text, Color on_color,
                const char* off_text, Color off_color) {
    ToggleTileHandles<State> h;
    h.sw = std::make_shared<SwitchWidget>(initial);
    h.status_lbl = std::make_shared<TextWidget>(initial ? on_text : off_text, 11.0f);
    h.status_lbl->setColor(initial ? on_color : off_color);

    auto status_lbl = h.status_lbl;
    auto apply = [status_lbl, on_text, on_color, off_text, off_color](bool on) {
        status_lbl->setText(on ? on_text : off_text);
        status_lbl->setColor(on ? on_color : off_color);
    };

    std::string svc = service;
    h.sw->setOnChanged([apply, svc, set_opcode, field](bool on) {
        apply(on);
        if (!g_shell_xbus) return;
        Request req{};
        req.*field = on ? 1u : 0u;
        g_shell_xbus->Call(svc.c_str(), set_opcode,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    auto sw = h.sw;
    h.update = [sw, apply, get](const State& st) {
        bool on = get(st);
        sw->setValue(on);
        apply(on);
    };
    return h;
}

// The compound "switch + slider" family: a SwitchWidget plus one SliderWidget
// driving a Request with TWO uint32 fields (enabled_ + a value, both addressed
// by pointer-to-member so oddly-named fields work), a value label formatted from
// the slider position, a transient "Saved ✓" status, and — for tiles that show
// one — an info line derived from State. Both controls funnel through one send().
// This is buildSwitchTile's shape with an extra-controls hook: the slider + its
// value formatter + an optional info formatter. Pass info_fmt = nullptr for tiles
// with no info line (e.g. Mono Audio, Stage Manager) — then info_lbl stays null.
template <class State>
struct SliderTileHandles {
    std::shared_ptr<SwitchWidget>     sw;
    std::shared_ptr<SliderWidget>     slider;
    std::shared_ptr<TextWidget>       value_lbl;
    std::shared_ptr<TextWidget>       status_lbl;
    std::shared_ptr<TextWidget>       info_lbl;   // null when info_fmt == nullptr
    std::function<void(const State&)> update;
};

template <class State, class Request>
static SliderTileHandles<State>
buildSliderTile(const char* service, uint32_t set_opcode, bool initial_on,
                uint32_t Request::* en_field, uint32_t Request::* val_field,
                float smin, float smax, float sinit,
                std::function<std::string(uint32_t)> value_fmt,
                std::function<bool(const State&)> get_on,
                std::function<uint32_t(const State&)> get_val,
                std::function<std::string(const State&)> info_fmt) {
    SliderTileHandles<State> h;
    h.sw = std::make_shared<SwitchWidget>(initial_on);
    h.slider = std::make_shared<SliderWidget>(sinit, smin, smax);
    h.slider->setFixedSize(140, 18);
    h.value_lbl = std::make_shared<TextWidget>(value_fmt(static_cast<uint32_t>(sinit)), 11.0f);
    h.value_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    h.status_lbl = std::make_shared<TextWidget>("", 10.0f);
    h.status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});
    if (info_fmt) {
        h.info_lbl = std::make_shared<TextWidget>("", 10.0f);
        h.info_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    }

    auto        sw         = h.sw;
    auto        slider     = h.slider;
    auto        value_lbl  = h.value_lbl;
    auto        status_lbl = h.status_lbl;
    std::string svc        = service;
    auto send = [sw, status_lbl, svc, set_opcode, en_field, val_field](uint32_t val) {
        if (!g_shell_xbus) return;
        Request req{};
        req.*en_field  = sw->value() ? 1u : 0u;
        req.*val_field = val;
        g_shell_xbus->Call(svc.c_str(), set_opcode,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [status_lbl](const XBusMessage&) { status_lbl->setText("Saved \xe2\x9c\x93"); });
    };
    h.sw->setOnChanged([send, slider](bool) { send(static_cast<uint32_t>(slider->value())); });
    h.slider->setOnChanged([value_lbl, value_fmt, send](float v) {
        uint32_t iv = static_cast<uint32_t>(v);
        value_lbl->setText(value_fmt(iv));
        send(iv);
    });

    auto info_lbl = h.info_lbl;
    h.update = [sw, slider, value_lbl, info_lbl, value_fmt, get_on, get_val, info_fmt]
               (const State& st) {
        sw->setValue(get_on(st));
        uint32_t v = get_val(st);
        slider->setValue(static_cast<float>(v));
        value_lbl->setText(value_fmt(v));
        if (info_lbl && info_fmt) info_lbl->setText(info_fmt(st));
    };
    return h;
}

// The compound "switch + mode-button row" family: a SwitchWidget plus a row of
// mutually-exclusive option buttons (a selected index) driving a Request with an
// enabled_ field + an index field (both pointer-to-member). Buttons highlight via
// the shared hl() convention (dim/hi when on, all-off when the switch is off), a
// transient "Saved ✓" status, and — for tiles that show one — a detail label with
// per-tile update logic (e.g. Spatial Audio's conditional device name). Pass
// detail_initial = nullptr (and detail_update = nullptr) for tiles with no detail
// line (e.g. Do Not Disturb). `Request` may be any packed struct (Spatial Audio
// sends a SpatialAudioConfig). req_extra, if given, sets any constant/extra
// request fields before the Call (e.g. Live Captions' fixed font_size_).
template <class State>
struct ButtonRowTileHandles {
    std::shared_ptr<SwitchWidget>                               sw;
    std::shared_ptr<std::vector<std::shared_ptr<ButtonWidget>>> btns;
    std::shared_ptr<TextWidget>                                 status_lbl;
    std::shared_ptr<TextWidget>                                 detail_lbl;  // null w/o detail
    std::function<void(const State&)>                           update;
};

template <class State, class Request>
static ButtonRowTileHandles<State>
buildButtonRowTile(const char* service, uint32_t set_opcode, bool initial_on,
                   uint32_t Request::* en_field, uint32_t Request::* idx_field,
                   std::vector<const char*> labels, int btn_w, int btn_h,
                   std::function<bool(const State&)> get_on,
                   std::function<uint32_t(const State&)> get_idx,
                   const char* detail_initial,
                   std::function<void(const State&, TextWidget&)> detail_update,
                   std::function<void(Request&)> req_extra = nullptr) {
    ButtonRowTileHandles<State> h;
    h.sw = std::make_shared<SwitchWidget>(initial_on);
    h.status_lbl = std::make_shared<TextWidget>("", 10.0f);
    h.status_lbl->setColor({0.50f, 0.80f, 0.50f, 1.0f});
    if (detail_initial) {
        h.detail_lbl = std::make_shared<TextWidget>(detail_initial, 10.0f);
        h.detail_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    }
    h.btns = std::make_shared<std::vector<std::shared_ptr<ButtonWidget>>>();
    for (const char* lbl : labels) {
        auto b = std::make_shared<ButtonWidget>(lbl);
        b->setFixedSize(btn_w, btn_h);
        h.btns->push_back(b);
    }

    auto cur        = std::make_shared<uint32_t>(0u);
    auto btns       = h.btns;
    auto sw         = h.sw;
    auto status_lbl = h.status_lbl;
    auto hl = [btns, cur, sw]() {
        Color hi  = {0.25f, 0.50f, 0.80f, 0.90f};
        Color dim = {0.22f, 0.22f, 0.28f, 0.90f};
        Color off = {0.16f, 0.16f, 0.18f, 0.60f};
        bool  en  = sw->value();
        for (size_t i = 0; i < btns->size(); ++i)
            (*btns)[i]->setBackgroundColor(
                !en ? off : static_cast<uint32_t>(i) == *cur ? hi : dim);
    };
    hl();

    std::string svc = service;
    auto send = [sw, cur, status_lbl, svc, set_opcode, en_field, idx_field, req_extra]() {
        if (!g_shell_xbus) return;
        Request req{};
        req.*en_field  = sw->value() ? 1u : 0u;
        req.*idx_field = *cur;
        if (req_extra) req_extra(req);
        g_shell_xbus->Call(svc.c_str(), set_opcode,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [status_lbl](const XBusMessage&) { status_lbl->setText("Saved \xe2\x9c\x93"); });
    };

    h.sw->setOnChanged([hl, send](bool) { hl(); send(); });
    for (size_t m = 0; m < h.btns->size(); ++m) {
        (*h.btns)[m]->onClick([m, cur, hl, send]() {
            *cur = static_cast<uint32_t>(m);
            hl();
            send();
        });
    }

    auto   detail_lbl = h.detail_lbl;
    size_t n          = h.btns->size();
    h.update = [sw, cur, hl, detail_lbl, get_on, get_idx, detail_update, n](const State& st) {
        sw->setValue(get_on(st));
        uint32_t idx = get_idx(st);
        *cur = idx < n ? idx : 0u;
        if (detail_lbl && detail_update) detail_update(st, *detail_lbl);
        hl();
    };
    return h;
}

// The "Enable/Disable button-pair" family: a status label plus an Enable button
// (shown when off) and a Disable button (shown when on); update() flips their
// visibility and rewrites the status line from the broadcast State. The click
// ACTIONS are caller lambdas (invoked only when g_shell_xbus is live), so this
// covers both the one-opcode Request{enabled_=1/0} style (Firewall, Ad-Blocker,
// Hotspot) and the two-separate-opcode style (Remote Desktop's Enable/Disable).
// The buttons are created unstyled at 72x28 — the caller sets bg/fg colours on
// the returned handles (Ad-Blocker uses setColor where the others use
// setBackgroundColor) and keeps any extra controls (config inputs, action
// buttons). extra_update(State), if given, runs after for per-tile side effects
// (e.g. Ad-Blocker's detail line, Hotspot pre-filling the SSID field).
template <class State>
struct EnableDisableTileHandles {
    std::shared_ptr<TextWidget>       status_lbl;
    std::shared_ptr<ButtonWidget>     enable_btn;
    std::shared_ptr<ButtonWidget>     disable_btn;
    std::function<void(const State&)> update;
};

template <class State>
static EnableDisableTileHandles<State>
buildEnableDisableTile(const char* disabled_text, Color disabled_color, Color enabled_color,
                       std::function<void()> on_enable, std::function<void()> on_disable,
                       std::function<bool(const State&)> get_on,
                       std::function<std::string(const State&)> enabled_text,
                       std::function<void(const State&)> extra_update = nullptr) {
    EnableDisableTileHandles<State> h;
    h.status_lbl = std::make_shared<TextWidget>(disabled_text, 11.0f);
    h.status_lbl->setColor(disabled_color);
    h.enable_btn = std::make_shared<ButtonWidget>("Enable",
        [on_enable]() { if (g_shell_xbus && on_enable) on_enable(); });
    h.enable_btn->setFixedSize(72, 28);
    h.disable_btn = std::make_shared<ButtonWidget>("Disable",
        [on_disable]() { if (g_shell_xbus && on_disable) on_disable(); });
    h.disable_btn->setFixedSize(72, 28);
    h.disable_btn->setVisibility(false);

    auto        status_lbl  = h.status_lbl;
    auto        enable_btn  = h.enable_btn;
    auto        disable_btn = h.disable_btn;
    std::string dtext       = disabled_text;
    h.update = [status_lbl, enable_btn, disable_btn, dtext, disabled_color, enabled_color,
                get_on, enabled_text, extra_update](const State& st) {
        bool on = get_on(st);
        if (on) {
            status_lbl->setText(enabled_text(st));
            status_lbl->setColor(enabled_color);
        } else {
            status_lbl->setText(dtext);
            status_lbl->setColor(disabled_color);
        }
        enable_btn->setVisibility(!on);
        disable_btn->setVisibility(on);
        if (extra_update) extra_update(st);
    };
    return h;
}



int main() {
    signal(SIGCHLD, SIG_IGN);
    ShellContext ctx;

    Logger::init("xshell");
    LOG_INFO("Initializing FreeBSD-X Desktop Shell...");
    fbx::xrender::FontManager::init();

    AppDiscoveryService discovery;
    auto apps = discovery.discoverApps("./apps");
    if (apps.empty()) apps = discovery.discoverApps("../apps");
    for (const auto& a : apps)
        if (!a.identifier.empty()) g_app_ids[a.executable_path] = a.identifier;

    // Launchpad — WrapRowWidget gives a true icon grid.
    std::vector<std::shared_ptr<Widget>> app_icons;
    app_icons.reserve(apps.size());
    // Assigned once start_menu exists (below); lets an app-icon click dismiss the
    // launcher so the newly launched window isn't left behind the open menu.
    auto close_launcher = std::make_shared<std::function<void()>>();
    for (const auto& app : apps) {
        auto icon = std::make_shared<ImageWidget>(app.icon_path);
        icon->setFixedSize(64, 64);
        auto lbl = std::make_shared<TextWidget>(app.name, 12.0f);
        lbl->setColor({1.0f, 1.0f, 1.0f, 0.9f});
        lbl->setAlignment(Alignment::Center);
        auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            icon, std::make_shared<Spacer>(1, 6), lbl
        });
        col->setAlignment(Alignment::Center);
        std::string exec = app.executable_path;
        auto btn = std::make_shared<ButtonWidget>(col, [exec, close_launcher](){
            launchApp(exec);
            if (*close_launcher) (*close_launcher)();
        });
        btn->setFixedSize(110, 110);
        btn->setPadding(12);
        app_icons.push_back(btn);
    }

    auto icon_grid = std::make_shared<WrapRowWidget>(std::move(app_icons));
    auto start_menu_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("Applications", 20.0f),
        std::make_shared<Spacer>(1, 16),
        icon_grid
    });
    start_menu_col->setPadding(28);
    // Stretch children to the column's (fixed) width so the icon grid receives a
    // real bound to wrap and center within, rather than the 100000px cross-axis
    // sentinel a non-stretch column hands down.
    start_menu_col->setCrossAxisAlignment(CrossAxisAlignment::Stretch);
    auto start_menu = std::make_shared<ScrollViewWidget>(start_menu_col);
    start_menu->setBackgroundColor({0.11f, 0.13f, 0.18f, 0.96f});
    start_menu->setBorderRadius(20.0f);
    start_menu->setFixedSize(520, 880);  // tall enough to show all apps (4-col grid)
    start_menu->setVisibility(false);
    *close_launcher = [start_menu]() { start_menu->setVisibility(false); };

    // Taskbar
    auto logo   = std::make_shared<ImageWidget>("core/shell/Resources/Branding/logo_x.svg");
    logo->setFixedSize(24, 24);
    // Just the X logo — the "FREEBSD-X" wordmark was removed to keep the start
    // button compact.
    auto start_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        logo
    });
    start_row->setCrossAxisAlignment(CrossAxisAlignment::Center);
    auto start_btn = std::make_shared<ButtonWidget>(start_row,
        [&start_menu](){ start_menu->setVisibility(!start_menu->isVisible()); });
    // Taskbar chrome is always dark; without an explicit background the button
    // paints theme.surface (white in the light theme) and swallows the logo.
    // Match the clock/weather buttons and stay transparent over the dark bar.
    start_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    // Square-ish box just wide enough for the 24px logo plus padding.
    start_btn->setFixedSize(48, 40);

    auto clock_w = std::make_shared<TextWidget>("--:--", 16.0f);
    clock_w->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    // clock_btn wraps clock_w; callback wired after cal_popover is built below
    auto clock_btn = std::make_shared<ButtonWidget>(clock_w, [](){});
    clock_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    clock_btn->setPadding(6);
    auto cpu_w   = std::make_shared<TextWidget>("CPU: --%", 13.0f);
    cpu_w->setColor({0.85f, 0.85f, 0.85f, 1.0f});
    auto mem_w   = std::make_shared<TextWidget>("MEM: --MB", 13.0f);
    mem_w->setColor({0.85f, 0.85f, 0.85f, 1.0f});

    // ── Network + Wi-Fi ── (carved into buildNetworkWifiPanel; status widgets +
    // sparklines + scan list + connect col + scan loader + disconnect btn escape —
    // the Wi-Fi connect-form widgets stay builder-local. Aliases keep the card/
    // broadcast/timer/OnConnect refs unchanged.)
    buildNetworkWifiPanel(ctx);
    auto& qp_conn_w        = ctx.qp_conn_w;
    auto& qp_ip_w          = ctx.qp_ip_w;
    auto& qp_signal_w      = ctx.qp_signal_w;
    auto& qp_bw_w          = ctx.qp_bw_w;
    auto& net_rx_spark_lbl = ctx.net_rx_spark_lbl;
    auto& net_tx_spark_lbl = ctx.net_tx_spark_lbl;
    auto& qp_net_list      = ctx.qp_net_list;
    auto& wifi_connect_col = ctx.wifi_connect_col;
    auto& do_scan_page     = ctx.do_scan_page;
    auto& qp_disc_btn      = ctx.qp_disc_btn;

    // ── Bluetooth device panel ── (carved into buildBluetoothDevicePanel; device
    // list/status + rebuild + scan-btn + adapter toggle (state/enable/disable/
    // toggle-row) + update_bt_ui escape — the paged PageFetcher stays builder-local.
    // Aliases keep card/broadcast/reply/timer refs unchanged.)
    buildBluetoothDevicePanel(ctx);
    auto& bt_devices_col     = ctx.bt_devices_col;
    auto& bt_status_lbl      = ctx.bt_status_lbl;
    auto& rebuild_bt_devices = ctx.rebuild_bt_devices;
    auto& bt_scan_btn        = ctx.bt_scan_btn;
    auto& bt_state_lbl       = ctx.bt_state_lbl;
    auto& bt_enable_btn      = ctx.bt_enable_btn;
    auto& bt_disable_btn     = ctx.bt_disable_btn;
    auto& bt_toggle_row      = ctx.bt_toggle_row;
    auto& update_bt_ui       = ctx.update_bt_ui;

    // ── VPN quick-connect ── (carved into buildVpnPanel; status/info + iface input
    // + connect/disc buttons + btn-row + update_vpn_ui escape. Aliases keep card/
    // broadcast/reply/timer refs unchanged.)
    buildVpnPanel(ctx);
    auto& vpn_status_lbl  = ctx.vpn_status_lbl;
    auto& vpn_info_lbl    = ctx.vpn_info_lbl;
    auto& vpn_iface_input = ctx.vpn_iface_input;
    auto& vpn_conn_btn    = ctx.vpn_conn_btn;
    auto& vpn_disc_btn    = ctx.vpn_disc_btn;
    auto& vpn_btn_row     = ctx.vpn_btn_row;
    auto& update_vpn_ui   = ctx.update_vpn_ui;

    // ── Game Mode ── (carved into buildGameModePanel; aliases keep downstream
    // card assembly + broadcast/timer capture lists unchanged)
    buildGameModePanel(ctx);
    auto& gm_status_lbl  = ctx.gm_status_lbl;
    auto& gm_app_lbl     = ctx.gm_app_lbl;
    auto& gm_enable_btn  = ctx.gm_enable_btn;
    auto& gm_disable_btn = ctx.gm_disable_btn;
    auto& gm_btn_row     = ctx.gm_btn_row;
    auto& update_gm_ui   = ctx.update_gm_ui;

    // ── Wi-Fi Hotspot ─────────────────────────────────────────────────────────
    // Wi-Fi Hotspot (Enable/Disable button-pair; status = SSID + client count;
    // extra_update pre-fills the SSID field from the running AP)
    auto& hs_ssid_input = ctx.hs_ssid_input;  // ctx field; populated by buildHotspotConfigPanel below, captured by extra_update
    auto hs = buildEnableDisableTile<HotspotStatus>(
        "Off", {0.5f, 0.5f, 0.55f, 1.0f}, {0.3f, 0.9f, 0.5f, 1.0f},
        []() { HotspotEnableRequest req{}; req.enabled_ = 1;
               g_shell_xbus->Call("com.freebsdx.Hotspot",
                   static_cast<uint32_t>(HotspotOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        []() { HotspotEnableRequest req{}; req.enabled_ = 0;
               g_shell_xbus->Call("com.freebsdx.Hotspot",
                   static_cast<uint32_t>(HotspotOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        [](const HotspotStatus& st) { return st.enabled_ != 0; },
        [](const HotspotStatus& st) {
            return std::format("● {}  ·  {} client{}", st.ssid_[0] ? st.ssid_ : "AP",
                st.client_count_, st.client_count_ == 1u ? "" : "s");
        },
        [&hs_ssid_input](const HotspotStatus& st) {
            if (st.enabled_ && st.ssid_[0] && hs_ssid_input->getText().empty())
                hs_ssid_input->setText(std::string(st.ssid_));
        });
    auto& hs_status_lbl  = hs.status_lbl;
    auto& hs_enable_btn  = hs.enable_btn;
    auto& hs_disable_btn = hs.disable_btn;
    auto& update_hs_ui   = hs.update;
    hs_enable_btn->setBackgroundColor({0.18f, 0.38f, 0.22f, 1.0f});
    hs_disable_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});

    auto hs_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        hs_enable_btn, std::make_shared<Spacer>(6, 1), hs_disable_btn
    });

    // ── Wi-Fi Hotspot config form ── (carved into buildHotspotConfigPanel;
    // hs_ssid_input (already aliased above) + hs_cfg_row1/row2 escape — hs_pass_input
    // + Apply button stay builder-local. Aliases keep the card refs unchanged.)
    buildHotspotConfigPanel(ctx);
    auto& hs_cfg_row1 = ctx.hs_cfg_row1;
    auto& hs_cfg_row2 = ctx.hs_cfg_row2;

    // ── Screen Cast ── (carved into buildScreenCastPanel; status/elapsed + receiver
    // col + scan/stop btns + ctrl-row + rebuild + update_cast_ui escape — the
    // CastFetcher stays builder-local. Aliases keep card/broadcast/reply/timer refs
    // unchanged.)
    buildScreenCastPanel(ctx);
    auto& cast_status_lbl         = ctx.cast_status_lbl;
    auto& cast_elapsed_lbl        = ctx.cast_elapsed_lbl;
    auto& cast_receiver_col       = ctx.cast_receiver_col;
    auto& cast_scan_btn           = ctx.cast_scan_btn;
    auto& cast_stop_btn           = ctx.cast_stop_btn;
    auto& cast_ctrl_row           = ctx.cast_ctrl_row;
    auto& rebuild_cast_receivers  = ctx.rebuild_cast_receivers;
    auto& update_cast_ui          = ctx.update_cast_ui;

    // ── Remote Desktop ────────────────────────────────────────────────────────
    // Remote Desktop (Enable/Disable button-pair; separate Enable/Disable opcodes,
    // keyed off st.running_; status = VNC port + client count + lock glyph)
    auto rd = buildEnableDisableTile<RemoteDesktopStatus>(
        "Off", {0.5f, 0.5f, 0.55f, 1.0f}, {0.3f, 0.9f, 0.5f, 1.0f},
        []() { g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                   static_cast<uint32_t>(RemoteDesktopOpcode::Enable), {}, nullptr); },
        []() { g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                   static_cast<uint32_t>(RemoteDesktopOpcode::Disable), {}, nullptr); },
        [](const RemoteDesktopStatus& st) { return st.running_ != 0; },
        [](const RemoteDesktopStatus& st) {
            std::string s = std::format("● Port {}  ·  {} client{}",
                st.port_, st.client_count_, st.client_count_ == 1 ? "" : "s");
            if (st.password_set_) s += "  🔒";
            return s;
        });
    auto& rd_status_lbl  = rd.status_lbl;
    auto& rd_enable_btn  = rd.enable_btn;
    auto& rd_disable_btn = rd.disable_btn;
    auto& update_rd_ui   = rd.update;
    rd_enable_btn->setBackgroundColor({0.18f, 0.38f, 0.22f, 1.0f});
    rd_disable_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});

    auto rd_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        rd_enable_btn, std::make_shared<Spacer>(6, 1), rd_disable_btn
    });
    // ── Remote Desktop password form ── (carved into buildRemoteDesktopPassForm;
    // only rd_pass_row escapes — pass input + Set button stay builder-local.)
    buildRemoteDesktopPassForm(ctx);
    auto& rd_pass_row = ctx.rd_pass_row;

    // ── Firewall quick-tile ───────────────────────────────────────────────────
    // Firewall (Enable/Disable button-pair; status = rule count)
    auto fw = buildEnableDisableTile<FirewallStatus>(
        "Disabled", {0.5f, 0.5f, 0.55f, 1.0f}, {0.3f, 0.9f, 0.5f, 1.0f},
        []() { FirewallEnableRequest req{}; req.enabled_ = 1;
               g_shell_xbus->Call("com.freebsdx.Firewall",
                   static_cast<uint32_t>(FirewallOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        []() { FirewallEnableRequest req{}; req.enabled_ = 0;
               g_shell_xbus->Call("com.freebsdx.Firewall",
                   static_cast<uint32_t>(FirewallOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        [](const FirewallStatus& st) { return st.enabled_ != 0; },
        [](const FirewallStatus& st) {
            std::string s = std::format("● {} rule{}", st.rule_count_,
                st.rule_count_ == 1u ? "" : "s");
            if (st.block_all_) s += "  🚨";
            return s;
        });
    auto& fw_status_lbl  = fw.status_lbl;
    auto& fw_enable_btn  = fw.enable_btn;
    auto& fw_disable_btn = fw.disable_btn;
    auto& update_fw_ui   = fw.update;
    fw_enable_btn->setBackgroundColor({0.18f, 0.38f, 0.22f, 1.0f});
    fw_disable_btn->setBackgroundColor({0.35f, 0.15f, 0.15f, 1.0f});

    auto fw_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        fw_enable_btn, std::make_shared<Spacer>(6, 1), fw_disable_btn
    });

    auto fw_blockall_btn = std::make_shared<ButtonWidget>("🚨 Block All Inbound", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.Firewall",
            static_cast<uint32_t>(FirewallOpcode::BlockAllInbound), {}, nullptr);
    });
    fw_blockall_btn->setFixedSize(162, 28);
    fw_blockall_btn->setBackgroundColor({0.55f, 0.10f, 0.10f, 1.0f});

    // ── DNS Ad-Blocker ────────────────────────────────────────────────────────
    // DNS Ad-Blocker (Enable/Disable button-pair; status = blocked-domain count,
    // plus a "list updated" detail line via the extra_update hook)
    auto ab_detail_lbl = std::make_shared<TextWidget>("", 10.0f);
    ab_detail_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});
    ab_detail_lbl->setVisibility(false);

    auto ab = buildEnableDisableTile<AdBlockStatus>(
        "Off", {0.5f, 0.5f, 0.55f, 1.0f}, {0.30f, 0.75f, 0.95f, 1.0f},
        []() { AdBlockEnableRequest req{}; req.enabled_ = 1u;
               g_shell_xbus->Call("com.freebsdx.AdBlock",
                   static_cast<uint32_t>(AdBlockOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        []() { AdBlockEnableRequest req{}; req.enabled_ = 0u;
               g_shell_xbus->Call("com.freebsdx.AdBlock",
                   static_cast<uint32_t>(AdBlockOpcode::SetEnabled),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr); },
        [](const AdBlockStatus& st) { return st.enabled_ != 0; },
        [](const AdBlockStatus& st) {
            std::string count_str;
            uint32_t d = st.domain_count_;
            if (d >= 1000000)     count_str = std::format("{:.1f} M", d / 1000000.0f);
            else if (d >= 1000)   count_str = std::format("{:.0f} K", d / 1000.0f);
            else                  count_str = std::to_string(d);
            return st.updating_ ? std::string("● Updating…")
                                : "● Blocking " + count_str + " domains";
        },
        [ab_detail_lbl](const AdBlockStatus& st) {
            if (st.enabled_) {
                if (st.list_updated_[0]) {
                    ab_detail_lbl->setText("List updated " + std::string(st.list_updated_));
                    ab_detail_lbl->setVisibility(true);
                }
            } else {
                ab_detail_lbl->setVisibility(false);
            }
        });
    auto& ab_status_lbl  = ab.status_lbl;
    auto& ab_enable_btn  = ab.enable_btn;
    auto& ab_disable_btn = ab.disable_btn;
    auto& update_ab_ui   = ab.update;
    ab_enable_btn->setColor({0.20f, 0.55f, 0.85f, 1.0f});
    ab_disable_btn->setColor({0.45f, 0.20f, 0.20f, 1.0f});

    auto ab_update_btn = std::make_shared<ButtonWidget>("↺ Update List", []() {
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.AdBlock",
            static_cast<uint32_t>(AdBlockOpcode::UpdateList), {}, nullptr);
    });
    ab_update_btn->setFixedSize(110, 26);

    auto ab_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ab_enable_btn, std::make_shared<Spacer>(6, 1), ab_disable_btn,
        std::make_shared<Spacer>(1, 1, true), ab_update_btn
    });

    // ── Camera & Microphone privacy ───────────────────────────────────────────
    buildPrivacyPanel(ctx);
    auto& cam_status_lbl    = ctx.cam_status_lbl;
    auto& cam_block_btn     = ctx.cam_block_btn;
    auto& cam_allow_btn     = ctx.cam_allow_btn;
    auto& cam_row           = ctx.cam_row;
    auto& mic_status_lbl    = ctx.mic_status_lbl;
    auto& mic_block_btn     = ctx.mic_block_btn;
    auto& mic_allow_btn     = ctx.mic_allow_btn;
    auto& mic_row           = ctx.mic_row;
    auto& update_privacy_ui = ctx.update_privacy_ui;

    // ── Accessibility quick panel ─────────────────────────────────────────────
    // Font size: 4 preset buttons (S / M / L / XL)
    static constexpr std::array<const char*, 4> kFontLabels = {"S", "M", "L", "XL"};
    auto a11y_font_btns = std::make_shared<
        std::array<std::shared_ptr<ButtonWidget>, 4>>();
    for (int i = 0; i < 4; ++i) {
        (*a11y_font_btns)[i] = std::make_shared<ButtonWidget>(
            std::string(kFontLabels[i]), [i]() {
                if (!g_shell_xbus) return;
                AccessibilityFontRequest req{};
                req.preset_ = static_cast<uint32_t>(i);
                g_shell_xbus->Call("com.freebsdx.A11ySettings",
                    static_cast<uint32_t>(AccessibilitySettingsOpcode::SetFontSize),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
            });
        (*a11y_font_btns)[i]->setFixedSize(46, 28);
    }
    auto a11y_font_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        (*a11y_font_btns)[0], std::make_shared<Spacer>(4, 1),
        (*a11y_font_btns)[1], std::make_shared<Spacer>(4, 1),
        (*a11y_font_btns)[2], std::make_shared<Spacer>(4, 1),
        (*a11y_font_btns)[3],
    });

    // Cursor scale slider 1×–3×
    auto a11y_cursor_val = std::make_shared<TextWidget>("1.0×", 11.0f);
    a11y_cursor_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    auto a11y_cursor_slider = std::make_shared<SliderWidget>(1.0f, 1.0f, 3.0f);
    a11y_cursor_slider->setOnChanged([a11y_cursor_val](float v) {
        a11y_cursor_val->setText(std::format("{:.1f}×", v));
        if (!g_shell_xbus) return;
        AccessibilityCursorRequest req{};
        req.scale_ = v;
        g_shell_xbus->Call("com.freebsdx.A11ySettings",
            static_cast<uint32_t>(AccessibilitySettingsOpcode::SetCursorScale),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    auto a11y_cursor_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🖱", 14.0f),
        std::make_shared<Spacer>(8, 1),
        a11y_cursor_slider,
        std::make_shared<Spacer>(8, 1),
        a11y_cursor_val,
    });

    // High Contrast + Magnifier toggles
    auto a11y_hc_switch  = std::make_shared<SwitchWidget>(false);
    auto a11y_mag_switch = std::make_shared<SwitchWidget>(false);
    a11y_hc_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        AccessibilityToggleRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.A11ySettings",
            static_cast<uint32_t>(AccessibilitySettingsOpcode::SetHighContrast),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    a11y_mag_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        AccessibilityToggleRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.A11ySettings",
            static_cast<uint32_t>(AccessibilitySettingsOpcode::SetMagnifier),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    auto a11y_toggle_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("High Contrast", 11.0f),
        std::make_shared<Spacer>(6, 1),
        a11y_hc_switch,
        std::make_shared<Spacer>(16, 1),
        std::make_shared<TextWidget>("Magnifier", 11.0f),
        std::make_shared<Spacer>(6, 1),
        a11y_mag_switch,
    });

    auto update_a11y_ui = [a11y_font_btns, a11y_cursor_slider, a11y_cursor_val,
                           a11y_hc_switch, a11y_mag_switch]
                          (const AccessibilityQuickSettings& st) {
        for (int i = 0; i < 4; ++i) {
            (*a11y_font_btns)[i]->setBackgroundColor(
                (static_cast<uint32_t>(i) == st.font_size_preset_)
                    ? Color{0.30f, 0.55f, 0.90f, 1.0f}
                    : Color{0.20f, 0.20f, 0.24f, 1.0f});
        }
        float cs = std::clamp(st.cursor_scale_, 1.0f, 3.0f);
        a11y_cursor_slider->setValue(cs);
        a11y_cursor_val->setText(std::format("{:.1f}×", cs));
        a11y_hc_switch->setValue(st.high_contrast_ != 0);
        a11y_mag_switch->setValue(st.magnifier_ != 0);
    };

    // ── Color Filter ── (carved into buildColorFilterPanel; switch + type-btns +
    // type-row + mono-switch + intensity slider/val + intensity-row + update_cf_ui
    // escape. kCfTypeLabels stays builder-local. Aliases keep card/broadcast/reply/
    // timer refs unchanged.)
    buildColorFilterPanel(ctx);
    auto& cf_switch           = ctx.cf_switch;
    auto& cf_type_btns        = ctx.cf_type_btns;
    auto& cf_type_row         = ctx.cf_type_row;
    auto& cf_mono_switch      = ctx.cf_mono_switch;
    auto& cf_intensity_val    = ctx.cf_intensity_val;
    auto& cf_intensity_slider = ctx.cf_intensity_slider;
    auto& cf_intensity_row    = ctx.cf_intensity_row;
    auto& update_cf_ui        = ctx.update_cf_ui;

    // ── Media playback controls ── (carved into buildMediaPlaybackPanel; title/
    // artist/progress + playing flag + play-btn + btn-row + update_mp_ui escape —
    // prev/next buttons stay builder-local. Aliases keep card/broadcast/reply/timer
    // refs unchanged.)
    buildMediaPlaybackPanel(ctx);
    auto& mp_title_lbl    = ctx.mp_title_lbl;
    auto& mp_artist_lbl   = ctx.mp_artist_lbl;
    auto& mp_progress_lbl = ctx.mp_progress_lbl;
    auto& mp_playing      = ctx.mp_playing;
    auto& mp_play_btn     = ctx.mp_play_btn;
    auto& mp_btn_row      = ctx.mp_btn_row;
    auto& update_mp_ui    = ctx.update_mp_ui;

    // ── Quick Settings Panel ─────────────────────────────────────────────────
    auto qs_wifi_switch = std::make_shared<SwitchWidget>(false);
    auto qs_wifi_sub    = std::make_shared<TextWidget>("Not connected", 11.0f);
    qs_wifi_sub->setColor({0.55f, 0.55f, 0.55f, 1.0f});
    qs_wifi_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus || on) return;  // on=true means reconnect (handled by Network service)
        g_shell_xbus->Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::Disconnect), {}, nullptr);
    });

    auto qs_bt_switch = std::make_shared<SwitchWidget>(false);
    qs_bt_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        BluetoothEnableRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    // ── Airplane Mode ── (carved into buildAirplaneModePanel; qs_wifi_switch +
    // qs_bt_switch passed by-ref (cross-coupled). switch + update_ap_ui escape.
    // Aliases keep card/broadcast/reply/timer refs unchanged.)
    buildAirplaneModePanel(ctx, qs_wifi_switch, qs_bt_switch);
    auto& ap_switch    = ctx.ap_switch;
    auto& update_ap_ui = ctx.update_ap_ui;

    // ── Location Services (QS toggle) ── (carved into buildLocationServicesTogglePanel;
    // switch/count/last + update_loc_ui escape. Aliases keep card/broadcast/reply/
    // timer refs unchanged.)
    buildLocationServicesTogglePanel(ctx);
    auto& loc_switch    = ctx.loc_switch;
    auto& loc_count_lbl = ctx.loc_count_lbl;
    auto& loc_last_lbl  = ctx.loc_last_lbl;
    auto& update_loc_ui = ctx.update_loc_ui;

    // ── Audio Output sink selector ── (carved into buildAudioOutputPanel; name-lbl/
    // pills-row + rebuild_aout escape — the per-sink pills are built in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildAudioOutputPanel(ctx);
    auto& aout_name_lbl  = ctx.aout_name_lbl;
    auto& aout_pills_row = ctx.aout_pills_row;
    auto& rebuild_aout   = ctx.rebuild_aout;

    // Screen Timeout quick-selector ── (carved into buildScreenTimeoutPanel;
    // aliases keep the card/broadcast/reply/timer refs unchanged. stout_hl +
    // kStoutPresets table stay builder-local.)
    buildScreenTimeoutPanel(ctx);
    auto& stout_btns      = ctx.stout_btns;
    auto& stout_cur       = ctx.stout_cur;
    auto& stout_row       = ctx.stout_row;
    auto& update_stout_ui = ctx.update_stout_ui;

    // Screen Rotation Lock ── (carved into buildRotationLockPanel; aliases keep
    // the card/broadcast/reply/timer refs unchanged.)
    buildRotationLockPanel(ctx);
    auto& rot_lock_switch = ctx.rot_lock_switch;
    auto& rot_orient_lbl  = ctx.rot_orient_lbl;
    auto& update_rot_ui   = ctx.update_rot_ui;

    // Microphone Mute ── (carved into buildMicMutePanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged.)
    buildMicMutePanel(ctx);
    auto& mic_mute_switch    = ctx.mic_mute_switch;
    auto& mic_mute_status    = ctx.mic_mute_status;
    auto& update_mic_mute_ui = ctx.update_mic_mute_ui;

    // Scroll Direction toggle
    // Hotspot toggle
    auto hotspot_switch     = std::make_shared<SwitchWidget>(false);
    auto hotspot_status_lbl = std::make_shared<TextWidget>("Off", 11.0f);
    hotspot_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    auto hotspot_ssid_lbl   = std::make_shared<TextWidget>("", 10.0f);
    hotspot_ssid_lbl->setColor({0.45f, 0.65f, 0.90f, 1.0f});

    auto update_hotspot_ui = [hotspot_switch, hotspot_status_lbl, hotspot_ssid_lbl]
                             (const HotspotState& st) {
        bool on = st.enabled_ != 0;
        hotspot_switch->setValue(on);
        if (on) {
            hotspot_status_lbl->setText(st.client_count_ == 0
                ? "Active"
                : std::format("Active — {} client{}", st.client_count_,
                              st.client_count_ != 1 ? "s" : ""));
            hotspot_status_lbl->setColor({0.35f, 0.80f, 0.45f, 1.0f});
            hotspot_ssid_lbl->setText(st.ssid_[0] ? std::string(st.ssid_) : "");
        } else {
            hotspot_status_lbl->setText("Off");
            hotspot_status_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
            hotspot_ssid_lbl->setText("");
        }
    };

    hotspot_switch->setOnChanged([hotspot_status_lbl, hotspot_ssid_lbl](bool on) {
        hotspot_status_lbl->setText(on ? "Starting…" : "Off");
        hotspot_status_lbl->setColor(on
            ? Color{0.95f, 0.72f, 0.20f, 1.0f}
            : Color{0.50f, 0.50f, 0.55f, 1.0f});
        if (!on) hotspot_ssid_lbl->setText("");
        if (!g_shell_xbus) return;
        HotspotRequest req{};
        req.enabled_ = on ? 1u : 0u;
        g_shell_xbus->Call("com.freebsdx.Hotspot",
            static_cast<uint32_t>(HotspotToggleOpcode::SetEnabled),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    // ── Time Zone ─────────────────────────────────────────────────────────────
    buildTimeZonePanel(ctx);
    auto& tz_idx       = ctx.tz_idx;
    auto& tz_name_lbl  = ctx.tz_name_lbl;
    auto& tz_disp_lbl  = ctx.tz_disp_lbl;
    auto& tz_time_lbl  = ctx.tz_time_lbl;
    auto& tz_prev_btn  = ctx.tz_prev_btn;
    auto& tz_next_btn  = ctx.tz_next_btn;
    auto& update_tz_ui = ctx.update_tz_ui;

    // ── NTP / Time Sync toggle ── (carved into buildNtpTogglePanel; switch/status/
    // server + update_ntp_ui escape. Aliases keep card/broadcast/reply/timer refs
    // unchanged.)
    buildNtpTogglePanel(ctx);
    auto& ntp_switch     = ctx.ntp_switch;
    auto& ntp_status_lbl = ctx.ntp_status_lbl;
    auto& ntp_server_lbl = ctx.ntp_server_lbl;
    auto& update_ntp_ui  = ctx.update_ntp_ui;

    // ── Wake Lock ─────────────────────────────────────────────────────────────
    buildWakeLockPanel(ctx);
    auto& wl_active     = ctx.wl_active;
    auto& wl_dur_cur    = ctx.wl_dur_cur;
    auto& wl_dur_btns   = ctx.wl_dur_btns;
    auto& wl_switch     = ctx.wl_switch;
    auto& wl_status_lbl = ctx.wl_status_lbl;
    auto& update_wl_ui  = ctx.update_wl_ui;

    // ── Remote Login (SSH daemon) ─────────────────────────────────────────────
    buildRemoteLoginPanel(ctx);
    auto& rl_switch    = ctx.rl_switch;
    auto& rl_info_lbl  = ctx.rl_info_lbl;
    auto& update_rl_ui = ctx.update_rl_ui;

    // ── Language / Locale ─────────────────────────────────────────────────────
    buildLocalePanel(ctx);
    auto& locale_cur        = ctx.locale_cur;
    auto& locale_btns       = ctx.locale_btns;
    auto& locale_sub_lbl    = ctx.locale_sub_lbl;
    auto& update_locale_ui  = ctx.update_locale_ui;

    // ── Accessibility ─────────────────────────────────────────────────────────
    buildA11yQuickPanel(ctx);
    auto& a11y_reader_on    = ctx.a11y_reader_on;
    auto& a11y_contrast_on  = ctx.a11y_contrast_on;
    auto& a11y_magnify_on   = ctx.a11y_magnify_on;
    auto& a11y_reader_btn   = ctx.a11y_reader_btn;
    auto& a11y_contrast_btn = ctx.a11y_contrast_btn;
    auto& a11y_magnify_btn  = ctx.a11y_magnify_btn;
    auto& update_a11y_ui_b  = ctx.update_a11y_ui_b;

    // ── Proxy Settings ────────────────────────────────────────────────────────
    buildProxyPanel(ctx);
    auto& proxy_mode_cur  = ctx.proxy_mode_cur;
    auto& proxy_mode_btns = ctx.proxy_mode_btns;
    auto& proxy_info_lbl  = ctx.proxy_info_lbl;
    auto& update_proxy_ui = ctx.update_proxy_ui;

    // ── DNS Configuration ─────────────────────────────────────────────────────
    buildDnsConfigPanel(ctx);
    auto& dns_mode_cur     = ctx.dns_mode_cur;
    auto& dns_mode_btns    = ctx.dns_mode_btns;
    auto& dns_resolver_lbl = ctx.dns_resolver_lbl;
    auto& update_dns_ui    = ctx.update_dns_ui;

    // Update Channel presets
    buildUpdateChannelPanel(ctx);
    auto& upd_ch_cur        = ctx.upd_ch_cur;
    auto& upd_ch_btns       = ctx.upd_ch_btns;
    auto& upd_ch_badge_lbl  = ctx.upd_ch_badge_lbl;
    auto& upd_ch_row        = ctx.upd_ch_row;
    auto& update_upd_ch_ui  = ctx.update_upd_ch_ui;

    // ── Package Signing ───────────────────────────────────────────────────────
    buildPackageSigningPanel(ctx);
    auto& pkgsign_req_switch = ctx.pkgsign_req_switch;
    auto& pkgsign_unk_switch = ctx.pkgsign_unk_switch;
    auto& pkgsign_cur        = ctx.pkgsign_cur;
    auto& update_pkgsign_ui  = ctx.update_pkgsign_ui;

    // ── Camera Devices ── (carved into buildCameraDevicesPanel; aliases keep the
    // card/broadcast/timer refs unchanged. CamFetcher machinery stays local.)
    buildCameraDevicesPanel(ctx);
    auto& cam_dev_count_lbl = ctx.cam_dev_count_lbl;
    auto& cam_dev_col       = ctx.cam_dev_col;
    auto& rebuild_cam_dev   = ctx.rebuild_cam_dev;

    // Text Size presets ── (carved into buildTextSizePanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged. text_size_hl stays local; the
    // text_size_row is assembled later in main() and references text_size_btns.)
    buildTextSizePanel(ctx);
    auto& text_size_cur       = ctx.text_size_cur;
    auto& text_size_btns      = ctx.text_size_btns;
    auto& update_text_size_ui = ctx.update_text_size_ui;

    // Display Scaling presets ── (carved into buildDisplayScalePanel; aliases
    // keep the card/broadcast/reply/timer refs unchanged. disp_scale_hl stays
    // local.)
    buildDisplayScalePanel(ctx);
    auto& disp_scale_cur       = ctx.disp_scale_cur;
    auto& disp_scale_btns      = ctx.disp_scale_btns;
    auto& update_disp_scale_ui = ctx.update_disp_scale_ui;
    auto& disp_scale_row       = ctx.disp_scale_row;

    // ── Spatial Audio ─────────────────────────────────────────────────────────
    // Spatial Audio (switch + mode button row; detail = current output device)
    auto saudio = buildButtonRowTile<SpatialAudioConfig, SpatialAudioConfig>(
        "com.freebsdx.SpatialAudio", static_cast<uint32_t>(SpatialAudioOpcode::SetConfig), false,
        &SpatialAudioConfig::enabled_, &SpatialAudioConfig::mode_,
        {"Stereo", "Surround", "Adaptive"}, 70, 24,
        [](const SpatialAudioConfig& cfg) { return cfg.enabled_ != 0; },
        [](const SpatialAudioConfig& cfg) { return cfg.mode_; },
        "No output device",
        [](const SpatialAudioConfig& cfg, TextWidget& lbl) {
            if (cfg.device_[0]) lbl.setText(std::string(cfg.device_));
        });
    auto& sa_enabled_sw = saudio.sw;
    auto& sa_mode_btns  = saudio.btns;
    auto& sa_device_lbl = saudio.detail_lbl;
    auto& sa_status_lbl = saudio.status_lbl;
    auto& update_sa_ui  = saudio.update;

    // ── System Sounds ─────────────────────────────────────────────────────────
    // System Sounds (switch + volume slider, "{}%" value, no info)
    auto sndfx = buildSliderTile<SystemSoundsState, SystemSoundsRequest>(
        "com.freebsdx.SystemSounds", static_cast<uint32_t>(SystemSoundsOpcode::SetState), true,
        &SystemSoundsRequest::enabled_, &SystemSoundsRequest::volume_pct_,
        0.0f, 100.0f, 70.0f,
        [](uint32_t v) { return std::format("{}%", v); },
        [](const SystemSoundsState& st) { return st.enabled_ != 0; },
        [](const SystemSoundsState& st) { return st.volume_pct_; },
        nullptr);
    auto& sndfx_enabled_sw = sndfx.sw;
    auto& sndfx_vol_slider = sndfx.slider;
    auto& sndfx_vol_val_lbl = sndfx.value_lbl;
    auto& sndfx_status_lbl = sndfx.status_lbl;
    auto& update_sndfx_ui = sndfx.update;
    sndfx_vol_val_lbl->setColor({0.72f, 0.72f, 0.72f, 1.0f});  // this tile's lighter grey

    // ── Live Captions ─────────────────────────────────────────────────────────
    // Live Captions (switch + position button row; fixed font_size_ = Medium)
    auto lcap = buildButtonRowTile<LiveCaptionsState, LiveCaptionsRequest>(
        "com.freebsdx.LiveCaptions", static_cast<uint32_t>(LiveCaptionsOpcode::SetState), false,
        &LiveCaptionsRequest::enabled_, &LiveCaptionsRequest::position_,
        {"Bottom", "Top"}, 70, 24,
        [](const LiveCaptionsState& st) { return st.enabled_ != 0; },
        [](const LiveCaptionsState& st) { return st.position_; },
        nullptr, nullptr,
        [](LiveCaptionsRequest& r) { r.font_size_ = 1u; });  // Medium
    auto& captions_enabled_sw = lcap.sw;
    auto& captions_pos_btns   = lcap.btns;
    auto& captions_status_lbl = lcap.status_lbl;
    auto& update_captions_ui  = lcap.update;

    // ── Data Saver ── (data-driven switch tile)
    auto dsaver = buildSwitchTile<DataSaverState, DataSaverRequest>(
        "com.freebsdx.DataSaver", static_cast<uint32_t>(DataSaverOpcode::SetState),
        [](const DataSaverState& st) {
            return st.saved_mb_today_ > 0
                ? std::format("{} MB saved today", st.saved_mb_today_) : std::string();
        });
    auto& dsaver_enabled_sw = dsaver.sw;
    auto& dsaver_saved_lbl   = dsaver.detail_lbl;
    auto& dsaver_status_lbl  = dsaver.status_lbl;
    auto& update_dsaver_ui   = dsaver.update;

    // ── Fast Charging ── (data-driven switch tile)
    auto fastchg = buildSwitchTile<FastChargingState, FastChargingRequest>(
        "com.freebsdx.FastCharging", static_cast<uint32_t>(FastChargingOpcode::SetState),
        [](const FastChargingState& st) {
            return st.charge_rate_w_ > 0
                ? std::format("{} W", st.charge_rate_w_) : std::string("Not charging");
        });
    auto& fastchg_enabled_sw = fastchg.sw;
    auto& fastchg_rate_lbl    = fastchg.detail_lbl;
    auto& fastchg_status_lbl  = fastchg.status_lbl;
    auto& update_fastchg_ui   = fastchg.update;

    // ── Do Not Disturb ───────────────────────────────────────────────────────
    // Do Not Disturb (switch + duration button row, no detail line)
    auto dnd = buildButtonRowTile<DoNotDisturbState, DoNotDisturbRequest>(
        "com.freebsdx.DoNotDisturb", static_cast<uint32_t>(DoNotDisturbOpcode::SetState), false,
        &DoNotDisturbRequest::enabled_, &DoNotDisturbRequest::duration_,
        {"Until off", "1 hour", "8 hours", "Tomorrow"}, 70, 24,
        [](const DoNotDisturbState& st) { return st.enabled_ != 0; },
        [](const DoNotDisturbState& st) { return st.duration_; },
        nullptr, nullptr);
    auto& silence_enabled_sw = dnd.sw;
    auto& silence_dur_btns   = dnd.btns;
    auto& silence_status_lbl = dnd.status_lbl;
    auto& update_silence_ui  = dnd.update;

    // ── Battery Health ── (carved into buildBatteryHealthPanel; aliases keep
    // the card/broadcast/timer references unchanged)
    buildBatteryHealthPanel(ctx);
    auto& bhealth_opt_sw     = ctx.bhealth_opt_sw;
    auto& bhealth_status_lbl = ctx.bhealth_status_lbl;
    auto& bhealth_info_lbl   = ctx.bhealth_info_lbl;
    auto& update_bhealth_ui  = ctx.update_bhealth_ui;

    // ── True Tone ──────────────────────────────────────────────────────────────
    // True Tone (switch + strength slider, with an ambient-light info line)
    auto ttone = buildSliderTile<TrueToneState, TrueToneRequest>(
        "com.freebsdx.TrueTone", static_cast<uint32_t>(TrueToneOpcode::SetState), false,
        &TrueToneRequest::enabled_, &TrueToneRequest::strength_,
        0.0f, 100.0f, 50.0f,
        [](uint32_t v) { return std::format("{}%", v); },
        [](const TrueToneState& st) { return st.enabled_ != 0; },
        [](const TrueToneState& st) { return st.strength_; },
        [](const TrueToneState& st) { return std::format("ambient {}K", st.ambient_k_); });
    auto& ttone_sw              = ttone.sw;
    auto& ttone_strength_slider = ttone.slider;
    auto& ttone_strength_lbl    = ttone.value_lbl;
    auto& ttone_status_lbl      = ttone.status_lbl;
    auto& ttone_info_lbl        = ttone.info_lbl;
    auto& update_ttone_ui       = ttone.update;

    // ── Optimized Storage ───────────────────────────────────────────────────────
    // Optimized Storage (switch + target% slider, with a reclaimable-space info line)
    auto ostore = buildSliderTile<OptStorageState, OptStorageRequest>(
        "com.freebsdx.OptStorage", static_cast<uint32_t>(OptStorageOpcode::SetState), false,
        &OptStorageRequest::enabled_, &OptStorageRequest::target_pct_,
        5.0f, 50.0f, 10.0f,
        [](uint32_t v) { return std::format("{}%", v); },
        [](const OptStorageState& st) { return st.enabled_ != 0; },
        [](const OptStorageState& st) { return st.target_pct_; },
        [](const OptStorageState& st) {
            return st.reclaimable_mb_ >= 1024
                ? std::format("{:.1f} GB reclaimable", st.reclaimable_mb_ / 1024.0f)
                : std::format("{} MB reclaimable", st.reclaimable_mb_);
        });
    auto& ostore_sw            = ostore.sw;
    auto& ostore_target_slider = ostore.slider;
    auto& ostore_target_lbl    = ostore.value_lbl;
    auto& ostore_status_lbl    = ostore.status_lbl;
    auto& ostore_info_lbl      = ostore.info_lbl;
    auto& update_ostore_ui     = ostore.update;

    // ── Mono Audio ───────────────────────────────────────────────────────────────
    // Mono Audio (switch + balance slider; value shown as "L12"/"C"/"R8", no info)
    auto monoaud = buildSliderTile<MonoAudioState, MonoAudioRequest>(
        "com.freebsdx.MonoAudio", static_cast<uint32_t>(MonoAudioOpcode::SetState), false,
        &MonoAudioRequest::enabled_, &MonoAudioRequest::balance_,
        0.0f, 100.0f, 50.0f,
        [](uint32_t b) -> std::string {           // balance → "L12" / "C" / "R8"
            if (b < 50) return std::format("L{}", 50 - b);
            if (b > 50) return std::format("R{}", b - 50);
            return "C";
        },
        [](const MonoAudioState& st) { return st.enabled_ != 0; },
        [](const MonoAudioState& st) { return st.balance_; },
        nullptr);
    auto& monoaud_sw            = monoaud.sw;
    auto& monoaud_balance_slider = monoaud.slider;
    auto& monoaud_balance_lbl   = monoaud.value_lbl;
    auto& monoaud_status_lbl    = monoaud.status_lbl;
    auto& update_monoaud_ui     = monoaud.update;

    // ── Stage Manager ─────────────────────────────────────────────────────────────
    // Stage Manager (switch + recent-count slider, plain-number value, no info)
    auto stagemgr = buildSliderTile<StageManagerState, StageManagerRequest>(
        "com.freebsdx.StageManager", static_cast<uint32_t>(StageManagerOpcode::SetState), false,
        &StageManagerRequest::enabled_, &StageManagerRequest::recent_count_,
        1.0f, 6.0f, 4.0f,
        [](uint32_t v) { return std::format("{}", v); },
        [](const StageManagerState& st) { return st.enabled_ != 0; },
        [](const StageManagerState& st) { return st.recent_count_; },
        nullptr);
    auto& stagemgr_sw           = stagemgr.sw;
    auto& stagemgr_count_slider = stagemgr.slider;
    auto& stagemgr_count_lbl    = stagemgr.value_lbl;
    auto& stagemgr_status_lbl   = stagemgr.status_lbl;
    auto& update_stagemgr_ui    = stagemgr.update;

    // ── Mouse & Pointer ── (carved into buildMousePointerPanel; speed slider/label
    // + natural/accel/tap switches + status + update_ptr_ui escape — ptr_speed_cur
    // + send_ptr helper stay builder-local. Aliases keep card/broadcast/reply/timer
    // refs unchanged.)
    buildMousePointerPanel(ctx);
    auto& ptr_speed_lbl    = ctx.ptr_speed_lbl;
    auto& ptr_speed_slider = ctx.ptr_speed_slider;
    auto& ptr_natural_sw   = ctx.ptr_natural_sw;
    auto& ptr_accel_sw     = ctx.ptr_accel_sw;
    auto& ptr_tap_sw       = ctx.ptr_tap_sw;
    auto& ptr_status_lbl   = ctx.ptr_status_lbl;
    auto& update_ptr_ui    = ctx.update_ptr_ui;

    // ── Audio EQ ── (carved into buildAudioEqPanel; status-lbl + 5 sliders/value-
    // labels + preset buttons + update_eq_ui escape — eq_gains/eq_preset_cur +
    // eq_hl_preset/send_eq helpers + kEqPresets/kEqPresetGains stay builder-local.
    // kEqBandNames stays a main()-local static (referenced by the card assembly).
    // Aliases keep card/broadcast/reply/timer refs unchanged.)
    static const char* kEqBandNames[] = {"Bass", "Lo-Mid", "Mid", "Hi-Mid", "Treble"};
    buildAudioEqPanel(ctx);
    auto& eq_status_lbl  = ctx.eq_status_lbl;
    auto& eq_sliders     = ctx.eq_sliders;
    auto& eq_val_lbls    = ctx.eq_val_lbls;
    auto& eq_preset_btns = ctx.eq_preset_btns;
    auto& update_eq_ui   = ctx.update_eq_ui;

    // ── Location Services ── (carved into buildLocationServicesPanel; a latent
    // panel — only the updater + rebuild pointer escape via aliases; the widgets
    // and LocAppFetcher machinery stay builder-local.)
    buildLocationServicesPanel(ctx);
    auto& update_loc_policy = ctx.update_loc_policy;
    auto& rebuild_loc       = ctx.rebuild_loc;

    // ── Screen Lock ── (carved into buildScreenLockPanel; aliases keep the
    // card/broadcast/timer references unchanged)
    buildScreenLockPanel(ctx);
    auto& slk_enabled_sw  = ctx.slk_enabled_sw;
    auto& slk_require_sw  = ctx.slk_require_sw;
    auto& slk_sleep_sw    = ctx.slk_sleep_sw;
    auto& slk_timeout_cur = ctx.slk_timeout_cur;
    auto& slk_status_lbl  = ctx.slk_status_lbl;
    auto& slk_timeout_btns = ctx.slk_timeout_btns;
    auto& slk_lock_btn    = ctx.slk_lock_btn;
    auto& slk_hl          = ctx.slk_hl;
    auto& update_slk_ui   = ctx.update_slk_ui;

    // ── Notif Schedule ── (carved into buildNotifSchedulePanel; aliases keep
    // the card/broadcast/timer references unchanged)
    buildNotifSchedulePanel(ctx);
    auto& nsch_enabled_sw    = ctx.nsch_enabled_sw;
    auto& nsch_start_h_input = ctx.nsch_start_h_input;
    auto& nsch_start_m_input = ctx.nsch_start_m_input;
    auto& nsch_end_h_input   = ctx.nsch_end_h_input;
    auto& nsch_end_m_input   = ctx.nsch_end_m_input;
    auto& nsch_calls_sw      = ctx.nsch_calls_sw;
    auto& nsch_alarms_sw     = ctx.nsch_alarms_sw;
    auto& nsch_weekend_sw    = ctx.nsch_weekend_sw;
    auto& nsch_status_lbl    = ctx.nsch_status_lbl;
    auto& nsch_apply_btn     = ctx.nsch_apply_btn;
    auto& update_nsch_ui     = ctx.update_nsch_ui;

    // ── Thermal Zones ── (carved into buildThermalZonesPanel; aliases keep the
    // card/broadcast/timer refs unchanged. TzFetcher machinery stays local.)
    buildThermalZonesPanel(ctx);
    auto& tz_count_lbl  = ctx.tz_count_lbl;
    auto& tz_col        = ctx.tz_col;
    auto& tz_status_lbl = ctx.tz_status_lbl;
    auto& rebuild_tz    = ctx.rebuild_tz;

    // ── DNS Config ────────────────────────────────────────────────────────────
    auto dns_primary_input   = std::make_shared<TextInputWidget>();
    auto dns_secondary_input = std::make_shared<TextInputWidget>();
    auto dns_doh_switch      = std::make_shared<SwitchWidget>(false);
    auto dns_doh_url_input   = std::make_shared<TextInputWidget>();
    auto dns_doh_row         = std::make_shared<FlexColumn>();
    auto dns_status_lbl      = std::make_shared<TextWidget>("", 10.0f);
    dns_primary_input->setPlaceholder("Primary DNS");
    dns_secondary_input->setPlaceholder("Secondary DNS");
    dns_doh_url_input->setPlaceholder("https://...");
    dns_doh_row->setVisibility(false);
    dns_doh_row->addChild(std::make_shared<Spacer>(1, 4));
    dns_doh_row->addChild(dns_doh_url_input);

    auto send_dns_cfg = [dns_primary_input, dns_secondary_input,
                         dns_doh_switch, dns_doh_url_input, dns_status_lbl]() {
        DnsResolverConfig cfg{};
        auto pri = dns_primary_input->getText();
        auto sec = dns_secondary_input->getText();
        auto url = dns_doh_url_input->getText();
        std::strncpy(cfg.primary_,   pri.c_str(), sizeof(cfg.primary_)   - 1);
        std::strncpy(cfg.secondary_, sec.c_str(), sizeof(cfg.secondary_) - 1);
        cfg.doh_enabled_ = dns_doh_switch->value() ? 1u : 0u;
        std::strncpy(cfg.doh_url_,   url.c_str(), sizeof(cfg.doh_url_)   - 1);
        std::vector<uint8_t> payload(sizeof(cfg));
        std::memcpy(payload.data(), &cfg, sizeof(cfg));
        if (!g_shell_xbus) return;
        g_shell_xbus->Call("com.freebsdx.DnsConfig",
            static_cast<uint32_t>(DnsConfigOpcode::SetConfig), payload,
            [dns_status_lbl](const XBusMessage&) {
                dns_status_lbl->setText("Applied \xe2\x9c\x93");
                dns_status_lbl->setColor({0.30f, 0.85f, 0.45f, 1.0f});
            });
    };

    auto update_dns_cfg_ui = [dns_primary_input, dns_secondary_input, dns_doh_switch,
                               dns_doh_url_input, dns_doh_row](const DnsResolverConfig& cfg) {
        dns_primary_input->setText(std::string(cfg.primary_));
        dns_secondary_input->setText(std::string(cfg.secondary_));
        bool doh = (cfg.doh_enabled_ != 0);
        dns_doh_switch->setValue(doh);
        dns_doh_url_input->setText(std::string(cfg.doh_url_));
        dns_doh_row->setVisibility(doh);
    };

    dns_doh_switch->setOnChanged([dns_doh_row, send_dns_cfg](bool on) {
        dns_doh_row->setVisibility(on);
        send_dns_cfg();
    });

    auto dns_apply_btn = std::make_shared<ButtonWidget>("Apply",
        [send_dns_cfg, dns_status_lbl]() {
            dns_status_lbl->setText("");
            send_dns_cfg();
        });
    dns_apply_btn->setBackgroundColor({0.20f, 0.50f, 0.95f, 1.0f});
    dns_apply_btn->setFixedSize(60, 26);

    auto dns_flush_btn = std::make_shared<ButtonWidget>("Flush Cache",
        [dns_status_lbl]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.DnsConfig",
                static_cast<uint32_t>(DnsConfigOpcode::Flush), {},
                [dns_status_lbl](const XBusMessage&) {
                    dns_status_lbl->setText("Cache flushed \xe2\x9c\x93");
                    dns_status_lbl->setColor({0.30f, 0.85f, 0.45f, 1.0f});
                });
        });
    dns_flush_btn->setBackgroundColor({0.35f, 0.35f, 0.38f, 1.0f});
    dns_flush_btn->setFixedSize(90, 26);

    // ── Wake-on-LAN ───────────────────────────────────────────────────────────
    auto wol_col        = std::make_shared<FlexColumn>();
    wol_col->setGap(4);
    auto wol_count_lbl  = std::make_shared<TextWidget>("No targets", 10.0f);
    wol_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

    // Add-target form
    auto wol_name_input = std::make_shared<TextInputWidget>();
    auto wol_mac_input  = std::make_shared<TextInputWidget>();
    wol_name_input->setPlaceholder("Name\xe2\x80\xa6");
    wol_name_input->setFlexGrow(1.0f);
    wol_mac_input->setPlaceholder("AA:BB:CC:DD:EE:FF");
    wol_mac_input->setFlexGrow(1.0f);

    auto rebuild_wol = std::make_shared<std::function<void()>>();

    auto wol_add_btn = std::make_shared<ButtonWidget>("Add",
        [wol_name_input, wol_mac_input, rebuild_wol]() {
            if (!g_shell_xbus) return;
            WakeOnLanTarget tgt{};
            std::strncpy(tgt.name_, wol_name_input->getText().c_str(),
                         sizeof(tgt.name_) - 1);
            std::strncpy(tgt.mac_, wol_mac_input->getText().c_str(),
                         sizeof(tgt.mac_) - 1);
            std::vector<uint8_t> buf(sizeof(tgt));
            std::memcpy(buf.data(), &tgt, sizeof(tgt));
            g_shell_xbus->Call("com.freebsdx.WakeOnLan",
                static_cast<uint32_t>(WakeOnLanOpcode::AddTarget), buf,
                [rebuild_wol](const XBusMessage&) {
                    if (rebuild_wol && *rebuild_wol) (*rebuild_wol)();
                });
            wol_name_input->setText("");
            wol_mac_input->setText("");
        });
    wol_add_btn->setFixedSize(50, 26);
    wol_add_btn->setBackgroundColor({0.20f, 0.38f, 0.22f, 1.0f});

    auto wol_add_row = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            wol_name_input, std::make_shared<Spacer>(6, 1),
            wol_mac_input,  std::make_shared<Spacer>(6, 1),
            wol_add_btn});
    wol_add_row->setAlignment(Alignment::Center);

    *rebuild_wol = [wol_col, wol_count_lbl,
                    rwol_weak = std::weak_ptr<std::function<void()>>(rebuild_wol)]() {
        if (!g_shell_xbus) return;
        wol_col->clearChildren();
        struct WolFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<WolFetcher>();
        pf->col   = wol_col;
        pf->count = wol_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<WolFetcher>(pf),
                     rwol_weak](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.WakeOnLan",
                static_cast<uint32_t>(WakeOnLanOpcode::ListTargets),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, rwol_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    WakeOnLanPage wp{};
                    std::memcpy(&wp, resp.payload_, sizeof(wp));
                    if (page == 0) {
                        pf->total = wp.total_;
                        pf->count->setText(wp.total_ == 0
                            ? "No targets"
                            : std::format("{} target{}", wp.total_,
                                          wp.total_ == 1 ? "" : "s"));
                    }
                    if (wp.valid_) {
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(wp.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto mac_lbl = std::make_shared<TextWidget>(
                            std::string(wp.mac_), 10.0f);
                        mac_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::string captured_mac(wp.mac_);
                        auto wake_btn = std::make_shared<ButtonWidget>("Wake",
                            [captured_mac]() {
                                if (!g_shell_xbus) return;
                                WakeOnLanRequest req{};
                                std::strncpy(req.mac_, captured_mac.c_str(),
                                             sizeof(req.mac_) - 1);
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.WakeOnLan",
                                    static_cast<uint32_t>(WakeOnLanOpcode::Send),
                                    buf, nullptr);
                            });
                        wake_btn->setFixedSize(50, 22);
                        wake_btn->setBackgroundColor({0.20f, 0.38f, 0.55f, 1.0f});

                        std::string captured_name(wp.name_);
                        auto rm_btn = std::make_shared<ButtonWidget>("\xe2\x9c\x95",
                            [captured_name, rwol_weak]() {
                                if (!g_shell_xbus) return;
                                WakeOnLanRemoveRequest req{};
                                std::strncpy(req.name_, captured_name.c_str(),
                                             sizeof(req.name_) - 1);
                                std::vector<uint8_t> buf(sizeof(req));
                                std::memcpy(buf.data(), &req, sizeof(req));
                                g_shell_xbus->Call("com.freebsdx.WakeOnLan",
                                    static_cast<uint32_t>(WakeOnLanOpcode::RemoveTarget),
                                    buf,
                                    [rwol_weak](const XBusMessage&) {
                                        if (auto r = rwol_weak.lock())
                                            if (*r) (*r)();
                                    });
                            });
                        rm_btn->setFixedSize(26, 22);
                        rm_btn->setBackgroundColor({0.45f, 0.18f, 0.18f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, std::make_shared<Spacer>(6, 1),
                                mac_lbl,  std::make_shared<Spacer>(6, 1),
                                wake_btn, std::make_shared<Spacer>(4, 1),
                                rm_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!wp.valid_ && page == 0) {
                        auto empty = std::make_shared<TextWidget>("No saved targets", 10.0f);
                        empty->setColor({0.40f, 0.40f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                    } else if (wp.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    // ── Accessibility ─────────────────────────────────────────────────────────
    auto a11y_cfg           = std::make_shared<AccessibilitySettings>();
    auto a11y_contrast_sw   = std::make_shared<SwitchWidget>(false);
    auto a11y_largetext_sw  = std::make_shared<SwitchWidget>(false);
    auto a11y_magnifier_sw  = std::make_shared<SwitchWidget>(false);
    auto a11y_mag_slider    = std::make_shared<SliderWidget>(200.0f, 100.0f, 500.0f);
    auto a11y_mag_val_lbl   = std::make_shared<TextWidget>("200%", 10.0f);
    a11y_mag_val_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    auto a11y_mag_row       = std::make_shared<FlexRow>(
        std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Scale", 11.0f),
            std::make_shared<Spacer>(8, 1),
            a11y_mag_slider,
            std::make_shared<Spacer>(6, 1),
            a11y_mag_val_lbl});
    a11y_mag_row->setAlignment(Alignment::Center);
    a11y_mag_row->setVisibility(false);
    auto a11y_reader_sw     = std::make_shared<SwitchWidget>(false);
    auto a11y_motion_sw     = std::make_shared<SwitchWidget>(false);
    auto a11y_sticky_sw     = std::make_shared<SwitchWidget>(false);

    auto send_a11y_cfg = [a11y_cfg, a11y_contrast_sw, a11y_largetext_sw,
                          a11y_magnifier_sw, a11y_mag_slider,
                          a11y_reader_sw, a11y_motion_sw, a11y_sticky_sw]() {
        if (!g_shell_xbus) return;
        AccessibilitySettings cfg{};
        cfg.high_contrast_      = a11y_contrast_sw->value()  ? 1u : 0u;
        cfg.large_text_         = a11y_largetext_sw->value() ? 1u : 0u;
        cfg.magnifier_          = a11y_magnifier_sw->value() ? 1u : 0u;
        cfg.magnifier_scale_    = static_cast<uint32_t>(a11y_mag_slider->value());
        cfg.screen_reader_      = a11y_reader_sw->value()    ? 1u : 0u;
        cfg.reduce_motion_      = a11y_motion_sw->value()    ? 1u : 0u;
        cfg.sticky_keys_        = a11y_sticky_sw->value()    ? 1u : 0u;
        cfg.slow_keys_          = a11y_cfg->slow_keys_;
        cfg.slow_keys_delay_ms_ = a11y_cfg->slow_keys_delay_ms_;
        cfg.bounce_keys_        = a11y_cfg->bounce_keys_;
        cfg.cursor_size_        = a11y_cfg->cursor_size_;
        *a11y_cfg = cfg;
        std::vector<uint8_t> buf(sizeof(cfg));
        std::memcpy(buf.data(), &cfg, sizeof(cfg));
        g_shell_xbus->Call("com.freebsdx.Accessibility",
            static_cast<uint32_t>(AccessibilityPanelOpcode::SetSettings), buf, nullptr);
    };

    a11y_contrast_sw->setOnChanged([send_a11y_cfg](bool) { send_a11y_cfg(); });
    a11y_largetext_sw->setOnChanged([send_a11y_cfg](bool) { send_a11y_cfg(); });
    a11y_magnifier_sw->setOnChanged([send_a11y_cfg, a11y_mag_row](bool on) {
        a11y_mag_row->setVisibility(on);
        send_a11y_cfg();
    });
    a11y_mag_slider->setOnChanged([send_a11y_cfg, a11y_mag_val_lbl](float v) {
        a11y_mag_val_lbl->setText(std::format("{}%", static_cast<uint32_t>(v)));
        send_a11y_cfg();
    });
    a11y_reader_sw->setOnChanged([send_a11y_cfg](bool) { send_a11y_cfg(); });
    a11y_motion_sw->setOnChanged([send_a11y_cfg](bool) { send_a11y_cfg(); });
    a11y_sticky_sw->setOnChanged([send_a11y_cfg](bool) { send_a11y_cfg(); });

    auto update_a11y_ui_c = [a11y_cfg, a11y_contrast_sw, a11y_largetext_sw,
                           a11y_magnifier_sw, a11y_mag_row, a11y_mag_slider,
                           a11y_mag_val_lbl, a11y_reader_sw, a11y_motion_sw,
                           a11y_sticky_sw](const AccessibilitySettings& cfg) {
        *a11y_cfg = cfg;
        a11y_contrast_sw->setValue(cfg.high_contrast_ != 0);
        a11y_largetext_sw->setValue(cfg.large_text_ != 0);
        a11y_magnifier_sw->setValue(cfg.magnifier_ != 0);
        float scale = std::clamp(static_cast<float>(cfg.magnifier_scale_), 100.0f, 500.0f);
        a11y_mag_slider->setValue(scale);
        a11y_mag_val_lbl->setText(std::format("{}%", cfg.magnifier_scale_));
        a11y_mag_row->setVisibility(cfg.magnifier_ != 0);
        a11y_reader_sw->setValue(cfg.screen_reader_ != 0);
        a11y_motion_sw->setValue(cfg.reduce_motion_ != 0);
        a11y_sticky_sw->setValue(cfg.sticky_keys_ != 0);
    };

    // ── Gesture Config ── (carved into buildGestureConfigPanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged. send_gest_cfg stays local.)
    buildGestureConfigPanel(ctx);
    auto& gest_cfg        = ctx.gest_cfg;
    auto& gest_switch     = ctx.gest_switch;
    auto& gest_swipe3_sl  = ctx.gest_swipe3_sl;
    auto& gest_pinch_sl   = ctx.gest_pinch_sl;
    auto& gest_tap_sw     = ctx.gest_tap_sw;
    auto& gest_nscroll_sw = ctx.gest_nscroll_sw;
    auto& gest_2fscroll_sw = ctx.gest_2fscroll_sw;
    auto& update_gest_ui  = ctx.update_gest_ui;

    // ── Display Arrange ───────────────────────────────────────────────────────
    auto disp_arr_col     = std::make_shared<FlexColumn>();
    disp_arr_col->setGap(5);
    auto disp_arr_count_lbl = std::make_shared<TextWidget>("", 10.0f);
    disp_arr_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    auto disp_mirror_switch = std::make_shared<SwitchWidget>(false);
    disp_mirror_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        DisplayMirrorRequest req{};
        req.enable_    = on ? 1u : 0u;
        req.source_id_ = 0u; // compositor picks secondary
        req.target_id_ = 0u;
        std::vector<uint8_t> buf(sizeof(req));
        std::memcpy(buf.data(), &req, sizeof(req));
        g_shell_xbus->Call("com.freebsdx.DisplayArrange",
            static_cast<uint32_t>(DisplayArrangeOpcode::SetMirror), buf, nullptr);
    });

    auto rebuild_disp_arr = std::make_shared<std::function<void()>>();
    *rebuild_disp_arr = [disp_arr_col, disp_arr_count_lbl, disp_mirror_switch,
                         rda_weak = std::weak_ptr<std::function<void()>>(rebuild_disp_arr)]() {
        if (!g_shell_xbus) return;
        disp_arr_col->clearChildren();
        struct DaFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::shared_ptr<SwitchWidget> mirror_sw;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
            bool     any_mirroring{false};
        };
        auto pf = std::make_shared<DaFetcher>();
        pf->col       = disp_arr_col;
        pf->count     = disp_arr_count_lbl;
        pf->mirror_sw = disp_mirror_switch;
        pf->fetch = [pf_weak = std::weak_ptr<DaFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg = page;
            g_shell_xbus->Call("com.freebsdx.DisplayArrange",
                static_cast<uint32_t>(DisplayArrangeOpcode::GetLayout),
                {reinterpret_cast<const uint8_t*>(&pg),
                 reinterpret_cast<const uint8_t*>(&pg) + sizeof(pg)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    DisplayOutputPage op{};
                    std::memcpy(&op, resp.payload_, sizeof(op));
                    if (page == 0) pf->total = op.total_;
                    if (op.valid_) {
                        if (op.mirroring_) pf->any_mirroring = true;
                        // Name + resolution label
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(op.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto res_lbl = std::make_shared<TextWidget>(
                            std::format("{}×{}", op.width_, op.height_), 10.0f);
                        res_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::shared_ptr<Widget> action_widget;
                        if (op.is_primary_) {
                            auto badge = std::make_shared<TextWidget>("\xe2\x98\x85 Primary", 10.0f);
                            badge->setColor({0.85f, 0.70f, 0.20f, 1.0f});
                            action_widget = badge;
                        } else {
                            uint32_t oid = op.output_id_;
                            auto set_btn = std::make_shared<ButtonWidget>("Set Primary",
                                [oid]() {
                                    if (!g_shell_xbus) return;
                                    DisplayOutputRequest req{};
                                    req.output_id_ = oid;
                                    std::vector<uint8_t> buf(sizeof(req));
                                    std::memcpy(buf.data(), &req, sizeof(req));
                                    g_shell_xbus->Call("com.freebsdx.DisplayArrange",
                                        static_cast<uint32_t>(DisplayArrangeOpcode::SetPrimary),
                                        buf, nullptr);
                                });
                            set_btn->setFixedSize(80, 22);
                            set_btn->setBackgroundColor({0.20f, 0.35f, 0.55f, 1.0f});
                            action_widget = set_btn;
                        }
                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, std::make_shared<Spacer>(6, 1),
                                res_lbl,  std::make_shared<Spacer>(8, 1),
                                action_widget});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (!op.has_more_) {
                        pf->count->setText(std::format("{} display{}",
                            pf->total, pf->total == 1 ? "" : "s"));
                        pf->mirror_sw->setValue(pf->any_mirroring);
                    } else {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    // ── GPU Monitor ── (carved into buildGpuMonitorPanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged.)
    buildGpuMonitorPanel(ctx);
    auto& gpu_name_lbl = ctx.gpu_name_lbl;
    auto& gpu_util_lbl = ctx.gpu_util_lbl;
    auto& gpu_vram_lbl = ctx.gpu_vram_lbl;
    auto& gpu_temp_lbl = ctx.gpu_temp_lbl;
    auto& gpu_fan_lbl  = ctx.gpu_fan_lbl;
    auto& update_gpu_ui = ctx.update_gpu_ui;

    // ── Admin Unlock ── (carved into buildAdminUnlockPanel; aliases keep the
    // card/broadcast/reply refs unchanged. inputs/button stay builder-local.)
    buildAdminUnlockPanel(ctx);
    auto& unlock_locked     = ctx.unlock_locked;
    auto& unlock_inputs_row = ctx.unlock_inputs_row;
    auto& unlock_status_lbl = ctx.unlock_status_lbl;

    // ── Screen Saver ── (carved into buildScreenSaverPanel; aliases keep the
    // card/broadcast/timer references unchanged)
    buildScreenSaverPanel(ctx);
    auto& ss_on          = ctx.ss_on;
    auto& ss_switch      = ctx.ss_switch;
    auto& ss_timeout_cur = ctx.ss_timeout_cur;
    auto& ss_mode_cur    = ctx.ss_mode_cur;
    auto& ss_timeout_btns = ctx.ss_timeout_btns;
    auto& ss_mode_btns   = ctx.ss_mode_btns;
    auto& update_ss_ui   = ctx.update_ss_ui;

    // ── Dock Layout ── (Dock Layout + Dev Overlay presets carved into
    // buildDockLayoutPanel; aliases keep the card/broadcast/timer refs unchanged)
    buildDockLayoutPanel(ctx);
    auto& dock_pos_cur    = ctx.dock_pos_cur;
    auto& dock_pos_btns   = ctx.dock_pos_btns;
    auto& dock_autohide   = ctx.dock_autohide;
    auto& update_dock_ui  = ctx.update_dock_ui;
    auto& dev_overlay_cur = ctx.dev_overlay_cur;
    auto& dev_overlay_btns = ctx.dev_overlay_btns;
    auto& dev_overlay_row = ctx.dev_overlay_row;
    auto& update_dev_overlay_ui = ctx.update_dev_overlay_ui;

    // ── XWayland ── (carved into buildXWaylandPanel; aliases keep the
    // card/broadcast/timer references unchanged)
    buildXWaylandPanel(ctx);
    auto& xwl_running  = ctx.xwl_running;
    auto& xwl_switch   = ctx.xwl_switch;
    auto& xwl_info_lbl = ctx.xwl_info_lbl;
    auto& update_xwl_ui = ctx.update_xwl_ui;

    // ── Terminal Sessions ─────────────────────────────────────────────────────
    auto term_sess_count_lbl = std::make_shared<TextWidget>("No sessions", 10.0f);
    term_sess_count_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});
    auto term_sess_col = std::make_shared<FlexColumn>();
    term_sess_col->setGap(4);

    auto rebuild_term_sess = std::make_shared<std::function<void()>>();
    *rebuild_term_sess = [term_sess_col, term_sess_count_lbl,
                          rts_weak = std::weak_ptr<std::function<void()>>(rebuild_term_sess)]() {
        if (!g_shell_xbus) return;
        term_sess_col->clearChildren();

        struct TsFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<TsFetcher>();
        pf->col   = term_sess_col;
        pf->count = term_sess_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<TsFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            TerminalSessionIdRequest req{};
            req.session_id_ = page;
            g_shell_xbus->Call("com.freebsdx.Terminal",
                static_cast<uint32_t>(TerminalOpcode::ListSessions),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    TerminalListPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("No sessions");
                        pf->count->setColor({0.50f, 0.50f, 0.55f, 1.0f});
                        return;
                    }
                    if (page == 0) {
                        pf->count->setText(std::format("{} session(s)", pf->total));
                        pf->count->setColor({0.35f, 0.80f, 0.50f, 1.0f});
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];

                        auto dot = std::make_shared<TextWidget>(
                            e.attached_ ? "\xe2\x97\x8f" : "\xe2\x97\x8b", 11.0f);
                        dot->setColor(e.attached_
                            ? Color{0.35f, 0.80f, 0.50f, 1.0f}
                            : Color{0.45f, 0.45f, 0.50f, 1.0f});

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 11.0f);
                        name_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        uint32_t sid = e.id_;
                        auto kill_btn = std::make_shared<ButtonWidget>("Kill",
                            [sid]() {
                                if (!g_shell_xbus) return;
                                TerminalSessionIdRequest req2{};
                                req2.session_id_ = sid;
                                g_shell_xbus->Call("com.freebsdx.Terminal",
                                    static_cast<uint32_t>(TerminalOpcode::DestroySession),
                                    {reinterpret_cast<const uint8_t*>(&req2), sizeof(req2)},
                                    nullptr);
                            });
                        kill_btn->setFixedSize(38, 22);
                        kill_btn->setBackgroundColor({0.45f, 0.15f, 0.15f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                dot, std::make_shared<Spacer>(4, 1),
                                name_lbl, kill_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    // ── Multi-Seat ── (carved into buildMultiSeatPanel; aliases keep the
    // card/broadcast/timer refs unchanged. SeatFetcher machinery stays local.)
    buildMultiSeatPanel(ctx);
    auto& seat_count_lbl = ctx.seat_count_lbl;
    auto& seat_col       = ctx.seat_col;
    auto& rebuild_seats  = ctx.rebuild_seats;

    // Color Profile presets ── (carved into buildColorProfilePanel; aliases keep
    // the card/broadcast/reply/timer refs unchanged. color_prof_hl stays local.)
    buildColorProfilePanel(ctx);
    auto& color_prof_cur       = ctx.color_prof_cur;
    auto& color_prof_btns      = ctx.color_prof_btns;
    auto& update_color_prof_ui = ctx.update_color_prof_ui;
    auto& color_prof_row       = ctx.color_prof_row;

    auto text_size_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("A Text", 12.0f),
        std::make_shared<Spacer>(1, 1, true),
        (*text_size_btns)[0],
        std::make_shared<Spacer>(4, 1),
        (*text_size_btns)[1],
        std::make_shared<Spacer>(4, 1),
        (*text_size_btns)[2],
    });

    // Cursor Size presets ── (carved into buildCursorSizePanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged. cursor_size_hl stays local.)
    buildCursorSizePanel(ctx);
    auto& cursor_size_cur       = ctx.cursor_size_cur;
    auto& cursor_size_btns      = ctx.cursor_size_btns;
    auto& update_cursor_size_ui = ctx.update_cursor_size_ui;
    auto& cursor_size_row       = ctx.cursor_size_row;

    // Reduce Motion toggle
    // Reduce Motion (state-label toggle: amber "Reduced" / dim "Standard")
    auto rmotion = buildToggleTile<ReduceMotionState, ReduceMotionRequest>(
        "com.freebsdx.ReduceMotion",
        static_cast<uint32_t>(ReduceMotionOpcode::SetEnabled), false,
        &ReduceMotionRequest::enabled_,
        [](const ReduceMotionState& st) { return st.enabled_ != 0; },
        "Reduced",  Color{0.95f, 0.72f, 0.20f, 1.0f},
        "Standard", Color{0.50f, 0.50f, 0.55f, 1.0f});
    auto& reduce_motion_switch     = rmotion.sw;
    auto& reduce_motion_status_lbl = rmotion.status_lbl;
    auto& update_reduce_motion_ui  = rmotion.update;

    // Stylus input ── (carved into buildStylusPanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged. stylus_pressure_hl stays local.)
    buildStylusPanel(ctx);
    auto& stylus_palm_switch   = ctx.stylus_palm_switch;
    auto& stylus_connected     = ctx.stylus_connected;
    auto& stylus_pressure_cur  = ctx.stylus_pressure_cur;
    auto& stylus_pressure_btns = ctx.stylus_pressure_btns;
    auto& stylus_conn_lbl      = ctx.stylus_conn_lbl;
    auto& stylus_pressure_row  = ctx.stylus_pressure_row;
    auto& update_stylus_ui     = ctx.update_stylus_ui;

    // Touchpad toggle
    // Touchpad (state-label toggle: green "Enabled" / red "Disabled")
    auto tp = buildToggleTile<TouchpadState, TouchpadRequest>(
        "com.freebsdx.Touchpad",
        static_cast<uint32_t>(TouchpadOpcode::SetEnabled), true,
        &TouchpadRequest::enabled_,
        [](const TouchpadState& st) { return st.enabled_ != 0; },
        "Enabled",  Color{0.35f, 0.80f, 0.45f, 1.0f},
        "Disabled", Color{0.85f, 0.35f, 0.35f, 1.0f});
    auto& touchpad_switch     = tp.sw;
    auto& touchpad_status_lbl = tp.status_lbl;
    auto& update_touchpad_ui  = tp.update;

    // Scroll direction (state-label toggle: text-only, colour unchanged)
    auto scrolldir = buildToggleTile<ScrollDirState, ScrollDirRequest>(
        "com.freebsdx.ScrollDir",
        static_cast<uint32_t>(ScrollDirOpcode::SetNatural), false,
        &ScrollDirRequest::natural_,
        [](const ScrollDirState& st) { return st.natural_ != 0; },
        "Natural",     Color{0.55f, 0.55f, 0.60f, 1.0f},
        "Traditional", Color{0.55f, 0.55f, 0.60f, 1.0f});
    auto& scroll_dir_switch    = scrolldir.sw;
    auto& scroll_dir_mode_lbl  = scrolldir.status_lbl;
    auto& update_scroll_dir_ui = scrolldir.update;

    auto qs_dnd_switch = std::make_shared<SwitchWidget>(false);
    qs_dnd_switch->setOnChanged([](bool on) { g_dnd = on; });

    auto qs_dark_switch = std::make_shared<SwitchWidget>(false);
    qs_dark_switch->setOnChanged([](bool on) {
        if (!g_shell_xbus) return;
        ThemeSetRequest req{};
        req.dark_mode_  = on ? 1u : 0u;
        req.accent_idx_ = g_accent_idx;
        g_shell_xbus->Call("com.freebsdx.Theme",
            static_cast<uint32_t>(ThemeOpcode::SetTheme),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    // Brightness
    auto qs_bright_val    = std::make_shared<TextWidget>("100%", 11.0f);
    auto qs_bright_slider = std::make_shared<SliderWidget>(100.0f, 0.0f, 100.0f);
    qs_bright_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    qs_bright_slider->setOnChanged([qs_bright_val](float v) {
        qs_bright_val->setText(std::format("{}%", static_cast<int>(v)));
        if (!g_shell_xbus) return;
        BacklightState req{};
        req.level_ = static_cast<uint32_t>(v);
        g_shell_xbus->Call("com.freebsdx.Backlight",
            static_cast<uint32_t>(BacklightOpcode::SetBrightness),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    // Volume
    auto qs_vol_val    = std::make_shared<TextWidget>("75%", 11.0f);
    auto qs_vol_slider = std::make_shared<SliderWidget>(75.0f, 0.0f, 100.0f);
    qs_vol_val->setColor({0.72f, 0.72f, 0.72f, 1.0f});
    qs_vol_slider->setOnChanged([qs_vol_val](float v) {
        qs_vol_val->setText(std::format("{}%", static_cast<int>(v)));
        if (!g_shell_xbus) return;
        AudioVolumeRequest req{};
        req.volume_ = static_cast<uint32_t>(v);
        req.muted_  = 0xFF; // leave mute state unchanged
        g_shell_xbus->Call("com.freebsdx.Audio",
            static_cast<uint32_t>(AudioOpcode::SetVolume),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });

    // ── Night Shift slider ── (carved into buildNightShiftPanel; aliases keep
    // the card/broadcast/timer references unchanged)
    buildNightShiftPanel(ctx);
    auto& qs_ns_switch     = ctx.qs_ns_switch;
    auto& qs_ns_val        = ctx.qs_ns_val;
    auto& qs_ns_slider     = ctx.qs_ns_slider;
    auto& qs_ns_hdr_row    = ctx.qs_ns_hdr_row;
    auto& qs_ns_slider_row = ctx.qs_ns_slider_row;
    auto& send_ns_config   = ctx.send_ns_config;

    // Toggle pill factory
    auto make_qs_toggle = [](const std::string& icon, const std::string& label,
                              std::shared_ptr<SwitchWidget> sw,
                              std::shared_ptr<TextWidget> sub = nullptr)
        -> std::shared_ptr<FlexColumn>
    {
        auto icon_w = std::make_shared<TextWidget>(icon, 22.0f);
        auto lbl_w  = std::make_shared<TextWidget>(label, 12.0f);
        lbl_w->setColor({0.9f, 0.9f, 0.9f, 1.0f});
        std::vector<std::shared_ptr<Widget>> children = {icon_w, std::make_shared<Spacer>(1,4), lbl_w};
        if (sub) children.push_back(sub);
        children.push_back(std::make_shared<Spacer>(1,6));
        children.push_back(sw);
        auto pill = std::make_shared<FlexColumn>(children);
        pill->setBackgroundColor({0.18f, 0.2f, 0.28f, 0.96f});
        pill->setPadding(12);
        pill->setFixedSize(96, 110);
        pill->setAlignment(Alignment::Center);
        return pill;
    };

    auto qs_wifi_pill = make_qs_toggle("📶", "Wi-Fi",     qs_wifi_switch, qs_wifi_sub);
    auto qs_bt_pill   = make_qs_toggle("🔵", "Bluetooth", qs_bt_switch);
    auto qs_dnd_pill  = make_qs_toggle("🔕", "DND",       qs_dnd_switch);
    auto qs_dark_pill = make_qs_toggle("🌙", "Dark Mode", qs_dark_switch);

    auto qs_toggles_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        qs_wifi_pill, std::make_shared<Spacer>(8, 1), qs_bt_pill,
        std::make_shared<Spacer>(8, 1), qs_dnd_pill,
        std::make_shared<Spacer>(8, 1), qs_dark_pill
    });

    // ── Focus mode toggle ── (carved into buildFocusToggleButton; qs_dnd_switch
    // passed by-ref (cross-coupled). focus_btn escapes; focus_lbl stays
    // builder-local. Alias keeps the card ref unchanged.)
    buildFocusToggleButton(ctx, qs_dnd_switch);
    auto& focus_btn = ctx.focus_btn;

    // ── Color theme presets ── (carved into buildColorThemePanel; qs_dark_switch
    // passed by-ref (cross-coupled). labels + idx + apply_tp + preset-row escape;
    // kPresets stays builder-local. Aliases keep card/broadcast/OnConnect refs
    // unchanged.)
    buildColorThemePanel(ctx, qs_dark_switch);
    auto& tp_labels         = ctx.tp_labels;
    auto& tp_idx            = ctx.tp_idx;
    auto& apply_tp          = ctx.apply_tp;
    auto& theme_presets_row = ctx.theme_presets_row;

    // Slider rows
    auto qs_bright_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🔆", 15.0f),
        std::make_shared<Spacer>(8, 1),
        qs_bright_slider,
        std::make_shared<Spacer>(8, 1),
        qs_bright_val
    });

    // Auto-Brightness toggle ── (carved into buildAutoBrightnessPanel; aliases
    // keep the card/broadcast/reply/timer refs unchanged.)
    buildAutoBrightnessPanel(ctx);
    auto& auto_br_switch   = ctx.auto_br_switch;
    auto& auto_br_lux_lbl  = ctx.auto_br_lux_lbl;
    auto& update_auto_br_ui = ctx.update_auto_br_ui;
    auto& auto_br_hdr_row  = ctx.auto_br_hdr_row;

    // HDR / VRR quick row — populated in OnConnect after display query
    auto qs_hdr_switch = std::make_shared<SwitchWidget>(false);
    auto qs_vrr_switch = std::make_shared<SwitchWidget>(false);
    auto qs_hdr_lbl    = std::make_shared<TextWidget>("HDR", 11.0f);
    auto qs_vrr_lbl    = std::make_shared<TextWidget>("VRR", 11.0f);
    qs_hdr_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    qs_vrr_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
    auto qs_disp_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🖥", 14.0f),
        std::make_shared<Spacer>(8, 1),
        qs_hdr_lbl, std::make_shared<Spacer>(4, 1), qs_hdr_switch,
        std::make_shared<Spacer>(12, 1),
        qs_vrr_lbl, std::make_shared<Spacer>(4, 1), qs_vrr_switch,
    });
    qs_disp_row->setVisibility(false); // shown only when at least one feature is capable

    // Display Refresh Rate preset buttons ── (carved into buildRefreshRatePanel;
    // aliases keep the card/broadcast/reply/timer refs unchanged. rrate_hl stays
    // local.)
    buildRefreshRatePanel(ctx);
    auto& rrate_cur       = ctx.rrate_cur;
    auto& rrate_mask      = ctx.rrate_mask;
    auto& rrate_btns      = ctx.rrate_btns;
    auto& update_rrate_ui = ctx.update_rrate_ui;
    auto& rrate_row       = ctx.rrate_row;

    // ── Keyboard Backlight ── (carved into buildKeyboardBacklightPanel; aliases
    // keep the card/broadcast/timer references unchanged)
    buildKeyboardBacklightPanel(ctx);
    auto& kb_bl_switch    = ctx.kb_bl_switch;
    auto& kb_bl_val       = ctx.kb_bl_val;
    auto& kb_bl_slider    = ctx.kb_bl_slider;
    auto& kb_bl_row       = ctx.kb_bl_row;
    auto& update_kb_bl_ui = ctx.update_kb_bl_ui;

    auto qs_vol_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🔊", 15.0f),
        std::make_shared<Spacer>(8, 1),
        qs_vol_slider,
        std::make_shared<Spacer>(8, 1),
        qs_vol_val
    });

    // Volume preset buttons
    static constexpr int kVolPresets[] = {25, 50, 75, 100};
    static constexpr int kNumVolPresets = 4;
    auto vol_preset_btns = std::make_shared<
        std::array<std::shared_ptr<ButtonWidget>, kNumVolPresets>>();

    auto apply_vol_preset = [qs_vol_slider, qs_vol_val, vol_preset_btns](int idx) {
        g_vol_preset = idx;
        saveVolPreset();
        int level = kVolPresets[idx];
        qs_vol_slider->setValue(static_cast<float>(level));
        qs_vol_val->setText(std::format("{}%", level));
        if (g_shell_xbus) {
            AudioVolumeRequest req{};
            req.volume_ = static_cast<uint32_t>(level);
            req.muted_  = 0xFF;
            g_shell_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::SetVolume),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        }
        for (int i = 0; i < kNumVolPresets; ++i) {
            (*vol_preset_btns)[i]->setBackgroundColor(
                i == idx ? Color{0.22f, 0.44f, 0.88f, 0.95f}
                         : Color{0.16f, 0.18f, 0.26f, 1.0f});
        }
    };

    auto vol_preset_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
    for (int i = 0; i < kNumVolPresets; ++i) {
        int pi = i;
        auto btn = std::make_shared<ButtonWidget>(
            std::format("{}%", kVolPresets[i]),
            [apply_vol_preset, pi]() { apply_vol_preset(pi); });
        btn->setFixedSize(58, 28);
        btn->setFontSize(11.0f);
        btn->setBackgroundColor({0.16f, 0.18f, 0.26f, 1.0f});
        (*vol_preset_btns)[i] = btn;
        vol_preset_row->addChild(btn);
        if (i < kNumVolPresets - 1) vol_preset_row->addChild(std::make_shared<Spacer>(4, 1));
    }

    // Full Quick Settings panel (replaces old narrow quick_panel)
    // ── Power menu (Sleep / Restart / Shut Down) ── (carved into buildPowerMenuPanel;
    // confirm-row + sleep/restart/shutdown/lock buttons escape — the confirm label,
    // pending opcode, Yes/Cancel buttons, and hide_confirm/make_power_btn helpers
    // stay builder-local. Aliases keep the card refs unchanged.)
    buildPowerMenuPanel(ctx);
    auto& power_confirm_row  = ctx.power_confirm_row;
    auto& power_sleep_btn    = ctx.power_sleep_btn;
    auto& power_restart_btn  = ctx.power_restart_btn;
    auto& power_shutdown_btn = ctx.power_shutdown_btn;
    auto& power_lock_btn     = ctx.power_lock_btn;

    // ── Auto-lock toggle ── (carved into buildAutoLockPanel; aliases keep the
    // card/broadcast/timer references unchanged)
    buildAutoLockPanel(ctx);
    auto& qs_autolock_switch = ctx.qs_autolock_switch;
    auto& qs_autolock_row    = ctx.qs_autolock_row;

    // ── Battery Saver toggle ── (carved into buildBatterySaverPanel; aliases
    // keep the card/broadcast references unchanged)
    buildBatterySaverPanel(ctx);
    auto& bat_saver_switch = ctx.bat_saver_switch;
    auto& bat_saver_lbl    = ctx.bat_saver_lbl;
    auto& bat_saver_row    = ctx.bat_saver_row;

    // ── Power profile switcher ── (Presentation Mode + Power Profile carved into
    // buildPowerProfilePanel; aliases keep the card/broadcast/timer refs unchanged)
    buildPowerProfilePanel(ctx);
    auto& pres_switch     = ctx.pres_switch;
    auto& pres_status_lbl = ctx.pres_status_lbl;
    auto& update_pres_ui  = ctx.update_pres_ui;
    auto& pp_status_lbl   = ctx.pp_status_lbl;
    auto& pp_row1         = ctx.pp_row1;
    auto& pp_row2         = ctx.pp_row2;
    auto& update_pp_ui    = ctx.update_pp_ui;

    // ── DND Schedule UI ── (carved into buildDndSchedulePanel; switch + from/to
    // labels + hdr/times rows + countdown label escape — the hour dec/inc buttons +
    // make_hour_* helpers stay builder-local. Aliases keep card/broadcast/reply/
    // timer refs unchanged.)
    buildDndSchedulePanel(ctx);
    auto& dnd_sched_switch  = ctx.dnd_sched_switch;
    auto& dnd_from_lbl      = ctx.dnd_from_lbl;
    auto& dnd_to_lbl        = ctx.dnd_to_lbl;
    auto& dnd_sched_hdr     = ctx.dnd_sched_hdr;
    auto& dnd_sched_times   = ctx.dnd_sched_times;
    auto& dnd_countdown_lbl = ctx.dnd_countdown_lbl;

    // ── Night Shift Schedule UI ── (carved into buildNightShiftSchedulePanel;
    // switch + from/to labels + hdr/times rows escape — the hour dec/inc buttons +
    // make_ns_hour_* helpers stay builder-local. Aliases keep card/broadcast/reply
    // refs unchanged.)
    buildNightShiftSchedulePanel(ctx);
    auto& ns_sched_switch = ctx.ns_sched_switch;
    auto& ns_from_lbl     = ctx.ns_from_lbl;
    auto& ns_to_lbl       = ctx.ns_to_lbl;
    auto& ns_sched_hdr    = ctx.ns_sched_hdr;
    auto& ns_sched_times  = ctx.ns_sched_times;

    // ── Night Light (compositor colour-temperature control) ── (carved into
    // buildNightLightPanel; aliases keep the card/broadcast/reply/timer refs
    // unchanged. send_nl_temp / send_nl_schedule stay local to the builder.)
    buildNightLightPanel(ctx);
    auto& nl_switch       = ctx.nl_switch;
    auto& nl_temp_val     = ctx.nl_temp_val;
    auto& nl_slider       = ctx.nl_slider;
    auto& nl_sched_switch = ctx.nl_sched_switch;
    auto& nl_from_input   = ctx.nl_from_input;
    auto& nl_to_input     = ctx.nl_to_input;
    auto& nl_hdr_row      = ctx.nl_hdr_row;
    auto& nl_slider_row   = ctx.nl_slider_row;
    auto& nl_sched_hdr    = ctx.nl_sched_hdr;
    auto& nl_sched_times  = ctx.nl_sched_times;
    auto& update_nl_ui    = ctx.update_nl_ui;

    // ── Focus Mode ── (carved into buildFocusModePanel; aliases keep the
    // card/broadcast/timer references unchanged)
    buildFocusModePanel(ctx);
    auto& focus_status_lbl = ctx.focus_status_lbl;
    auto& focus_end_btn    = ctx.focus_end_btn;
    auto& focus_dur_row    = ctx.focus_dur_row;
    auto& focus_cat_row    = ctx.focus_cat_row;
    auto& update_focus_ui  = ctx.update_focus_ui;

    // ── Display Settings overlay ── (carved into buildDisplaySettingsPanel;
    // aliases keep the card/broadcast/root-composition references unchanged)
    buildDisplaySettingsPanel(ctx);
    auto& disp_col     = ctx.disp_col;
    auto& disp_overlay = ctx.disp_overlay;
    auto& disp_btn     = ctx.disp_btn;

    // ── Session restore ── (carved into buildSessionRestorePanel; aliases keep
    // the card/broadcast/timer references unchanged. Takes `apps` by reference.)
    buildSessionRestorePanel(ctx, apps);
    auto& live_titles    = ctx.live_titles;
    auto& sess_hdr_lbl   = ctx.sess_hdr_lbl;
    auto& sess_switch    = ctx.sess_switch;
    auto& sess_sw_row    = ctx.sess_sw_row;
    auto& sess_count_lbl = ctx.sess_count_lbl;
    auto& sess_save_row  = ctx.sess_save_row;

    auto power_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        power_lock_btn,
        std::make_shared<Spacer>(6, 1),
        power_sleep_btn,
        std::make_shared<Spacer>(6, 1),
        power_restart_btn,
        std::make_shared<Spacer>(6, 1),
        power_shutdown_btn,
    });

    // ── Pomodoro Focus Timer ── (Pomodoro + Wallpaper rotation carved into
    // buildPomodoroPanel; aliases keep the card/broadcast/timer refs unchanged)
    buildPomodoroPanel(ctx);
    auto& pomo_phase_lbl = ctx.pomo_phase_lbl;
    auto& pomo_cd_lbl    = ctx.pomo_cd_lbl;
    auto& pomo_tbar_w    = ctx.pomo_tbar_w;
    auto& pomo_start_lbl = ctx.pomo_start_lbl;
    auto& pomo_hdr_row   = ctx.pomo_hdr_row;
    auto& pomo_btn_row   = ctx.pomo_btn_row;
    auto& wp_rotate_switch = ctx.wp_rotate_switch;
    auto& wp_folder_lbl    = ctx.wp_folder_lbl;
    auto& wp_interval_lbl  = ctx.wp_interval_lbl;
    auto& wp_interval_dec  = ctx.wp_interval_dec;
    auto& wp_interval_inc  = ctx.wp_interval_inc;
    auto& update_wp_interval_lbl = ctx.update_wp_interval_lbl;

    // ── Per-workspace wallpaper strip ── (carved into buildWorkspaceWallpaperPanel;
    // open-fn (body wired in OnConnect) + btns + update-hl + hdr/row escape. Aliases
    // keep the card/broadcast/timer refs unchanged.)
    buildWorkspaceWallpaperPanel(ctx);
    auto& ws_wp_open_fn   = ctx.ws_wp_open_fn;
    auto& ws_wp_btns      = ctx.ws_wp_btns;
    auto& ws_wp_update_hl = ctx.ws_wp_update_hl;
    auto& ws_wp_hdr_lbl   = ctx.ws_wp_hdr_lbl;
    auto& ws_wp_row       = ctx.ws_wp_row;

    // ── Keyboard layout switcher ── (carved into buildKeyboardLayoutPanel;
    // aliases keep the card/broadcast/timer references unchanged)
    buildKeyboardLayoutPanel(ctx);
    auto& kb_active_lbl = ctx.kb_active_lbl;
    auto& kb_layout_row = ctx.kb_layout_row;

    auto wp_rotate_hdr = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("🖼 Wallpaper Rotation", 13.0f),
        std::make_shared<Spacer>(1, 1, true),
        wp_rotate_switch
    });
    auto wp_interval_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("Interval:", 12.0f),
        std::make_shared<Spacer>(6, 1),
        wp_interval_dec,
        std::make_shared<Spacer>(6, 1),
        wp_interval_lbl,
        std::make_shared<Spacer>(6, 1),
        wp_interval_inc
    });

    // Network data usage label
    auto qp_data_w = std::make_shared<TextWidget>("", 12.0f);
    qp_data_w->setColor({0.55f, 0.75f, 0.55f, 1.0f});

    auto qs_panel = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        // Header
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Quick Settings", 17.0f),
            std::make_shared<Spacer>(1, 1, true),
            disp_btn,
        }),
        std::make_shared<Spacer>(1, 14),
        // Airplane Mode
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("✈ Airplane Mode", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            ap_switch,
        }),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // Hotspot
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x93\xa1 Hotspot", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            hotspot_status_lbl,
            std::make_shared<Spacer>(8, 1),
            hotspot_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        hotspot_ssid_lbl,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // NTP / Time Sync
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x95\x90 Time Sync", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            ntp_status_lbl,
            std::make_shared<Spacer>(8, 1),
            ntp_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        ntp_server_lbl,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // Time Zone
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x95\x90 Time Zone", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            tz_prev_btn,
            std::make_shared<Spacer>(6, 1),
            tz_name_lbl,
            std::make_shared<Spacer>(6, 1),
            tz_next_btn,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            tz_disp_lbl,
            std::make_shared<Spacer>(10, 1),
            tz_time_lbl,
        }),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // DNS Configuration
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x9b\xa1 DNS", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            (*dns_mode_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*dns_mode_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*dns_mode_btns)[2],
        }),
        std::make_shared<Spacer>(1, 4),
        dns_resolver_lbl,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // Proxy Settings
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8c\x90 Proxy", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            (*proxy_mode_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*proxy_mode_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*proxy_mode_btns)[2],
        }),
        std::make_shared<Spacer>(1, 4),
        proxy_info_lbl,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // Remote Login
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x90 Remote Login", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            rl_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        rl_info_lbl,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 10),
        // Language / Locale
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8c\x8d Language", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            (*locale_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*locale_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*locale_btns)[2],
            std::make_shared<Spacer>(4, 1),
            (*locale_btns)[3],
        }),
        std::make_shared<Spacer>(1, 4),
        locale_sub_lbl,
        std::make_shared<Spacer>(1, 10),
        // Toggles
        qs_toggles_row,
        std::make_shared<Spacer>(1, 10),
        // Media controls
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 8),
        mp_title_lbl,
        mp_artist_lbl,
        std::make_shared<Spacer>(1, 4),
        mp_btn_row,
        std::make_shared<Spacer>(1, 4),
        mp_progress_lbl,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<DividerWidget>(0.4f),
        std::make_shared<Spacer>(1, 8),
        // Theme presets
        theme_presets_row,
        std::make_shared<Spacer>(1, 8),
        // Focus mode
        focus_btn,
        std::make_shared<Spacer>(1, 10),
        // Wake Lock
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x98\x80 Keep Awake", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            (*wl_dur_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*wl_dur_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*wl_dur_btns)[2],
            std::make_shared<Spacer>(8, 1),
            wl_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        wl_status_lbl,
        std::make_shared<Spacer>(1, 10),
        // DND Schedule
        dnd_sched_hdr,
        std::make_shared<Spacer>(1, 6),
        dnd_sched_times,
        std::make_shared<Spacer>(1, 4),
        dnd_countdown_lbl,
        std::make_shared<Spacer>(1, 10),
        // Brightness
        qs_bright_row,
        std::make_shared<Spacer>(1, 8),
        auto_br_hdr_row,
        std::make_shared<Spacer>(1, 4),
        auto_br_lux_lbl,
        std::make_shared<Spacer>(1, 10),
        qs_disp_row,
        std::make_shared<Spacer>(1, 8),
        rrate_row,
        std::make_shared<Spacer>(1, 10),
        // Keyboard Backlight
        kb_bl_row,
        std::make_shared<Spacer>(1, 6),
        // Night Shift
        qs_ns_hdr_row,
        std::make_shared<Spacer>(1, 6),
        qs_ns_slider_row,
        std::make_shared<Spacer>(1, 8),
        ns_sched_hdr,
        std::make_shared<Spacer>(1, 6),
        ns_sched_times,
        std::make_shared<Spacer>(1, 10),
        // Night Light (compositor gamma / colour-temperature)
        nl_hdr_row,
        std::make_shared<Spacer>(1, 6),
        nl_slider_row,
        std::make_shared<Spacer>(1, 6),
        nl_sched_hdr,
        std::make_shared<Spacer>(1, 4),
        nl_sched_times,
        std::make_shared<Spacer>(1, 10),
        // Focus Mode
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🎯 Focus", 13.0f),
            std::make_shared<Spacer>(8, 1),
            focus_status_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        focus_dur_row,
        std::make_shared<Spacer>(1, 6),
        focus_cat_row,
        std::make_shared<Spacer>(1, 10),
        // Volume
        qs_vol_row,
        std::make_shared<Spacer>(1, 6),
        vol_preset_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Audio Output ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🔊 Output", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            aout_name_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        aout_pills_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // Network section
        std::make_shared<TextWidget>("Network", 13.0f),
        std::make_shared<Spacer>(1, 8),
        qp_conn_w, qp_ip_w, qp_signal_w, qp_bw_w,
        net_rx_spark_lbl, net_tx_spark_lbl,
        qp_data_w,
        std::make_shared<Spacer>(1, 8),
        qp_disc_btn,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<ButtonWidget>("Scan", [do_scan_page]() {
            if (do_scan_page && *do_scan_page) (*do_scan_page)(0);
        }),
        std::make_shared<Spacer>(1, 6),
        wifi_connect_col,
        std::make_shared<Spacer>(1, 4),
        std::make_shared<ScrollViewWidget>(qp_net_list),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Bluetooth ─────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🔵 Bluetooth", 13.0f),
            std::make_shared<Spacer>(8, 1),
            bt_state_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        bt_toggle_row,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Paired Devices", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            bt_scan_btn,
        }),
        std::make_shared<Spacer>(1, 4),
        bt_status_lbl,
        bt_devices_col,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── VPN ──────────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🔒 VPN", 13.0f),
            std::make_shared<Spacer>(8, 1),
            vpn_status_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        vpn_info_lbl,
        std::make_shared<Spacer>(1, 6),
        vpn_iface_input,
        std::make_shared<Spacer>(1, 6),
        vpn_btn_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Game Mode ─────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🎮 Game Mode", 13.0f),
            std::make_shared<Spacer>(8, 1),
            gm_status_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        gm_app_lbl,
        std::make_shared<Spacer>(1, 6),
        gm_btn_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Wi-Fi Hotspot ─────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("📡 Hotspot", 13.0f),
            std::make_shared<Spacer>(8, 1),
            hs_status_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        hs_btn_row,
        std::make_shared<Spacer>(1, 6),
        hs_cfg_row1,
        std::make_shared<Spacer>(1, 4),
        hs_cfg_row2,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Screen Cast ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("📺 Cast", 13.0f),
            std::make_shared<Spacer>(8, 1),
            cast_status_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        cast_elapsed_lbl,
        std::make_shared<Spacer>(1, 6),
        cast_ctrl_row,
        std::make_shared<Spacer>(1, 6),
        cast_receiver_col,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Remote Desktop ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🖥 Remote Desktop", 13.0f),
            std::make_shared<Spacer>(8, 1),
            rd_status_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        rd_btn_row,
        std::make_shared<Spacer>(1, 6),
        rd_pass_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Firewall ──────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🛡 Firewall", 13.0f),
            std::make_shared<Spacer>(8, 1),
            fw_status_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        fw_btn_row,
        std::make_shared<Spacer>(1, 6),
        fw_blockall_btn,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── DNS Ad-Blocker ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🚫 Ad Blocker", 13.0f),
            std::make_shared<Spacer>(8, 1),
            ab_status_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        ab_detail_lbl,
        std::make_shared<Spacer>(1, 6),
        ab_btn_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Privacy ───────────────────────────────────────────────────────────
        std::make_shared<TextWidget>("🔏 Privacy", 13.0f),
        std::make_shared<Spacer>(1, 6),
        cam_row,
        std::make_shared<Spacer>(1, 6),
        mic_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Accessibility ─────────────────────────────────────────────────────
        std::make_shared<TextWidget>("♿ Accessibility", 13.0f),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Font", 11.0f),
            std::make_shared<Spacer>(8, 1),
            a11y_font_row,
        }),
        std::make_shared<Spacer>(1, 6),
        a11y_cursor_row,
        std::make_shared<Spacer>(1, 6),
        a11y_toggle_row,
        std::make_shared<Spacer>(1, 10),
        // ── Color Filter ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🎨 Color Filter", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            cf_switch,
        }),
        std::make_shared<Spacer>(1, 6),
        cf_type_row,
        std::make_shared<Spacer>(1, 6),
        cf_intensity_row,
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Monochrome", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            cf_mono_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Location Services ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("📍 Location", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            loc_count_lbl,
            std::make_shared<Spacer>(6, 1),
            loc_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        loc_last_lbl,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Screen Timeout ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("⏱ Timeout", 12.0f),
        }),
        std::make_shared<Spacer>(1, 6),
        stout_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Screen Rotation Lock ──────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🔄 Rotation", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            rot_orient_lbl,
            std::make_shared<Spacer>(8, 1),
            rot_lock_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Microphone Mute ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("🎤 Mic", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            mic_mute_status,
            std::make_shared<Spacer>(8, 1),
            mic_mute_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Camera Devices ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x93\xb7 Camera", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            cam_dev_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        cam_dev_col,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Update Channel ────────────────────────────────────────────────────
        upd_ch_row,
        std::make_shared<Spacer>(1, 4),
        upd_ch_badge_lbl,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Package Signing ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x8f Require Signed", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            pkgsign_req_switch,
        }),
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Allow Unknown", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            pkgsign_unk_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Dock Layout ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x8a\x9e Dock", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            (*dock_pos_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*dock_pos_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*dock_pos_btns)[2],
            std::make_shared<Spacer>(8, 1),
            dock_autohide,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Screen Saver ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xa5 Screen Saver", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            ss_switch,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*ss_mode_btns)[0], std::make_shared<Spacer>(4, 1),
            (*ss_mode_btns)[1], std::make_shared<Spacer>(4, 1),
            (*ss_mode_btns)[2], std::make_shared<Spacer>(1, 1, true),
            (*ss_timeout_btns)[0], std::make_shared<Spacer>(4, 1),
            (*ss_timeout_btns)[1], std::make_shared<Spacer>(4, 1),
            (*ss_timeout_btns)[2], std::make_shared<Spacer>(4, 1),
            (*ss_timeout_btns)[3],
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Display Scaling ───────────────────────────────────────────────────
        disp_scale_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Color Profile ─────────────────────────────────────────────────────
        color_prof_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Text Size ─────────────────────────────────────────────────────────
        text_size_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Cursor Size ───────────────────────────────────────────────────────
        cursor_size_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Reduce Motion ─────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8e\x9e Motion", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            reduce_motion_status_lbl,
            std::make_shared<Spacer>(8, 1),
            reduce_motion_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Touchpad ──────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xb1 Touchpad", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            touchpad_status_lbl,
            std::make_shared<Spacer>(8, 1),
            touchpad_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Stylus ────────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x9c\x8f Stylus", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            std::make_shared<TextWidget>("Palm Reject", 11.0f),
            std::make_shared<Spacer>(6, 1),
            stylus_palm_switch,
        }),
        std::make_shared<Spacer>(1, 6),
        stylus_pressure_row,
        std::make_shared<Spacer>(1, 4),
        stylus_conn_lbl,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Scroll Direction ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xb1 Scroll", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            scroll_dir_mode_lbl,
            std::make_shared<Spacer>(8, 1),
            scroll_dir_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Dev Overlay ───────────────────────────────────────────────────────
        dev_overlay_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── XWayland ──────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\xaa\x9f XWayland", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            xwl_switch,
        }),
        std::make_shared<Spacer>(1, 4),
        xwl_info_lbl,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Terminal Sessions ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x8c\xa8 Terminal", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            term_sess_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        term_sess_col,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Multi-Seat ────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\xaa\x91 Seats", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            seat_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        seat_col,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Power Profile ─────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("⚡ Power Profile", 13.0f),
            std::make_shared<Spacer>(8, 1),
            pp_status_lbl,
        }),
        std::make_shared<Spacer>(1, 8),
        pp_row1,
        std::make_shared<Spacer>(1, 6),
        pp_row2,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Presentation Mode ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xa5 Presentation", 13.0f),
            std::make_shared<Spacer>(1, 1, true),
            pres_status_lbl,
            std::make_shared<Spacer>(8, 1),
            pres_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        qs_autolock_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── GPU Monitor ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xa5 GPU", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            gpu_util_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            gpu_name_lbl,
            std::make_shared<Spacer>(1, 1, true),
            gpu_vram_lbl,
            std::make_shared<Spacer>(8, 1),
            gpu_temp_lbl,
            std::make_shared<Spacer>(8, 1),
            gpu_fan_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Display Arrange ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x96\xb5 Displays", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            disp_arr_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        disp_arr_col,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Mirror displays", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            disp_mirror_switch,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Gesture Config ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x91\x86 Gestures", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            gest_switch,
        }),
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Swipe", 11.0f),
            std::make_shared<Spacer>(8, 1),
            gest_swipe3_sl,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Pinch", 11.0f),
            std::make_shared<Spacer>(8, 1),
            gest_pinch_sl,
        }),
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Tap to click", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            gest_tap_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Natural scroll", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            gest_nscroll_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Two-finger scroll", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            gest_2fscroll_sw,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Spatial Audio ─────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8e\xa7 Spatial Audio", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            sa_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        sa_device_lbl,
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*sa_mode_btns)[0], std::make_shared<Spacer>(4, 1),
            (*sa_mode_btns)[1], std::make_shared<Spacer>(4, 1),
            (*sa_mode_btns)[2],
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            sa_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── System Sounds ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x8a System Sounds", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            sndfx_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Volume", 11.0f),
            std::make_shared<Spacer>(8, 1),
            sndfx_vol_slider,
            std::make_shared<Spacer>(6, 1),
            sndfx_vol_val_lbl,
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            sndfx_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Live Captions ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x92\xac Live Captions", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            captions_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*captions_pos_btns)[0], std::make_shared<Spacer>(4, 1),
            (*captions_pos_btns)[1],
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            captions_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Data Saver ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x93\xb6 Data Saver", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            dsaver_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        dsaver_saved_lbl,
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            dsaver_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Fast Charging ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x9a\xa1 Fast Charging", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            fastchg_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        fastchg_rate_lbl,
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            fastchg_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Do Not Disturb ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8c\x99 Do Not Disturb", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            silence_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*silence_dur_btns)[0], std::make_shared<Spacer>(4, 1),
            (*silence_dur_btns)[1], std::make_shared<Spacer>(4, 1),
            (*silence_dur_btns)[2], std::make_shared<Spacer>(4, 1),
            (*silence_dur_btns)[3],
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            silence_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Battery Health ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x8b Battery Health", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            bhealth_opt_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        bhealth_info_lbl,
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            bhealth_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── True Tone ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8c\x88 True Tone", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            ttone_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Strength", 11.0f),
            std::make_shared<Spacer>(8, 1),
            ttone_strength_slider,
            std::make_shared<Spacer>(6, 1),
            ttone_strength_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        ttone_info_lbl,
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            ttone_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Optimized Storage ───────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x92\xbe Optimized Storage", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            ostore_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Free target", 11.0f),
            std::make_shared<Spacer>(8, 1),
            ostore_target_slider,
            std::make_shared<Spacer>(6, 1),
            ostore_target_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        ostore_info_lbl,
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            ostore_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Mono Audio ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x8a Mono Audio", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            monoaud_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Balance", 11.0f),
            std::make_shared<Spacer>(8, 1),
            monoaud_balance_slider,
            std::make_shared<Spacer>(6, 1),
            monoaud_balance_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            monoaud_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Stage Manager ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x97\x94 Stage Manager", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            stagemgr_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Recent apps", 11.0f),
            std::make_shared<Spacer>(8, 1),
            stagemgr_count_slider,
            std::make_shared<Spacer>(6, 1),
            stagemgr_count_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            stagemgr_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Mouse & Pointer ───────────────────────────────────────────────────
        std::make_shared<TextWidget>("\xf0\x9f\x96\xb1 Mouse & Pointer", 12.0f),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Speed", 11.0f),
            std::make_shared<Spacer>(8, 1),
            ptr_speed_slider,
            std::make_shared<Spacer>(6, 1),
            ptr_speed_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Natural scroll", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            ptr_natural_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Acceleration", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            ptr_accel_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Tap to click", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            ptr_tap_sw,
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            ptr_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Audio EQ ──────────────────────────────────────────────────────────
        std::make_shared<TextWidget>("\xf0\x9f\x8e\x9a Audio EQ", 12.0f),
        std::make_shared<Spacer>(1, 6),
        // Preset pills row
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*eq_preset_btns)[0], std::make_shared<Spacer>(4, 1),
            (*eq_preset_btns)[1], std::make_shared<Spacer>(4, 1),
            (*eq_preset_btns)[2], std::make_shared<Spacer>(4, 1),
            (*eq_preset_btns)[3], std::make_shared<Spacer>(4, 1),
            (*eq_preset_btns)[4],
        }),
        std::make_shared<Spacer>(1, 8),
        // Per-band rows
        [&]() -> std::shared_ptr<FlexColumn> {
            auto col = std::make_shared<FlexColumn>();
            col->setGap(4);
            for (int b = 0; b < 5; ++b) {
                col->addChild(std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    [&]() -> std::shared_ptr<TextWidget> {
                        auto lbl = std::make_shared<TextWidget>(kEqBandNames[b], 10.0f);
                        lbl->setColor({0.60f, 0.60f, 0.65f, 1.0f});
                        lbl->setFixedSize(46, 1);
                        return lbl;
                    }(),
                    std::make_shared<Spacer>(4, 1),
                    eq_sliders[b],
                    std::make_shared<Spacer>(6, 1),
                    eq_val_lbls[b],
                }));
            }
            return col;
        }(),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            eq_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Screen Lock ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x92 Screen Lock", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            slk_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Auto-lock after", 11.0f),
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*slk_timeout_btns)[0], std::make_shared<Spacer>(4, 1),
            (*slk_timeout_btns)[1], std::make_shared<Spacer>(4, 1),
            (*slk_timeout_btns)[2], std::make_shared<Spacer>(4, 1),
            (*slk_timeout_btns)[3],
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            (*slk_timeout_btns)[4], std::make_shared<Spacer>(4, 1),
            (*slk_timeout_btns)[5],
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Require password", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            slk_require_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Lock on sleep", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            slk_sleep_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            slk_lock_btn,
            std::make_shared<Spacer>(1, 1, true),
            slk_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Notif Schedule ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x94\x94 Notif Schedule", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            nsch_enabled_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("From", 11.0f),
            std::make_shared<Spacer>(6, 1),
            nsch_start_h_input,
            std::make_shared<TextWidget>(":", 11.0f),
            nsch_start_m_input,
            std::make_shared<Spacer>(12, 1),
            std::make_shared<TextWidget>("To", 11.0f),
            std::make_shared<Spacer>(6, 1),
            nsch_end_h_input,
            std::make_shared<TextWidget>(":", 11.0f),
            nsch_end_m_input,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Allow calls", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            nsch_calls_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Allow alarms", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            nsch_alarms_sw,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("Weekends only", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            nsch_weekend_sw,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            nsch_apply_btn,
            std::make_shared<Spacer>(1, 1, true),
            nsch_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Thermal Zones ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x8c\xa1 Thermal Zones", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            tz_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        tz_col,
        tz_status_lbl,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── DNS Config ────────────────────────────────────────────────────────
        std::make_shared<TextWidget>("\xf0\x9f\x94\x92 DNS Config", 12.0f),
        std::make_shared<Spacer>(1, 6),
        dns_primary_input,
        std::make_shared<Spacer>(1, 4),
        dns_secondary_input,
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("DNS-over-HTTPS", 11.0f),
            std::make_shared<Spacer>(1, 1, true),
            dns_doh_switch,
        }),
        dns_doh_row,
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            dns_apply_btn,
            std::make_shared<Spacer>(8, 1),
            dns_flush_btn,
            std::make_shared<Spacer>(1, 1, true),
            dns_status_lbl,
        }),
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Wake-on-LAN ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xf0\x9f\x93\xa1 Wake-on-LAN", 12.0f),
            std::make_shared<Spacer>(1, 1, true),
            wol_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        wol_col,
        std::make_shared<Spacer>(1, 6),
        wol_add_row,
        std::make_shared<Spacer>(1, 16),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // ── Admin Unlock (visible only when screen is locked) ─────────────────
        unlock_inputs_row,
        unlock_status_lbl,
        std::make_shared<Spacer>(1, 8),
        bat_saver_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        // Focus Timer
        pomo_hdr_row,
        std::make_shared<Spacer>(1, 8),
        pomo_cd_lbl,
        std::make_shared<Spacer>(1, 8),
        pomo_btn_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        ws_wp_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        ws_wp_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 10),
        kb_layout_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        wp_rotate_hdr,
        std::make_shared<Spacer>(1, 6),
        wp_folder_lbl,
        std::make_shared<Spacer>(1, 6),
        wp_interval_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        sess_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        sess_sw_row,
        std::make_shared<Spacer>(1, 6),
        sess_save_row,
        std::make_shared<Spacer>(1, 10),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 12),
        power_row,
        std::make_shared<Spacer>(1, 8),
        power_confirm_row,
    });
    qs_panel->setBackgroundColor({0.1f, 0.12f, 0.18f, 0.96f});
    qs_panel->setPadding(18);
    qs_panel->setFixedSize(340, 920);
    qs_panel->setStackAnchor(StackAnchor::TopRight);  // anchor near the tray, not centred
    qs_panel->setMarginAll(14.0f);                    // float clear of the screen edge
    qs_panel->setVisibility(false);

    // Network indicator button — shows signal bars + SSID; click toggles qs_panel
    auto net_btn = std::make_shared<ButtonWidget>("✕", [qs_panel]() {
        qs_panel->setVisibility(!qs_panel->isVisible());
    });
    net_btn->setPadding(6);
    net_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});

    // Bluetooth button — toggles qs_panel
    auto bt_btn = std::make_shared<ButtonWidget>("BT", [qs_panel]() {
        qs_panel->setVisibility(!qs_panel->isVisible());
    });
    bt_btn->setPadding(6);
    bt_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});

    // Volume button — toggles qs_panel
    auto vol_btn = std::make_shared<ButtonWidget>("🔊", [qs_panel]() {
        qs_panel->setVisibility(!qs_panel->isVisible());
    });
    vol_btn->setPadding(6);
    vol_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});

    // Workspace pips. Single workspace for now (matches the compositor's
    // WindowStack::kWorkspaceCount), so the switcher strip is built but left out
    // of the taskbar below — raise this and re-add ws_row to re-enable it.
    constexpr int kNumWorkspaces = 1;
    auto ws_pips = std::make_shared<std::array<std::shared_ptr<TextWidget>, kNumWorkspaces>>();
    std::vector<std::shared_ptr<Widget>> pip_widgets;
    pip_widgets.reserve(kNumWorkspaces * 2 - 1);
    for (int i = 0; i < kNumWorkspaces; ++i) {
        auto pip = std::make_shared<TextWidget>(std::to_string(i + 1), 12.0f);
        pip->setColor(i == 0
            ? Color{1.0f, 1.0f, 1.0f, 1.0f}
            : Color{0.35f, 0.35f, 0.4f, 1.0f});
        (*ws_pips)[i] = pip;
        pip_widgets.push_back(pip);
        if (i < kNumWorkspaces - 1)
            pip_widgets.push_back(std::make_shared<Spacer>(5, 1));
    }
    auto ws_row = std::make_shared<FlexRow>(std::move(pip_widgets));
    (void)ws_row;  // single workspace: built for easy re-enable, not shown in the taskbar

    // Taskbar window list — pre-allocated chips, one per open window (max 7)
    constexpr int kMaxTaskbarWindows = 7;
    struct TaskbarChip {
        uint32_t surface_id{0};
        std::shared_ptr<TextWidget>  label;
        std::shared_ptr<FlexColumn> card;
    };
    auto tbar_chips = std::make_shared<std::array<TaskbarChip, kMaxTaskbarWindows>>();
    auto tbar_ids   = std::make_shared<std::array<uint32_t,   kMaxTaskbarWindows>>();
    tbar_ids->fill(0);

    std::vector<std::shared_ptr<Widget>> tbar_widgets;
    tbar_widgets.reserve(kMaxTaskbarWindows * 2 - 1);
    for (int i = 0; i < kMaxTaskbarWindows; ++i) {
        auto lbl = std::make_shared<TextWidget>("", 12.0f);
        lbl->setColor({0.9f, 0.9f, 0.9f, 1.0f});
        lbl->setAlignment(Alignment::Center);
        auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{lbl});
        card->setBackgroundColor({0.15f, 0.15f, 0.22f, 0.9f});
        card->setPadding(8);
        card->setFixedSize(110, 28);
        card->setVisibility(false);
        (*tbar_chips)[i].label = lbl;
        (*tbar_chips)[i].card  = card;
        int idx = i;
        auto btn = std::make_shared<ButtonWidget>(card, [tbar_ids, idx]() {
            uint32_t sid = (*tbar_ids)[idx];
            if (sid == 0) return;
            if (auto* bus = XApplication::getBus()) {
                std::span<const uint8_t> payload(
                    reinterpret_cast<const uint8_t*>(&sid), sizeof(sid));
                bus->Call("com.freebsdx.Window",
                          static_cast<uint32_t>(WindowOpcode::Raise),
                          payload, nullptr);
            }
        });
        // Empty slots hide the (opaque) card; keep the button transparent so
        // they don't paint theme.surface (white) over the dark taskbar.
        btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
        tbar_widgets.push_back(btn);
        if (i < kMaxTaskbarWindows - 1)
            tbar_widgets.push_back(std::make_shared<Spacer>(4, 1));
    }
    auto tbar_row = std::make_shared<FlexRow>(std::move(tbar_widgets));

    // ── OSD overlay (volume / brightness) ── (creation carved into
    // buildOsdOverlayPanel; the slider re-wiring below stays in main() as it
    // touches other panels' sliders. Aliases keep refs unchanged.)
    buildOsdOverlayPanel(ctx);
    auto& osd_overlay = ctx.osd_overlay;
    auto& show_osd    = ctx.show_osd;

    // Re-wire brightness and volume sliders to also fire the OSD
    qs_bright_slider->setOnChanged([qs_bright_val, show_osd](float v) {
        qs_bright_val->setText(std::format("{}%", static_cast<int>(v)));
        if (!g_shell_xbus) return;
        BacklightState req{};
        req.level_ = static_cast<uint32_t>(v);
        g_shell_xbus->Call("com.freebsdx.Backlight",
            static_cast<uint32_t>(BacklightOpcode::SetBrightness),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        (*show_osd)("🔆", static_cast<int>(v));
    });
    qs_vol_slider->setOnChanged([qs_vol_val, show_osd](float v) {
        qs_vol_val->setText(std::format("{}%", static_cast<int>(v)));
        if (!g_shell_xbus) return;
        AudioVolumeRequest req{};
        req.volume_ = static_cast<uint32_t>(v);
        req.muted_  = 0xFF;
        g_shell_xbus->Call("com.freebsdx.Audio",
            static_cast<uint32_t>(AudioOpcode::SetVolume),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
        (*show_osd)("🔊", static_cast<int>(v));
    });

    // ── Updates button ── (carved into buildUpdatesButtonPanel; aliases keep
    // the card/broadcast/timer references unchanged. Takes `apps` by reference.)
    buildUpdatesButtonPanel(ctx, apps);
    auto& updates_btn    = ctx.updates_btn;
    auto& update_upd_btn = ctx.update_upd_btn;

    // ── Battery indicator ── (carved into buildBatteryIndicatorPanel; aliases
    // keep the card/broadcast/timer references unchanged)
    buildBatteryIndicatorPanel(ctx);
    auto& bat_btn        = ctx.bat_btn;
    auto& update_battery = ctx.update_battery;

    // ── Notification badge button ── (carved into buildNotifBadgePanel; the
    // click is wired later once action_center exists. Aliases keep refs stable.)
    buildNotifBadgePanel(ctx);
    auto& notif_unread    = ctx.notif_unread;
    auto& notif_badge_lbl = ctx.notif_badge_lbl;
    auto& notif_badge_box = ctx.notif_badge_box;
    auto& notif_tbar_btn  = ctx.notif_tbar_btn;

    // ── Pinned app launchers ──────────────────────────────────────────────────
    // PinnedEntry: {identifier, display_name, executable_path}
    struct PinnedEntry { std::string id, name, exec; };
    auto pinned_data = std::make_shared<std::vector<PinnedEntry>>();
    auto pinned_row  = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});

    // Rebuild the pinned row from pinned_data (called after any pin/unpin change)
    auto rebuild_pinned = std::make_shared<std::function<void()>>();
    *rebuild_pinned = [pinned_data, pinned_row]() {
        pinned_row->clearChildren();
        for (const auto& entry : *pinned_data) {
            auto lbl = std::make_shared<TextWidget>(entry.name, 11.0f);
            lbl->setColor({0.85f, 0.85f, 0.85f, 1.0f});
            lbl->setAlignment(Alignment::Center);
            auto card = std::make_shared<FlexColumn>(
                std::vector<std::shared_ptr<Widget>>{lbl});
            card->setBackgroundColor({0.12f, 0.14f, 0.20f, 0.90f});
            card->setPadding(8);
            card->setFixedSize(90, 28);
            std::string exe = entry.exec;
            pinned_row->addChild(
                std::make_shared<ButtonWidget>(card, [exe]() { launchApp(exe); }));
            pinned_row->addChild(std::make_shared<Spacer>(4, 1));
        }
    };

    // Persist pinned list to config: comma-separated identifiers in "shell.pinned"
    auto save_pinned = [pinned_data]() {
        if (!g_shell_xbus) return;
        std::string ids;
        for (const auto& e : *pinned_data) {
            if (!ids.empty()) ids += ',';
            ids += e.id;
        }
        ConfigValue kv{};
        std::strncpy(kv.key_,   "shell.pinned", sizeof(kv.key_) - 1);
        std::strncpy(kv.value_, ids.c_str(),    sizeof(kv.value_) - 1);
        g_shell_xbus->Call("com.freebsdx.Configuration", 0x02,
            {reinterpret_cast<const uint8_t*>(&kv), sizeof(kv)}, nullptr);
    };

    // ── System tray icon row ── (carved into buildSystemTrayPanel; aliases keep
    // the taskbar/broadcast/timer references unchanged)
    buildSystemTrayPanel(ctx);
    auto& tray_row  = ctx.tray_row;
    auto& load_tray = ctx.load_tray;

    // ── Weather taskbar widget ────────────────────────────────────────────────
    // Shows current conditions; updated on WeatherChanged broadcast.
    // Clicking launches Weather.xapp.
    // The ⌨ keyboard panel (vkb_anchor/vkb_panel) is built much further down —
    // it needs the key grid — but the taskbar toggle button must be a *real*
    // widget right here. A null slot in the row is dropped by the reconciler, so
    // a forward-declared-null button would silently never appear in the taskbar.
    // Forward-declare the panels and capture them by reference (same pattern as
    // start_menu above) so the toggle works once they're assigned below.
    std::shared_ptr<FlexColumn> vkb_anchor;
    std::shared_ptr<FlexColumn> vkb_panel;
    auto vkb_btn = std::make_shared<ButtonWidget>("\xe2\x8c\xa8", [&vkb_anchor, &vkb_panel]() {
        if (!vkb_anchor) return;
        bool show = !vkb_anchor->isVisible();
        vkb_anchor->setVisibility(show);
        if (vkb_panel) vkb_panel->setVisibility(show);
    });
    vkb_btn->setPadding(6);
    vkb_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    auto weather_btn = std::make_shared<ButtonWidget>("--°");
    weather_btn->setFontSize(12.0f);
    weather_btn->setPadding(6);
    weather_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    weather_btn->setColor(0.75f, 0.80f, 0.95f, 1.0f);

    auto taskbar = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        start_btn, std::make_shared<Spacer>(20, 1),
        pinned_row, std::make_shared<Spacer>(8, 1),
        tbar_row,  std::make_shared<Spacer>(1, 1, true),
        tray_row,  std::make_shared<Spacer>(8, 1),
        vol_btn, std::make_shared<Spacer>(8, 1),
        bt_btn,  std::make_shared<Spacer>(8, 1),
        vkb_btn, std::make_shared<Spacer>(8, 1),
        net_btn, std::make_shared<Spacer>(20, 1),
        pomo_tbar_w, std::make_shared<Spacer>(10, 1),
        updates_btn, std::make_shared<Spacer>(6, 1),
        bat_btn, std::make_shared<Spacer>(4, 1),
        notif_tbar_btn, std::make_shared<Spacer>(8, 1),
        clock_btn
    });
    // Modern floating bar: inset from the screen edges, rounded corners, and a
    // darker translucent "glass" fill instead of an edge-to-edge slab.
    taskbar->setPadding(8);
    taskbar->setBackgroundColor({0.09f, 0.10f, 0.13f, 0.88f});
    taskbar->setBorderRadius(20.0f);
    taskbar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    // Inset 14px left/right so the rounded bar floats free of the screen edges.
    taskbar->setMargin(0.0f, 14.0f, 0.0f, 14.0f);

    // ── Spotlight Search ──────────────────────────────────────────────────────
    // Input box
    auto search_input = std::make_shared<TextInputWidget>("🔍  Search apps, files, settings…");

    // Section headers
    auto make_sec_hdr = [](const std::string& title) {
        auto t = std::make_shared<TextWidget>(title, 10.0f);
        t->setColor({0.45f, 0.55f, 0.75f, 1.0f});
        auto row = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 8), t, std::make_shared<Spacer>(1, 4)
        });
        return row;
    };

    // ── Apps section ──────────────────────────────────────────────────────────
    auto sec_apps_hdr = make_sec_hdr("APPS");
    auto sec_apps     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    struct AppEntry { std::string name, exec, id; };
    std::vector<AppEntry> app_entries;
    for (const auto& app : apps)
        app_entries.push_back({app.name, app.executable_path, app.identifier});

    // ── Files section ─────────────────────────────────────────────────────────
    auto sec_files_hdr = make_sec_hdr("FILES");
    auto sec_files     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    // ── Settings section ──────────────────────────────────────────────────────
    auto sec_settings_hdr = make_sec_hdr("SETTINGS");
    auto sec_settings     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    // ── Math section ──────────────────────────────────────────────────────────
    auto sec_math_hdr = make_sec_hdr("CALCULATOR");
    auto math_result  = std::make_shared<TextWidget>("", 18.0f);
    math_result->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    auto math_result_value = std::make_shared<std::string>(); // just the numeric result
    auto math_copy_btn = std::make_shared<ButtonWidget>("📋 Copy", [math_result_value]() {
        if (math_result_value->empty() || !g_shell_xbus) return;
        struct { char text[252]; uint8_t pad[4]; } req{};
        std::strncpy(req.text, math_result_value->c_str(), sizeof(req.text) - 1);
        g_shell_xbus->Call("com.freebsdx.Clipboard", 0x01,
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
    });
    math_copy_btn->setFixedSize(90, 28);
    math_copy_btn->setFontSize(12.0f);
    auto math_open_btn = std::make_shared<ButtonWidget>("Open in Calculator", [apps]() {
        for (const auto& a : apps) {
            std::string nl = a.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (nl.find("calc") != std::string::npos) { launchApp(a.executable_path); break; }
        }
    });
    auto math_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        math_copy_btn, std::make_shared<Spacer>(8, 1), math_open_btn
    });
    auto sec_math_content = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        math_result, std::make_shared<Spacer>(1,4), math_btn_row
    });

    // ── Web section ───────────────────────────────────────────────────────────
    auto sec_web_hdr  = make_sec_hdr("WEB");
    auto web_btn_lbl  = std::make_shared<TextWidget>("Search DuckDuckGo", 14.0f);
    web_btn_lbl->setColor({0.6f, 0.8f, 1.0f, 1.0f});

    // Current web query for the web button — shared so click handler reads it at click time
    auto current_query = std::make_shared<std::string>();

    auto web_btn = std::make_shared<ButtonWidget>(web_btn_lbl, [current_query, apps]() {
        if (current_query->empty() || !g_shell_xbus) return;
        std::string url = "https://duckduckgo.com/?q=";
        for (char c : *current_query) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c=='-' || c=='_' || c=='.' || c=='~')
                url += c;
            else
                url += std::format("%{:02X}", static_cast<unsigned char>(c));
        }
        // Find Browser app and launch with URL
        for (const auto& a : apps) {
            std::string nl = a.name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (nl.find("browser") != std::string::npos) {
                launchApp(a.executable_path, url);
                break;
            }
        }
    });

    // ── Search result row factory (used by recent, apps, and file searches) ────
    auto make_result_row = [](const std::string& icon, const std::string& label,
                               const std::string& sub, std::function<void()> action)
        -> std::shared_ptr<Widget>
    {
        auto icon_w  = std::make_shared<TextWidget>(icon, 16.0f);
        auto label_w = std::make_shared<TextWidget>(label, 13.0f);
        label_w->setColor({0.92f, 0.92f, 0.92f, 1.0f});
        std::vector<std::shared_ptr<Widget>> row_ch = {icon_w, std::make_shared<Spacer>(10,1), label_w};
        if (!sub.empty()) {
            auto sub_w = std::make_shared<TextWidget>(sub, 10.0f);
            sub_w->setColor({0.5f, 0.5f, 0.5f, 1.0f});
            row_ch.push_back(std::make_shared<Spacer>(8,1));
            row_ch.push_back(sub_w);
        }
        auto row = std::make_shared<FlexRow>(row_ch);
        row->setPadding(6);
        auto col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            row, std::make_shared<Spacer>(1,2)
        });
        return std::make_shared<ButtonWidget>(col, std::move(action));
    };

    // ── Recent files section (shown when query is empty) ─────────────────────
    auto sec_recent_hdr = make_sec_hdr("RECENT");
    auto sec_recent     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    auto rebuild_recent = std::make_shared<std::function<void()>>();
    *rebuild_recent = [sec_recent, sec_recent_hdr, make_result_row]() {
        sec_recent->clearChildren();
        bool any = false;
        for (const auto& [name, path] : g_recent_files) {
            std::string p = path;
            sec_recent->addChild(make_result_row("📄", name, "", [p]() {
                if (!g_shell_xbus) return;
                MimeQueryRequest qr{};
                std::strncpy(qr.path_, p.c_str(), sizeof(qr.path_) - 1);
                qr.is_path_ = 1;
                g_shell_xbus->Call("com.freebsdx.MIME",
                    static_cast<uint32_t>(MimeOpcode::QueryHandler),
                    {reinterpret_cast<const uint8_t*>(&qr), sizeof(qr)},
                    [p](const XBusMessage& resp) {
                        MimeHandlerInfo info{};
                        std::memcpy(&info, resp.payload_, sizeof(info));
                        if (info.app_id_[0]) launchApp(info.app_id_, p);
                    });
            }));
            any = true;
        }
        sec_recent_hdr->setVisibility(any);
        sec_recent->setVisibility(any);
    };

    // ── Recent search queries section (shown when query is empty) ────────────
    auto sec_queries_hdr = make_sec_hdr("RECENT SEARCHES");
    auto sec_queries     = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    auto rebuild_queries = std::make_shared<std::function<void()>>();
    *rebuild_queries = [sec_queries, sec_queries_hdr, search_input]() {
        sec_queries->clearChildren();
        bool any = false;
        for (const auto& q : g_recent_queries) {
            std::string qcopy = q;
            auto lbl  = std::make_shared<TextWidget>("🔍  " + q, 13.0f);
            auto btn  = std::make_shared<ButtonWidget>(lbl,
                [qcopy, search_input]() {
                    search_input->setText(qcopy);
                });
            sec_queries->addChild(btn);
            any = true;
        }
        sec_queries_hdr->setVisibility(any);
        sec_queries->setVisibility(any);
    };

    // ── Combined results list ─────────────────────────────────────────────────
    auto results_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        sec_queries_hdr,  sec_queries,
        sec_recent_hdr,   sec_recent,
        sec_apps_hdr,     sec_apps,
        sec_files_hdr,    sec_files,
        sec_settings_hdr, sec_settings,
        sec_math_hdr,     sec_math_content,
        sec_web_hdr,      std::make_shared<FlexColumn>(
            std::vector<std::shared_ptr<Widget>>{web_btn})
    });

    // All sections hidden when query is empty (recent shown separately)
    sec_queries_hdr->setVisibility(false);
    sec_queries->setVisibility(false);
    sec_recent_hdr->setVisibility(false);
    sec_recent->setVisibility(false);
    sec_apps_hdr->setVisibility(false);
    sec_files_hdr->setVisibility(false);
    sec_settings_hdr->setVisibility(false);
    sec_math_hdr->setVisibility(false);
    sec_math_content->setVisibility(false);
    sec_web_hdr->setVisibility(false);

    // File search runs on a background thread; results posted back via shared state
    auto file_results_pending = std::make_shared<std::vector<std::string>>();
    auto file_results_ready   = std::make_shared<std::atomic<bool>>(false);
    auto file_search_query    = std::make_shared<std::string>();

    // The main search handler
    auto last_saved_query = std::make_shared<std::string>();

    search_input->setOnChanged([
        sec_apps, sec_apps_hdr, app_entries,
        sec_files, sec_files_hdr,
        sec_settings, sec_settings_hdr,
        sec_math_hdr, sec_math_content, math_result, math_result_value,
        sec_web_hdr, web_btn_lbl,
        file_results_pending, file_results_ready, file_search_query,
        current_query, make_result_row,
        apps,
        pinned_data, rebuild_pinned, save_pinned,
        sec_recent, sec_recent_hdr, rebuild_recent,
        sec_queries, sec_queries_hdr, rebuild_queries,
        last_saved_query
    ](const std::string& raw_query) mutable
    {
        const bool empty = raw_query.empty();

        // When going from non-empty → empty, save the last typed query
        if (empty && !last_saved_query->empty()) {
            pushRecentQuery(*last_saved_query);
            last_saved_query->clear();
        } else if (!empty) {
            *last_saved_query = raw_query;
        }

        *current_query = raw_query;

        // Show recent searches + recent files when query is empty
        if (empty) {
            (*rebuild_queries)();
            (*rebuild_recent)();
        } else {
            sec_queries_hdr->setVisibility(false);
            sec_queries->setVisibility(false);
            sec_recent_hdr->setVisibility(false);
            sec_recent->setVisibility(false);
        }

        // ── Apps ──────────────────────────────────────────────────────────────
        std::string ql = raw_query;
        std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        sec_apps->clearChildren();
        int app_hits = 0;
        for (const auto& ae : app_entries) {
            const auto& [name, exec, appid] = ae;
            std::string nl = name;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
            if (!empty && nl.find(ql) != std::string::npos) {
                std::string e = exec, id = appid, n = name;
                bool pinned = std::any_of(pinned_data->begin(), pinned_data->end(),
                    [&id](const auto& p){ return p.id == id; });
                auto pin_lbl = std::make_shared<TextWidget>(pinned ? "📌" : "○", 13.0f);
                auto pin_btn = std::make_shared<ButtonWidget>(pin_lbl,
                    [id, n, e, pinned_data, rebuild_pinned, save_pinned]() mutable {
                        auto it = std::find_if(pinned_data->begin(), pinned_data->end(),
                            [&id](const auto& p){ return p.id == id; });
                        if (it != pinned_data->end())
                            pinned_data->erase(it);
                        else
                            pinned_data->push_back({id, n, e});
                        save_pinned();
                        (*rebuild_pinned)();
                    });
                auto app_result = make_result_row("📦", name, "", [e](){ launchApp(e); });
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    app_result, std::make_shared<Spacer>(1, 1, true), pin_btn
                });
                sec_apps->addChild(row);
                if (++app_hits >= 5) break;
            }
        }
        sec_apps_hdr->setVisibility(!empty && app_hits > 0);

        // ── Settings ──────────────────────────────────────────────────────────
        sec_settings->clearChildren();
        int set_hits = 0;
        if (!empty) {
            for (int i = 0; kSettingsKeywords[i].keyword; ++i) {
                std::string kw = kSettingsKeywords[i].keyword;
                if (ql.find(kw) != std::string::npos || kw.find(ql) != std::string::npos) {
                    std::string lbl = kSettingsKeywords[i].label;
                    sec_settings->addChild(make_result_row("⚙", lbl, "Settings", [apps]() {
                        // Launch Settings app
                        for (const auto& a : apps) {
                            std::string nl = a.name;
                            std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                            if (nl.find("settings") != std::string::npos ||
                                nl.find("setting") != std::string::npos)
                            {
                                launchApp(a.executable_path);
                                break;
                            }
                        }
                    }));
                    if (++set_hits >= 3) break;
                }
            }
        }
        sec_settings_hdr->setVisibility(!empty && set_hits > 0);

        // ── Math ──────────────────────────────────────────────────────────────
        if (!empty) {
            auto mres = quickMathEval(raw_query);
            if (!mres.empty()) {
                // Strip leading "= " to get just the numeric value for copy
                *math_result_value = (mres.size() > 2 && mres[0] == '=')
                    ? mres.substr(2) : mres;
                math_result->setText(raw_query + "  " + mres);
                sec_math_hdr->setVisibility(true);
                sec_math_content->setVisibility(true);
            } else {
                sec_math_hdr->setVisibility(false);
                sec_math_content->setVisibility(false);
            }
        } else {
            sec_math_hdr->setVisibility(false);
            sec_math_content->setVisibility(false);
        }

        // ── Web ───────────────────────────────────────────────────────────────
        if (!empty) {
            web_btn_lbl->setText("🌐  Search DuckDuckGo for \u201c" + raw_query + "\u201d");
            sec_web_hdr->setVisibility(true);
        } else {
            sec_web_hdr->setVisibility(false);
        }

        // ── Files (background thread) ─────────────────────────────────────────
        sec_files->clearChildren();
        sec_files_hdr->setVisibility(false);
        if (!empty && raw_query.size() >= 2) {
            *file_search_query = raw_query;
            file_results_ready->store(false);
            std::string fq = raw_query;
            std::thread([fq, file_results_pending, file_results_ready]() {
                auto res = searchFiles(fq, 6);
                *file_results_pending = std::move(res);
                file_results_ready->store(true);
            }).detach();
        }
    });

    auto search_results_scroll = std::make_shared<ScrollViewWidget>(results_col);
    search_results_scroll->setFlexGrow(1.0f);

    auto search_portal = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        search_input,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.5f),
        std::make_shared<Spacer>(1, 8),
        search_results_scroll
    });
    search_portal->setBackgroundColor({0.08f, 0.09f, 0.14f, 0.96f});
    search_portal->setPadding(24);
    search_portal->setFixedSize(660, 520);
    search_portal->setVisibility(false);

    // Notification Center — persistent history with per-card dismiss and Clear All
    auto notif_history = std::make_shared<std::vector<NotifEntry>>();

    // Forward-declare scroll list and empty label for circular capture in lambdas
    auto notif_list_col  = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto notif_empty_lbl = std::make_shared<TextWidget>("No notifications", 14.0f);
    notif_empty_lbl->setColor({0.45f, 0.45f, 0.5f, 1.0f});
    notif_empty_lbl->setAlignment(Alignment::Center);

    auto make_notif_card = [notif_history, notif_list_col, notif_empty_lbl](
            uint32_t id, const std::string& title,
            const std::string& message, const std::string& ts,
            const std::string& app_name)
        -> std::shared_ptr<FlexColumn>
    {
        auto title_w = std::make_shared<TextWidget>(title, 13.0f);
        title_w->setColor({1.0f, 1.0f, 1.0f, 1.0f});

        auto msg_w = std::make_shared<TextWidget>(message, 11.0f);
        msg_w->setColor({0.75f, 0.75f, 0.75f, 1.0f});

        auto ts_w = std::make_shared<TextWidget>(ts, 10.0f);
        ts_w->setColor({0.45f, 0.45f, 0.5f, 1.0f});
        ts_w->setAlignment(Alignment::End);

        // Helper: dismiss this card from history + notify service
        auto do_dismiss = [id, notif_history, notif_list_col, notif_empty_lbl]() {
            for (auto& e : *notif_history) {
                if (e.id == id) { e.dismissed = true; e.card->setVisibility(false); break; }
            }
            bool any = false;
            for (const auto& e : *notif_history) if (!e.dismissed) { any = true; break; }
            notif_empty_lbl->setVisibility(!any);
            if (g_shell_xbus) {
                uint32_t did = id;
                g_shell_xbus->Call("com.freebsdx.Notification",
                    static_cast<uint32_t>(NotificationOpcode::Dismiss),
                    {reinterpret_cast<const uint8_t*>(&did), sizeof(did)}, nullptr);
            }
        };

        auto dismiss_btn = std::make_shared<ButtonWidget>("✕", do_dismiss);
        dismiss_btn->setPadding(4);

        // Snooze buttons — 5 min, 15 min, 30 min
        auto make_snooze_btn = [=](int minutes) {
            std::string lbl = std::to_string(minutes) + "m";
            auto btn = std::make_shared<ButtonWidget>(lbl,
                [=]() mutable {
                    SnoozedNotif sn;
                    sn.id       = id;
                    sn.title    = title;
                    sn.message  = message;
                    sn.app_name = app_name;
                    sn.wake_at  = std::time(nullptr) + minutes * 60;
                    {
                        std::lock_guard<std::mutex> lk(g_snooze_mtx);
                        g_snoozed.push_back(std::move(sn));
                    }
                    do_dismiss();
                });
            btn->setFontSize(10.0f);
            btn->setFixedSize(34, 22);
            btn->setBackgroundColor({0.18f, 0.22f, 0.32f, 1.0f});
            return btn;
        };

        auto snooze_lbl = std::make_shared<TextWidget>("💤", 10.0f);
        snooze_lbl->setColor({0.55f, 0.65f, 0.80f, 1.0f});
        auto snooze_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            snooze_lbl,
            std::make_shared<Spacer>(4, 1),
            make_snooze_btn(5),
            std::make_shared<Spacer>(3, 1),
            make_snooze_btn(15),
            std::make_shared<Spacer>(3, 1),
            make_snooze_btn(30)
        });

        auto header_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            title_w,
            std::make_shared<Spacer>(1, 1, true),
            ts_w,
            std::make_shared<Spacer>(6, 1),
            dismiss_btn
        });

        auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            header_row,
            std::make_shared<Spacer>(1, 4),
            msg_w,
            std::make_shared<Spacer>(1, 6),
            snooze_row,
            std::make_shared<Spacer>(1, 8),
            std::make_shared<DividerWidget>(0.3f),
        });
        card->setPadding(12);
        card->setBackgroundColor({0.15f, 0.16f, 0.22f, 0.97f});
        return card;
    };

    // "Clear All" button
    auto notif_clear_btn = std::make_shared<ButtonWidget>("Clear All",
        [notif_history, notif_list_col, notif_empty_lbl]() {
            for (auto& e : *notif_history) e.dismissed = true;
            notif_history->clear();
            notif_list_col->clearChildren();
            notif_empty_lbl->setVisibility(true);
        });

    auto notif_header_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<TextWidget>("Notifications", 18.0f),
        std::make_shared<Spacer>(1, 1, true),
        notif_clear_btn
    });

    auto notif_scroll = std::make_shared<ScrollViewWidget>(notif_list_col);
    notif_scroll->setFlexGrow(1.0f);

    // ── App Notifications section ─────────────────────────────────────────────
    // known_apps: app names seen in this session (populated from notification broadcasts)
    // muted_apps: apps muted server-side (populated from GetMutedApps on connect)
    auto known_apps = std::make_shared<std::vector<std::string>>();
    auto muted_apps = std::make_shared<std::set<std::string>>();

    auto app_notif_col = std::make_shared<FlexColumn>();
    app_notif_col->setGap(2);

    // rebuild_app_list: re-renders the per-app toggle rows
    auto rebuild_app_list = std::make_shared<std::function<void()>>();
    *rebuild_app_list = [known_apps, muted_apps, app_notif_col, rebuild_app_list]() {
        app_notif_col->clearChildren();
        if (known_apps->empty()) {
            auto lbl = std::make_shared<TextWidget>("No apps yet", 12.0f);
            lbl->setColor(0.4f, 0.4f, 0.45f, 1.0f);
            lbl->setPadding(8);
            app_notif_col->addChild(lbl);
            return;
        }
        for (const auto& app : *known_apps) {
            bool is_muted = muted_apps->count(app) > 0;
            auto name_lbl = std::make_shared<TextWidget>(app, 13.0f);
            name_lbl->setColor(0.88f, 0.88f, 0.88f, 1.0f);

            auto sw = std::make_shared<SwitchWidget>(!is_muted);
            std::string app_copy = app;
            sw->setOnChanged([app_copy, muted_apps, rebuild_app_list](bool on) {
                if (!g_shell_xbus) return;
                NotifAppMuteRequest req{};
                std::strncpy(req.app_name_, app_copy.c_str(),
                             sizeof(req.app_name_) - 1);
                auto op = on ? NotificationOpcode::UnmuteApp
                             : NotificationOpcode::MuteApp;
                g_shell_xbus->Call("com.freebsdx.Notification",
                    static_cast<uint32_t>(op),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                    nullptr);
                // Optimistically update local set
                if (on) muted_apps->erase(app_copy);
                else     muted_apps->insert(app_copy);
                if (*rebuild_app_list) (*rebuild_app_list)();
            });

            auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                name_lbl,
                std::make_shared<Spacer>(1, 1, true),
                sw
            });
            row->setPadding(8);
            app_notif_col->addChild(row);
        }
    };

    auto app_notif_hdr = std::make_shared<TextWidget>("App Notifications", 14.0f);
    app_notif_hdr->setColor(0.6f, 0.6f, 0.65f, 1.0f);

    // ── Quick Notes mini-grid + Clipboard history mini-panel (action center) ──
    // (carved into buildNotesMiniPanel / buildClipboardMiniPanel; *rebuild_clip_ac
    // is wired later in OnConnect. Aliases keep the card/broadcast/timer refs
    // unchanged.)
    buildNotesMiniPanel(ctx);
    buildClipboardMiniPanel(ctx);
    auto& notes_mini_col     = ctx.notes_mini_col;
    auto& notes_mini_hdr_row = ctx.notes_mini_hdr_row;
    auto& rebuild_notes_mini = ctx.rebuild_notes_mini;
    auto& clip_ac_hdr_row    = ctx.clip_ac_hdr_row;
    auto& clip_ac_col        = ctx.clip_ac_col;
    auto& rebuild_clip_ac    = ctx.rebuild_clip_ac;

    // ── Idle auto-lock cycle selector + Contact quick-search (action center) ──
    // (carved into buildIdleLockPanel / buildContactSearchPanel; do_contact_search
    // is wired later in OnConnect. Aliases keep the card/broadcast refs unchanged.)
    buildIdleLockPanel(ctx);
    buildContactSearchPanel(ctx);
    auto& idle_lock_btn        = ctx.idle_lock_btn;
    auto& contact_hdr_lbl      = ctx.contact_hdr_lbl;
    auto& contact_search_input = ctx.contact_search_input;
    auto& contact_results_col  = ctx.contact_results_col;
    auto& do_contact_search    = ctx.do_contact_search;

    // ── App Store ─────────────────────────────────────────────────────────────
    auto store_hdr_lbl    = std::make_shared<TextWidget>("\xf0\x9f\x9b\x8d App Store", 14.0f);
    store_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto store_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    store_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto store_col        = std::make_shared<FlexColumn>();
    store_col->setGap(5);

    auto store_search_input = std::make_shared<TextInputWidget>();
    store_search_input->setPlaceholder("Search apps\xe2\x80\xa6");
    store_search_input->setFlexGrow(1.0f);

    auto rebuild_store = std::make_shared<std::function<void(std::string)>>();
    *rebuild_store = [store_col, store_status_lbl,
                      rs_weak = std::weak_ptr<std::function<void(std::string)>>(rebuild_store)](
                         std::string query) {
        if (!g_shell_xbus) return;
        store_col->clearChildren();
        store_status_lbl->setText("Searching\xe2\x80\xa6");
        store_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

        struct StoreFetcher {
            std::shared_ptr<FlexColumn>         col;
            std::shared_ptr<TextWidget>         status;
            std::function<void(uint32_t)>       fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<StoreFetcher>();
        pf->col    = store_col;
        pf->status = store_status_lbl;

        bool is_search = !query.empty();
        pf->fetch = [pf_weak = std::weak_ptr<StoreFetcher>(pf),
                     is_search, query](uint32_t page) {
            if (!g_shell_xbus) return;
            if (is_search) {
                AppManagerSearchRequest req{};
                std::strncpy(req.query_, query.c_str(), sizeof(req.query_) - 1);
                req.page_ = page;
                g_shell_xbus->Call("com.freebsdx.AppManager",
                    static_cast<uint32_t>(AppManagerCatalogOpcode::SearchRemote),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                    [pf_weak, page](const XBusMessage& resp) {
                        auto pf = pf_weak.lock();
                        if (!pf) return;
                        AppManagerRemotePage pg{};
                        std::memcpy(&pg, resp.payload_, sizeof(pg));
                        if (page == 0) pf->total = pg.total_;
                        if (page == 0 && (pg.total_ == 0 || !pg.valid_)) {
                            pf->status->setText("No results");
                            return;
                        }
                        if (page == 0)
                            pf->status->setText(std::format("{} result(s)", pf->total));
                        if (pg.valid_) {
                            auto name_lbl = std::make_shared<TextWidget>(
                                std::string(pg.name_), 12.0f);
                            name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                            name_lbl->setFlexGrow(1.0f);

                            auto cat_lbl = std::make_shared<TextWidget>(
                                std::string(pg.category_), 10.0f);
                            cat_lbl->setColor({0.40f, 0.65f, 0.80f, 1.0f});

                            auto ver_lbl = std::make_shared<TextWidget>(
                                std::string(pg.version_), 10.0f);
                            ver_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

                            std::string app_id = std::string(pg.identifier_);
                            bool already = (pg.installed_ != 0);
                            auto inst_btn = std::make_shared<ButtonWidget>(
                                already ? "Installed" : "Install",
                                [app_id, already]() {
                                    if (already || !g_shell_xbus) return;
                                    AppManagerRemoteInstallRequest req{};
                                    std::strncpy(req.identifier_, app_id.c_str(),
                                                 sizeof(req.identifier_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.AppManager",
                                        static_cast<uint32_t>(
                                            AppManagerCatalogOpcode::InstallRemote),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        nullptr);
                                });
                            inst_btn->setFixedSize(70, 22);
                            inst_btn->setBackgroundColor(already
                                ? Color{0.28f, 0.28f, 0.32f, 1.0f}
                                : Color{0.20f, 0.55f, 0.90f, 1.0f});

                            auto row = std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{
                                    name_lbl, cat_lbl,
                                    std::make_shared<Spacer>(4, 1),
                                    ver_lbl,
                                    std::make_shared<Spacer>(6, 1), inst_btn});
                            row->setAlignment(Alignment::Center);
                            pf->col->addChild(row);
                        }
                        if (pg.has_more_) {
                            if (auto p = pf_weak.lock()) p->fetch(page + 1);
                        }
                    });
            } else {
                AppManagerPageRequest req{};
                req.page_ = page;
                g_shell_xbus->Call("com.freebsdx.AppManager",
                    static_cast<uint32_t>(AppManagerCatalogOpcode::ListRemote),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                    [pf_weak, page](const XBusMessage& resp) {
                        auto pf = pf_weak.lock();
                        if (!pf) return;
                        AppManagerRemotePage pg{};
                        std::memcpy(&pg, resp.payload_, sizeof(pg));
                        if (page == 0) pf->total = pg.total_;
                        if (page == 0 && (pg.total_ == 0 || !pg.valid_)) {
                            pf->status->setText("Catalog empty");
                            return;
                        }
                        if (page == 0)
                            pf->status->setText(std::format("{} app(s) available", pf->total));
                        if (pg.valid_) {
                            auto name_lbl = std::make_shared<TextWidget>(
                                std::string(pg.name_), 12.0f);
                            name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                            name_lbl->setFlexGrow(1.0f);

                            auto cat_lbl = std::make_shared<TextWidget>(
                                std::string(pg.category_), 10.0f);
                            cat_lbl->setColor({0.40f, 0.65f, 0.80f, 1.0f});

                            auto ver_lbl = std::make_shared<TextWidget>(
                                std::string(pg.version_), 10.0f);
                            ver_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

                            std::string app_id = std::string(pg.identifier_);
                            bool already = (pg.installed_ != 0);
                            auto inst_btn = std::make_shared<ButtonWidget>(
                                already ? "Installed" : "Install",
                                [app_id, already]() {
                                    if (already || !g_shell_xbus) return;
                                    AppManagerRemoteInstallRequest req{};
                                    std::strncpy(req.identifier_, app_id.c_str(),
                                                 sizeof(req.identifier_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.AppManager",
                                        static_cast<uint32_t>(
                                            AppManagerCatalogOpcode::InstallRemote),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        nullptr);
                                });
                            inst_btn->setFixedSize(70, 22);
                            inst_btn->setBackgroundColor(already
                                ? Color{0.28f, 0.28f, 0.32f, 1.0f}
                                : Color{0.20f, 0.55f, 0.90f, 1.0f});

                            auto row = std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{
                                    name_lbl, cat_lbl,
                                    std::make_shared<Spacer>(4, 1),
                                    ver_lbl,
                                    std::make_shared<Spacer>(6, 1), inst_btn});
                            row->setAlignment(Alignment::Center);
                            pf->col->addChild(row);
                        }
                        if (pg.has_more_) {
                            if (auto p = pf_weak.lock()) p->fetch(page + 1);
                        }
                    });
            }
        };
        pf->fetch(0);
    };

    auto store_search_btn = std::make_shared<ButtonWidget>("Search",
        [store_search_input, rebuild_store]() {
            if (rebuild_store && *rebuild_store)
                (*rebuild_store)(store_search_input->getText());
        });
    store_search_btn->setFixedSize(64, 28);

    auto store_update_btn = std::make_shared<ButtonWidget>("\xe2\x86\xbb Update",
        [store_status_lbl]() {
            if (!g_shell_xbus) return;
            store_status_lbl->setText("Updating catalog\xe2\x80\xa6");
            store_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
            g_shell_xbus->Call("com.freebsdx.AppManager",
                static_cast<uint32_t>(AppManagerCatalogOpcode::UpdateCatalog),
                {}, nullptr);
        });
    store_update_btn->setFixedSize(76, 26);

    auto store_search_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        store_search_input, std::make_shared<Spacer>(6, 1), store_search_btn});
    store_search_row->setAlignment(Alignment::Center);

    // ── Installed Apps ────────────────────────────────────────────────────────
    auto iapps_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x93\xb1 Installed Apps", 14.0f);
    iapps_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto iapps_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    iapps_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto iapps_prog_lbl  = std::make_shared<TextWidget>("", 11.0f);
    iapps_prog_lbl->setColor({0.35f, 0.70f, 0.95f, 1.0f});
    iapps_prog_lbl->setVisibility(false);
    auto iapps_col = std::make_shared<FlexColumn>();
    iapps_col->setGap(5);

    auto rebuild_iapps = std::make_shared<std::function<void()>>();
    *rebuild_iapps = [iapps_col, iapps_count_lbl, iapps_prog_lbl,
                      ri_weak = std::weak_ptr<std::function<void()>>(rebuild_iapps)]() {
        if (!g_shell_xbus) return;
        iapps_col->clearChildren();
        iapps_prog_lbl->setVisibility(false);

        struct IAppsFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<IAppsFetcher>();
        pf->col   = iapps_col;
        pf->count = iapps_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<IAppsFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.AppManager",
                static_cast<uint32_t>(AppManagerOpcode::ListApps),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    AppList pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("No apps installed");
                        return;
                    }
                    if (page == 0)
                        pf->count->setText(std::format("{} app(s)", pf->total));
                    if (pg.count_ > 0) {
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(pg.app_.name_), 12.0f);
                        name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto ver_lbl = std::make_shared<TextWidget>(
                            std::string(pg.app_.version_), 10.0f);
                        ver_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        std::string app_id = std::string(pg.app_.identifier_);
                        auto uninst_btn = std::make_shared<ButtonWidget>("Remove",
                            [app_id]() {
                                if (!g_shell_xbus) return;
                                AppIdentifierRequest req{};
                                std::strncpy(req.identifier_, app_id.c_str(),
                                             sizeof(req.identifier_) - 1);
                                g_shell_xbus->Call("com.freebsdx.AppManager",
                                    static_cast<uint32_t>(AppManagerOpcode::Uninstall),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        uninst_btn->setFixedSize(60, 22);
                        uninst_btn->setBackgroundColor({0.45f, 0.15f, 0.15f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, ver_lbl,
                                std::make_shared<Spacer>(6, 1), uninst_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    // ── Font Manager ──────────────────────────────────────────────────────────
    auto fonts_hdr_lbl   = std::make_shared<TextWidget>("\xf0\x9f\x94\xa4 Fonts", 14.0f);
    fonts_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto fonts_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    fonts_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto fonts_col = std::make_shared<FlexColumn>();
    fonts_col->setGap(4);

    auto rebuild_fonts = std::make_shared<std::function<void()>>();
    *rebuild_fonts = [fonts_col, fonts_count_lbl,
                      rf_weak = std::weak_ptr<std::function<void()>>(rebuild_fonts)]() {
        if (!g_shell_xbus) return;
        fonts_col->clearChildren();

        struct FontsFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::shared_ptr<TextWidget>   count;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<FontsFetcher>();
        pf->col   = fonts_col;
        pf->count = fonts_count_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<FontsFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            FontPageRequest req{};
            req.page_ = page;
            g_shell_xbus->Call("com.freebsdx.FontManager",
                static_cast<uint32_t>(FontManagerOpcode::ListFonts),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    FontListPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->count->setText("No fonts installed");
                        return;
                    }
                    if (page == 0)
                        pf->count->setText(std::format("{} font(s)", pf->total));
                    if (pg.count_ > 0) {
                        const auto& f = pg.font_;

                        auto family_lbl = std::make_shared<TextWidget>(
                            std::string(f.family_), 11.0f);
                        family_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                        family_lbl->setFlexGrow(1.0f);

                        auto style_lbl = std::make_shared<TextWidget>(
                            std::string(f.style_), 10.0f);
                        style_lbl->setColor({0.50f, 0.50f, 0.55f, 1.0f});

                        auto fmt_lbl = std::make_shared<TextWidget>(
                            std::string(f.format_), 10.0f);
                        fmt_lbl->setColor({0.40f, 0.65f, 0.40f, 1.0f});

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                family_lbl, style_lbl,
                                std::make_shared<Spacer>(6, 1), fmt_lbl});
                        row->setAlignment(Alignment::Center);

                        if (f.is_user_installed_) {
                            std::string font_path = std::string(f.path_);
                            auto rm_btn = std::make_shared<ButtonWidget>("Remove",
                                [font_path]() {
                                    if (!g_shell_xbus) return;
                                    FontRemoveRequest req{};
                                    std::strncpy(req.path_, font_path.c_str(),
                                                 sizeof(req.path_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.FontManager",
                                        static_cast<uint32_t>(FontManagerOpcode::RemoveFont),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        nullptr);
                                });
                            rm_btn->setFixedSize(60, 22);
                            rm_btn->setBackgroundColor({0.45f, 0.15f, 0.15f, 1.0f});
                            row->addChild(std::make_shared<Spacer>(6, 1));
                            row->addChild(rm_btn);
                        }

                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    auto fonts_refresh_btn = std::make_shared<ButtonWidget>("\xe2\x86\xba Refresh",
        [rebuild_fonts]() {
            if (rebuild_fonts && *rebuild_fonts) (*rebuild_fonts)();
        });
    fonts_refresh_btn->setFixedSize(84, 26);

    // ── Startup apps section in action center ── (carved into
    // buildStartupAppsPanel; *rebuild_startup_apps is wired later in OnConnect.
    // Aliases keep the card/broadcast refs unchanged.)
    buildStartupAppsPanel(ctx);
    auto& startup_apps_hdr_lbl = ctx.startup_apps_hdr_lbl;
    auto& startup_apps_col     = ctx.startup_apps_col;
    auto& rebuild_startup_apps = ctx.rebuild_startup_apps;

    // ── Window rules section ──────────────────────────────────────────────────
    auto win_rules_hdr_lbl = std::make_shared<TextWidget>("📐 Window Rules", 14.0f);
    win_rules_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto win_rules_col = std::make_shared<FlexColumn>();
    win_rules_col->setGap(4);
    auto rebuild_win_rules = std::make_shared<std::function<void()>>();

    // New-rule input field + Add button
    auto win_rule_input = std::make_shared<TextInputWidget>();
    win_rule_input->setPlaceholder("Window title keyword…");
    auto win_rule_add_btn = std::make_shared<ButtonWidget>("+ Add",
        [win_rule_input, rebuild_win_rules]() {
            std::string kw = win_rule_input->getText();
            if (kw.empty()) return;
            WinRule r; r.title_match = kw;
            g_win_rules.push_back(r);
            saveWinRules();
            win_rule_input->setText("");
            if (*rebuild_win_rules) (*rebuild_win_rules)();
        });
    win_rule_add_btn->setFontSize(12.0f);
    win_rule_add_btn->setFixedSize(60, 28);

    auto win_rules_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        win_rules_hdr_lbl,
        std::make_shared<Spacer>(1, 1, true),
    });

    *rebuild_win_rules = [win_rules_col]() {
        win_rules_col->clearChildren();
        for (int ri = 0; ri < static_cast<int>(g_win_rules.size()); ++ri) {
            int idx = ri;
            auto& rule = g_win_rules[idx];

            // Title keyword label
            auto kw_lbl = std::make_shared<TextWidget>(rule.title_match, 11.0f);
            kw_lbl->setColor({0.88f, 0.88f, 0.88f, 1.0f});

            // Max toggle
            auto max_sw = std::make_shared<SwitchWidget>(rule.maximize);
            max_sw->setOnChanged([idx](bool v) {
                if (idx < static_cast<int>(g_win_rules.size()))
                    g_win_rules[idx].maximize = v;
                saveWinRules();
            });
            auto max_lbl = std::make_shared<TextWidget>("Max", 10.0f);
            max_lbl->setColor({0.55f, 0.65f, 0.75f, 1.0f});

            // Always-on-top toggle
            auto top_sw = std::make_shared<SwitchWidget>(rule.always_on_top);
            top_sw->setOnChanged([idx](bool v) {
                if (idx < static_cast<int>(g_win_rules.size()))
                    g_win_rules[idx].always_on_top = v;
                saveWinRules();
            });
            auto top_lbl = std::make_shared<TextWidget>("Top", 10.0f);
            top_lbl->setColor({0.55f, 0.65f, 0.75f, 1.0f});

            // Delete button
            auto del_btn = std::make_shared<ButtonWidget>("✕",
                [idx, win_rules_col]() {
                    if (idx < static_cast<int>(g_win_rules.size()))
                        g_win_rules.erase(g_win_rules.begin() + idx);
                    saveWinRules();
                    // Full rebuild triggered externally via the shared_ptr
                    // Schedule via flag — we're inside the lambda, so just clear children and re-fill
                    win_rules_col->clearChildren();
                    for (int ri2 = 0; ri2 < static_cast<int>(g_win_rules.size()); ++ri2) {
                        auto lbl2 = std::make_shared<TextWidget>(g_win_rules[ri2].title_match, 11.0f);
                        lbl2->setColor({0.88f, 0.88f, 0.88f, 1.0f});
                        win_rules_col->addChild(lbl2);
                    }
                });
            del_btn->setFontSize(10.0f);
            del_btn->setPadding(3);

            auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                kw_lbl,
                std::make_shared<Spacer>(1, 1, true),
                max_lbl, std::make_shared<Spacer>(3, 1), max_sw,
                std::make_shared<Spacer>(6, 1),
                top_lbl, std::make_shared<Spacer>(3, 1), top_sw,
                std::make_shared<Spacer>(6, 1),
                del_btn
            });
            row->setPadding(4);
            win_rules_col->addChild(row);
        }
        if (g_win_rules.empty()) {
            auto lbl = std::make_shared<TextWidget>("No rules — add one below", 11.0f);
            lbl->setColor({0.4f, 0.4f, 0.45f, 1.0f});
            lbl->setPadding(4);
            win_rules_col->addChild(lbl);
        }
    };
    auto win_rule_add_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        win_rule_input, std::make_shared<Spacer>(6, 1), win_rule_add_btn
    });

    // ── Hotkey remapper section ───────────────────────────────────────────────
    struct HotkeyDef {
        const char*  name;
        uint32_t     action_id;  // InputEventType value
        uint32_t     def_kc;     // default keycode (K_* constant)
        uint32_t     def_mods;   // default modifiers (Super=8)
    };
    static const HotkeyDef kHotkeyDefs[] = {
        {"Search",        6,  57, 8},  // Super+Space
        {"Dashboard",     7,  30, 8},  // Super+A
        {"Quick Note",   45,  49, 8},  // Super+N
        {"Clipboard",    37,  47, 8},  // Super+V
        {"Color Picker", 38,  25, 8},  // Super+P
        {"Help",         44,  53, 8},  // Super+/
    };
    static constexpr int kNumHotkeys = 6;

    auto hotkey_hdr_lbl = std::make_shared<TextWidget>("🎹 Hotkey Remapper", 14.0f);
    hotkey_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto hotkey_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    hotkey_status_lbl->setColor({0.55f, 0.75f, 1.0f, 1.0f});

    // One binding label per action; updated by rebuild_hotkeys
    auto hotkey_binding_lbls = std::make_shared<std::array<std::shared_ptr<TextWidget>, kNumHotkeys>>();
    auto hotkey_rows_col = std::make_shared<FlexColumn>();
    hotkey_rows_col->setGap(4);

    auto rebuild_hotkeys = std::make_shared<std::function<void()>>();
    *rebuild_hotkeys = [hotkey_rows_col, hotkey_binding_lbls,
                        hotkey_status_lbl]() {
        hotkey_rows_col->clearChildren();
        for (int i = 0; i < kNumHotkeys; ++i) {
            const auto& def = kHotkeyDefs[i];
            uint32_t kc   = def.def_kc,  mods = def.def_mods;
            auto it = g_hotkey_overrides.find(def.action_id);
            if (it != g_hotkey_overrides.end()) { kc = it->second.first; mods = it->second.second; }

            auto name_lbl = std::make_shared<TextWidget>(std::string(def.name), 12.0f);
            name_lbl->setColor({0.88f, 0.88f, 0.88f, 1.0f});

            auto bind_lbl = std::make_shared<TextWidget>(fmtHotkey(kc, mods), 11.0f);
            bind_lbl->setColor({0.55f, 0.75f, 1.0f, 1.0f});
            (*hotkey_binding_lbls)[i] = bind_lbl;

            int ai = i;
            auto remap_btn = std::make_shared<ButtonWidget>("Remap",
                [ai, hotkey_status_lbl]() {
                    g_hotkey_capture_action.store(ai);
                    hotkey_status_lbl->setText("Press new key combo…");
                });
            remap_btn->setFontSize(10.0f);
            remap_btn->setFixedSize(54, 22);

            auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                name_lbl,
                std::make_shared<Spacer>(1, 1, true),
                bind_lbl,
                std::make_shared<Spacer>(6, 1),
                remap_btn
            });
            row->setPadding(3);
            hotkey_rows_col->addChild(row);
        }
    };

    // ── Scratch pad section ── (carved into buildScratchPadPanel; aliases keep
    // the card/broadcast/OnConnect refs unchanged. hdr_lbl + clear_btn stay
    // local to the builder.)
    buildScratchPadPanel(ctx);
    auto& scratch_input   = ctx.scratch_input;
    auto& scratch_hdr_row = ctx.scratch_hdr_row;

    // ── System resource sparklines section ── (carved into buildSparklinesPanel;
    // aliases keep the card/broadcast/timer refs unchanged.)
    buildSparklinesPanel(ctx);
    auto& spark_hdr_lbl = ctx.spark_hdr_lbl;
    auto& spark_cpu_lbl = ctx.spark_cpu_lbl;
    auto& spark_ram_lbl = ctx.spark_ram_lbl;

    // ── Downloads manager ─────────────────────────────────────────────────────
    auto dl_hdr_lbl = std::make_shared<TextWidget>("⬇ Downloads", 14.0f);
    dl_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto dl_col = std::make_shared<FlexColumn>();
    dl_col->setGap(6);

    auto rebuild_dl = std::make_shared<std::function<void()>>();
    *rebuild_dl = [dl_col,
                   rdl_weak = std::weak_ptr<std::function<void()>>(rebuild_dl)]() {
        if (!g_shell_xbus) return;
        dl_col->clearChildren();

        struct DlFetcher {
            std::shared_ptr<FlexColumn> col;
            std::function<void(uint32_t)> fetch;
        };
        auto df = std::make_shared<DlFetcher>();
        df->col = dl_col;
        df->fetch = [df_weak = std::weak_ptr<DlFetcher>(df)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Downloads",
                static_cast<uint32_t>(DownloadOpcode::ListDownloads),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [df_weak, page](const XBusMessage& resp) {
                    auto df = df_weak.lock();
                    if (!df) return;
                    DownloadPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No downloads", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        df->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        // Filename row
                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.filename_), 12.0f);
                        name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});

                        // Progress / status row
                        std::shared_ptr<Widget> status_w;
                        if (e.state_ == 0) {
                            // Active: mini ASCII progress bar + speed
                            int pct = (e.total_bytes_ > 0)
                                ? static_cast<int>(e.received_bytes_ * 20 / e.total_bytes_)
                                : 0;
                            std::string bar;
                            for (int b = 0; b < 20; ++b) bar += (b < pct) ? "▓" : "░";
                            float speed = e.speed_kbps_ / 1024.0f;
                            auto info = std::format("{}  {:.1f} MB/s", bar, speed);
                            auto prog_lbl = std::make_shared<TextWidget>(info, 10.0f);
                            prog_lbl->setFontFamily("monospace");
                            prog_lbl->setColor({0.40f, 0.75f, 0.95f, 1.0f});
                            status_w = prog_lbl;
                        } else if (e.state_ == 1) {
                            // Done: size + Open button
                            float mb = e.total_bytes_ / (1024.0f * 1024.0f);
                            auto done_lbl = std::make_shared<TextWidget>(
                                std::format("Done  {:.1f} MB", mb), 10.0f);
                            done_lbl->setColor({0.40f, 0.75f, 0.45f, 1.0f});
                            uint32_t eid = e.id_;
                            auto open_btn = std::make_shared<ButtonWidget>("Open",
                                [eid]() {
                                    if (!g_shell_xbus) return;
                                    DownloadFileRequest req{};
                                    req.id_ = eid;
                                    g_shell_xbus->Call("com.freebsdx.Downloads",
                                        static_cast<uint32_t>(DownloadOpcode::OpenFile),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        nullptr);
                                });
                            open_btn->setFixedSize(48, 22);
                            auto done_row = std::make_shared<FlexRow>(
                                std::vector<std::shared_ptr<Widget>>{done_lbl,
                                    std::make_shared<Spacer>(1, 1, true), open_btn});
                            done_row->setAlignment(Alignment::Center);
                            status_w = done_row;
                        } else {
                            // Failed
                            auto fail_lbl = std::make_shared<TextWidget>("Failed", 10.0f);
                            fail_lbl->setColor({0.90f, 0.40f, 0.35f, 1.0f});
                            status_w = fail_lbl;
                        }

                        auto entry = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{name_lbl, status_w});
                        entry->setGap(2);
                        df->col->addChild(entry);
                    }
                    if (pg.has_more_) {
                        if (auto p = df_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        df->fetch(0);
    };

    auto dl_clear_btn = std::make_shared<ButtonWidget>("🗑 Clear Completed",
        [rebuild_dl]() {
            if (!g_shell_xbus) return;
            g_shell_xbus->Call("com.freebsdx.Downloads",
                static_cast<uint32_t>(DownloadOpcode::ClearCompleted), {}, nullptr);
            if (rebuild_dl && *rebuild_dl) (*rebuild_dl)();
        });
    dl_clear_btn->setFixedSize(140, 26);

    // ── Package Updates ───────────────────────────────────────────────────────
    auto pkg_hdr_lbl = std::make_shared<TextWidget>("📦 Updates", 14.0f);
    pkg_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto pkg_status_lbl = std::make_shared<TextWidget>("", 11.0f);
    pkg_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto pkg_col = std::make_shared<FlexColumn>();
    pkg_col->setGap(6);

    auto rebuild_pkg = std::make_shared<std::function<void()>>();
    *rebuild_pkg = [pkg_col, pkg_status_lbl,
                    rpkg_weak = std::weak_ptr<std::function<void()>>(rebuild_pkg)]() {
        if (!g_shell_xbus) return;
        pkg_col->clearChildren();

        struct PkgFetcher {
            std::shared_ptr<FlexColumn>  col;
            std::shared_ptr<TextWidget>  status;
            std::function<void(uint32_t)> fetch;
            uint32_t total{0};
        };
        auto pf = std::make_shared<PkgFetcher>();
        pf->col    = pkg_col;
        pf->status = pkg_status_lbl;
        pf->fetch = [pf_weak = std::weak_ptr<PkgFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_num = page;
            g_shell_xbus->Call("com.freebsdx.Packages",
                static_cast<uint32_t>(PackageOpcode::ListUpdates),
                {reinterpret_cast<const uint8_t*>(&pg_num),
                 reinterpret_cast<const uint8_t*>(&pg_num) + sizeof(pg_num)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    PackagePage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0) pf->total = pg.total_;
                    if (page == 0 && pg.total_ == 0) {
                        pf->status->setText("Up to date");
                        pf->status->setColor({0.35f, 0.80f, 0.50f, 1.0f});
                        auto empty = std::make_shared<TextWidget>(
                            "No pending updates", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    if (page == 0) {
                        pf->status->setText(std::format("{} update(s) available",
                                                        pf->total));
                        pf->status->setColor({0.95f, 0.75f, 0.25f, 1.0f});
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        float mb = e.total_bytes_ / (1024.0f * 1024.0f);

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_), 12.0f);
                        name_lbl->setColor({0.85f, 0.85f, 0.90f, 1.0f});
                        name_lbl->setFlexGrow(1.0f);

                        auto ver_lbl = std::make_shared<TextWidget>(
                            std::string(e.old_ver_) + " → " + std::string(e.new_ver_),
                            10.0f);
                        ver_lbl->setColor({0.35f, 0.80f, 0.50f, 1.0f});

                        auto size_lbl = std::make_shared<TextWidget>(
                            std::format("{:.1f} MB", mb), 10.0f);
                        size_lbl->setColor({0.45f, 0.45f, 0.50f, 1.0f});

                        uint32_t pid = e.pkg_id_;
                        auto hold_btn = std::make_shared<ButtonWidget>("Hold",
                            [pid]() {
                                if (!g_shell_xbus) return;
                                PackageHoldRequest req{};
                                req.pkg_id_ = pid;
                                g_shell_xbus->Call("com.freebsdx.Packages",
                                    static_cast<uint32_t>(PackageOpcode::HoldPackage),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        hold_btn->setFixedSize(46, 22);

                        auto info_col = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{ver_lbl, size_lbl});
                        info_col->setGap(1);

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl, info_col,
                                std::make_shared<Spacer>(6, 1), hold_btn});
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    auto pkg_update_all_btn = std::make_shared<ButtonWidget>("↑ Update All",
        [pkg_status_lbl]() {
            if (!g_shell_xbus) return;
            pkg_status_lbl->setText("Updating…");
            pkg_status_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
            g_shell_xbus->Call("com.freebsdx.Packages",
                static_cast<uint32_t>(PackageOpcode::UpdateAll), {}, nullptr);
        });
    pkg_update_all_btn->setFixedSize(106, 26);
    pkg_update_all_btn->setColor({0.25f, 0.55f, 0.85f, 1.0f});

    auto pkg_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_pkg]() {
        if (rebuild_pkg && *rebuild_pkg) (*rebuild_pkg)();
    });
    pkg_refresh_btn->setFixedSize(84, 26);

    auto pkg_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        pkg_update_all_btn, std::make_shared<Spacer>(6, 1), pkg_refresh_btn
    });

    // ── Thermal monitor ───────────────────────────────────────────────────────
    auto therm_hdr_lbl = std::make_shared<TextWidget>("🌡 Thermal", 14.0f);
    therm_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto therm_col = std::make_shared<FlexColumn>();
    therm_col->setGap(4);

    auto rebuild_therm = std::make_shared<std::function<void()>>();
    *rebuild_therm = [therm_col,
                      rt_weak = std::weak_ptr<std::function<void()>>(rebuild_therm)]() {
        if (!g_shell_xbus) return;
        therm_col->clearChildren();

        struct ThermFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<ThermFetcher>();
        pf->col = therm_col;
        pf->fetch = [pf_weak = std::weak_ptr<ThermFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_idx = page;
            g_shell_xbus->Call("com.freebsdx.Thermal",
                static_cast<uint32_t>(ThermalSensorOpcode::GetReadings),
                {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    ThermalPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No thermal sensors", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& rd = pg.readings_[i];
                        std::string label(rd.label_[0] ? rd.label_ : "?");

                        if (rd.temp_c_ != INT32_MIN) {
                            // Temperature row
                            int t = rd.temp_c_;
                            Color col =
                                t >= 85 ? Color{0.95f, 0.30f, 0.25f, 1.0f} :
                                t >= 70 ? Color{0.95f, 0.65f, 0.20f, 1.0f} :
                                          Color{0.35f, 0.85f, 0.45f, 1.0f};
                            auto lbl = std::make_shared<TextWidget>(
                                std::format("{:<18} {:>3} °C", label, t), 12.0f);
                            lbl->setFontFamily("monospace");
                            lbl->setColor(col);
                            pf->col->addChild(lbl);
                        }
                        if (rd.fan_rpm_ > 0) {
                            // Fan row (may accompany a temp or stand alone)
                            auto fan_lbl = std::make_shared<TextWidget>(
                                std::format("{:<18} {:>5} RPM",
                                    rd.temp_c_ == INT32_MIN ? label : "  └ Fan",
                                    rd.fan_rpm_), 12.0f);
                            fan_lbl->setFontFamily("monospace");
                            fan_lbl->setColor({0.55f, 0.65f, 0.80f, 1.0f});
                            pf->col->addChild(fan_lbl);
                        }
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    // ── Active Connections ────────────────────────────────────────────────────
    auto conn_hdr_lbl = std::make_shared<TextWidget>("🔗 Active Connections", 14.0f);
    conn_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto conn_col = std::make_shared<FlexColumn>();
    conn_col->setGap(4);

    auto rebuild_conn = std::make_shared<std::function<void()>>();
    *rebuild_conn = [conn_col,
                     rc_weak = std::weak_ptr<std::function<void()>>(rebuild_conn)]() {
        if (!g_shell_xbus) return;
        conn_col->clearChildren();

        struct ConnFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<ConnFetcher>();
        pf->col = conn_col;
        pf->fetch = [pf_weak = std::weak_ptr<ConnFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_idx = page;
            g_shell_xbus->Call("com.freebsdx.Netstat",
                static_cast<uint32_t>(NetstatOpcode::ListConnections),
                {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    NetstatPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No active connections", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& c = pg.conns_[i];
                        std::string app_s(c.app_[0] ? c.app_ : "(unknown)");
                        std::string remote_s(c.remote_);
                        bool estab = std::strcmp(c.state_, "ESTABLISHED") == 0;

                        auto app_lbl = std::make_shared<TextWidget>(
                            app_s + "  " + std::string(c.proto_), 12.0f);
                        app_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        auto remote_lbl = std::make_shared<TextWidget>(remote_s, 10.0f);
                        remote_lbl->setColor(estab
                            ? Color{0.35f, 0.80f, 0.50f, 1.0f}
                            : Color{0.55f, 0.55f, 0.60f, 1.0f});

                        uint32_t conn_id = c.conn_id_;
                        auto kill_btn = std::make_shared<ButtonWidget>("✕ Kill",
                            [conn_id]() {
                                if (!g_shell_xbus) return;
                                NetstatKillRequest req{};
                                req.conn_id_ = conn_id;
                                g_shell_xbus->Call("com.freebsdx.Netstat",
                                    static_cast<uint32_t>(NetstatOpcode::KillConnection),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        kill_btn->setFixedSize(56, 24);
                        kill_btn->setBackgroundColor({0.45f, 0.12f, 0.12f, 1.0f});

                        auto info_col = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{app_lbl, remote_lbl});
                        info_col->setGap(1);

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                info_col, std::make_shared<Spacer>(1, 1, true), kill_btn});
                        row->setGap(6);
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    auto conn_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_conn]() {
        if (rebuild_conn && *rebuild_conn) (*rebuild_conn)();
    });
    conn_refresh_btn->setFixedSize(84, 26);

    // ── License & Support ── (carved into buildLicensePanel; aliases keep the
    // card/broadcast/GetState-reply refs unchanged.)
    buildLicensePanel(ctx);
    auto& lic_hdr_lbl     = ctx.lic_hdr_lbl;
    auto& lic_key_lbl     = ctx.lic_key_lbl;
    auto& lic_tier_lbl    = ctx.lic_tier_lbl;
    auto& lic_expiry_lbl  = ctx.lic_expiry_lbl;
    auto& lic_seats_lbl   = ctx.lic_seats_lbl;
    auto& lic_support_lbl = ctx.lic_support_lbl;
    auto& lic_copy_btn    = ctx.lic_copy_btn;
    auto& update_lic_ui   = ctx.update_lic_ui;

    // ── App Permissions ───────────────────────────────────────────────────────
    auto perm_hdr_lbl  = std::make_shared<TextWidget>("\xf0\x9f\x94\x92 App Permissions", 14.0f);
    perm_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto perm_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    perm_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto perm_col      = std::make_shared<FlexColumn>();
    perm_col->setGap(8);

    auto rebuild_perms = std::make_shared<std::function<void()>>();
    *rebuild_perms = [perm_col, perm_count_lbl,
                      rp_weak = std::weak_ptr<std::function<void()>>(rebuild_perms)]() {
        if (!g_shell_xbus) return;
        perm_col->clearChildren();

        struct PermFetcher {
            std::shared_ptr<FlexColumn>                    col;
            std::shared_ptr<TextWidget>                    count_lbl;
            std::weak_ptr<std::function<void()>>           rebuild_weak;
            uint32_t                                       total_seen{0};
            std::shared_ptr<std::function<void(uint32_t)>> fetch;
        };
        auto pf = std::make_shared<PermFetcher>();
        pf->col          = perm_col;
        pf->count_lbl    = perm_count_lbl;
        pf->rebuild_weak = rp_weak;

        pf->fetch = std::make_shared<std::function<void(uint32_t)>>(
            [pf](uint32_t page) {
                if (!g_shell_xbus) return;
                uint32_t pg = page;
                g_shell_xbus->Call("com.freebsdx.AppPerms",
                    static_cast<uint32_t>(AppPermOpcode::ListPerms),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [pf, page](const XBusMessage& r) {
                        AppPermPage ap{};
                        std::memcpy(&ap, r.payload_, sizeof(ap));
                        if (page == 0) {
                            pf->col->clearChildren();
                            pf->total_seen = 0;
                        }
                        pf->total_seen += ap.count_;

                        for (uint32_t i = 0; i < ap.count_; ++i) {
                            const AppPermEntry& e = ap.entries_[i];
                            std::string app_id(e.app_id_);
                            std::string app_name(e.name_);

                            auto row = std::make_shared<FlexColumn>();
                            row->setGap(4);

                            auto name_lbl = std::make_shared<TextWidget>(app_name, 11.0f);
                            name_lbl->setColor({0.40f, 0.65f, 0.95f, 1.0f});

                            auto toggles = std::make_shared<FlexRow>();
                            toggles->setGap(10);

                            // Helper to make a permission toggle
                            auto make_perm_toggle = [&](const char* label, uint32_t cur_val,
                                                        uint32_t perm_idx) {
                                auto lbl = std::make_shared<TextWidget>(label, 10.0f);
                                lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                                auto sw = std::make_shared<SwitchWidget>(cur_val != 0);
                                sw->setOnChanged([app_id, perm_idx, pf](bool on) {
                                    if (!g_shell_xbus) return;
                                    AppPermSetRequest req{};
                                    std::strncpy(req.app_id_, app_id.c_str(),
                                                 sizeof(req.app_id_) - 1);
                                    req.perm_  = perm_idx;
                                    req.value_ = on ? 1u : 0u;
                                    g_shell_xbus->Call("com.freebsdx.AppPerms",
                                        static_cast<uint32_t>(AppPermOpcode::SetPerm),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        [pf](const XBusMessage&) {
                                            if (auto rb = pf->rebuild_weak.lock())
                                                if (*rb) (*rb)();
                                        });
                                });
                                auto pair = std::make_shared<FlexRow>();
                                pair->setGap(4);
                                pair->addChild(lbl);
                                pair->addChild(sw);
                                return pair;
                            };

                            toggles->addChild(make_perm_toggle(
                                "\xf0\x9f\x93\xb7", e.camera_,   0)); // 📷
                            toggles->addChild(make_perm_toggle(
                                "\xf0\x9f\x8e\xa4", e.mic_,      1)); // 🎤
                            toggles->addChild(make_perm_toggle(
                                "\xf0\x9f\x93\x8d", e.location_, 2)); // 📍

                            row->addChild(name_lbl);
                            row->addChild(toggles);
                            pf->col->addChild(row);
                        }

                        uint32_t n = pf->total_seen;
                        pf->count_lbl->setText(n == 0 ? "No apps"
                            : std::format("{} app{}", n, n != 1 ? "s" : ""));

                        if (ap.has_more_ && pf->fetch)
                            (*pf->fetch)(page + 1);
                        else if (n == 0)
                            pf->col->addChild(
                                std::make_shared<TextWidget>("No apps with permissions.", 11.0f));
                    });
            });
        (*pf->fetch)(0);
    };

    // ── Input Methods ─────────────────────────────────────────────────────────
    auto im_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x8c\xa8 Input Methods", 14.0f);
    im_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto im_active_lbl = std::make_shared<TextWidget>("", 11.0f);
    im_active_lbl->setColor({0.40f, 0.75f, 0.55f, 1.0f});
    auto im_col        = std::make_shared<FlexColumn>();
    im_col->setGap(6);

    auto rebuild_im = std::make_shared<std::function<void()>>();
    *rebuild_im = [im_col, im_active_lbl,
                   ri_weak = std::weak_ptr<std::function<void()>>(rebuild_im)]() {
        if (!g_shell_xbus) return;
        im_col->clearChildren();

        struct ImFetcher {
            std::shared_ptr<FlexColumn>                    col;
            std::shared_ptr<TextWidget>                    active_lbl;
            std::weak_ptr<std::function<void()>>           rebuild_weak;
            std::shared_ptr<std::function<void(uint32_t)>> fetch;
        };
        auto imf = std::make_shared<ImFetcher>();
        imf->col        = im_col;
        imf->active_lbl = im_active_lbl;
        imf->rebuild_weak = ri_weak;

        imf->fetch = std::make_shared<std::function<void(uint32_t)>>(
            [imf](uint32_t page) {
                if (!g_shell_xbus) return;
                uint32_t pg = page;
                g_shell_xbus->Call("com.freebsdx.InputMethod",
                    static_cast<uint32_t>(ImOpcode::ListMethods),
                    {reinterpret_cast<const uint8_t*>(&pg), sizeof(pg)},
                    [imf, page](const XBusMessage& r) {
                        ImPage ip{};
                        std::memcpy(&ip, r.payload_, sizeof(ip));
                        if (page == 0) imf->col->clearChildren();

                        for (uint32_t i = 0; i < ip.count_; ++i) {
                            const ImEntry& e = ip.entries_[i];
                            std::string im_name(e.name_);
                            std::string lang(e.lang_);
                            bool active = e.active_ != 0;

                            if (active)
                                imf->active_lbl->setText(im_name);

                            auto row = std::make_shared<FlexRow>();
                            row->setGap(8);

                            auto name_lbl = std::make_shared<TextWidget>(im_name, 11.0f);
                            name_lbl->setColor(active
                                ? Color{0.40f, 0.65f, 0.95f, 1.0f}
                                : Color{0.75f, 0.75f, 0.80f, 1.0f});

                            auto lang_lbl = std::make_shared<TextWidget>(lang, 10.0f);
                            lang_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});

                            auto spacer = std::make_shared<Spacer>(1, 1, true);

                            if (active) {
                                auto badge = std::make_shared<TextWidget>("Active", 10.0f);
                                badge->setColor({0.35f, 0.80f, 0.45f, 1.0f});
                                row->addChild(name_lbl);
                                row->addChild(lang_lbl);
                                row->addChild(spacer);
                                row->addChild(badge);
                            } else {
                                auto sw_btn = std::make_shared<ButtonWidget>("Switch");
                                sw_btn->setFixedSize(56, 22);
                                sw_btn->setBackgroundColor({0.15f, 0.35f, 0.70f, 1.0f});
                                sw_btn->onClick([im_name, imf]() {
                                    if (!g_shell_xbus) return;
                                    ImSetRequest req{};
                                    std::strncpy(req.name_, im_name.c_str(),
                                                 sizeof(req.name_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.InputMethod",
                                        static_cast<uint32_t>(ImOpcode::SetActive),
                                        {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                        [imf](const XBusMessage&) {
                                            if (auto rb = imf->rebuild_weak.lock())
                                                if (*rb) (*rb)();
                                        });
                                });
                                row->addChild(name_lbl);
                                row->addChild(lang_lbl);
                                row->addChild(spacer);
                                row->addChild(sw_btn);
                            }
                            imf->col->addChild(row);
                        }

                        if (ip.has_more_ && imf->fetch)
                            (*imf->fetch)(page + 1);
                        else if (page == 0 && ip.count_ == 0) {
                            imf->active_lbl->setText("None configured");
                            imf->col->addChild(
                                std::make_shared<TextWidget>("No input methods installed.", 11.0f));
                        }
                    });
            });
        (*imf->fetch)(0);
    };

    // ── Process Manager ───────────────────────────────────────────────────────
    auto proc_hdr_lbl = std::make_shared<TextWidget>("⚙ Processes", 14.0f);
    proc_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto proc_col = std::make_shared<FlexColumn>();
    proc_col->setGap(3);

    auto rebuild_proc = std::make_shared<std::function<void()>>();
    *rebuild_proc = [proc_col,
                     rpr_weak = std::weak_ptr<std::function<void()>>(rebuild_proc)]() {
        if (!g_shell_xbus) return;
        proc_col->clearChildren();

        struct ProcFetcher {
            std::shared_ptr<FlexColumn>   col;
            std::function<void(uint32_t)> fetch;
        };
        auto pf = std::make_shared<ProcFetcher>();
        pf->col = proc_col;
        pf->fetch = [pf_weak = std::weak_ptr<ProcFetcher>(pf)](uint32_t page) {
            if (!g_shell_xbus) return;
            uint32_t pg_idx = page;
            g_shell_xbus->Call("com.freebsdx.Process",
                static_cast<uint32_t>(ProcessOpcode::ListTop),
                {reinterpret_cast<const uint8_t*>(&pg_idx), sizeof(pg_idx)},
                [pf_weak, page](const XBusMessage& resp) {
                    auto pf = pf_weak.lock();
                    if (!pf) return;
                    ProcessPage pg{};
                    std::memcpy(&pg, resp.payload_, sizeof(pg));
                    if (page == 0 && pg.total_ == 0) {
                        auto empty = std::make_shared<TextWidget>("No processes", 11.0f);
                        empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        pf->col->addChild(empty);
                        return;
                    }
                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        float cpu = e.cpu_pct10_ / 10.0f;
                        Color cpu_col =
                            cpu >= 50.0f ? Color{0.95f, 0.30f, 0.25f, 1.0f} :
                            cpu >= 20.0f ? Color{0.95f, 0.65f, 0.20f, 1.0f} :
                                           Color{0.35f, 0.85f, 0.45f, 1.0f};

                        auto name_lbl = std::make_shared<TextWidget>(
                            std::string(e.name_[0] ? e.name_ : "?"), 12.0f);
                        name_lbl->setColor({0.88f, 0.88f, 0.92f, 1.0f});

                        uint32_t rss_mb = e.rss_kb_ / 1024;
                        auto stats_lbl = std::make_shared<TextWidget>(
                            std::format("{:.1f}%  {}MB  pid {}", cpu, rss_mb, e.pid_),
                            10.0f);
                        stats_lbl->setColor(cpu_col);

                        uint32_t pid = e.pid_;
                        auto kill_btn = std::make_shared<ButtonWidget>("✕",
                            [pid]() {
                                if (!g_shell_xbus) return;
                                ProcessKillRequest req{};
                                req.pid_    = pid;
                                req.signal_ = 15; // SIGTERM
                                g_shell_xbus->Call("com.freebsdx.Process",
                                    static_cast<uint32_t>(ProcessOpcode::Kill),
                                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                    nullptr);
                            });
                        kill_btn->setFixedSize(28, 28);
                        kill_btn->setBackgroundColor({0.45f, 0.12f, 0.12f, 1.0f});

                        auto info_col = std::make_shared<FlexColumn>(
                            std::vector<std::shared_ptr<Widget>>{name_lbl, stats_lbl});
                        info_col->setGap(1);

                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                info_col, std::make_shared<Spacer>(1, 1, true), kill_btn});
                        row->setGap(6);
                        row->setAlignment(Alignment::Center);
                        pf->col->addChild(row);
                    }
                    if (pg.has_more_) {
                        if (auto p = pf_weak.lock()) p->fetch(page + 1);
                    }
                });
        };
        pf->fetch(0);
    };

    auto proc_refresh_btn = std::make_shared<ButtonWidget>("↺ Refresh", [rebuild_proc]() {
        if (rebuild_proc && *rebuild_proc) (*rebuild_proc)();
    });
    proc_refresh_btn->setFixedSize(84, 26);

    // ── Energy Impact ─────────────────────────────────────────────────────────
    auto energy_hdr_lbl   = std::make_shared<TextWidget>("\xe2\x9a\xa1 Energy Impact", 14.0f);
    energy_hdr_lbl->setColor({0.8f, 0.8f, 0.85f, 1.0f});
    auto energy_count_lbl = std::make_shared<TextWidget>("", 11.0f);
    energy_count_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
    auto energy_col       = std::make_shared<FlexColumn>();
    energy_col->setGap(5);

    auto rebuild_energy   = std::make_shared<std::function<void()>>();
    *rebuild_energy = [energy_col, energy_count_lbl,
                       re_weak = std::weak_ptr<std::function<void()>>(rebuild_energy)]() {
        if (!g_shell_xbus) return;
        energy_col->clearChildren();

        struct EnergyFetcher {
            std::shared_ptr<FlexColumn>          col;
            std::shared_ptr<TextWidget>          count_lbl;
            std::weak_ptr<std::function<void()>> rebuild_weak;
            uint32_t                             total{0};
            std::function<void(uint32_t)>        fetch;
        };
        auto ef = std::make_shared<EnergyFetcher>();
        ef->col          = energy_col;
        ef->count_lbl    = energy_count_lbl;
        ef->rebuild_weak = re_weak;

        ef->fetch = [ef](uint32_t page) {
            g_shell_xbus->Call("com.freebsdx.Energy",
                static_cast<uint32_t>(EnergyOpcode::ListUsage),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [ef](const XBusMessage& r) {
                    EnergyPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.page_ == 0) ef->total = pg.total_;

                    static const char* kImpactLabels[] = {
                        "Low", "Medium", "High", "Very High"
                    };
                    static const Color kImpactColors[] = {
                        {0.30f, 0.85f, 0.45f, 1.0f}, // green
                        {0.90f, 0.72f, 0.20f, 1.0f}, // amber
                        {0.95f, 0.48f, 0.15f, 1.0f}, // orange
                        {0.85f, 0.25f, 0.25f, 1.0f}, // red
                    };

                    for (uint32_t i = 0; i < pg.count_; ++i) {
                        const auto& e = pg.entries_[i];
                        std::string name(e.app_name_,
                            std::min(std::strlen(e.app_name_), sizeof(e.app_name_) - 1));
                        uint32_t impact = std::min(e.impact_, 3u);

                        auto name_lbl   = std::make_shared<TextWidget>(name, 11.0f);
                        name_lbl->setColor({0.45f, 0.70f, 1.0f, 1.0f});
                        auto wake_lbl   = std::make_shared<TextWidget>(
                            std::format("{}/s", e.wakeups_sec_), 10.0f);
                        wake_lbl->setColor({0.55f, 0.55f, 0.60f, 1.0f});
                        auto cpu_lbl    = std::make_shared<TextWidget>(
                            std::format("{}%", e.cpu_avg_pct_), 10.0f);
                        cpu_lbl->setColor({0.65f, 0.65f, 0.70f, 1.0f});
                        auto impact_lbl = std::make_shared<TextWidget>(
                            kImpactLabels[impact], 10.0f);
                        impact_lbl->setColor(kImpactColors[impact]);

                        ef->col->addChild(std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                name_lbl,
                                std::make_shared<Spacer>(1, 1, true),
                                wake_lbl,
                                std::make_shared<Spacer>(8, 1),
                                cpu_lbl,
                                std::make_shared<Spacer>(8, 1),
                                impact_lbl,
                            }));
                    }

                    if (pg.has_more_) {
                        ef->fetch(pg.page_ + 1);
                    } else {
                        ef->count_lbl->setText(ef->total > 0
                            ? std::format("{} processes", ef->total) : "");
                    }
                });
        };
        ef->fetch(0);
    };

    // ── Memory Pressure ── (carved into buildMemPressurePanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged.)
    buildMemPressurePanel(ctx);
    auto& mem_hdr_lbl    = ctx.mem_hdr_lbl;
    auto& mem_level_lbl  = ctx.mem_level_lbl;
    auto& mem_ram_lbl    = ctx.mem_ram_lbl;
    auto& mem_swap_lbl   = ctx.mem_swap_lbl;
    auto& mem_faults_lbl = ctx.mem_faults_lbl;
    auto& mem_bar_row    = ctx.mem_bar_row;
    auto& mem_bar_blocks = ctx.mem_bar_blocks;
    auto& update_mem_ui  = ctx.update_mem_ui;

    // ── Frame Timing ── (carved into buildFrameTimingPanel; aliases keep the
    // card/broadcast/reply/timer refs unchanged.)
    buildFrameTimingPanel(ctx);
    auto& ft_hdr_lbl   = ctx.ft_hdr_lbl;
    auto& ft_avg_lbl   = ctx.ft_avg_lbl;
    auto& ft_p99_lbl   = ctx.ft_p99_lbl;
    auto& ft_jank_lbl  = ctx.ft_jank_lbl;
    auto& ft_gpu_lbl   = ctx.ft_gpu_lbl;
    auto& ft_vsync_lbl = ctx.ft_vsync_lbl;
    auto& ft_bars      = ctx.ft_bars;
    auto& ft_bar_row   = ctx.ft_bar_row;
    auto& update_ft_ui = ctx.update_ft_ui;

    // ── File Sync ── (carved into buildFileSyncPanel; header/count/col +
    // rebuild_file_sync escape — the local FsFetcher struct stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildFileSyncPanel(ctx);
    auto& fs_hdr_lbl        = ctx.fs_hdr_lbl;
    auto& fs_count_lbl      = ctx.fs_count_lbl;
    auto& fs_col            = ctx.fs_col;
    auto& rebuild_file_sync = ctx.rebuild_file_sync;

    // ── Audio Routing ── (carved into buildAudioRoutingPanel; header/count/col +
    // rebuild_audio_routes escape — the local AudioRouteFetcher struct stays in
    // the rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildAudioRoutingPanel(ctx);
    auto& ar_hdr_lbl           = ctx.ar_hdr_lbl;
    auto& ar_count_lbl         = ctx.ar_count_lbl;
    auto& audio_route_col      = ctx.audio_route_col;
    auto& rebuild_audio_routes = ctx.rebuild_audio_routes;

    // ── Network Shares ── (carved into buildNetworkSharesPanel; header/count/col +
    // rebuild_shares escape — the local ShareFetcher struct stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildNetworkSharesPanel(ctx);
    auto& ns_hdr_lbl     = ctx.ns_hdr_lbl;
    auto& ns_count_lbl   = ctx.ns_count_lbl;
    auto& share_col      = ctx.share_col;
    auto& rebuild_shares = ctx.rebuild_shares;

    // ── Printer Queue ── (carved into buildPrinterQueuePanel; count/col +
    // rebuild_print_queue escape — the local PrintFetcher struct + the latent
    // header label stay builder-local. Aliases keep broadcast/timer refs
    // unchanged.)
    buildPrinterQueuePanel(ctx);
    auto& print_count_lbl     = ctx.print_count_lbl;
    auto& print_col           = ctx.print_col;
    auto& rebuild_print_queue = ctx.rebuild_print_queue;

    // ── Package Updates ── (carved into buildPackageUpdatesPanel; header/count/col
    // + Upgrade-All/Check buttons + rebuild_pkg_upd escape — the local PkgFetcher
    // struct stays in the rebuild. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildPackageUpdatesPanel(ctx);
    auto& pkg_upd_hdr_lbl   = ctx.pkg_upd_hdr_lbl;
    auto& pkg_upd_count_lbl = ctx.pkg_upd_count_lbl;
    auto& pkg_upd_col       = ctx.pkg_upd_col;
    auto& pkg_upd_all_btn   = ctx.pkg_upd_all_btn;
    auto& pkg_upd_check_btn = ctx.pkg_upd_check_btn;
    auto& rebuild_pkg_upd   = ctx.rebuild_pkg_upd;

    // ── ZFS Pool Health ── (carved into buildZfsPoolsPanel; header/count/col +
    // rebuild_zfs escape — the local ZfsFetcher struct stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildZfsPoolsPanel(ctx);
    auto& zfs_hdr_lbl   = ctx.zfs_hdr_lbl;
    auto& zfs_count_lbl = ctx.zfs_count_lbl;
    auto& zfs_col       = ctx.zfs_col;
    auto& rebuild_zfs   = ctx.rebuild_zfs;

    // ── Active Jails ── (carved into buildActiveJailsPanel; header/count/col +
    // refresh-btn + rebuild_jails escape — the local JailFetcher struct stays in
    // the rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildActiveJailsPanel(ctx);
    auto& jail_hdr_lbl     = ctx.jail_hdr_lbl;
    auto& jail_count_lbl   = ctx.jail_count_lbl;
    auto& jail_col         = ctx.jail_col;
    auto& jail_refresh_btn = ctx.jail_refresh_btn;
    auto& rebuild_jails    = ctx.rebuild_jails;

    // ── Boot Environments ── (carved into buildBootEnvPanel; header/count/col +
    // refresh-btn + rebuild_be escape — the local BeFetcher struct stays in the
    // rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildBootEnvPanel(ctx);
    auto& be_hdr_lbl     = ctx.be_hdr_lbl;
    auto& be_count_lbl   = ctx.be_count_lbl;
    auto& be_col         = ctx.be_col;
    auto& be_refresh_btn = ctx.be_refresh_btn;
    auto& rebuild_be     = ctx.rebuild_be;

    // ── Containers ── (carved into buildContainersPanel; header/count/col +
    // rebuild_containers escape — the local ContainerFetcher struct stays in the
    // rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildContainersPanel(ctx);
    auto& ct_hdr_lbl         = ctx.ct_hdr_lbl;
    auto& ct_count_lbl       = ctx.ct_count_lbl;
    auto& ct_col             = ctx.ct_col;
    auto& rebuild_containers = ctx.rebuild_containers;

    // ── Virtual Machines ── (carved into buildVirtualMachinesPanel; header/count/
    // col + rebuild_vms escape — the local VmFetcher struct stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildVirtualMachinesPanel(ctx);
    auto& vm_hdr_lbl   = ctx.vm_hdr_lbl;
    auto& vm_count_lbl = ctx.vm_count_lbl;
    auto& vm_col       = ctx.vm_col;
    auto& rebuild_vms  = ctx.rebuild_vms;

    // ── Kernel Modules ── (carved into buildKernelModulesPanel; header/count/col +
    // refresh-btn + rebuild_kmod escape — the local KmodFetcher struct stays in
    // the rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildKernelModulesPanel(ctx);
    auto& kmod_hdr_lbl     = ctx.kmod_hdr_lbl;
    auto& kmod_count_lbl   = ctx.kmod_count_lbl;
    auto& kmod_col         = ctx.kmod_col;
    auto& kmod_refresh_btn = ctx.kmod_refresh_btn;
    auto& rebuild_kmod     = ctx.rebuild_kmod;

    // ── Screenshot & Recordings ── (carved into buildCapturesPanel; header/count/
    // col + screenshot-btn + rebuild_captures escape — the local CapFetcher struct
    // stays in the rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildCapturesPanel(ctx);
    auto& cap_hdr_lbl        = ctx.cap_hdr_lbl;
    auto& cap_count_lbl      = ctx.cap_count_lbl;
    auto& cap_col            = ctx.cap_col;
    auto& cap_screenshot_btn = ctx.cap_screenshot_btn;
    auto& rebuild_captures   = ctx.rebuild_captures;

    // ── System Information ── (carved into buildSysInfoPanel; aliases keep the
    // card/broadcast/reply refs unchanged. make_si_row stays local to builder.)
    buildSysInfoPanel(ctx);
    auto& si_hdr_lbl       = ctx.si_hdr_lbl;
    auto& si_col           = ctx.si_col;
    auto& si_hostname_lbl  = ctx.si_hostname_lbl;
    auto& si_os_lbl        = ctx.si_os_lbl;
    auto& si_cpu_lbl       = ctx.si_cpu_lbl;
    auto& si_cores_lbl     = ctx.si_cores_lbl;
    auto& si_ram_lbl       = ctx.si_ram_lbl;
    auto& si_gpu_lbl       = ctx.si_gpu_lbl;
    auto& si_uptime_lbl    = ctx.si_uptime_lbl;
    auto& si_serial_lbl    = ctx.si_serial_lbl;
    auto& update_sysinfo_ui = ctx.update_sysinfo_ui;

    // ── Crash Reporter ── (carved into buildCrashReporterPanel; aliases keep the
    // card/broadcast/timer refs unchanged. CrashFetcher machinery stays local.)
    buildCrashReporterPanel(ctx);
    auto& crash_hdr_lbl   = ctx.crash_hdr_lbl;
    auto& crash_col       = ctx.crash_col;
    auto& rebuild_crash   = ctx.rebuild_crash;
    auto& crash_clear_btn = ctx.crash_clear_btn;

    // ── Security Events ── (carved into buildSecurityEventsPanel; aliases keep
    // the card/broadcast/timer refs unchanged. SecFetcher + the refresh/clear
    // buttons stay builder-local, kept alive by sec_btn_row.)
    buildSecurityEventsPanel(ctx);
    auto& sec_hdr_lbl = ctx.sec_hdr_lbl;
    auto& sec_col     = ctx.sec_col;
    auto& rebuild_sec = ctx.rebuild_sec;
    auto& sec_btn_row = ctx.sec_btn_row;

    // ── Keychain ── (carved into buildKeychainPanel; header/count/col + lock-btn +
    // rebuild_keychain escape — the local KcFetcher struct stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildKeychainPanel(ctx);
    auto& kc_hdr_lbl       = ctx.kc_hdr_lbl;
    auto& kc_count_lbl     = ctx.kc_count_lbl;
    auto& kc_col           = ctx.kc_col;
    auto& kc_lock_btn      = ctx.kc_lock_btn;
    auto& rebuild_keychain = ctx.rebuild_keychain;

    // ── System Services ── (carved into buildSystemServicesPanel; header/col +
    // refresh-btn + rebuild_svc escape — the local SvcFetcher struct stays in the
    // rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildSystemServicesPanel(ctx);
    auto& svc_hdr_lbl     = ctx.svc_hdr_lbl;
    auto& svc_col         = ctx.svc_col;
    auto& svc_refresh_btn = ctx.svc_refresh_btn;
    auto& rebuild_svc     = ctx.rebuild_svc;

    // ── Login Sessions ── (carved into buildLoginSessionsPanel; header/col/
    // refresh-btn + rebuild_lsess escape — aliases keep card/broadcast/timer refs
    // unchanged.)
    buildLoginSessionsPanel(ctx);
    auto& lsess_hdr_lbl     = ctx.lsess_hdr_lbl;
    auto& lsess_col         = ctx.lsess_col;
    auto& lsess_refresh_btn = ctx.lsess_refresh_btn;
    auto& rebuild_lsess     = ctx.rebuild_lsess;

    // ── Battery Drain ── (carved into buildBatteryDrainPanel; header/col/refresh-
    // btn + rebuild_bdrain escape — aliases keep card/broadcast/timer refs
    // unchanged.)
    buildBatteryDrainPanel(ctx);
    auto& bdrain_hdr_lbl     = ctx.bdrain_hdr_lbl;
    auto& bdrain_col         = ctx.bdrain_col;
    auto& bdrain_refresh_btn = ctx.bdrain_refresh_btn;
    auto& rebuild_bdrain     = ctx.rebuild_bdrain;

    // ── Cron Jobs ── (carved into buildCronJobsPanel; header/col/refresh-btn +
    // rebuild_cron escape — aliases keep the card/broadcast/timer refs unchanged.)
    buildCronJobsPanel(ctx);
    auto& cron_hdr_lbl     = ctx.cron_hdr_lbl;
    auto& cron_col         = ctx.cron_col;
    auto& cron_refresh_btn = ctx.cron_refresh_btn;
    auto& rebuild_cron     = ctx.rebuild_cron;

    // ── GPU Statistics ── (carved into buildGpuStatsPanel; only the updater
    // escapes — alias keeps the broadcast/reply/timer refs unchanged.)
    buildGpuStatsPanel(ctx);
    auto& update_gpu_ui_b = ctx.update_gpu_ui_b;

    // ── Startup Items ── (carved into buildStartupItemsPanel; header/col/refresh-
    // btn + rebuild_startup escape — aliases keep the card/broadcast/timer refs
    // unchanged.)
    buildStartupItemsPanel(ctx);
    auto& startup_hdr_lbl     = ctx.startup_hdr_lbl;
    auto& startup_col         = ctx.startup_col;
    auto& startup_refresh_btn = ctx.startup_refresh_btn;
    auto& rebuild_startup     = ctx.rebuild_startup;

    // ── Mounted Volumes ── (carved into buildMountedVolumesPanel; header/col +
    // rebuild_vol escape — aliases keep the card/broadcast/timer refs unchanged.)
    buildMountedVolumesPanel(ctx);
    auto& vol_hdr_lbl = ctx.vol_hdr_lbl;
    auto& vol_col     = ctx.vol_col;
    auto& rebuild_vol = ctx.rebuild_vol;

    // ── Open Network Connections ── (carved into buildNetConnsPanel; header/col/
    // refresh-btn + rebuild_netconn escape — aliases keep the card/broadcast/timer
    // refs unchanged.)
    buildNetConnsPanel(ctx);
    auto& netconn_hdr_lbl     = ctx.netconn_hdr_lbl;
    auto& netconn_col         = ctx.netconn_col;
    auto& netconn_refresh_btn = ctx.netconn_refresh_btn;
    auto& rebuild_netconn     = ctx.rebuild_netconn;

    // ── SSH Keys ── (carved into buildSshKeysPanel; header/count/known labels +
    // col + rebuild_sshkey escape — aliases keep the card/broadcast/timer refs
    // unchanged.)
    buildSshKeysPanel(ctx);
    auto& sshkey_hdr_lbl   = ctx.sshkey_hdr_lbl;
    auto& sshkey_count_lbl = ctx.sshkey_count_lbl;
    auto& sshkey_known_lbl = ctx.sshkey_known_lbl;
    auto& sshkey_col       = ctx.sshkey_col;
    auto& rebuild_sshkey   = ctx.rebuild_sshkey;

    // ── Environment Variables ── (carved into buildEnvVarsPanel; header/count +
    // col + form-row + rebuild_envvar escape — the Set-Var form inputs/button
    // stay builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildEnvVarsPanel(ctx);
    auto& envvar_hdr_lbl   = ctx.envvar_hdr_lbl;
    auto& envvar_count_lbl = ctx.envvar_count_lbl;
    auto& envvar_col       = ctx.envvar_col;
    auto& envvar_form_row  = ctx.envvar_form_row;
    auto& rebuild_envvar   = ctx.rebuild_envvar;

    // ── System Logs ── (carved into buildSysLogPanel; header/col/filter-row/
    // refresh-btn + rebuild_syslog escape — the filter state, filter buttons and
    // highlight helper stay builder-local. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildSysLogPanel(ctx);
    auto& syslog_hdr_lbl     = ctx.syslog_hdr_lbl;
    auto& syslog_col         = ctx.syslog_col;
    auto& syslog_filter_row  = ctx.syslog_filter_row;
    auto& syslog_refresh_btn = ctx.syslog_refresh_btn;
    auto& rebuild_syslog     = ctx.rebuild_syslog;

    // ── USB Devices ── (carved into buildUsbDevicesPanel; header/count/col/
    // rescan-btn + rebuild_usb escape — aliases keep card/broadcast/timer refs
    // unchanged.)
    buildUsbDevicesPanel(ctx);
    auto& usb_hdr_lbl   = ctx.usb_hdr_lbl;
    auto& usb_count_lbl = ctx.usb_count_lbl;
    auto& usb_col       = ctx.usb_col;
    auto& usb_rescan_btn = ctx.usb_rescan_btn;
    auto& rebuild_usb   = ctx.rebuild_usb;

    // ── Calendar quick-add section ── (carved into buildCalendarQuickAddPanel;
    // header/input/status + cal_add_row escape — the "+ Add" button stays
    // builder-local. Aliases keep card/broadcast refs unchanged.)
    buildCalendarQuickAddPanel(ctx);
    auto& cal_add_hdr_lbl = ctx.cal_add_hdr_lbl;
    auto& cal_add_input   = ctx.cal_add_input;
    auto& cal_add_status  = ctx.cal_add_status;
    auto& cal_add_row     = ctx.cal_add_row;

    // ── 3-Day Weather Forecast section ── (carved into buildWeatherForecastPanel;
    // header + 3 label arrays + forecast_rebuild + forecast_row escape — the
    // forecast_rebuild body is assigned later in OnConnect via the alias. Aliases
    // keep card/broadcast refs unchanged.)
    buildWeatherForecastPanel(ctx);
    auto& forecast_hdr_lbl  = ctx.forecast_hdr_lbl;
    auto& forecast_day_lbls  = ctx.forecast_day_lbls;
    auto& forecast_icon_lbls = ctx.forecast_icon_lbls;
    auto& forecast_temp_lbls = ctx.forecast_temp_lbls;
    auto& forecast_rebuild   = ctx.forecast_rebuild;
    auto& forecast_row       = ctx.forecast_row;

    // Per-app volume section
    // ── Quick countdown timer section ── (carved into buildQuickTimerPanel;
    // header/input/cd-lbl/label-input/status + timer_btn_row escape — the
    // Start/Stop buttons stay builder-local. Aliases keep card/broadcast/timer
    // refs unchanged.)
    buildQuickTimerPanel(ctx);
    auto& timer_hdr_lbl     = ctx.timer_hdr_lbl;
    auto& timer_input       = ctx.timer_input;
    auto& timer_cd_lbl      = ctx.timer_cd_lbl;
    auto& timer_label_input = ctx.timer_label_input;
    auto& timer_status_lbl  = ctx.timer_status_lbl;
    auto& timer_btn_row     = ctx.timer_btn_row;

    // ── Pomodoro session log section ── (carved into buildPomodoroLogPanel;
    // col + rebuild_pomo_log + pomo_log_hdr_row escape — the header label + Clear
    // button stay builder-local, rebuild body assigned later in OnConnect. Aliases
    // keep card/broadcast/timer refs unchanged.)
    buildPomodoroLogPanel(ctx);
    auto& pomo_log_col      = ctx.pomo_log_col;
    auto& rebuild_pomo_log  = ctx.rebuild_pomo_log;
    auto& pomo_log_hdr_row  = ctx.pomo_log_hdr_row;

    // ── App usage heatmap section ── (carved into buildAppHeatmapPanel; bar-lbl +
    // hr-lbl + heat_hdr_row escape — the header label + Reset button stay
    // builder-local. Aliases keep card/broadcast/OnConnect refs unchanged.)
    buildAppHeatmapPanel(ctx);
    auto& heat_bar_lbl = ctx.heat_bar_lbl;
    auto& heat_hr_lbl  = ctx.heat_hr_lbl;
    auto& heat_hdr_row = ctx.heat_hdr_row;

    // ── Screenshot & Recording ── (carved into buildScreenshotPanel; header/status/
    // last-path/open-btn/recording/toggle/rec-status + btn-row + last-row escape —
    // the Full/Delay buttons stay builder-local. Aliases keep card/broadcast/reply
    // refs unchanged.)
    buildScreenshotPanel(ctx);
    auto& ss_hdr_lbl     = ctx.ss_hdr_lbl;
    auto& ss_status_lbl  = ctx.ss_status_lbl;
    auto& ss_last_path   = ctx.ss_last_path;
    auto& ss_open_btn    = ctx.ss_open_btn;
    auto& ss_recording   = ctx.ss_recording;
    auto& rec_toggle_btn = ctx.rec_toggle_btn;
    auto& rec_status_lbl = ctx.rec_status_lbl;
    auto& ss_btn_row     = ctx.ss_btn_row;
    auto& ss_last_row    = ctx.ss_last_row;

    // ── Storage quick-view ── (carved into buildStoragePanel; header/col +
    // rebuild_storage escape — the local PoolFetcher + fmtBytes/fmtBar helpers stay
    // in the rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildStoragePanel(ctx);
    auto& stor_hdr_lbl    = ctx.stor_hdr_lbl;
    auto& stor_col        = ctx.stor_col;
    auto& rebuild_storage = ctx.rebuild_storage;

    // ── Disk I/O monitor ── (carved into buildDiskIoPanel; header/col + refresh-btn
    // + rebuild_diskio escape — the local DioFetcher stays in the rebuild. Aliases
    // keep card/broadcast/timer refs unchanged.)
    buildDiskIoPanel(ctx);
    auto& diskio_hdr_lbl     = ctx.diskio_hdr_lbl;
    auto& diskio_col         = ctx.diskio_col;
    auto& diskio_refresh_btn = ctx.diskio_refresh_btn;
    auto& rebuild_diskio     = ctx.rebuild_diskio;

    // ── SMART Disk Health ── (carved into buildSmartHealthPanel; header/col +
    // refresh-btn + rebuild_smart escape — the local SmartFetcher stays in the
    // rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildSmartHealthPanel(ctx);
    auto& smart_hdr_lbl     = ctx.smart_hdr_lbl;
    auto& smart_col         = ctx.smart_col;
    auto& smart_refresh_btn = ctx.smart_refresh_btn;
    auto& rebuild_smart     = ctx.rebuild_smart;

    // ── Disk Usage ── (carved into buildDiskUsagePanel; header/status/col +
    // scan-btn + rebuild_disk_usage escape — the local DuFetcher stays in the
    // rebuild. Aliases keep card/broadcast/timer refs unchanged.)
    buildDiskUsagePanel(ctx);
    auto& du_hdr_lbl         = ctx.du_hdr_lbl;
    auto& du_status_lbl      = ctx.du_status_lbl;
    auto& du_col             = ctx.du_col;
    auto& du_scan_btn        = ctx.du_scan_btn;
    auto& rebuild_disk_usage = ctx.rebuild_disk_usage;

    // ── Archive ── (carved into buildArchivePanel; header/status + extract/create
    // rows escape — the form inputs + buttons stay builder-local, held by the rows.
    // Aliases keep card/broadcast/reply/timer refs unchanged.)
    buildArchivePanel(ctx);
    auto& arch_hdr_lbl     = ctx.arch_hdr_lbl;
    auto& arch_status_lbl  = ctx.arch_status_lbl;
    auto& arch_extract_row = ctx.arch_extract_row;
    auto& arch_create_row  = ctx.arch_create_row;

    // ── Print Queue ── (carved into buildPrintSpoolerPanel; header/col + refresh-
    // btn + rebuild_print escape — the local PrintFetcher stays in the rebuild.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildPrintSpoolerPanel(ctx);
    auto& print_hdr_lbl_b   = ctx.print_hdr_lbl_b;
    auto& print_col_b       = ctx.print_col_b;
    auto& print_refresh_btn = ctx.print_refresh_btn;
    auto& rebuild_print     = ctx.rebuild_print;

    // ── USB Device Monitor ── (carved into buildUsbMonitorPanel; LATENT — only
    // rebuild_usb_b escapes (broadcast/timer). The header/col/refresh-btn stay
    // builder-local. Alias keeps the broadcast/timer refs unchanged.)
    buildUsbMonitorPanel(ctx);
    auto& rebuild_usb_b = ctx.rebuild_usb_b;

    // ── Bookmarks ── (carved into buildBookmarksPanel; header/count/col/clear-btn
    // + rebuild_bm escape — the local BmFetcher stays in the rebuild. Aliases keep
    // card/broadcast/timer refs unchanged.)
    buildBookmarksPanel(ctx);
    auto& bm_hdr_lbl   = ctx.bm_hdr_lbl;
    auto& bm_count_lbl = ctx.bm_count_lbl;
    auto& bm_col       = ctx.bm_col;
    auto& bm_clear_btn = ctx.bm_clear_btn;
    auto& rebuild_bm   = ctx.rebuild_bm;

    // ── Active Transfers ── (carved into buildActiveTransfersPanel; header/count/
    // col + rebuild_xfr escape — the XfrFetcher struct + xfrf stay builder-local,
    // kept alive by the rebuild closure. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildActiveTransfersPanel(ctx);
    auto& xfr_hdr_lbl   = ctx.xfr_hdr_lbl;
    auto& xfr_count_lbl = ctx.xfr_count_lbl;
    auto& xfr_col       = ctx.xfr_col;
    auto& rebuild_xfr   = ctx.rebuild_xfr;

    // ── Game Controllers ── (carved into buildGameControllersPanel; header/count/
    // col + rebuild_gc escape — the GcFetcher struct + gcf stay builder-local, kept
    // alive by the rebuild closure. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildGameControllersPanel(ctx);
    auto& gc_hdr_lbl   = ctx.gc_hdr_lbl;
    auto& gc_count_lbl = ctx.gc_count_lbl;
    auto& gc_col       = ctx.gc_col;
    auto& rebuild_gc   = ctx.rebuild_gc;

    // ── Saved Wi-Fi Networks ── (carved into buildSavedWifiPanel; header/count/col
    // + rebuild_swifi escape — the SwifiFetcher struct + swf stay builder-local,
    // kept alive by the rebuild closure. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildSavedWifiPanel(ctx);
    auto& swifi_hdr_lbl   = ctx.swifi_hdr_lbl;
    auto& swifi_count_lbl = ctx.swifi_count_lbl;
    auto& swifi_col       = ctx.swifi_col;
    auto& rebuild_swifi   = ctx.rebuild_swifi;

    // ── Security Keys ── (carved into buildSecurityKeysPanel; header/count/col +
    // rebuild_seckey + status-lbl + register-btn escape — the SeckeyFetcher struct
    // + skf stay builder-local, kept alive by the rebuild closure. Aliases keep
    // card/broadcast/timer refs unchanged.)
    buildSecurityKeysPanel(ctx);
    auto& seckey_hdr_lbl      = ctx.seckey_hdr_lbl;
    auto& seckey_count_lbl    = ctx.seckey_count_lbl;
    auto& seckey_col          = ctx.seckey_col;
    auto& rebuild_seckey      = ctx.rebuild_seckey;
    auto& seckey_status_lbl   = ctx.seckey_status_lbl;
    auto& seckey_register_btn = ctx.seckey_register_btn;

    // ── Text Replacement ── (carved into buildTextReplacementPanel; header/count/
    // col + rebuild_txtrep + add-form widgets escape — the TxtrepFetcher struct +
    // trf stay builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildTextReplacementPanel(ctx);
    auto& txtrep_hdr_lbl        = ctx.txtrep_hdr_lbl;
    auto& txtrep_count_lbl      = ctx.txtrep_count_lbl;
    auto& txtrep_col            = ctx.txtrep_col;
    auto& rebuild_txtrep        = ctx.rebuild_txtrep;
    auto& txtrep_shortcut_input = ctx.txtrep_shortcut_input;
    auto& txtrep_phrase_input   = ctx.txtrep_phrase_input;
    auto& txtrep_add_status_lbl = ctx.txtrep_add_status_lbl;
    auto& txtrep_add_btn        = ctx.txtrep_add_btn;

    // ── App Time Limits ── (carved into buildAppTimeLimitsPanel; header/count/col
    // + rebuild_applimit + add-form widgets escape — the AppLimitFetcher struct +
    // alf stay builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildAppTimeLimitsPanel(ctx);
    auto& applimit_hdr_lbl        = ctx.applimit_hdr_lbl;
    auto& applimit_count_lbl      = ctx.applimit_count_lbl;
    auto& applimit_col            = ctx.applimit_col;
    auto& rebuild_applimit        = ctx.rebuild_applimit;
    auto& applimit_name_input     = ctx.applimit_name_input;
    auto& applimit_min_input      = ctx.applimit_min_input;
    auto& applimit_add_status_lbl = ctx.applimit_add_status_lbl;
    auto& applimit_add_btn        = ctx.applimit_add_btn;

    // ── Notification Sounds ── (carved into buildNotifSoundsPanel; header/count/
    // col + rebuild_notifsnd escape — the NotifSoundFetcher struct + nsf stay
    // builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildNotifSoundsPanel(ctx);
    auto& notifsnd_hdr_lbl   = ctx.notifsnd_hdr_lbl;
    auto& notifsnd_count_lbl = ctx.notifsnd_count_lbl;
    auto& notifsnd_col       = ctx.notifsnd_col;
    auto& rebuild_notifsnd   = ctx.rebuild_notifsnd;

    // ── Login Items ── (carved into buildLoginItemsPanel; header/count/col +
    // rebuild_litem escape — the LoginItemFetcher struct + lif stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildLoginItemsPanel(ctx);
    auto& litem_hdr_lbl   = ctx.litem_hdr_lbl;
    auto& litem_count_lbl = ctx.litem_count_lbl;
    auto& litem_col       = ctx.litem_col;
    auto& rebuild_litem   = ctx.rebuild_litem;

    // ── Trusted Devices ── (carved into buildTrustedDevicesPanel; header/count/col
    // + rebuild_tdev escape — the TrustedDevFetcher struct + tdf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildTrustedDevicesPanel(ctx);
    auto& tdev_hdr_lbl   = ctx.tdev_hdr_lbl;
    auto& tdev_count_lbl = ctx.tdev_count_lbl;
    auto& tdev_col       = ctx.tdev_col;
    auto& rebuild_tdev   = ctx.rebuild_tdev;

    // ── Keyboard Shortcuts ── (carved into buildKeyboardShortcutsPanel; header/
    // count/col + rebuild_kbsc escape — the KeyShortcutFetcher struct + ksf stay
    // builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildKeyboardShortcutsPanel(ctx);
    auto& kbsc_hdr_lbl   = ctx.kbsc_hdr_lbl;
    auto& kbsc_count_lbl = ctx.kbsc_count_lbl;
    auto& kbsc_col       = ctx.kbsc_col;
    auto& rebuild_kbsc   = ctx.rebuild_kbsc;

    // ── Default Apps ── (carved into buildDefaultAppsPanel; header/count/col +
    // rebuild_dapp escape — the DefaultAppFetcher struct + daf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildDefaultAppsPanel(ctx);
    auto& dapp_hdr_lbl   = ctx.dapp_hdr_lbl;
    auto& dapp_count_lbl = ctx.dapp_count_lbl;
    auto& dapp_col       = ctx.dapp_col;
    auto& rebuild_dapp   = ctx.rebuild_dapp;

    // ── Reminders ── (carved into buildRemindersPanel; header/count/col +
    // rebuild_rem escape — the RemFetcher struct + remf stay builder-local. Aliases
    // keep card/broadcast/timer refs unchanged.)
    buildRemindersPanel(ctx);
    auto& rem_hdr_lbl   = ctx.rem_hdr_lbl;
    auto& rem_count_lbl = ctx.rem_count_lbl;
    auto& rem_col       = ctx.rem_col;
    auto& rebuild_rem   = ctx.rebuild_rem;

    // ── Scheduled Backups ── (carved into buildScheduledBackupsPanel; header/count/
    // col + rebuild_bkp escape — the BkpFetcher struct + bkpf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildScheduledBackupsPanel(ctx);
    auto& bkp_hdr_lbl   = ctx.bkp_hdr_lbl;
    auto& bkp_count_lbl = ctx.bkp_count_lbl;
    auto& bkp_col       = ctx.bkp_col;
    auto& rebuild_bkp   = ctx.rebuild_bkp;

    // ── TOTP Authenticator ── (carved into buildTotpPanel; header/count/col +
    // rebuild_totp escape — the TotpFetcher struct + tpf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildTotpPanel(ctx);
    auto& totp_hdr_lbl   = ctx.totp_hdr_lbl;
    auto& totp_count_lbl = ctx.totp_count_lbl;
    auto& totp_col       = ctx.totp_col;
    auto& rebuild_totp   = ctx.rebuild_totp;

    // ── Bluetooth Audio ── (carved into buildBtAudioPanel; header/count/col +
    // rebuild_btaudio escape — the BtAudioFetcher struct + baf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildBtAudioPanel(ctx);
    auto& btaudio_hdr_lbl   = ctx.btaudio_hdr_lbl;
    auto& btaudio_count_lbl = ctx.btaudio_count_lbl;
    auto& btaudio_col       = ctx.btaudio_col;
    auto& rebuild_btaudio   = ctx.rebuild_btaudio;

    // ── App Sandbox ── (carved into buildAppSandboxPanel; header/count/col +
    // rebuild_sandbox escape — the SandboxFetcher struct + sbf stay builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildAppSandboxPanel(ctx);
    auto& sandbox_hdr_lbl   = ctx.sandbox_hdr_lbl;
    auto& sandbox_count_lbl = ctx.sandbox_count_lbl;
    auto& sandbox_col       = ctx.sandbox_col;
    auto& rebuild_sandbox   = ctx.rebuild_sandbox;

    // ── USB Devices ── (carved into buildUsbDeviceDetailPanel; header/count/col/
    // status + rebuild_usbdev escape — the UsbDevFetcher struct + udf stay
    // builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildUsbDeviceDetailPanel(ctx);
    auto& usbdev_hdr_lbl    = ctx.usbdev_hdr_lbl;
    auto& usbdev_count_lbl  = ctx.usbdev_count_lbl;
    auto& usbdev_col        = ctx.usbdev_col;
    auto& usbdev_status_lbl = ctx.usbdev_status_lbl;
    auto& rebuild_usbdev    = ctx.rebuild_usbdev;

    // ── Network Shares ── (carved into buildNetworkSharesMountPanel; header/count/
    // col/form-col/status/toggle-btn + rebuild_ns escape — the NsFetcher + all
    // mount-form widgets stay builder-local (held by ns_form_col; nspf kept alive
    // by rebuild_ns). Aliases keep card/broadcast/timer refs unchanged.)
    buildNetworkSharesMountPanel(ctx);
    auto& nsm_hdr_lbl   = ctx.nsm_hdr_lbl;
    auto& nsm_count_lbl = ctx.nsm_count_lbl;
    auto& ns_col        = ctx.ns_col;
    auto& ns_form_col   = ctx.ns_form_col;
    auto& ns_status_lbl = ctx.ns_status_lbl;
    auto& ns_toggle_btn = ctx.ns_toggle_btn;
    auto& rebuild_ns    = ctx.rebuild_ns;

    // ── System Log ── (carved into buildSystemLogPanel; header/col/level-mask/
    // filter-buttons + rebuild_slog escape — the SlogFetcher + highlight closure
    // stay builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildSystemLogPanel(ctx);
    auto& slog_hdr_lbl     = ctx.slog_hdr_lbl;
    auto& slog_col         = ctx.slog_col;
    auto& slog_level_mask  = ctx.slog_level_mask;
    auto& slog_filter_btns = ctx.slog_filter_btns;
    auto& rebuild_slog     = ctx.rebuild_slog;

    // ── Remote Desktop ── (carved into buildRemoteDesktopPanel; header/count/col/
    // form-col/status/toggle-btn + rebuild_rdp escape — the connect-form widgets
    // stay builder-local (held by rdp_form_col). Aliases keep card/broadcast/reply/
    // timer refs unchanged.)
    buildRemoteDesktopPanel(ctx);
    auto& rdp_hdr_lbl    = ctx.rdp_hdr_lbl;
    auto& rdp_count_lbl  = ctx.rdp_count_lbl;
    auto& rdp_col        = ctx.rdp_col;
    auto& rdp_form_col   = ctx.rdp_form_col;
    auto& rdp_status_lbl = ctx.rdp_status_lbl;
    auto& rdp_toggle_btn = ctx.rdp_toggle_btn;
    auto& rebuild_rdp    = ctx.rebuild_rdp;

    // ── Input Devices ── (carved into buildInputDevicesPanel; header/count/col +
    // rebuild_indev escape — the IndevFetcher stays builder-local. Aliases keep
    // card/broadcast/timer refs unchanged.)
    buildInputDevicesPanel(ctx);
    auto& indev_hdr_lbl   = ctx.indev_hdr_lbl;
    auto& indev_count_lbl = ctx.indev_count_lbl;
    auto& indev_col       = ctx.indev_col;
    auto& rebuild_indev   = ctx.rebuild_indev;

    // ── Clipboard History ── (carved into buildClipboardHistoryPanel; header/count/
    // col/clear-btn + rebuild_clip escape — the ClipFetcher stays builder-local.
    // Aliases keep card/broadcast/timer refs unchanged.)
    buildClipboardHistoryPanel(ctx);
    auto& clip_hdr_lbl   = ctx.clip_hdr_lbl;
    auto& clip_count_lbl = ctx.clip_count_lbl;
    auto& clip_col       = ctx.clip_col;
    auto& clip_clear_btn = ctx.clip_clear_btn;
    auto& rebuild_clip   = ctx.rebuild_clip;

    // ── Screenshot Gallery ── (carved into buildScreenshotGalleryPanel; header/
    // count/col/capture-btn + rebuild_scap escape — the ScapFetcher stays
    // builder-local. Aliases keep card/broadcast/timer refs unchanged.)
    buildScreenshotGalleryPanel(ctx);
    auto& scap_hdr_lbl   = ctx.scap_hdr_lbl;
    auto& scap_count_lbl = ctx.scap_count_lbl;
    auto& scap_col       = ctx.scap_col;
    auto& scap_btn       = ctx.scap_btn;
    auto& rebuild_scap   = ctx.rebuild_scap;

    // ── Upcoming Events ── (carved into buildUpcomingEventsPanel; header/count/col/
    // range-index/range-buttons + rebuild_uev escape — the UevFetcher + highlight
    // closure stay builder-local. Aliases keep card/broadcast/timer refs
    // unchanged.)
    buildUpcomingEventsPanel(ctx);
    auto& uev_hdr_lbl    = ctx.uev_hdr_lbl;
    auto& uev_count_lbl  = ctx.uev_count_lbl;
    auto& uev_col        = ctx.uev_col;
    auto& uev_range_cur  = ctx.uev_range_cur;
    auto& uev_range_btns = ctx.uev_range_btns;
    auto& rebuild_uev    = ctx.rebuild_uev;

    // ── Recent Files Panel ── (carved into buildRecentFilesPanel; header/col +
    // rebuild_rf escape. Aliases keep card/broadcast refs unchanged.)
    buildRecentFilesPanel(ctx);
    auto& rf_hdr_lbl = ctx.rf_hdr_lbl;
    auto& rf_col     = ctx.rf_col;
    auto& rebuild_rf = ctx.rebuild_rf;

    // ── Named Clipboard Slots ── (carved into buildClipboardSlotsPanel; header/col
    // + preview-label array escape — the per-slot Save/Copy buttons stay
    // builder-local (held by cs_col rows). Aliases keep card/broadcast refs
    // unchanged.)
    buildClipboardSlotsPanel(ctx);
    auto& cs_hdr_lbl      = ctx.cs_hdr_lbl;
    auto& cs_col          = ctx.cs_col;
    auto& cs_preview_lbls = ctx.cs_preview_lbls;

    // ── AC stub sections (App Volume / Screen Time / DND-priority) ── (carved into
    // buildAcStubSections; hdr/col + empty rebuild_X escape — the rebuild bodies
    // are wired later in OnConnect via these aliases.)
    buildAcStubSections(ctx);
    auto& audio_vol_hdr_lbl   = ctx.audio_vol_hdr_lbl;
    auto& audio_vol_col       = ctx.audio_vol_col;
    auto& rebuild_audio_vol   = ctx.rebuild_audio_vol;
    auto& screen_time_hdr_lbl = ctx.screen_time_hdr_lbl;
    auto& screen_time_col     = ctx.screen_time_col;
    auto& rebuild_screen_time = ctx.rebuild_screen_time;
    auto& dnd_exc_hdr_lbl     = ctx.dnd_exc_hdr_lbl;
    auto& dnd_exc_col         = ctx.dnd_exc_col;
    auto& rebuild_dnd_exc     = ctx.rebuild_dnd_exc;

    auto action_center = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        notif_header_row,
        std::make_shared<Spacer>(1, 16),
        notif_empty_lbl,
        notif_scroll,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        notes_mini_hdr_row,
        std::make_shared<Spacer>(1, 6),
        notes_mini_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        clip_ac_hdr_row,
        std::make_shared<Spacer>(1, 6),
        clip_ac_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        contact_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        contact_search_input,
        std::make_shared<Spacer>(1, 6),
        contact_results_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        idle_lock_btn,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        app_notif_hdr,
        std::make_shared<Spacer>(1, 8),
        app_notif_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            store_hdr_lbl, std::make_shared<Spacer>(1, 1, true),
            store_status_lbl, std::make_shared<Spacer>(8, 1), store_update_btn}),
        std::make_shared<Spacer>(1, 6),
        store_search_row,
        std::make_shared<Spacer>(1, 6),
        store_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            iapps_hdr_lbl, std::make_shared<Spacer>(8, 1), iapps_count_lbl}),
        std::make_shared<Spacer>(1, 4),
        iapps_prog_lbl,
        std::make_shared<Spacer>(1, 6),
        iapps_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            fonts_hdr_lbl, std::make_shared<Spacer>(8, 1),
            fonts_count_lbl, std::make_shared<Spacer>(1, 1, true),
            fonts_refresh_btn}),
        std::make_shared<Spacer>(1, 6),
        fonts_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        startup_apps_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        startup_apps_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        dnd_exc_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        dnd_exc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        screen_time_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        screen_time_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        win_rules_hdr_row,
        std::make_shared<Spacer>(1, 6),
        win_rules_col,
        std::make_shared<Spacer>(1, 6),
        win_rule_add_row,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        hotkey_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        hotkey_rows_col,
        std::make_shared<Spacer>(1, 4),
        hotkey_status_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        scratch_hdr_row,
        std::make_shared<Spacer>(1, 6),
        scratch_input,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        spark_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        spark_cpu_lbl,
        std::make_shared<Spacer>(1, 4),
        spark_ram_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        dl_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        dl_clear_btn,
        std::make_shared<Spacer>(1, 6),
        dl_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            pkg_hdr_lbl, std::make_shared<Spacer>(8, 1), pkg_status_lbl}),
        std::make_shared<Spacer>(1, 6),
        pkg_btn_row,
        std::make_shared<Spacer>(1, 6),
        pkg_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        therm_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        therm_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        conn_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        conn_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        conn_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            lic_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            lic_tier_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        lic_key_lbl,
        std::make_shared<Spacer>(1, 4),
        lic_support_lbl,
        std::make_shared<Spacer>(1, 4),
        lic_expiry_lbl,
        std::make_shared<Spacer>(1, 4),
        lic_seats_lbl,
        std::make_shared<Spacer>(1, 8),
        lic_copy_btn,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            perm_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            perm_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        perm_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            im_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            im_active_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        im_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        proc_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        proc_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        proc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Energy Impact
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            energy_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            energy_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        energy_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            mem_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            mem_level_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        mem_bar_row,
        std::make_shared<Spacer>(1, 4),
        mem_ram_lbl,
        std::make_shared<Spacer>(1, 2),
        mem_swap_lbl,
        std::make_shared<Spacer>(1, 2),
        mem_faults_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        ft_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            ft_avg_lbl,
            std::make_shared<Spacer>(12, 1),
            ft_p99_lbl,
            std::make_shared<Spacer>(12, 1),
            ft_jank_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            ft_gpu_lbl,
            std::make_shared<Spacer>(12, 1),
            ft_vsync_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        ft_bar_row,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // File Sync
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            fs_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            fs_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        fs_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Audio Routing
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            ar_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            ar_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        audio_route_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Network Shares
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            ns_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            ns_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        share_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            pkg_upd_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            pkg_upd_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            pkg_upd_check_btn,
            std::make_shared<Spacer>(8, 1),
            pkg_upd_all_btn,
        }),
        std::make_shared<Spacer>(1, 6),
        pkg_upd_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            zfs_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            zfs_count_lbl,
        }),
        std::make_shared<Spacer>(1, 8),
        zfs_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            jail_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            jail_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        jail_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        jail_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            be_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            be_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        be_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        be_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            kmod_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            kmod_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        kmod_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        kmod_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Virtual Machines
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            vm_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            vm_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        vm_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Containers
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            ct_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            ct_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        ct_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            cap_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            cap_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        cap_screenshot_btn,
        std::make_shared<Spacer>(1, 6),
        cap_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // System Information
        si_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        si_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        crash_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        crash_clear_btn,
        std::make_shared<Spacer>(1, 6),
        crash_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        sec_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        sec_btn_row,
        std::make_shared<Spacer>(1, 6),
        sec_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            kc_hdr_lbl, std::make_shared<Spacer>(1, 1, true),
            kc_count_lbl, std::make_shared<Spacer>(8, 1), kc_lock_btn}),
        std::make_shared<Spacer>(1, 6),
        kc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        svc_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        svc_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        svc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        lsess_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        lsess_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        lsess_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        bdrain_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        bdrain_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        bdrain_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        cron_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        cron_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        cron_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        startup_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        startup_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        startup_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        netconn_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        netconn_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        netconn_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        vol_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        vol_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── SSH Keys ─────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            sshkey_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            sshkey_count_lbl,
        }),
        std::make_shared<Spacer>(1, 4),
        sshkey_known_lbl,
        std::make_shared<Spacer>(1, 8),
        sshkey_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Environment Variables ─────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            envvar_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            envvar_count_lbl,
        }),
        std::make_shared<Spacer>(1, 8),
        envvar_form_row,
        std::make_shared<Spacer>(1, 8),
        envvar_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        syslog_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        syslog_filter_row,
        std::make_shared<Spacer>(1, 6),
        syslog_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        syslog_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── USB Devices ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            usb_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            usb_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        usb_rescan_btn,
        std::make_shared<Spacer>(1, 6),
        usb_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        forecast_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        forecast_row,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        cal_add_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        cal_add_row,
        std::make_shared<Spacer>(1, 4),
        cal_add_status,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        timer_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        timer_label_input,
        std::make_shared<Spacer>(1, 4),
        timer_input,
        std::make_shared<Spacer>(1, 6),
        timer_btn_row,
        std::make_shared<Spacer>(1, 6),
        timer_cd_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        pomo_log_hdr_row,
        std::make_shared<Spacer>(1, 6),
        pomo_log_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        heat_hdr_row,
        std::make_shared<Spacer>(1, 6),
        heat_bar_lbl,
        std::make_shared<Spacer>(1, 2),
        heat_hr_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        ss_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        ss_btn_row,
        std::make_shared<Spacer>(1, 6),
        ss_last_row,
        std::make_shared<Spacer>(1, 8),
        rec_toggle_btn,
        std::make_shared<Spacer>(1, 4),
        rec_status_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        stor_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        stor_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        diskio_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        diskio_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        diskio_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        smart_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        smart_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        smart_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        du_hdr_lbl,
        std::make_shared<Spacer>(1, 4),
        du_status_lbl,
        std::make_shared<Spacer>(1, 4),
        du_scan_btn,
        std::make_shared<Spacer>(1, 6),
        du_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Archive ──────────────────────────────────────────────────────────
        arch_hdr_lbl,
        std::make_shared<Spacer>(1, 4),
        arch_status_lbl,
        std::make_shared<Spacer>(1, 8),
        arch_extract_row,
        std::make_shared<Spacer>(1, 6),
        arch_create_row,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // Print spooler (com.freebsdx.Print / PrintSpoolerOpcode — backed by print_service.cc)
        print_hdr_lbl_b,
        std::make_shared<Spacer>(1, 6),
        print_refresh_btn,
        std::make_shared<Spacer>(1, 6),
        print_col_b,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            bm_hdr_lbl, std::make_shared<Spacer>(8, 1),
            bm_count_lbl, std::make_shared<Spacer>(1, 1, true),
            bm_clear_btn}),
        std::make_shared<Spacer>(1, 6),
        bm_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Active Transfers ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            xfr_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            xfr_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        xfr_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Game Controllers ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            gc_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            gc_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        gc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Saved Wi-Fi Networks ────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            swifi_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            swifi_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        swifi_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Security Keys ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            seckey_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            seckey_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        seckey_col,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            seckey_register_btn,
            std::make_shared<Spacer>(8, 1),
            seckey_status_lbl,
        }),
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Text Replacement ────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            txtrep_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            txtrep_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        txtrep_col,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            txtrep_shortcut_input,
            std::make_shared<Spacer>(6, 1),
            txtrep_phrase_input,
            std::make_shared<Spacer>(6, 1),
            txtrep_add_btn,
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            txtrep_add_status_lbl,
        }),
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── App Time Limits ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            applimit_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            applimit_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        applimit_col,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            applimit_name_input,
            std::make_shared<Spacer>(6, 1),
            applimit_min_input,
            std::make_shared<Spacer>(6, 1),
            applimit_add_btn,
        }),
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<Spacer>(1, 1, true),
            applimit_add_status_lbl,
        }),
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Notification Sounds ─────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            notifsnd_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            notifsnd_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        notifsnd_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Login Items ─────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            litem_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            litem_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        litem_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Trusted Devices ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            tdev_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            tdev_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        tdev_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Keyboard Shortcuts ──────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            kbsc_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            kbsc_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        kbsc_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Default Apps ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            dapp_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            dapp_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        dapp_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Reminders ────────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            rem_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            rem_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        rem_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Scheduled Backups ─────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            bkp_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            bkp_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        bkp_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── TOTP Authenticator ────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            totp_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            totp_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        totp_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Bluetooth Audio ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            btaudio_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            btaudio_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        btaudio_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── App Sandbox ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            sandbox_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            sandbox_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        sandbox_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── USB Devices ──────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            usbdev_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            usbdev_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        usbdev_col,
        usbdev_status_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Network Shares ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            nsm_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            nsm_count_lbl,
            std::make_shared<Spacer>(1, 1, true),
            ns_toggle_btn,
        }),
        std::make_shared<Spacer>(1, 6),
        ns_col,
        ns_form_col,
        ns_status_lbl,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── System Log ───────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            slog_hdr_lbl,
            std::make_shared<Spacer>(1, 1, true),
            (*slog_filter_btns)[0], std::make_shared<Spacer>(4, 1),
            (*slog_filter_btns)[1], std::make_shared<Spacer>(4, 1),
            (*slog_filter_btns)[2], std::make_shared<Spacer>(4, 1),
            (*slog_filter_btns)[3],
        }),
        std::make_shared<Spacer>(1, 6),
        slog_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Remote Desktop ───────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            rdp_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            rdp_count_lbl,
            std::make_shared<Spacer>(1, 1, true),
            rdp_toggle_btn,
        }),
        std::make_shared<Spacer>(1, 6),
        rdp_col,
        rdp_form_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Input Devices ────────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            indev_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            indev_count_lbl,
        }),
        std::make_shared<Spacer>(1, 6),
        indev_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Clipboard History ────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            clip_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            clip_count_lbl,
            std::make_shared<Spacer>(1, 1, true),
            clip_clear_btn,
        }),
        std::make_shared<Spacer>(1, 6),
        clip_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Screenshot Gallery ───────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            scap_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            scap_count_lbl,
            std::make_shared<Spacer>(1, 1, true),
            scap_btn,
        }),
        std::make_shared<Spacer>(1, 6),
        scap_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        // ── Upcoming Events ──────────────────────────────────────────────────
        std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            uev_hdr_lbl,
            std::make_shared<Spacer>(8, 1),
            uev_count_lbl,
            std::make_shared<Spacer>(1, 1, true),
            (*uev_range_btns)[0],
            std::make_shared<Spacer>(4, 1),
            (*uev_range_btns)[1],
            std::make_shared<Spacer>(4, 1),
            (*uev_range_btns)[2],
        }),
        std::make_shared<Spacer>(1, 6),
        uev_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        rf_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        rf_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        cs_hdr_lbl,
        std::make_shared<Spacer>(1, 6),
        cs_col,
        std::make_shared<Spacer>(1, 12),
        std::make_shared<DividerWidget>(0.25f),
        std::make_shared<Spacer>(1, 10),
        audio_vol_hdr_lbl,
        std::make_shared<Spacer>(1, 8),
        audio_vol_col
    });
    action_center->setBackgroundColor({0.07f, 0.07f, 0.12f, 0.94f});
    action_center->setPadding(20);
    action_center->setFixedSize(320, 1052);  // 1080 − 2×14 inset so it floats top+bottom
    action_center->setStackAnchor(StackAnchor::TopRight);  // anchor to the right edge
    action_center->setMarginAll(14.0f);
    action_center->setVisibility(false);

    // Wire notification bell button now that action_center exists
    *notif_tbar_btn = ButtonWidget(notif_tbar_btn->child(),
        [action_center, notif_badge_box, notif_unread]() {
            bool opening = !action_center->isVisible();
            action_center->setVisibility(opening);
            if (opening) {
                *notif_unread = 0;
                notif_badge_box->setVisibility(false);
            }
        });
    notif_tbar_btn->setPadding(6);
    notif_tbar_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f}); // reassignment reset styling

    // Window switcher overlay (Alt+Tab) — 7 pre-allocated chips
    constexpr int kMaxSwitcherWindows = 7;
    struct SwitcherChip {
        uint32_t surface_id{0};
        std::shared_ptr<TextWidget>  label;
        std::shared_ptr<FlexColumn> card;
    };
    auto switcher_chips = std::make_shared<std::array<SwitcherChip, kMaxSwitcherWindows>>();
    std::vector<std::shared_ptr<Widget>> chip_widgets;
    chip_widgets.reserve(kMaxSwitcherWindows * 2 - 1);
    for (int i = 0; i < kMaxSwitcherWindows; ++i) {
        auto lbl = std::make_shared<TextWidget>("", 13.0f);
        lbl->setColor({1.0f, 1.0f, 1.0f, 1.0f});
        lbl->setAlignment(Alignment::Center);
        auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{lbl});
        card->setBackgroundColor({0.2f, 0.2f, 0.28f, 0.95f});
        card->setPadding(14);
        card->setFixedSize(130, 50);
        card->setVisibility(false);
        (*switcher_chips)[i].label = lbl;
        (*switcher_chips)[i].card  = card;
        chip_widgets.push_back(card);
        if (i < kMaxSwitcherWindows - 1)
            chip_widgets.push_back(std::make_shared<Spacer>(8, 1));
    }
    auto switcher_row   = std::make_shared<FlexRow>(std::move(chip_widgets));
    auto switcher_panel = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{switcher_row});
    switcher_panel->setBackgroundColor({0.07f, 0.07f, 0.1f, 0.93f});
    switcher_panel->setPadding(18);
    switcher_panel->setVisibility(false);

    // ── Virtual Keyboard Panel ───────────────────────────────────────────────

    // XBus send helper — fires a VirtualKeyInput to the compositor
    auto vkb_send = [](uint32_t keycode, uint32_t mods) {
        if (!g_shell_xbus) return;
        VirtualKeyInput vk{};
        vk.keycode_   = keycode;
        vk.modifiers_ = mods;
        g_shell_xbus->Call("com.freebsdx.VirtualKeyboard",
            static_cast<uint32_t>(VirtualKeyboardOpcode::InjectKey),
            {reinterpret_cast<const uint8_t*>(&vk), sizeof(vk)}, nullptr);
    };

    // Container that rows are added into; rebuilt on every Shift/Caps/Sym toggle
    auto vkb_rows_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    // rebuild_vkb: shared_ptr<function> so Caps/Shift buttons can re-invoke it
    auto rebuild_vkb = std::make_shared<std::function<void()>>();
    *rebuild_vkb = [vkb_rows_col, vkb_send, rebuild_vkb, &vkb_anchor, &vkb_panel]() mutable {
        vkb_rows_col->clearChildren();
        bool upper = vkb_shift || vkb_caps;
        auto sp = [](int w=4){ return std::make_shared<Spacer>(w,1); };

        // ── Row 1: numbers + Backspace ───────────────────────────────────────
        struct NK { const char* lo; const char* hi; uint32_t kc; };
        static const NK num_keys[] = {
            {"`","~",K_GRAVE},{"1","!",K_1},{"2","@",K_2},{"3","#",K_3},{"4","$",K_4},
            {"5","%",K_5},{"6","^",K_6},{"7","&",K_7},{"8","*",K_8},{"9","(",K_9},
            {"0",")",K_0},{"-","_",K_MINUS},{"=","+",K_EQUAL}
        };
        std::vector<std::shared_ptr<Widget>> row1;
        for (const auto& k : num_keys) {
            std::string lbl = (vkb_sym || upper) ? k.hi : k.lo;
            uint32_t mods = (lbl == std::string(k.lo)) ? 0u : MOD_SHIFT;
            auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
                std::make_shared<TextWidget>(lbl, 14.0f)
            });
            card->setBackgroundColor({0.22f, 0.23f, 0.3f, 1.0f});
            card->setAlignment(Alignment::Center);
            card->setFixedSize(68, 50);
            row1.push_back(std::make_shared<ButtonWidget>(card,
                [vkb_send, kc=k.kc, mods](){ vkb_send(kc, mods); }));
            row1.push_back(sp());
        }
        // Backspace
        auto bs_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x8c\xab", 16.0f)  // ⌫
        });
        bs_card->setBackgroundColor({0.28f, 0.18f, 0.18f, 1.0f});
        bs_card->setAlignment(Alignment::Center);
        bs_card->setFixedSize(90, 50);
        row1.push_back(std::make_shared<ButtonWidget>(bs_card,
            [vkb_send](){ vkb_send(K_BACKSPACE, 0); }));
        vkb_rows_col->addChild(std::make_shared<FlexRow>(row1));
        vkb_rows_col->addChild(std::make_shared<Spacer>(1, 4));

        // ── Row 2: QWERTY row ────────────────────────────────────────────────
        static const std::pair<const char*, uint32_t> qrow[] = {
            {"q",K_Q},{"w",K_W},{"e",K_E},{"r",K_R},{"t",K_T},
            {"y",K_Y},{"u",K_U},{"i",K_I},{"o",K_O},{"p",K_P},
            {"[",K_LBRACK},{"]",K_RBRACK},{"\\",K_BACKSLASH}
        };
        std::vector<std::shared_ptr<Widget>> row2;
        for (const auto& [ch, kc] : qrow) {
            std::string lbl;
            uint32_t mods2 = 0u;
            if (ch[0] >= 'a' && ch[0] <= 'z') {
                lbl = upper ? std::string(1, static_cast<char>(std::toupper(ch[0]))) : ch;
                mods2 = upper ? MOD_SHIFT : 0u;
            } else { lbl = ch; }
            auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
                std::make_shared<TextWidget>(lbl, 14.0f)
            });
            card->setBackgroundColor({0.22f, 0.23f, 0.3f, 1.0f});
            card->setAlignment(Alignment::Center);
            card->setFixedSize(70, 50);
            row2.push_back(std::make_shared<ButtonWidget>(card,
                [vkb_send, kc=kc, mods2](){ vkb_send(kc, mods2); }));
            row2.push_back(sp());
        }
        vkb_rows_col->addChild(std::make_shared<FlexRow>(row2));
        vkb_rows_col->addChild(std::make_shared<Spacer>(1, 4));

        // ── Row 3: ASDF + Caps Lock + Enter ──────────────────────────────────
        static const std::pair<const char*, uint32_t> arow[] = {
            {"a",K_A},{"s",K_S},{"d",K_D},{"f",K_F},{"g",K_G},
            {"h",K_H},{"j",K_J},{"k",K_K},{"l",K_L},{";",K_SEMI},{"'",K_APOS}
        };
        std::vector<std::shared_ptr<Widget>> row3;
        // Caps Lock
        auto caps_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>(vkb_caps ? "\xe2\x87\xaaON" : "\xe2\x87\xaa", 13.0f)  // ⇪
        });
        caps_card->setBackgroundColor(vkb_caps
            ? Color{0.3f, 0.5f, 0.3f, 1.0f}
            : Color{0.28f, 0.28f, 0.35f, 1.0f});
        caps_card->setAlignment(Alignment::Center);
        caps_card->setFixedSize(90, 50);
        auto rebuild_ref = rebuild_vkb;
        row3.push_back(std::make_shared<ButtonWidget>(caps_card, [rebuild_ref]() {
            vkb_caps = !vkb_caps;
            if (rebuild_ref && *rebuild_ref) (*rebuild_ref)();
        }));
        row3.push_back(sp());
        for (const auto& [ch, kc] : arow) {
            std::string lbl;
            uint32_t mods2 = 0u;
            if (ch[0] >= 'a' && ch[0] <= 'z') {
                lbl = upper ? std::string(1, static_cast<char>(std::toupper(ch[0]))) : ch;
                mods2 = upper ? MOD_SHIFT : 0u;
            } else { lbl = ch; }
            auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
                std::make_shared<TextWidget>(lbl, 14.0f)
            });
            card->setBackgroundColor({0.22f, 0.23f, 0.3f, 1.0f});
            card->setAlignment(Alignment::Center);
            card->setFixedSize(70, 50);
            row3.push_back(std::make_shared<ButtonWidget>(card,
                [vkb_send, kc=kc, mods2](){ vkb_send(kc, mods2); }));
            row3.push_back(sp());
        }
        // Enter
        auto enter_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x86\xb5", 16.0f)  // ↵
        });
        enter_card->setBackgroundColor({0.2f, 0.3f, 0.2f, 1.0f});
        enter_card->setAlignment(Alignment::Center);
        enter_card->setFixedSize(100, 50);
        row3.push_back(std::make_shared<ButtonWidget>(enter_card,
            [vkb_send](){ vkb_send(K_ENTER, 0); }));
        vkb_rows_col->addChild(std::make_shared<FlexRow>(row3));
        vkb_rows_col->addChild(std::make_shared<Spacer>(1, 4));

        // ── Row 4: ZXCV + Shift ──────────────────────────────────────────────
        static const std::pair<const char*, uint32_t> zrow[] = {
            {"z",K_Z},{"x",K_X},{"c",K_C},{"v",K_V},{"b",K_B},
            {"n",K_N},{"m",K_M},{",",K_COMMA},{".",K_DOT},{"/",K_SLASH}
        };
        std::vector<std::shared_ptr<Widget>> row4;
        // Left Shift
        auto lshift_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>(vkb_shift ? "\xe2\x87\xa7ON" : "\xe2\x87\xa7", 14.0f)  // ⇧
        });
        lshift_card->setBackgroundColor(vkb_shift
            ? Color{0.3f, 0.3f, 0.55f, 1.0f}
            : Color{0.28f, 0.28f, 0.35f, 1.0f});
        lshift_card->setAlignment(Alignment::Center);
        lshift_card->setFixedSize(100, 50);
        row4.push_back(std::make_shared<ButtonWidget>(lshift_card, [rebuild_ref]() {
            vkb_shift = !vkb_shift;
            if (rebuild_ref && *rebuild_ref) (*rebuild_ref)();
        }));
        row4.push_back(sp());
        for (const auto& [ch, kc] : zrow) {
            std::string lbl;
            uint32_t mods2 = 0u;
            if (ch[0] >= 'a' && ch[0] <= 'z') {
                lbl = upper ? std::string(1, static_cast<char>(std::toupper(ch[0]))) : ch;
                mods2 = upper ? MOD_SHIFT : 0u;
            } else { lbl = ch; }
            auto card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
                std::make_shared<TextWidget>(lbl, 14.0f)
            });
            card->setBackgroundColor({0.22f, 0.23f, 0.3f, 1.0f});
            card->setAlignment(Alignment::Center);
            card->setFixedSize(70, 50);
            row4.push_back(std::make_shared<ButtonWidget>(card,
                [vkb_send, kc=kc, mods2](){ vkb_send(kc, mods2); }));
            row4.push_back(sp());
        }
        vkb_rows_col->addChild(std::make_shared<FlexRow>(row4));
        vkb_rows_col->addChild(std::make_shared<Spacer>(1, 4));

        // ── Row 5: Sym toggle, Space, Hide ───────────────────────────────────
        auto sym_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>(vkb_sym ? "ABC" : "!@#", 13.0f)
        });
        sym_card->setBackgroundColor(vkb_sym
            ? Color{0.4f, 0.3f, 0.1f, 1.0f}
            : Color{0.28f, 0.28f, 0.35f, 1.0f});
        sym_card->setAlignment(Alignment::Center);
        sym_card->setFixedSize(90, 50);

        auto space_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("SPACE", 13.0f)
        });
        space_card->setBackgroundColor({0.22f, 0.23f, 0.3f, 1.0f});
        space_card->setAlignment(Alignment::Center);
        space_card->setFixedSize(520, 50);

        auto hide_card = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
            std::make_shared<TextWidget>("\xe2\x8c\xa8\xe2\x96\xbe", 13.0f)  // ⌨▾
        });
        hide_card->setBackgroundColor({0.28f, 0.28f, 0.35f, 1.0f});
        hide_card->setAlignment(Alignment::Center);
        hide_card->setFixedSize(80, 50);

        std::vector<std::shared_ptr<Widget>> row5;
        row5.push_back(std::make_shared<ButtonWidget>(sym_card, [rebuild_ref]() {
            vkb_sym = !vkb_sym;
            if (rebuild_ref && *rebuild_ref) (*rebuild_ref)();
        }));
        row5.push_back(sp(8));
        row5.push_back(std::make_shared<ButtonWidget>(space_card,
            [vkb_send](){ vkb_send(K_SPACE, 0); }));
        row5.push_back(sp(8));
        // Hide button: dismiss the on-screen keyboard (anchor + panel are
        // captured by reference; assigned below where the panel is built).
        row5.push_back(std::make_shared<ButtonWidget>(hide_card, [&vkb_anchor, &vkb_panel]() {
            if (vkb_anchor) vkb_anchor->setVisibility(false);
            if (vkb_panel)  vkb_panel->setVisibility(false);
        }));
        vkb_rows_col->addChild(std::make_shared<FlexRow>(row5));
    };

    // Populate initial layout
    (*rebuild_vkb)();

    // Build the keyboard panel (fills the forward-declared vkb_panel used by the
    // taskbar ⌨ toggle button created near the top of the shell).
    vkb_panel = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        vkb_rows_col
    });
    vkb_panel->setBackgroundColor({0.12f, 0.13f, 0.18f, 0.97f});
    vkb_panel->setPadding(10);
    vkb_panel->setFixedSize(1100, 270);
    vkb_panel->setVisibility(false);

    // Bottom-anchoring container: flexible spacer pushes keyboard to the bottom,
    // then a fixed spacer clears the taskbar (48px).
    vkb_anchor = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(1, 1, true),
        vkb_panel,
        std::make_shared<Spacer>(1, 48)
    });
    vkb_anchor->setFixedSize(1100, 10000);
    vkb_anchor->setVisibility(false);
    // vkb_btn (the taskbar ⌨ toggle) is built near the top of the shell and
    // toggles vkb_anchor/vkb_panel by reference now that they're assigned.

    // ── File Chooser Overlay ─────────────────────────────────────────────────
    // Presented on behalf of sandboxed apps when they request OpenFile/SaveFile.
    auto fc_request_id   = std::make_shared<uint32_t>(0);
    auto fc_current_path = std::make_shared<std::string>("");
    auto fc_selected     = std::make_shared<std::string>("");
    auto fc_filter_mime  = std::make_shared<std::string>("");
    auto fc_save_mode    = std::make_shared<bool>(false);

    auto fc_title_w = std::make_shared<TextWidget>("Open File", 17.0f);
    fc_title_w->setColor({1.0f, 1.0f, 1.0f, 1.0f});

    auto fc_path_w = std::make_shared<TextWidget>("", 11.0f);
    fc_path_w->setColor({0.65f, 0.70f, 0.80f, 1.0f});

    auto fc_list_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});

    auto fc_sel_w = std::make_shared<TextWidget>("", 12.0f);
    fc_sel_w->setColor({0.55f, 0.85f, 0.55f, 1.0f});

    auto fc_save_field = std::make_shared<TextInputWidget>("");

    // rebuild_fc — populates fc_list_col from a directory path.
    // Uses std::filesystem directly (xshell is a trusted, non-sandboxed process).
    auto rebuild_fc = std::make_shared<std::function<void(const std::string&)>>();
    *rebuild_fc = [fc_list_col, fc_current_path, fc_path_w, fc_filter_mime,
                   fc_selected, fc_sel_w,
                   rebuild_fc_weak = std::weak_ptr<std::function<void(const std::string&)>>(rebuild_fc)](
                       const std::string& path) {
        namespace fs = std::filesystem;
        *fc_current_path = path;

        // Shorten $HOME prefix for display
        std::string display = path;
        const char* home = std::getenv("HOME");
        if (home && display.starts_with(home))
            display = "~" + display.substr(std::strlen(home));
        fc_path_w->setText(display);

        fc_list_col->clearChildren();
        *fc_selected = "";
        fc_sel_w->setText("");

        // Collect directory entries — dirs first, then files
        std::vector<std::pair<std::string, bool>> entries; // {name, is_dir}
        try {
            for (const auto& e : fs::directory_iterator(
                    path, fs::directory_options::skip_permission_denied)) {
                std::string name = e.path().filename().string();
                if (name.empty() || name[0] == '.') continue; // skip hidden
                entries.emplace_back(name, e.is_directory());
            }
        } catch (...) {}
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second; // dirs first
            return a.first < b.first;
        });

        const std::string& filter = *fc_filter_mime;
        // Simple category map for MIME-prefix filtering ("image/*", "video/*", …)
        static const std::pair<const char*, const char*> kExtMime[] = {
            {"jpg","image/"},{"jpeg","image/"},{"png","image/"},{"gif","image/"},
            {"webp","image/"},{"bmp","image/"},{"svg","image/"},{"ico","image/"},
            {"mp4","video/"},{"mkv","video/"},{"avi","video/"},{"mov","video/"},{"webm","video/"},
            {"mp3","audio/"},{"flac","audio/"},{"ogg","audio/"},{"wav","audio/"},{"aac","audio/"},
            {"txt","text/"},{"md","text/"},{"html","text/"},{"css","text/"},{"js","text/"},
            {"json","text/"},{"xml","text/"},{"csv","text/"},{"sh","text/"},
            {"pdf","application/"},{"zip","application/"},{"tar","application/"},
            {"gz","application/"},{"xpkg","application/"},
            {nullptr, nullptr}
        };

        for (const auto& [name, is_dir] : entries) {
            if (!is_dir && !filter.empty() && filter != "*/*") {
                // Apply MIME category filter (e.g. "image/*")
                auto dot = name.rfind('.');
                std::string ext = (dot != std::string::npos) ? name.substr(dot + 1) : "";
                for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool pass = false;
                // Wildcard category: "image/*"
                if (filter.size() > 2 && filter.back() == '*') {
                    std::string cat = filter.substr(0, filter.size() - 1); // "image/"
                    for (auto* p = kExtMime; p->first; ++p) {
                        if (ext == p->first && std::string(p->second).starts_with(cat)) {
                            pass = true; break;
                        }
                    }
                } else {
                    pass = true; // non-wildcard: show all
                }
                if (!pass) continue;
            }

            std::string icon = is_dir ? "📁" : "📄";
            std::string label = is_dir ? (name + "/") : name;
            std::string full_path = path + "/" + name;

            std::shared_ptr<Widget> entry_row;
            if (is_dir) {
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    std::make_shared<TextWidget>(icon, 13.0f),
                    std::make_shared<Spacer>(6, 1),
                    [&]{ auto w = std::make_shared<TextWidget>(label, 12.0f);
                         w->setColor({0.9f, 0.9f, 0.95f, 1.0f}); return w; }()
                });
                std::string fp = full_path;
                auto rf_w = rebuild_fc_weak;
                entry_row = std::make_shared<ButtonWidget>(row, [fp, rf_w]() {
                    if (auto rf = rf_w.lock()) (*rf)(fp);
                });
            } else {
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    std::make_shared<TextWidget>(icon, 13.0f),
                    std::make_shared<Spacer>(6, 1),
                    [&]{ auto w = std::make_shared<TextWidget>(label, 12.0f);
                         w->setColor({0.9f, 0.9f, 0.95f, 1.0f}); return w; }()
                });
                std::string fp = full_path;
                auto sel = fc_selected;
                auto sel_w = fc_sel_w;
                entry_row = std::make_shared<ButtonWidget>(row, [fp, sel, sel_w]() {
                    *sel = fp;
                    auto f = std::filesystem::path(fp).filename().string();
                    sel_w->setText(f);
                });
            }
            entry_row->setPadding(3);
            fc_list_col->addChild(entry_row);
        }
    };

    // Back button — navigate to parent directory
    auto fc_back_btn = std::make_shared<ButtonWidget>("← Back",
        [fc_current_path, rebuild_fc]() {
            if (!rebuild_fc || !*rebuild_fc) return;
            namespace fs = std::filesystem;
            fs::path p(*fc_current_path);
            if (p.has_parent_path() && p != p.root_path())
                (*rebuild_fc)(p.parent_path().string());
        });
    fc_back_btn->setPadding(6);

    // ── Clipboard History Overlay ─────────────────────────────────────────────
    // Toggled by Super+V (InputEventType::ClipboardHistoryToggle).
    // Shows last 30 clipboard entries; click-to-repaste sets clipboard + dismisses.

    auto ch_entries = std::make_shared<std::vector<std::pair<uint32_t, std::string>>>(); // {id, text}
    auto ch_list_col_ptr = std::make_shared<std::shared_ptr<Widget>>(nullptr);

    auto ch_panel_col = std::make_shared<FlexColumn>();
    ch_panel_col->setGap(0);

    // Header
    auto ch_hdr = std::make_shared<FlexRow>();
    ch_hdr->setPadding(14);
    ch_hdr->setGap(8);

    auto ch_title = std::make_shared<TextWidget>("Clipboard History", 15.0f);
    ch_title->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    ch_title->setFlexGrow(1.0f);
    ch_hdr->addChild(ch_title);

    // "Clear All" button placeholder — wired after xbus is available
    auto ch_clear_btn = std::make_shared<ButtonWidget>("Clear");
    ch_clear_btn->setFixedSize(52, 26);
    ch_clear_btn->setFontSize(12.0f);
    ch_clear_btn->setBackgroundColor({0.35f, 0.13f, 0.13f, 1.0f});
    ch_hdr->addChild(ch_clear_btn);

    ch_panel_col->addChild(ch_hdr);

    auto ch_divider = std::make_shared<DividerWidget>();
    ch_panel_col->addChild(ch_divider);

    // Transform toolbar — operates on the last-tapped clipboard entry
    auto ch_selected_text = std::make_shared<std::string>();
    auto ch_xform_status  = std::make_shared<TextWidget>("", 11.0f);
    ch_xform_status->setColor({0.55f, 0.85f, 0.55f, 1.0f});

    auto make_xform_btn = [&](const char* label,
                              std::function<std::string(std::string)> fn) {
        auto btn = std::make_shared<ButtonWidget>(label,
            [ch_selected_text, ch_xform_status, fn = std::move(fn)]() {
                if (ch_selected_text->empty()) {
                    ch_xform_status->setText("Tap an entry first");
                    return;
                }
                std::string out = fn(*ch_selected_text);
                if (!g_shell_xbus) return;
                char buf[256] = {};
                std::strncpy(buf, out.c_str(), sizeof(buf) - 1);
                g_shell_xbus->Call("com.freebsdx.Clipboard", 0x01,
                    {reinterpret_cast<const uint8_t*>(buf), sizeof(buf)},
                    [ch_xform_status](const XBusMessage&) {
                        ch_xform_status->setText("✓ Copied");
                    });
            });
        btn->setFixedSize(62, 26);
        btn->setFontSize(11.0f);
        btn->setBackgroundColor({0.18f, 0.20f, 0.30f, 1.0f});
        return btn;
    };

    auto ch_upper_btn = make_xform_btn("UPPER", [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s; });
    auto ch_lower_btn = make_xform_btn("lower", [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; });
    auto ch_title_btn = make_xform_btn("Title", [](std::string s) {
        bool cap = true;
        for (char& c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) { cap = true; }
            else { c = cap ? static_cast<char>(::toupper(c))
                           : static_cast<char>(::tolower(c)); cap = false; }
        }
        return s;
    });
    auto ch_trim_btn = make_xform_btn("Trim", [](std::string s) {
        auto l = s.find_first_not_of(" \t\n\r");
        auto r = s.find_last_not_of(" \t\n\r");
        return (l == std::string::npos) ? std::string{} : s.substr(l, r - l + 1);
    });

    auto ch_xform_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        ch_upper_btn, std::make_shared<Spacer>(4, 1),
        ch_lower_btn, std::make_shared<Spacer>(4, 1),
        ch_title_btn, std::make_shared<Spacer>(4, 1),
        ch_trim_btn,  std::make_shared<Spacer>(8, 1),
        ch_xform_status
    });
    ch_xform_row->setPadding(8);
    ch_xform_row->setBackgroundColor({0.11f, 0.12f, 0.18f, 1.0f});
    ch_panel_col->addChild(ch_xform_row);
    ch_panel_col->addChild(std::make_shared<DividerWidget>());

    auto ch_list_col = std::make_shared<FlexColumn>();
    ch_list_col->setGap(1);
    *ch_list_col_ptr = ch_list_col;

    auto ch_scroll = std::make_shared<ScrollViewWidget>(ch_list_col);
    ch_scroll->setFlexGrow(1.0f);
    ch_panel_col->addChild(ch_scroll);

    auto ch_overlay = std::make_shared<ContainerWidget>(ch_panel_col);
    ch_overlay->setBackgroundColor({0.13f, 0.14f, 0.20f, 0.97f});
    ch_overlay->setFixedSize(400, 520);
    ch_overlay->setVisibility(false);

    // ── Color Picker overlay (Super+P) ───────────────────────────────────────
    // HSV state shared between slider callbacks and the Run lambda
    auto cp_h = std::make_shared<float>(200.0f);  // hue   0-360
    auto cp_s = std::make_shared<float>(0.70f);   // sat   0-1
    auto cp_v = std::make_shared<float>(0.85f);   // value 0-1
    auto cp_recent = std::make_shared<std::vector<std::array<float,3>>>();

    // Forward-declare update lambda so slider callbacks can call it
    auto update_cp = std::make_shared<std::function<void()>>();

    // Swatch preview
    auto cp_swatch = std::make_shared<FlexRow>();
    cp_swatch->setFixedSize(286, 64);
    cp_swatch->setBackgroundColor({0.20f, 0.67f, 0.85f, 1.0f}); // initial ~H200 S70 V85

    // Hex / RGB text labels
    auto cp_hex_lbl = std::make_shared<TextWidget>("#33AADA", 18.0f);
    cp_hex_lbl->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    cp_hex_lbl->setFontWeight(600); // SemiBold

    auto cp_rgb_lbl = std::make_shared<TextWidget>("rgb(51, 170, 218)", 11.0f);
    cp_rgb_lbl->setColor(0.65f, 0.65f, 0.65f, 1.0f);

    // Slider value labels
    auto cp_h_lbl = std::make_shared<TextWidget>("H 200", 11.0f);
    cp_h_lbl->setColor(0.65f, 0.65f, 0.65f, 1.0f);
    cp_h_lbl->setFixedSize(40, 16);

    auto cp_s_lbl = std::make_shared<TextWidget>("S  70%", 11.0f);
    cp_s_lbl->setColor(0.65f, 0.65f, 0.65f, 1.0f);
    cp_s_lbl->setFixedSize(40, 16);

    auto cp_v_lbl = std::make_shared<TextWidget>("V  85%", 11.0f);
    cp_v_lbl->setColor(0.65f, 0.65f, 0.65f, 1.0f);
    cp_v_lbl->setFixedSize(40, 16);

    // HSV sliders — callbacks forward to update_cp once it's populated in Run
    auto cp_h_slider = std::make_shared<SliderWidget>(200.0f, 0.0f, 360.0f,
        [update_cp, cp_h](float v) { *cp_h = v; if (*update_cp) (*update_cp)(); });
    cp_h_slider->setFixedSize(0, 20);

    auto cp_s_slider = std::make_shared<SliderWidget>(0.70f, 0.0f, 1.0f,
        [update_cp, cp_s](float v) { *cp_s = v; if (*update_cp) (*update_cp)(); });
    cp_s_slider->setFixedSize(0, 20);

    auto cp_v_slider = std::make_shared<SliderWidget>(0.85f, 0.0f, 1.0f,
        [update_cp, cp_v](float v) { *cp_v = v; if (*update_cp) (*update_cp)(); });
    cp_v_slider->setFixedSize(0, 20);

    // Recent colors row (up to 8 swatches)
    auto cp_recent_row = std::make_shared<FlexRow>();
    cp_recent_row->setGap(5);
    cp_recent_row->setFixedSize(0, 28);

    // Copy buttons (callbacks set inside Run lambda where xbus is available)
    auto cp_copy_hex_btn = std::make_shared<ButtonWidget>("Copy HEX", nullptr);
    cp_copy_hex_btn->setFixedSize(0, 28);
    cp_copy_hex_btn->setBackgroundColor({0.18f, 0.42f, 0.80f, 1.0f});

    auto cp_copy_rgb_btn = std::make_shared<ButtonWidget>("Copy RGB", nullptr);
    cp_copy_rgb_btn->setFixedSize(0, 28);
    cp_copy_rgb_btn->setBackgroundColor({0.22f, 0.22f, 0.28f, 1.0f});

    // Assemble rows
    auto mk_slider_row = [](std::shared_ptr<Widget> label,
                             std::shared_ptr<Widget> slider) {
        auto row = std::make_shared<FlexRow>();
        row->setCrossAxisAlignment(CrossAxisAlignment::Center);
        row->setGap(8);
        row->addChild(label);
        auto wrap = std::make_shared<FlexRow>();
        wrap->setFlexGrow(1.0f);
        wrap->addChild(slider);
        row->addChild(wrap);
        return row;
    };

    auto cp_copy_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        cp_copy_hex_btn, std::make_shared<Spacer>(8, 1), cp_copy_rgb_btn
    });

    auto cp_panel_col = std::make_shared<FlexColumn>();
    cp_panel_col->setGap(8);
    cp_panel_col->setPadding(12);
    {
        auto title = std::make_shared<TextWidget>("🎨  Color Picker", 14.0f);
        title->setFontWeight(600); // SemiBold
        title->setColor(1.0f, 1.0f, 1.0f, 1.0f);
        cp_panel_col->addChild(title);
    }
    cp_panel_col->addChild(cp_swatch);
    cp_panel_col->addChild(cp_hex_lbl);
    cp_panel_col->addChild(cp_rgb_lbl);
    cp_panel_col->addChild(std::make_shared<DividerWidget>());
    cp_panel_col->addChild(mk_slider_row(cp_h_lbl, cp_h_slider));
    cp_panel_col->addChild(mk_slider_row(cp_s_lbl, cp_s_slider));
    cp_panel_col->addChild(mk_slider_row(cp_v_lbl, cp_v_slider));
    cp_panel_col->addChild(std::make_shared<DividerWidget>());
    cp_panel_col->addChild(cp_recent_row);
    cp_panel_col->addChild(cp_copy_row);

    auto cp_overlay = std::make_shared<ContainerWidget>(cp_panel_col);
    cp_overlay->setBackgroundColor({0.12f, 0.13f, 0.19f, 0.97f});
    cp_overlay->setFixedSize(320, 400);
    cp_overlay->setVisibility(false);

    // OK / Cancel buttons
    auto fc_overlay_ptr = std::make_shared<std::shared_ptr<Widget>>(nullptr);
    // When ≥ 0, the OK button sets a per-workspace wallpaper instead of responding to FileChooser
    auto fc_wp_ws_idx = std::make_shared<int>(-1);
    auto fc_ok_btn = std::make_shared<ButtonWidget>("Open",
        [fc_overlay_ptr, fc_request_id, fc_selected, fc_save_mode, fc_current_path, fc_save_field,
         fc_wp_ws_idx]() {
            std::string path;
            if (*fc_save_mode) {
                std::string fname = fc_save_field->getText();
                if (fname.empty()) return;
                path = *fc_current_path + "/" + fname;
            } else {
                path = *fc_selected;
                if (path.empty()) return;
            }
            if (!g_shell_xbus) return;

            // Wallpaper-picker mode: set per-workspace wallpaper directly
            if (*fc_wp_ws_idx >= 0) {
                WallpaperWorkspaceRequest req{};
                req.workspace_idx_ = static_cast<uint32_t>(*fc_wp_ws_idx);
                req.fit_mode_      = 0u; // Cover
                std::strncpy(req.path_, path.c_str(), sizeof(req.path_) - 1);
                g_shell_xbus->Call("com.freebsdx.Wallpaper",
                    static_cast<uint32_t>(WallpaperOpcode::SetWorkspaceWallpaper),
                    {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                *fc_wp_ws_idx = -1;
                if (*fc_overlay_ptr) (*fc_overlay_ptr)->setVisibility(false);
                return;
            }

            FileChooserResponse resp{};
            resp.opcode_      = static_cast<uint32_t>(FileChooserOpcode::Respond);
            resp.request_id_  = *fc_request_id;
            resp.cancelled_   = 0;
            resp.count_       = 1;
            std::strncpy(resp.path_, path.c_str(), sizeof(resp.path_) - 1);

            g_shell_xbus->Call("com.freebsdx.FileChooser",
                static_cast<uint32_t>(FileChooserOpcode::Respond),
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)},
                [](const XBusMessage&) {});

            if (*fc_overlay_ptr) (*fc_overlay_ptr)->setVisibility(false);
        });
    fc_ok_btn->setPadding(8);
    fc_ok_btn->setBackgroundColor({0.22f, 0.45f, 0.88f, 1.0f});

    auto fc_cancel_btn = std::make_shared<ButtonWidget>("Cancel",
        [fc_overlay_ptr, fc_request_id]() {
            if (!g_shell_xbus) return;
            FileChooserResponse resp{};
            resp.opcode_      = static_cast<uint32_t>(FileChooserOpcode::Respond);
            resp.request_id_  = *fc_request_id;
            resp.cancelled_   = 1;
            g_shell_xbus->Call("com.freebsdx.FileChooser",
                static_cast<uint32_t>(FileChooserOpcode::Respond),
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)},
                [](const XBusMessage&) {});
            if (*fc_overlay_ptr) (*fc_overlay_ptr)->setVisibility(false);
        });
    fc_cancel_btn->setPadding(8);

    auto fc_list_scroll = std::make_shared<ScrollViewWidget>(fc_list_col);
    fc_list_scroll->setFixedSize(560, 340);

    auto fc_nav_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        fc_back_btn, std::make_shared<Spacer>(10, 1), fc_path_w
    });
    fc_nav_row->setPadding(8);

    auto fc_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(1, 1, true),
        fc_cancel_btn, std::make_shared<Spacer>(10, 1), fc_ok_btn
    });
    fc_btn_row->setPadding(8);

    auto fc_title_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        fc_title_w, std::make_shared<Spacer>(1, 1, true)
    });
    fc_title_row->setPadding(12);

    auto fc_panel_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        fc_title_row,
        std::make_shared<DividerWidget>(),
        fc_nav_row,
        std::make_shared<DividerWidget>(),
        fc_list_scroll,
        std::make_shared<DividerWidget>(),
        fc_save_field,
        fc_sel_w,
        fc_btn_row
    });
    fc_panel_col->setPadding(4);

    auto fc_overlay = std::make_shared<ContainerWidget>(fc_panel_col);
    fc_overlay->setBackgroundColor({0.13f, 0.14f, 0.20f, 0.97f});
    fc_overlay->setFixedSize(600, 520);
    fc_overlay->setVisibility(false);

    // Store back-pointer so ok/cancel buttons can hide the panel
    *fc_overlay_ptr = fc_overlay;

    // fc_save_field only visible in save mode; sel_w only in open mode
    fc_save_field->setVisibility(false);

    // ── Calendar popover (clock click) ──────────────────────────────────────
    // Mini month-view calendar with today highlighted and today's events listed.
    // Navigation: < prev month | "Month Year" | next month >
    // Clicking a day cell selects it and filters the event list to that day.

    // Helpers for calendar math
    auto cal_days_in_month = [](int y, int m) -> int {
        std::tm t{}; t.tm_year = y - 1900; t.tm_mon = m; // day 0 of next month
        t.tm_mday = 0; t.tm_isdst = -1;
        std::mktime(&t);
        return t.tm_mday;
    };
    auto cal_first_weekday = [](int y, int m) -> int { // 0=Sun
        std::tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1;
        t.tm_mday = 1; t.tm_isdst = -1;
        std::mktime(&t);
        return t.tm_wday;
    };

    static const char* kDowHdr[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    static const char* kMonthNames[] = {
        "","January","February","March","April","May","June",
        "July","August","September","October","November","December"
    };

    // State
    auto cal_year      = std::make_shared<int>(0);
    auto cal_month     = std::make_shared<int>(0);  // 1-based
    auto cal_sel_day   = std::make_shared<int>(0);  // selected day (0 = none)
    auto cal_events    = std::make_shared<std::vector<std::pair<uint8_t,std::string>>>(); // {day, label}

    // Widgets
    auto cal_title_w   = std::make_shared<TextWidget>("", 14.0f);
    cal_title_w->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    auto cal_grid      = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto cal_events_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    auto cal_events_hdr = std::make_shared<TextWidget>("Events", 11.0f);
    cal_events_hdr->setColor({0.60f, 0.65f, 0.80f, 1.0f});

    // Forward-declare rebuild so day buttons can call it for selected-day event filter
    auto rebuild_cal_events = std::make_shared<std::function<void()>>();

    auto rebuild_cal_grid = std::make_shared<std::function<void()>>();
    *rebuild_cal_grid = [=, &cal_days_in_month, &cal_first_weekday]() {
        int y = *cal_year, m = *cal_month;
        int today_y = 0, today_m = 0, today_d = 0;
        {
            std::time_t now = std::time(nullptr);
            std::tm* lt = std::localtime(&now);
            if (lt) { today_y = lt->tm_year+1900; today_m = lt->tm_mon+1; today_d = lt->tm_mday; }
        }

        cal_title_w->setText(std::string(kMonthNames[m]) + " " + std::to_string(y));

        // DOW header row
        auto dow_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
        for (const char* d : kDowHdr) {
            auto lbl = std::make_shared<TextWidget>(d, 10.0f);
            lbl->setColor({0.55f, 0.60f, 0.70f, 1.0f});
            lbl->setFixedSize(38, 18);
            lbl->setAlignment(Alignment::Center);
            dow_row->addChild(lbl);
        }

        cal_grid->clearChildren();
        cal_grid->addChild(dow_row);

        int first_wd = cal_first_weekday(y, m);
        int days     = cal_days_in_month(y, m);
        int cell = 0;
        auto cur_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});

        for (int pad = 0; pad < first_wd; ++pad) {
            cur_row->addChild(std::make_shared<ContainerWidget>(38.0f, 32.0f));
            ++cell;
        }

        for (int d = 1; d <= days; ++d) {
            bool is_today = (y == today_y && m == today_m && d == today_d);
            bool is_sel   = (d == *cal_sel_day);
            auto day_lbl  = std::make_shared<TextWidget>(std::to_string(d), 12.0f);
            day_lbl->setAlignment(Alignment::Center);
            if (is_today)
                day_lbl->setColor({0.18f, 0.48f, 1.0f, 1.0f});
            else
                day_lbl->setColor({0.85f, 0.85f, 0.85f, 1.0f});

            auto day_cell = std::make_shared<ContainerWidget>(day_lbl);
            day_cell->setFixedSize(38, 32);
            day_cell->setAlignment(Alignment::Center);
            if (is_today || is_sel)
                day_cell->setBackgroundColor({0.14f, 0.22f, 0.45f, 0.90f});
            else
                day_cell->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});

            int captured_d = d;
            cur_row->addChild(std::make_shared<ButtonWidget>(day_cell,
                [cal_sel_day, rebuild_cal_events, captured_d, rebuild_cal_grid]() {
                    *cal_sel_day = captured_d;
                    if (rebuild_cal_events && *rebuild_cal_events)
                        (*rebuild_cal_events)();
                    if (rebuild_cal_grid && *rebuild_cal_grid)
                        (*rebuild_cal_grid)();
                }));
            ++cell;

            if (cell % 7 == 0) {
                cal_grid->addChild(cur_row);
                cur_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{});
            }
        }
        if (cell % 7 != 0) cal_grid->addChild(cur_row);
    };

    *rebuild_cal_events = [=]() {
        cal_events_col->clearChildren();
        int sel = *cal_sel_day;
        if (sel == 0) {
            // default: today
            std::time_t now = std::time(nullptr);
            std::tm* lt = std::localtime(&now);
            if (lt) sel = lt->tm_mday;
        }
        bool any = false;
        for (const auto& [day, label] : *cal_events) {
            if (day != sel) continue;
            auto ev_lbl = std::make_shared<TextWidget>(label, 11.0f);
            ev_lbl->setColor({0.80f, 0.80f, 0.90f, 1.0f});
            cal_events_col->addChild(ev_lbl);
            any = true;
        }
        if (!any) {
            auto no_ev = std::make_shared<TextWidget>("No events", 11.0f);
            no_ev->setColor({0.45f, 0.45f, 0.50f, 1.0f});
            cal_events_col->addChild(no_ev);
        }
        cal_events_hdr->setText(std::format("Events — {:02d}/{}", sel, *cal_month));
    };

    // Load events for current display month from CalendarService
    auto load_cal_month = std::make_shared<std::function<void(uint32_t)>>();
    *load_cal_month = [cal_events, rebuild_cal_events, cal_year, cal_month, load_cal_month](uint32_t page) {
        if (!g_shell_xbus) return;
        CalendarListRequest req{};
        req.year_  = static_cast<uint16_t>(*cal_year);
        req.month_ = static_cast<uint8_t>(*cal_month);
        req.page_  = page;
        g_shell_xbus->Call("com.freebsdx.Calendar",
            static_cast<uint32_t>(CalendarOpcode::ListEvents),
            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
            [cal_events, rebuild_cal_events, cal_year, cal_month, load_cal_month, page]
            (const XBusMessage& r) {
                CalendarEventPage ep{};
                std::memcpy(&ep, r.payload_, sizeof(ep));
                if (ep.valid_) {
                    char buf[80];
                    if (ep.all_day_)
                        std::snprintf(buf, sizeof(buf), "● %s", ep.title_);
                    else
                        std::snprintf(buf, sizeof(buf), "%02d:%02d  %s",
                                      ep.hour_, ep.minute_, ep.title_);
                    cal_events->push_back({ep.day_, std::string(buf)});
                    if (page + 1 < ep.total_)
                        (*load_cal_month)(page + 1);
                    else if (rebuild_cal_events && *rebuild_cal_events)
                        (*rebuild_cal_events)();
                } else {
                    if (rebuild_cal_events && *rebuild_cal_events)
                        (*rebuild_cal_events)();
                }
            });
    };

    // Navigation button helpers
    auto go_to_month = [=](int y, int m) {
        if (m < 1)  { --y; m = 12; }
        if (m > 12) { ++y; m = 1;  }
        *cal_year = y; *cal_month = m; *cal_sel_day = 0;
        cal_events->clear();
        (*rebuild_cal_grid)();
        (*load_cal_month)(0);
    };

    auto cal_prev_btn = std::make_shared<ButtonWidget>("‹", [=]() {
        go_to_month(*cal_year, *cal_month - 1);
    });
    auto cal_next_btn = std::make_shared<ButtonWidget>("›", [=]() {
        go_to_month(*cal_year, *cal_month + 1);
    });
    cal_prev_btn->setFontSize(18.0f); cal_prev_btn->setPadding(6);
    cal_next_btn->setFontSize(18.0f); cal_next_btn->setPadding(6);

    auto cal_header_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        cal_prev_btn,
        std::make_shared<Spacer>(1, 1, true),
        cal_title_w,
        std::make_shared<Spacer>(1, 1, true),
        cal_next_btn
    });
    cal_header_row->setPadding(8);

    auto cal_events_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        cal_events_hdr
    });
    cal_events_hdr_row->setPadding(6);

    auto cal_panel_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        cal_header_row,
        std::make_shared<DividerWidget>(),
        cal_grid,
        std::make_shared<Spacer>(1, 8),
        std::make_shared<DividerWidget>(),
        cal_events_hdr_row,
        cal_events_col
    });
    cal_panel_col->setPadding(6);

    auto cal_popover = std::make_shared<ContainerWidget>(cal_panel_col);
    cal_popover->setBackgroundColor({0.10f, 0.11f, 0.18f, 0.97f});
    cal_popover->setFixedSize(290, 420);
    cal_popover->setVisibility(false);

    // Wire clock_btn to toggle the popover, initialising month on first open
    *clock_btn = ButtonWidget(clock_btn->child(), [cal_popover, go_to_month]() {
        bool opening = !cal_popover->isVisible();
        cal_popover->setVisibility(opening);
        if (opening) {
            std::time_t now = std::time(nullptr);
            std::tm* lt = std::localtime(&now);
            if (lt) go_to_month(lt->tm_year + 1900, lt->tm_mon + 1);
        }
    });
    clock_btn->setPadding(6);
    clock_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f}); // reassignment reset styling

    // ── Keyboard shortcut help overlay (Super+/) ─────────────────────────────
    // Static table of all system shortcuts; toggled by ShortcutHelpToggle broadcast.
    auto make_sc_row = [](const std::string& key, const std::string& desc) {
        auto key_lbl = std::make_shared<TextWidget>(key, 12.0f);
        key_lbl->setColor({0.95f, 0.85f, 0.40f, 1.0f}); // amber for keys
        key_lbl->setFixedSize(180, 18);
        auto desc_lbl = std::make_shared<TextWidget>(desc, 12.0f);
        desc_lbl->setColor({0.80f, 0.80f, 0.85f, 1.0f});
        auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
            key_lbl, std::make_shared<Spacer>(12, 1), desc_lbl
        });
        row->setPadding(3);
        return row;
    };

    struct ScSection {
        std::string title;
        std::vector<std::pair<std::string,std::string>> entries;
    };
    std::vector<ScSection> sc_sections = {
        { "Windows",  { {"Super + ↑",          "Maximise window"},
                        {"Super + ↓",          "Restore window"},
                        {"Super + ← / →",      "Snap window left / right"},
                        {"Super + Shift + ←/→","Move window to next output"},
                        {"Super + Q",          "Close focused window"},
                        {"Super + A",          "Cycle window layout"} } },
        { "Apps",     { {"Super + Space",       "Spotlight search"},
                        {"Super + V",           "Clipboard history"},
                        {"Super + C",           "Notification centre"},
                        {"Super + P",           "Colour picker"},
                        {"Super + /",           "This shortcut guide"} } },
        { "System",   { {"Super + L",            "Lock screen"},
                        {"Print Screen",        "Full-screen screenshot"},
                        {"Shift + Print Screen","Region screenshot"},
                        {"Alt + Tab",           "Switch windows (forward)"},
                        {"Alt + Shift + Tab",   "Switch windows (backward)"},
                        {"Ctrl + Alt + Esc",    "Force-quit focused app"},
                        {"Super + F3",          "Toggle compositor debug HUD"} } },
        { "Media",    { {"Volume Up / Down",    "Adjust system volume"},
                        {"Mute",                "Toggle mute"},
                        {"Brightness Up / Down","Adjust screen brightness"},
                        {"Play / Pause",        "Media playback toggle"},
                        {"Next / Prev",         "Next / previous track"} } },
    };

    auto sc_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{});
    sc_col->setGap(2.0f);

    for (const auto& sec : sc_sections) {
        auto hdr_lbl = std::make_shared<TextWidget>(sec.title, 11.0f);
        hdr_lbl->setColor({0.55f, 0.70f, 1.0f, 1.0f});
        sc_col->addChild(std::make_shared<Spacer>(1, 8));
        sc_col->addChild(hdr_lbl);
        sc_col->addChild(std::make_shared<DividerWidget>());
        for (const auto& [k, d] : sec.entries)
            sc_col->addChild(make_sc_row(k, d));
    }

    auto sc_title = std::make_shared<TextWidget>("Keyboard Shortcuts", 16.0f);
    sc_title->setColor({1.0f, 1.0f, 1.0f, 1.0f});
    auto sc_close_btn = std::make_shared<ButtonWidget>("✕", [](){});
    sc_close_btn->setFontSize(14.0f);
    sc_close_btn->setPadding(6);

    auto sc_header_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        sc_title, std::make_shared<Spacer>(1, 1, true), sc_close_btn
    });
    sc_header_row->setPadding(12);

    auto sc_scroll = std::make_shared<ScrollViewWidget>(sc_col);
    sc_scroll->setFixedSize(520, 420);

    auto sc_panel_col = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        sc_header_row,
        std::make_shared<DividerWidget>(),
        sc_scroll
    });

    auto shortcuts_overlay = std::make_shared<ContainerWidget>(sc_panel_col);
    shortcuts_overlay->setBackgroundColor({0.10f, 0.11f, 0.18f, 0.97f});
    shortcuts_overlay->setFixedSize(560, 500);
    shortcuts_overlay->setVisibility(false);

    *sc_close_btn = ButtonWidget(sc_close_btn->child(), [shortcuts_overlay]() {
        shortcuts_overlay->setVisibility(false);
    });

    // ── Quick Note Scratchpad overlay (Super+N) ───────────────────────────────
    // Floating panel: title field, body area, 5 colour pills, Save button.
    // Notes are stored server-side via NotesService.
    static const Color kNotePillColors[] = {
        {0.35f, 0.35f, 0.45f, 1.0f}, // 0=default (slate)
        {0.70f, 0.60f, 0.10f, 1.0f}, // 1=yellow
        {0.15f, 0.55f, 0.20f, 1.0f}, // 2=green
        {0.12f, 0.35f, 0.75f, 1.0f}, // 3=blue
        {0.65f, 0.15f, 0.35f, 1.0f}, // 4=pink
    };

    auto qn_color_idx = std::make_shared<int>(0); // active color index
    auto qn_note_id   = std::make_shared<uint32_t>(0); // 0 = new note

    auto qn_title_field = std::make_shared<TextInputWidget>();
    qn_title_field->setPlaceholder("Title");
    qn_title_field->setFixedSize(340, 32);

    auto qn_body_field = std::make_shared<TextInputWidget>();
    qn_body_field->setPlaceholder("Write a note…");
    qn_body_field->setFixedSize(340, 80);

    // Color pill row
    auto qn_pill_row = std::make_shared<FlexRow>();
    qn_pill_row->setGap(6);
    std::array<std::shared_ptr<ButtonWidget>, 5> qn_pills;
    for (int ci = 0; ci < 5; ++ci) {
        auto pill = std::make_shared<ButtonWidget>(
            std::make_shared<ContainerWidget>(nullptr),
            [ci, qn_color_idx, &qn_pills]() {
                *qn_color_idx = ci;
                for (int j = 0; j < 5; ++j)
                    qn_pills[j]->setBackgroundColor(
                        j == ci ? Color{1.0f,1.0f,1.0f,0.35f} : Color{0,0,0,0});
            });
        pill->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
        pill->setFixedSize(26, 26);
        // inner dot showing the colour
        auto dot = std::make_shared<ContainerWidget>(nullptr);
        dot->setBackgroundColor(kNotePillColors[ci]);
        dot->setFixedSize(20, 20);
        // ButtonWidget stores child; rebuild with inner dot
        pill = std::make_shared<ButtonWidget>(dot,
            [ci, qn_color_idx, &qn_pills]() {
                *qn_color_idx = ci;
                for (int j = 0; j < 5; ++j)
                    qn_pills[j]->setPadding(j == ci ? 2 : 3);
            });
        pill->setPadding(3);
        pill->setFixedSize(26, 26);
        qn_pills[ci] = pill;
        qn_pill_row->addChild(pill);
    }

    auto qn_save_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("Save", 13.0f), [](){});
    qn_save_btn->setBackgroundColor({0.22f, 0.44f, 0.88f, 1.0f});
    qn_save_btn->setPadding(8);
    qn_save_btn->setFixedSize(80, 32);

    auto qn_clear_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("New", 13.0f), [](){});
    qn_clear_btn->setBackgroundColor({0.20f, 0.20f, 0.28f, 1.0f});
    qn_clear_btn->setPadding(8);
    qn_clear_btn->setFixedSize(60, 32);

    auto qn_btn_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        qn_pill_row,
        std::make_shared<Spacer>(1, 1, true),
        qn_clear_btn,
        std::make_shared<Spacer>(4, 1),
        qn_save_btn
    });
    qn_btn_row->setGap(4);

    auto qn_hdr_lbl = std::make_shared<TextWidget>("Quick Note", 15.0f);
    qn_hdr_lbl->setColor(0.90f, 0.90f, 0.95f, 1.0f);

    auto qn_close_btn = std::make_shared<ButtonWidget>(
        std::make_shared<TextWidget>("✕", 13.0f), [](){});
    qn_close_btn->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});

    auto qn_hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
        qn_hdr_lbl,
        std::make_shared<Spacer>(1, 1, true),
        qn_close_btn
    });

    auto qn_inner = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        qn_hdr_row,
        std::make_shared<Spacer>(1, 10),
        qn_title_field,
        std::make_shared<Spacer>(1, 6),
        qn_body_field,
        std::make_shared<Spacer>(1, 10),
        qn_btn_row
    });
    qn_inner->setPadding(16);
    qn_inner->setGap(0);

    auto notes_overlay = std::make_shared<ContainerWidget>(qn_inner);
    notes_overlay->setBackgroundColor({0.11f, 0.11f, 0.18f, 0.97f});
    notes_overlay->setFixedSize(372, 240);
    notes_overlay->setVisibility(false);

    // Wire close button
    *qn_close_btn = ButtonWidget(qn_close_btn->child(), [notes_overlay]() {
        notes_overlay->setVisibility(false);
    });

    // Wire clear/new button
    *qn_clear_btn = ButtonWidget(qn_clear_btn->child(),
        [qn_title_field, qn_body_field, qn_note_id, qn_color_idx, &qn_pills]() {
            qn_title_field->setText("");
            qn_body_field->setText("");
            *qn_note_id   = 0;
            *qn_color_idx = 0;
            for (int j = 0; j < 5; ++j) qn_pills[j]->setPadding(j == 0 ? 2 : 3);
        });

    // Wire save button (creates new or updates existing note)
    *qn_save_btn = ButtonWidget(qn_save_btn->child(),
        [qn_title_field, qn_body_field, qn_note_id, qn_color_idx]() {
            if (!g_shell_xbus) return;
            NoteEntry entry{};
            entry.id_     = *qn_note_id;
            entry.color_  = static_cast<uint32_t>(*qn_color_idx);
            std::strncpy(entry.title_, qn_title_field->text().c_str(),
                         sizeof(entry.title_) - 1);
            std::strncpy(entry.body_,  qn_body_field->text().c_str(),
                         sizeof(entry.body_)  - 1);
            auto op = (*qn_note_id == 0)
                ? static_cast<uint32_t>(NotesOpcode::Add)
                : static_cast<uint32_t>(NotesOpcode::Update);
            g_shell_xbus->Call("com.freebsdx.Notes", op,
                {reinterpret_cast<const uint8_t*>(&entry), sizeof(entry)},
                [qn_note_id](const XBusMessage& r) {
                    NotesListPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    if (pg.count_ > 0 && pg.entry_.id_)
                        *qn_note_id = pg.entry_.id_;
                });
        });

    // Root — two-layer background: solid colour fallback + optional wallpaper image
    auto wallpaper = std::make_shared<ContainerWidget>(nullptr);
    // Transparent: the desktop background is drawn by the compositor (below app
    // windows), so the shell's own desktop area must not paint over apps. Only
    // the shell's panels/overlays draw opaque pixels, on top of apps.
    wallpaper->setBackgroundColor({0.0f, 0.0f, 0.0f, 0.0f});
    wallpaper->setFixedSize(10000, 10000);

    // Wallpaper image slot: empty = hidden, filled with ImageWidget when wallpaper is set
    auto wallpaper_img_slot = std::make_shared<FlexColumn>(
        std::vector<std::shared_ptr<Widget>>{});
    wallpaper_img_slot->setFixedSize(10000, 10000);

    auto desktop = std::make_shared<FlexColumn>(std::vector<std::shared_ptr<Widget>>{
        std::make_shared<Spacer>(1, 1, true),
        taskbar,
        // Lift the floating bar clear of the very bottom edge.
        std::make_shared<Spacer>(1, 12)
    });
    // Stretch children to the column's width (the screen width). Without this the
    // taskbar row is laid out with the unbounded cross-axis sentinel (100000px),
    // so its flexible spacer expands to ~98000px and shoves the whole system tray
    // (clock, CPU, volume, …) far off the right edge of the screen.
    desktop->setCrossAxisAlignment(CrossAxisAlignment::Stretch);

    // NB: wallpaper_img_slot is intentionally NOT in this stack. The compositor
    // now draws the desktop wallpaper below all app windows (see VulkanCompositor::
    // setWallpaper); painting it here (shell surface, above apps) would cover them.
    // The slot stays wired to the Wallpaper service calls below (harmless no-ops on
    // a detached widget) so re-enabling shell-side rendering later is a one-liner.
    auto root_overlay = std::make_shared<StackWidget>(std::vector<std::shared_ptr<Widget>>{
        wallpaper, desktop, start_menu, search_portal, qs_panel, action_center, switcher_panel, vkb_anchor, fc_overlay, ch_overlay, cp_overlay, shortcuts_overlay, cal_popover, disp_overlay, osd_overlay, notes_overlay
    });
    root_overlay->setAlignment(Alignment::Center);

    return XApplication::Run(root_overlay, "/tmp/freebsdx.sock", false, true,
        [clock_w, cpu_w, mem_w, net_btn, search_portal, action_center,
         qs_panel, qs_wifi_switch, qs_wifi_sub, qs_bt_switch, qs_dnd_switch, qs_dark_switch,
         qs_bright_slider, qs_bright_val, qs_vol_slider, qs_vol_val,
         bt_btn, vol_btn,
         qp_conn_w, qp_ip_w, qp_signal_w, qp_bw_w,
         qp_disc_btn, do_scan_page, wifi_connect_col,
         rebuild_bt_devices, bt_status_lbl,
         ss_status_lbl, ss_last_path, ss_open_btn, ss_recording, rec_toggle_btn, rec_status_lbl,
         vpn_status_lbl, vpn_info_lbl, vpn_conn_btn, vpn_disc_btn, update_vpn_ui,
         rd_status_lbl, rd_enable_btn, rd_disable_btn, update_rd_ui,
         rebuild_print,
         bt_state_lbl, bt_enable_btn, bt_disable_btn, update_bt_ui,
         rebuild_usb,
         mp_title_lbl, mp_artist_lbl, mp_progress_lbl, mp_play_btn, mp_playing, update_mp_ui,
         rebuild_storage,
         notif_history, notif_list_col, notif_empty_lbl, make_notif_card,
         ws_pips, switcher_chips, switcher_panel,
         tbar_chips, tbar_ids, apps,
         tray_row, load_tray,
         fc_overlay, fc_request_id, fc_current_path, fc_filter_mime, fc_save_mode,
         fc_title_w, fc_ok_btn, fc_save_field, fc_sel_w, fc_selected,
         ch_overlay, ch_entries, ch_list_col_ptr, ch_clear_btn,
         cp_overlay, cp_h, cp_s, cp_v, cp_recent,
         cp_swatch, cp_hex_lbl, cp_rgb_lbl,
         cp_h_lbl, cp_s_lbl, cp_v_lbl,
         update_cp, cp_copy_hex_btn, cp_copy_rgb_btn, cp_recent_row,
         rebuild_fc,
         weather_btn,
         file_results_ready, file_results_pending,
         sec_files, sec_files_hdr, make_result_row,
         wallpaper_img_slot,
         pinned_data, rebuild_pinned,
         show_osd, notif_unread, notif_badge_lbl, notif_badge_box,
         bat_btn, update_battery,
         shortcuts_overlay,
         qs_ns_switch, qs_ns_slider, qs_ns_val, send_ns_config,
         clock_btn, cal_popover,
         known_apps, muted_apps, rebuild_app_list,
         pomo_phase_lbl, pomo_cd_lbl, pomo_tbar_w, pomo_start_lbl,
         disp_col, dnd_sched_switch, dnd_from_lbl, dnd_to_lbl,
         ns_sched_switch, ns_from_lbl, ns_to_lbl,
         update_upd_btn, qs_autolock_switch,
         notes_overlay, notes_mini_col, rebuild_notes_mini,
         clip_ac_col, rebuild_clip_ac,
         startup_apps_col, rebuild_startup_apps,
         dnd_exc_col, rebuild_dnd_exc,
         screen_time_col, rebuild_screen_time,
         audio_vol_col, rebuild_audio_vol,
         scratch_input,
         spark_cpu_lbl, spark_ram_lbl,
         therm_col, rebuild_therm,
         conn_col, rebuild_conn,
         proc_col, rebuild_proc,
         cam_status_lbl, cam_block_btn, cam_allow_btn,
         mic_status_lbl, mic_block_btn, mic_allow_btn, update_privacy_ui,
         a11y_font_btns, a11y_cursor_slider, a11y_cursor_val,
         a11y_hc_switch, a11y_mag_switch, update_a11y_ui,
         cf_switch, cf_type_btns, cf_mono_switch, cf_intensity_slider,
         cf_intensity_val, update_cf_ui,
         lsess_col, rebuild_lsess,
         kb_bl_switch, kb_bl_slider, kb_bl_val, update_kb_bl_ui,
         bdrain_col, rebuild_bdrain,
         ap_switch, update_ap_ui,
         cron_col, rebuild_cron,
         loc_switch, update_loc_ui,
         aout_name_lbl, aout_pills_row, rebuild_aout,
         stout_btns, stout_cur, update_stout_ui,
         rot_lock_switch, update_rot_ui,
         netconn_col, rebuild_netconn,
         mic_mute_switch, update_mic_mute_ui,
         vol_col, rebuild_vol,
         startup_col, rebuild_startup,
         syslog_col, rebuild_syslog,
         update_gpu_ui,
         auto_br_switch, update_auto_br_ui,
         sshkey_col, sshkey_count_lbl, sshkey_known_lbl, rebuild_sshkey,
         rrate_btns, rrate_cur, rrate_mask, update_rrate_ui,
         envvar_col, envvar_count_lbl, rebuild_envvar,
         scroll_dir_switch, update_scroll_dir_ui,
         usb_col, usb_count_lbl, rebuild_usb_b,
         pres_switch, update_pres_ui,
         mem_level_lbl, mem_ram_lbl, mem_swap_lbl, mem_faults_lbl, mem_bar_blocks, update_mem_ui,
         touchpad_switch, update_touchpad_ui,
         print_col, print_count_lbl, rebuild_print_queue,
         reduce_motion_switch, update_reduce_motion_ui,
         pkg_upd_col, pkg_upd_count_lbl, pkg_upd_all_btn, rebuild_pkg_upd,
         disp_scale_cur, disp_scale_btns, update_disp_scale_ui,
         perm_col, perm_count_lbl, rebuild_perms,
         dev_overlay_cur, dev_overlay_btns, update_dev_overlay_ui,
         ft_avg_lbl, ft_p99_lbl, ft_jank_lbl, ft_gpu_lbl, ft_vsync_lbl, ft_bars, update_ft_ui,
         color_prof_cur, color_prof_btns, update_color_prof_ui,
         im_col, im_active_lbl, rebuild_im,
         stylus_palm_switch, stylus_connected, stylus_pressure_cur,
         stylus_pressure_btns, stylus_conn_lbl, update_stylus_ui,
         ntp_switch, ntp_status_lbl, ntp_server_lbl, update_ntp_ui,
         upd_ch_cur, upd_ch_btns, upd_ch_badge_lbl, update_upd_ch_ui,
         text_size_cur, text_size_btns, update_text_size_ui,
         cursor_size_cur, cursor_size_btns, update_cursor_size_ui,
         zfs_col, zfs_count_lbl, rebuild_zfs,
         hotspot_switch, hotspot_status_lbl, hotspot_ssid_lbl, update_hotspot_ui,
         jail_col, jail_count_lbl, rebuild_jails,
         cap_col, cap_count_lbl, rebuild_captures,
         be_col, be_count_lbl, rebuild_be,
         kmod_col, kmod_count_lbl, rebuild_kmod,
         lic_key_lbl, lic_tier_lbl, lic_expiry_lbl, lic_seats_lbl, lic_support_lbl, lic_copy_btn, update_lic_ui,
         dl_col, rebuild_dl,
         fw_status_lbl, fw_enable_btn, fw_disable_btn, update_fw_ui,
         ab_status_lbl, ab_detail_lbl, ab_enable_btn, ab_disable_btn, update_ab_ui,
         svc_col, rebuild_svc,
         pp_status_lbl, update_pp_ui,
         gm_status_lbl, gm_app_lbl, gm_enable_btn, gm_disable_btn, update_gm_ui,
         sec_col, rebuild_sec,
         hs_status_lbl, hs_enable_btn, hs_disable_btn, update_hs_ui,
         cast_status_lbl, cast_elapsed_lbl, cast_stop_btn, cast_scan_btn,
         cast_receiver_col, rebuild_cast_receivers, update_cast_ui,
         pkg_col, pkg_status_lbl, rebuild_pkg,
         diskio_col, rebuild_diskio,
         nl_switch, nl_slider, nl_temp_val, nl_sched_switch,
         nl_from_input, nl_to_input, update_nl_ui,
         focus_status_lbl, focus_end_btn, focus_dur_row, update_focus_ui,
         smart_col, rebuild_smart,
         crash_col, rebuild_crash,
         cal_add_input, cal_add_status,
         forecast_day_lbls, forecast_icon_lbls, forecast_temp_lbls, forecast_rebuild,
         heat_bar_lbl,
         sess_switch, sess_count_lbl, live_titles,
         kb_active_lbl, dnd_countdown_lbl,
         net_rx_spark_lbl, net_tx_spark_lbl,
         pomo_log_col, rebuild_pomo_log,
         bat_saver_switch, bat_saver_lbl,
         ch_selected_text, ch_xform_status,
         vol_preset_btns, apply_vol_preset,
         contact_search_input, contact_results_col, do_contact_search,
         timer_input, timer_label_input, timer_cd_lbl, timer_status_lbl,
         rebuild_rf, cs_preview_lbls,
         qs_hdr_switch, qs_vrr_switch, qs_hdr_lbl, qs_vrr_lbl, qs_disp_row,
         win_rules_col, rebuild_win_rules,
         hotkey_rows_col, hotkey_status_lbl, hotkey_binding_lbls, rebuild_hotkeys,
         ws_wp_btns, ws_wp_update_hl, ws_wp_open_fn,
         fc_wp_ws_idx,
         qp_data_w, wp_rotate_switch, wp_folder_lbl, update_wp_interval_lbl,
         tz_idx, tz_name_lbl, tz_disp_lbl, tz_time_lbl, update_tz_ui,
         energy_col, energy_count_lbl, rebuild_energy,
         rl_switch, rl_info_lbl, update_rl_ui,
         si_hostname_lbl, si_os_lbl, si_cpu_lbl, si_cores_lbl,
         si_ram_lbl, si_gpu_lbl, si_uptime_lbl, si_serial_lbl, update_sysinfo_ui,
         a11y_reader_on, a11y_contrast_on, a11y_magnify_on,
         a11y_reader_btn, a11y_contrast_btn, a11y_magnify_btn, update_a11y_ui_b,
         proxy_mode_cur, proxy_mode_btns, update_proxy_ui, proxy_info_lbl,
         dns_mode_cur, dns_mode_btns, update_dns_ui, dns_resolver_lbl,
         audio_route_col, ar_count_lbl, rebuild_audio_routes,
         wl_active, wl_switch, wl_dur_cur, wl_dur_btns, wl_status_lbl, update_wl_ui,
         share_col, ns_count_lbl, rebuild_shares,
         vm_col, vm_count_lbl, rebuild_vms,
         ct_col, ct_count_lbl, rebuild_containers,
         dock_pos_cur, dock_pos_btns, dock_autohide, update_dock_ui,
         du_col, du_status_lbl, rebuild_disk_usage,
         locale_cur, locale_btns, locale_sub_lbl, update_locale_ui,
         fs_col, fs_count_lbl, rebuild_file_sync,
         ss_on, ss_switch, ss_timeout_cur, ss_timeout_btns,
         ss_mode_cur, ss_mode_btns, update_ss_ui,
         kc_col, kc_count_lbl, kc_lock_btn, rebuild_keychain,
         xwl_running, xwl_switch, xwl_info_lbl, update_xwl_ui,
         iapps_col, iapps_count_lbl, iapps_prog_lbl, rebuild_iapps,
         term_sess_col, term_sess_count_lbl, rebuild_term_sess,
         fonts_col, fonts_count_lbl, rebuild_fonts,
         pkgsign_req_switch, pkgsign_unk_switch, pkgsign_cur, update_pkgsign_ui,
         store_col, store_status_lbl, store_search_input, rebuild_store,
         cam_dev_col, cam_dev_count_lbl, rebuild_cam_dev,
         bm_col, bm_count_lbl, rebuild_bm,
         seat_count_lbl, seat_col, rebuild_seats,
         arch_status_lbl,
         unlock_locked, unlock_inputs_row, unlock_status_lbl,
         uev_col, uev_count_lbl, uev_range_cur, rebuild_uev,
         dns_primary_input, dns_secondary_input, dns_doh_switch,
         dns_doh_url_input, dns_doh_row, update_dns_cfg_ui,
         ns_col, nsm_count_lbl, ns_status_lbl, rebuild_ns,
         wol_col, wol_count_lbl, rebuild_wol,
         slog_col, slog_level_mask, rebuild_slog,
         a11y_cfg, a11y_contrast_sw, a11y_largetext_sw, a11y_magnifier_sw,
         a11y_mag_row, a11y_mag_slider, a11y_mag_val_lbl,
         a11y_reader_sw, a11y_motion_sw, a11y_sticky_sw, update_a11y_ui_c,
         rdp_col, rdp_count_lbl, rdp_status_lbl, rebuild_rdp,
         gest_cfg, gest_switch, gest_swipe3_sl, gest_pinch_sl,
         gest_tap_sw, gest_nscroll_sw, gest_2fscroll_sw, update_gest_ui,
         indev_col, indev_count_lbl, rebuild_indev,
         disp_arr_col, disp_arr_count_lbl, disp_mirror_switch, rebuild_disp_arr,
         clip_col, clip_count_lbl, rebuild_clip,
         gpu_name_lbl, gpu_util_lbl, gpu_vram_lbl, gpu_temp_lbl, gpu_fan_lbl, update_gpu_ui_b,
         scap_col, scap_count_lbl, rebuild_scap,
         tz_col, tz_count_lbl, tz_status_lbl, rebuild_tz,
         usbdev_col, usbdev_count_lbl, usbdev_status_lbl, rebuild_usbdev,
         slk_enabled_sw, slk_require_sw, slk_sleep_sw,
         slk_timeout_cur, slk_status_lbl, slk_hl, update_slk_ui,
         btaudio_col, btaudio_count_lbl, rebuild_btaudio,
         nsch_enabled_sw, nsch_start_h_input, nsch_start_m_input,
         nsch_end_h_input, nsch_end_m_input,
         nsch_calls_sw, nsch_alarms_sw, nsch_weekend_sw,
         nsch_status_lbl, update_nsch_ui,
         sandbox_col, sandbox_count_lbl, rebuild_sandbox,
         tp_labels, tp_idx, apply_tp,
         rebuild_bkp, rebuild_loc, rebuild_rem, rebuild_totp, rebuild_xfr,
         update_eq_ui, update_loc_policy, update_ptr_ui, update_sa_ui,
         update_sndfx_ui, rebuild_gc,
         update_captions_ui, rebuild_swifi,
         update_dsaver_ui, rebuild_seckey,
         update_fastchg_ui, rebuild_txtrep,
         update_silence_ui, rebuild_applimit,
         update_bhealth_ui, rebuild_notifsnd,
         update_ttone_ui, rebuild_litem,
         update_ostore_ui, rebuild_tdev,
         update_monoaud_ui, rebuild_kbsc,
         update_stagemgr_ui, rebuild_dapp](XBusClient& xbus)
    {
        g_shell_xbus = &xbus;
        LOG_INFO("Shell connected to XBus. Starting status polling...");
        runAutostart();   // the session's compositor is up: launch login items
        loadRecentFiles();
        loadRecentQueries();
        loadClipSlots();
        // Populate named clipboard slot previews
        for (int i = 0; i < kNumClipSlots; ++i) {
            std::string prev = g_clip_slots[i];
            if (prev.size() > 28) prev = prev.substr(0, 27) + "…";
            cs_preview_lbls[i]->setText(prev.empty() ? "(empty)" : prev);
        }
        // Populate recent files panel
        (*rebuild_rf)();
        // Initial Wi-Fi SSID and BT device list
        xbus.Call("com.freebsdx.Network",
            static_cast<uint32_t>(NetworkOpcode::GetStatus), {},
            [](const XBusMessage& r) {
                NetworkStatus n{};
                std::memcpy(&n, r.payload_, sizeof(n));
                if (n.is_connected_ && n.current_ssid_[0])
                    g_wifi_ssid = std::string(n.current_ssid_);
            });
        if (rebuild_bt_devices && *rebuild_bt_devices) (*rebuild_bt_devices)();
        // Initial Bluetooth adapter state
        xbus.Call("com.freebsdx.Bluetooth",
            static_cast<uint32_t>(BluetoothOpcode::GetState), {},
            [update_bt_ui](const XBusMessage& r) {
                BluetoothState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_bt_ui(st);
            });
        // Initial USB device list
        if (rebuild_usb && *rebuild_usb) (*rebuild_usb)();
        // Initial thermal readings
        if (rebuild_therm && *rebuild_therm) (*rebuild_therm)();
        // Initial active connections
        if (rebuild_conn && *rebuild_conn) (*rebuild_conn)();
        // Initial process list
        if (rebuild_proc && *rebuild_proc) (*rebuild_proc)();
        // Initial disk I/O stats
        if (rebuild_diskio && *rebuild_diskio) (*rebuild_diskio)();
        // Initial package update list
        if (rebuild_pkg && *rebuild_pkg) (*rebuild_pkg)();
        // Initial cast state
        xbus.Call("com.freebsdx.Cast",
            static_cast<uint32_t>(CastOpcode::GetState), {},
            [update_cast_ui](const XBusMessage& r) {
                CastState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_cast_ui(st);
            });
        // Initial crash report list
        if (rebuild_crash && *rebuild_crash) (*rebuild_crash)();
        // Initial security events
        if (rebuild_sec && *rebuild_sec) (*rebuild_sec)();
        // Initial Game Mode state
        xbus.Call("com.freebsdx.GameMode",
            static_cast<uint32_t>(GameModeOpcode::GetState), {},
            [update_gm_ui](const XBusMessage& r) {
                GameModeState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_gm_ui(st);
            });
        // Initial SMART health
        if (rebuild_smart && *rebuild_smart) (*rebuild_smart)();
        // Initial Focus Mode state
        xbus.Call("com.freebsdx.FocusMode",
            static_cast<uint32_t>(FocusModeOpcode::GetState), {},
            [update_focus_ui](const XBusMessage& r) {
                FocusModeState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_focus_ui(st);
            });
        // Initial Night Light state
        xbus.Call("com.freebsdx.NightLight",
            static_cast<uint32_t>(NightLightOpcode::GetState), {},
            [update_nl_ui](const XBusMessage& r) {
                NightLightState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_nl_ui(st);
            });
        // Initial hotspot state
        xbus.Call("com.freebsdx.Hotspot",
            static_cast<uint32_t>(HotspotOpcode::GetStatus), {},
            [update_hs_ui](const XBusMessage& r) {
                HotspotStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_hs_ui(st);
            });
        // Initial downloads list
        if (rebuild_dl && *rebuild_dl) (*rebuild_dl)();
        // Initial accessibility settings
        xbus.Call("com.freebsdx.A11ySettings",
            static_cast<uint32_t>(AccessibilitySettingsOpcode::GetSettings), {},
            [update_a11y_ui](const XBusMessage& r) {
                AccessibilityQuickSettings st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_a11y_ui(st);
            });
        // Initial privacy state
        xbus.Call("com.freebsdx.Privacy",
            static_cast<uint32_t>(PrivacyOpcode::GetStatus), {},
            [update_privacy_ui](const XBusMessage& r) {
                PrivacyStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_privacy_ui(st);
            });
        // Initial services list
        if (rebuild_svc && *rebuild_svc) (*rebuild_svc)();
        // Initial login sessions list
        if (rebuild_lsess && *rebuild_lsess) (*rebuild_lsess)();
        // Initial keyboard backlight state
        xbus.Call("com.freebsdx.KeyboardBacklight",
            static_cast<uint32_t>(KeyboardBacklightOpcode::GetState), {},
            [update_kb_bl_ui](const XBusMessage& r) {
                KeyboardBacklightState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_kb_bl_ui(st);
            });
        // Initial battery drain list
        if (rebuild_bdrain && *rebuild_bdrain) (*rebuild_bdrain)();
        // Initial airplane mode state
        xbus.Call("com.freebsdx.AirplaneMode",
            static_cast<uint32_t>(AirplaneModeOpcode::GetState), {},
            [update_ap_ui](const XBusMessage& r) {
                AirplaneModeState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ap_ui(st);
            });
        // Initial cron job list
        if (rebuild_cron && *rebuild_cron) (*rebuild_cron)();
        // Initial audio output sink list
        if (rebuild_aout && *rebuild_aout) (*rebuild_aout)();
        // Initial open network connections
        if (rebuild_netconn && *rebuild_netconn) (*rebuild_netconn)();
        // Initial screen timeout
        xbus.Call("com.freebsdx.ScreenTimeout",
            static_cast<uint32_t>(ScreenTimeoutOpcode::GetTimeout), {},
            [update_stout_ui](const XBusMessage& r) {
                ScreenTimeoutState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_stout_ui(st);
            });
        // Initial mounted volumes
        if (rebuild_vol && *rebuild_vol) (*rebuild_vol)();
        // Initial rotation lock state
        xbus.Call("com.freebsdx.RotationLock",
            static_cast<uint32_t>(RotationLockOpcode::GetState), {},
            [update_rot_ui](const XBusMessage& r) {
                RotationLockState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_rot_ui(st);
            });
        // Initial syslog entries
        if (rebuild_syslog && *rebuild_syslog) (*rebuild_syslog)();
        // Initial mic mute state
        xbus.Call("com.freebsdx.MicMute",
            static_cast<uint32_t>(MicMuteOpcode::GetState), {},
            [update_mic_mute_ui](const XBusMessage& r) {
                MicMuteState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_mic_mute_ui(st);
            });
        // Initial startup items list
        if (rebuild_startup && *rebuild_startup) (*rebuild_startup)();
        // Initial location services state
        xbus.Call("com.freebsdx.LocationServices",
            static_cast<uint32_t>(LocationOpcode::GetState), {},
            [update_loc_ui](const XBusMessage& r) {
                LocationState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_loc_ui(st);
            });
        // Initial GPU statistics
        xbus.Call("com.freebsdx.GpuStats",
            static_cast<uint32_t>(GpuOpcode::GetStats), {},
            [update_gpu_ui_b](const XBusMessage& r) {
                GpuStats st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_gpu_ui_b(st);
            });
        // Initial auto-brightness state
        xbus.Call("com.freebsdx.AutoBrightness",
            static_cast<uint32_t>(AutoBrightnessOpcode::GetState), {},
            [update_auto_br_ui](const XBusMessage& r) {
                AutoBrightnessState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_auto_br_ui(st);
            });
        // Initial SSH keys list
        if (rebuild_sshkey && *rebuild_sshkey) (*rebuild_sshkey)();
        // Initial display refresh rate
        xbus.Call("com.freebsdx.RefreshRate",
            static_cast<uint32_t>(RefreshRateOpcode::GetRate), {},
            [update_rrate_ui](const XBusMessage& r) {
                RefreshRateState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_rrate_ui(st);
            });
        // Initial environment variables list
        if (rebuild_envvar && *rebuild_envvar) (*rebuild_envvar)();
        // Initial scroll direction
        xbus.Call("com.freebsdx.ScrollDir",
            static_cast<uint32_t>(ScrollDirOpcode::GetDir), {},
            [update_scroll_dir_ui](const XBusMessage& r) {
                ScrollDirState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_scroll_dir_ui(st);
            });
        // Initial USB device list
        if (rebuild_usb_b && *rebuild_usb_b) (*rebuild_usb_b)();
        // Initial presentation mode state
        xbus.Call("com.freebsdx.Presentation",
            static_cast<uint32_t>(PresentationOpcode::GetState), {},
            [update_pres_ui](const XBusMessage& r) {
                PresentationState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_pres_ui(st);
            });
        // Initial memory pressure
        xbus.Call("com.freebsdx.MemPressure",
            static_cast<uint32_t>(MemPressureOpcode::GetPressure), {},
            [update_mem_ui](const XBusMessage& r) {
                MemPressureState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_mem_ui(st);
            });
        // Initial touchpad state
        xbus.Call("com.freebsdx.Touchpad",
            static_cast<uint32_t>(TouchpadOpcode::GetState), {},
            [update_touchpad_ui](const XBusMessage& r) {
                TouchpadState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_touchpad_ui(st);
            });
        // Initial printer queue
        if (rebuild_print_queue && *rebuild_print_queue) (*rebuild_print_queue)();
        // Initial reduce motion state
        xbus.Call("com.freebsdx.ReduceMotion",
            static_cast<uint32_t>(ReduceMotionOpcode::GetState), {},
            [update_reduce_motion_ui](const XBusMessage& r) {
                ReduceMotionState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_reduce_motion_ui(st);
            });
        // Initial package updates list
        if (rebuild_pkg_upd && *rebuild_pkg_upd) (*rebuild_pkg_upd)();
        // Initial display scale
        xbus.Call("com.freebsdx.DisplayScale",
            static_cast<uint32_t>(DisplayScaleOpcode::GetScale), {},
            [update_disp_scale_ui](const XBusMessage& r) {
                DisplayScaleState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_disp_scale_ui(st);
            });
        // Initial app permissions list
        if (rebuild_perms && *rebuild_perms) (*rebuild_perms)();
        // Initial dev overlay mode
        xbus.Call("com.freebsdx.DevOverlay",
            static_cast<uint32_t>(DevOverlayOpcode::GetState), {},
            [update_dev_overlay_ui](const XBusMessage& r) {
                DevOverlayState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_dev_overlay_ui(st);
            });
        // Initial frame timing stats
        xbus.Call("com.freebsdx.FramePerf",
            static_cast<uint32_t>(FramePerfOpcode::GetStats), {},
            [update_ft_ui](const XBusMessage& r) {
                FramePerfState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ft_ui(st);
            });
        // Initial color profile
        xbus.Call("com.freebsdx.ColorProfile",
            static_cast<uint32_t>(ColorProfileOpcode::GetProfile), {},
            [update_color_prof_ui](const XBusMessage& r) {
                ColorProfileState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_color_prof_ui(st);
            });
        // Initial input method list
        if (rebuild_im && *rebuild_im) (*rebuild_im)();
        // Initial stylus state
        xbus.Call("com.freebsdx.Stylus",
            static_cast<uint32_t>(StylusOpcode::GetState), {},
            [update_stylus_ui](const XBusMessage& r) {
                StylusState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_stylus_ui(st);
            });
        // Initial captures list
        if (rebuild_captures && *rebuild_captures) (*rebuild_captures)();
        // Initial NTP state
        xbus.Call("com.freebsdx.NTP",
            static_cast<uint32_t>(NtpOpcode::GetState), {},
            [update_ntp_ui](const XBusMessage& r) {
                NtpState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ntp_ui(st);
            });
        // Initial boot environment list
        if (rebuild_be && *rebuild_be) (*rebuild_be)();
        // Initial update channel + pending count
        xbus.Call("com.freebsdx.UpdateChannel",
            static_cast<uint32_t>(UpdateChannelOpcode::GetChannel), {},
            [update_upd_ch_ui](const XBusMessage& r) {
                UpdateChannelState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_upd_ch_ui(st);
            });
        // Initial license info (one-shot, no polling)
        xbus.Call("com.freebsdx.License",
            static_cast<uint32_t>(LicenseOpcode::GetInfo), {},
            [update_lic_ui](const XBusMessage& r) {
                LicenseState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_lic_ui(st);
            });
        // Initial text size
        xbus.Call("com.freebsdx.TextSize",
            static_cast<uint32_t>(TextSizeOpcode::GetSize), {},
            [update_text_size_ui](const XBusMessage& r) {
                TextSizeState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_text_size_ui(st);
            });
        // Initial cursor size
        xbus.Call("com.freebsdx.CursorSize",
            static_cast<uint32_t>(CursorSizeOpcode::GetSize), {},
            [update_cursor_size_ui](const XBusMessage& r) {
                CursorSizeState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_cursor_size_ui(st);
            });
        // Initial ZFS pool list
        if (rebuild_zfs && *rebuild_zfs) (*rebuild_zfs)();
        // Initial hotspot state
        xbus.Call("com.freebsdx.Hotspot",
            static_cast<uint32_t>(HotspotToggleOpcode::GetState), {},
            [update_hotspot_ui](const XBusMessage& r) {
                HotspotState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_hotspot_ui(st);
            });
        // Initial jail list
        if (rebuild_jails && *rebuild_jails) (*rebuild_jails)();
        // Initial kernel module list
        if (rebuild_kmod && *rebuild_kmod) (*rebuild_kmod)();
        // Initial time zone
        xbus.Call("com.freebsdx.Timezone",
            static_cast<uint32_t>(TimezoneOpcode::GetTZ), {},
            [update_tz_ui](const XBusMessage& r) {
                TimezoneState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_tz_ui(st);
            });
        // Initial energy impact list
        if (rebuild_energy && *rebuild_energy) (*rebuild_energy)();
        // Initial remote login (sshd) state
        xbus.Call("com.freebsdx.RemoteLogin",
            static_cast<uint32_t>(RemoteLoginOpcode::GetState), {},
            [update_rl_ui](const XBusMessage& r) {
                RemoteLoginState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_rl_ui(st);
            });
        // System information (one-shot, no polling)
        xbus.Call("com.freebsdx.SysInfo",
            static_cast<uint32_t>(SysInfoOpcode::GetInfo), {},
            [update_sysinfo_ui](const XBusMessage& r) {
                SysInfoState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_sysinfo_ui(st);
            });
        // Initial accessibility state
        xbus.Call("com.freebsdx.Accessibility",
            static_cast<uint32_t>(A11yOpcode::GetState), {},
            [update_a11y_ui_b](const XBusMessage& r) {
                A11yState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_a11y_ui_b(st);
            });
        // Initial proxy config
        xbus.Call("com.freebsdx.Proxy",
            static_cast<uint32_t>(ProxyOpcode::GetConfig), {},
            [update_proxy_ui](const XBusMessage& r) {
                ProxyConfig st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_proxy_ui(st);
            });
        // Initial DNS config
        xbus.Call("com.freebsdx.DNS",
            static_cast<uint32_t>(DnsOpcode::GetConfig), {},
            [update_dns_ui](const XBusMessage& r) {
                DnsConfig st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_dns_ui(st);
            });
        // Initial audio routing list
        if (rebuild_audio_routes && *rebuild_audio_routes) (*rebuild_audio_routes)();
        // Initial wake lock state
        xbus.Call("com.freebsdx.WakeLock",
            static_cast<uint32_t>(WakeLockOpcode::GetState), {},
            [update_wl_ui](const XBusMessage& r) {
                WakeLockState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_wl_ui(st);
            });
        // Initial network shares list
        if (rebuild_shares && *rebuild_shares) (*rebuild_shares)();
        // Initial VM list
        if (rebuild_vms && *rebuild_vms) (*rebuild_vms)();
        // Initial container list
        if (rebuild_containers && *rebuild_containers) (*rebuild_containers)();
        // Initial dock settings
        xbus.Call("com.freebsdx.Dock",
            static_cast<uint32_t>(DockOpcode::GetSettings), {},
            [update_dock_ui](const XBusMessage& r) {
                DockSettings st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_dock_ui(st);
            });
        // Initial disk usage results (stale from last scan, if any)
        if (rebuild_disk_usage && *rebuild_disk_usage) (*rebuild_disk_usage)();
        // Initial locale
        xbus.Call("com.freebsdx.Locale",
            static_cast<uint32_t>(LocaleOpcode::GetLocale), {},
            [update_locale_ui](const XBusMessage& r) {
                LocaleState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_locale_ui(st);
            });
        // Initial file sync job list
        if (rebuild_file_sync && *rebuild_file_sync) (*rebuild_file_sync)();
        // Initial screen saver config
        xbus.Call("com.freebsdx.ScreenSaver",
            static_cast<uint32_t>(ScreenSaverOpcode::GetConfig), {},
            [update_ss_ui](const XBusMessage& r) {
                ScreenSaverConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_ss_ui(cfg.timeout_secs_, cfg.mode_);
            });
        // Initial keychain list
        if (rebuild_keychain && *rebuild_keychain) (*rebuild_keychain)();
        // Initial XWayland status
        xbus.Call("com.freebsdx.XWayland",
            static_cast<uint32_t>(XWaylandOpcode::GetStatus), {},
            [update_xwl_ui](const XBusMessage& r) {
                XWaylandStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_xwl_ui(st);
            });
        // Initial installed apps list
        if (rebuild_iapps && *rebuild_iapps) (*rebuild_iapps)();
        // Initial terminal session list
        if (rebuild_term_sess && *rebuild_term_sess) (*rebuild_term_sess)();
        // Initial font list
        if (rebuild_fonts && *rebuild_fonts) (*rebuild_fonts)();
        // Initial package signing policy
        xbus.Call("com.freebsdx.PackageSigning",
            static_cast<uint32_t>(PackageSigningOpcode::GetPolicy), {},
            [update_pkgsign_ui](const XBusMessage& r) {
                PkgSigningPolicy pol{};
                std::memcpy(&pol, r.payload_, sizeof(pol));
                update_pkgsign_ui(pol);
            });
        // Initial app store catalog (list page 0)
        if (rebuild_store && *rebuild_store) (*rebuild_store)("");
        // Initial camera device list
        if (rebuild_cam_dev && *rebuild_cam_dev) (*rebuild_cam_dev)();
        // Initial bookmarks list
        if (rebuild_bm && *rebuild_bm) (*rebuild_bm)();
        // Initial seat list
        if (rebuild_seats && *rebuild_seats) (*rebuild_seats)();
        // Initial lock state — show admin unlock row if already locked
        xbus.Call("com.freebsdx.Lock",
            static_cast<uint32_t>(LockOpcode::GetState), {},
            [unlock_locked, unlock_inputs_row, unlock_status_lbl](const XBusMessage& r) {
                LockState ls{};
                std::memcpy(&ls, r.payload_, sizeof(ls));
                *unlock_locked = (ls.locked_ != 0);
                unlock_inputs_row->setVisibility(*unlock_locked);
                unlock_status_lbl->setVisibility(*unlock_locked);
            });
        // Initial upcoming events (today range)
        if (rebuild_uev && *rebuild_uev) (*rebuild_uev)();
        // Initial DNS config
        xbus.Call("com.freebsdx.DnsConfig",
            static_cast<uint32_t>(DnsConfigOpcode::GetConfig), {},
            [update_dns_cfg_ui](const XBusMessage& r) {
                DnsResolverConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_dns_cfg_ui(cfg);
            });
        // Initial spatial audio config
        xbus.Call("com.freebsdx.SpatialAudio",
            static_cast<uint32_t>(SpatialAudioOpcode::GetConfig), {},
            [update_sa_ui](const XBusMessage& r) {
                SpatialAudioConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_sa_ui(cfg);
            });
        // Initial active transfer list
        if (rebuild_xfr && *rebuild_xfr) (*rebuild_xfr)();
        // Initial system sounds state
        xbus.Call("com.freebsdx.SystemSounds",
            static_cast<uint32_t>(SystemSoundsOpcode::GetState), {},
            [update_sndfx_ui](const XBusMessage& r) {
                SystemSoundsState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_sndfx_ui(st);
            });
        // Initial game controller list
        if (rebuild_gc && *rebuild_gc) (*rebuild_gc)();
        // Initial live captions state
        xbus.Call("com.freebsdx.LiveCaptions",
            static_cast<uint32_t>(LiveCaptionsOpcode::GetState), {},
            [update_captions_ui](const XBusMessage& r) {
                LiveCaptionsState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_captions_ui(st);
            });
        // Initial saved wi-fi network list
        if (rebuild_swifi && *rebuild_swifi) (*rebuild_swifi)();
        // Initial data saver state
        xbus.Call("com.freebsdx.DataSaver",
            static_cast<uint32_t>(DataSaverOpcode::GetState), {},
            [update_dsaver_ui](const XBusMessage& r) {
                DataSaverState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_dsaver_ui(st);
            });
        // Initial security key list
        if (rebuild_seckey && *rebuild_seckey) (*rebuild_seckey)();
        // Initial fast charging state
        xbus.Call("com.freebsdx.FastCharging",
            static_cast<uint32_t>(FastChargingOpcode::GetState), {},
            [update_fastchg_ui](const XBusMessage& r) {
                FastChargingState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_fastchg_ui(st);
            });
        // Initial text replacement list
        if (rebuild_txtrep && *rebuild_txtrep) (*rebuild_txtrep)();
        // Initial do not disturb state
        xbus.Call("com.freebsdx.DoNotDisturb",
            static_cast<uint32_t>(DoNotDisturbOpcode::GetState), {},
            [update_silence_ui](const XBusMessage& r) {
                DoNotDisturbState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_silence_ui(st);
            });
        // Initial app time limit list
        if (rebuild_applimit && *rebuild_applimit) (*rebuild_applimit)();
        // Initial battery health state
        xbus.Call("com.freebsdx.BatteryHealth",
            static_cast<uint32_t>(BatteryHealthOpcode::GetState), {},
            [update_bhealth_ui](const XBusMessage& r) {
                BatteryHealthState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_bhealth_ui(st);
            });
        // Initial notification sound list
        if (rebuild_notifsnd && *rebuild_notifsnd) (*rebuild_notifsnd)();
        // Initial true tone state
        xbus.Call("com.freebsdx.TrueTone",
            static_cast<uint32_t>(TrueToneOpcode::GetState), {},
            [update_ttone_ui](const XBusMessage& r) {
                TrueToneState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ttone_ui(st);
            });
        // Initial login items list
        if (rebuild_litem && *rebuild_litem) (*rebuild_litem)();
        // Initial optimized storage state
        xbus.Call("com.freebsdx.OptStorage",
            static_cast<uint32_t>(OptStorageOpcode::GetState), {},
            [update_ostore_ui](const XBusMessage& r) {
                OptStorageState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ostore_ui(st);
            });
        // Initial trusted devices list
        if (rebuild_tdev && *rebuild_tdev) (*rebuild_tdev)();
        // Initial mono audio state
        xbus.Call("com.freebsdx.MonoAudio",
            static_cast<uint32_t>(MonoAudioOpcode::GetState), {},
            [update_monoaud_ui](const XBusMessage& r) {
                MonoAudioState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_monoaud_ui(st);
            });
        // Initial keyboard shortcuts list
        if (rebuild_kbsc && *rebuild_kbsc) (*rebuild_kbsc)();
        // Initial stage manager state
        xbus.Call("com.freebsdx.StageManager",
            static_cast<uint32_t>(StageManagerOpcode::GetState), {},
            [update_stagemgr_ui](const XBusMessage& r) {
                StageManagerState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_stagemgr_ui(st);
            });
        // Initial default apps list
        if (rebuild_dapp && *rebuild_dapp) (*rebuild_dapp)();
        // Initial pointer config
        xbus.Call("com.freebsdx.Pointer",
            static_cast<uint32_t>(PointerOpcode::GetConfig), {},
            [update_ptr_ui](const XBusMessage& r) {
                PointerConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_ptr_ui(cfg);
            });
        // Initial reminders list
        if (rebuild_rem && *rebuild_rem) (*rebuild_rem)();
        // Initial audio EQ config
        xbus.Call("com.freebsdx.AudioEq",
            static_cast<uint32_t>(AudioEqOpcode::GetEq), {},
            [update_eq_ui](const XBusMessage& r) {
                AudioEqConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_eq_ui(cfg);
            });
        // Initial backup job list
        if (rebuild_bkp && *rebuild_bkp) (*rebuild_bkp)();
        // Initial location state + app list
        xbus.Call("com.freebsdx.Location",
            static_cast<uint32_t>(LocationOpcode::GetState), {},
            [update_loc_policy](const XBusMessage& r) {
                LocationState s{};
                std::memcpy(&s, r.payload_, sizeof(s));
                update_loc_policy(s);
            });
        if (rebuild_loc && *rebuild_loc) (*rebuild_loc)();
        // Initial TOTP account list
        if (rebuild_totp && *rebuild_totp) (*rebuild_totp)();
        // Initial screen lock policy
        xbus.Call("com.freebsdx.ScreenLock",
            static_cast<uint32_t>(ScreenLockOpcode::GetPolicy), {},
            [update_slk_ui](const XBusMessage& r) {
                ScreenLockPolicy p{};
                std::memcpy(&p, r.payload_, sizeof(p));
                update_slk_ui(p);
            });
        // Initial BT audio device list
        if (rebuild_btaudio && *rebuild_btaudio) (*rebuild_btaudio)();
        // Initial notif schedule
        xbus.Call("com.freebsdx.NotifSchedule",
            static_cast<uint32_t>(NotifScheduleOpcode::GetSchedule), {},
            [update_nsch_ui](const XBusMessage& r) {
                NotifSchedule s{};
                std::memcpy(&s, r.payload_, sizeof(s));
                update_nsch_ui(s);
            });
        // Initial app sandbox list
        if (rebuild_sandbox && *rebuild_sandbox) (*rebuild_sandbox)();
        // Initial thermal zones
        if (rebuild_tz && *rebuild_tz) (*rebuild_tz)();
        // Initial USB device list
        if (rebuild_usbdev && *rebuild_usbdev) (*rebuild_usbdev)();
        // Initial network share list
        if (rebuild_ns && *rebuild_ns) (*rebuild_ns)();
        // Initial Wake-on-LAN target list
        if (rebuild_wol && *rebuild_wol) (*rebuild_wol)();
        // Initial system log (all levels)
        if (rebuild_slog && *rebuild_slog) (*rebuild_slog)();
        // Initial accessibility settings
        xbus.Call("com.freebsdx.Accessibility",
            static_cast<uint32_t>(AccessibilityPanelOpcode::GetSettings), {},
            [update_a11y_ui_c](const XBusMessage& r) {
                AccessibilitySettings cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_a11y_ui_c(cfg);
            });
        // Initial remote desktop sessions
        if (rebuild_rdp && *rebuild_rdp) (*rebuild_rdp)();
        // Initial gesture config
        xbus.Call("com.freebsdx.Gesture",
            static_cast<uint32_t>(GestureOpcode::GetConfig), {},
            [update_gest_ui](const XBusMessage& r) {
                GestureConfig cfg{};
                std::memcpy(&cfg, r.payload_, sizeof(cfg));
                update_gest_ui(cfg);
            });
        // Initial input device list
        if (rebuild_indev && *rebuild_indev) (*rebuild_indev)();
        // Initial display layout
        if (rebuild_disp_arr && *rebuild_disp_arr) (*rebuild_disp_arr)();
        // Initial clipboard history
        if (rebuild_clip && *rebuild_clip) (*rebuild_clip)();
        // Initial GPU stats
        xbus.Call("com.freebsdx.GpuMonitor",
            static_cast<uint32_t>(GpuMonitorOpcode::GetStats), {},
            [update_gpu_ui](const XBusMessage& r) {
                GpuStatsDetail st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_gpu_ui(st);
            });
        // Initial screenshot gallery
        if (rebuild_scap && *rebuild_scap) (*rebuild_scap)();
        // Initial color filter state
        xbus.Call("com.freebsdx.ColorFilter",
            static_cast<uint32_t>(ColorFilterOpcode::GetState), {},
            [update_cf_ui](const XBusMessage& r) {
                ColorFilterState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_cf_ui(st);
            });
        // Initial ad-blocker state
        xbus.Call("com.freebsdx.AdBlock",
            static_cast<uint32_t>(AdBlockOpcode::GetStatus), {},
            [update_ab_ui](const XBusMessage& r) {
                AdBlockStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_ab_ui(st);
            });
        // Initial firewall state
        xbus.Call("com.freebsdx.Firewall",
            static_cast<uint32_t>(FirewallOpcode::GetStatus), {},
            [update_fw_ui](const XBusMessage& r) {
                FirewallStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_fw_ui(st);
            });
        // Initial power profile state
        xbus.Call("com.freebsdx.PowerProfile",
            static_cast<uint32_t>(PowerProfileOpcode::GetProfile), {},
            [update_pp_ui](const XBusMessage& r) {
                PowerProfileState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_pp_ui(st);
            });
        if (rebuild_storage && *rebuild_storage) (*rebuild_storage)();
        // Initial VPN state
        xbus.Call("com.freebsdx.VPN",
            static_cast<uint32_t>(VpnOpcode::GetStatus), {},
            [update_vpn_ui](const XBusMessage& r) {
                VpnStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_vpn_ui(st);
            });
        // Initial Remote Desktop state
        xbus.Call("com.freebsdx.RemoteDesktop",
            static_cast<uint32_t>(RemoteDesktopOpcode::GetStatus), {},
            [update_rd_ui](const XBusMessage& r) {
                RemoteDesktopStatus st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_rd_ui(st);
            });
        // Initial printer list
        if (rebuild_print && *rebuild_print) (*rebuild_print)();
        // Initial screenshot/recording state
        xbus.Call("com.freebsdx.Screenshot",
            static_cast<uint32_t>(ScreenshotOpcode::GetStatus), {},
            [ss_recording, rec_toggle_btn, rec_status_lbl](const XBusMessage& r) {
                ScreenshotRecordingState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                *ss_recording = st.recording_ != 0;
                rec_toggle_btn->setText(*ss_recording ? "■ Stop" : "⏺ Record");
                rec_toggle_btn->setBackgroundColor(*ss_recording
                    ? Color{0.6f, 0.12f, 0.12f, 1.0f}
                    : Color{0.32f, 0.12f, 0.12f, 1.0f});
                if (*ss_recording)
                    rec_status_lbl->setText(std::format("● {} fps  {} frames",
                        st.fps_, st.frame_count_));
            });
        loadDndSched();
        loadNsSched();
        loadStartupApps();
        loadThemePreset();
        loadScratchNote();
        scratch_input->setText(g_scratch_note);
        // Pomo log — wire rebuild and load history
        *rebuild_pomo_log = [pomo_log_col]() {
            pomo_log_col->clearChildren();
            std::vector<PomoLogEntry> snap;
            { std::lock_guard<std::mutex> lk(g_pomo_log_mtx); snap = g_pomo_log; }
            if (snap.empty()) {
                auto empty = std::make_shared<TextWidget>("No sessions yet", 11.0f);
                empty->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                pomo_log_col->addChild(empty);
                return;
            }
            static const char* kPhaseNames[] = {"🍅 Work", "☕ Short Break", "🛌 Long Break"};
            // Show most recent entries first (up to 12)
            int start = static_cast<int>(snap.size()) > 12
                ? static_cast<int>(snap.size()) - 12 : 0;
            for (int i = static_cast<int>(snap.size()) - 1; i >= start; --i) {
                const auto& e = snap[static_cast<size_t>(i)];
                struct tm lt{}; localtime_r(&e.ts, &lt);
                char tbuf[20];
                std::strftime(tbuf, sizeof(tbuf), "%H:%M", &lt);
                int m = e.duration / 60, s = e.duration % 60;
                std::string ph = (e.phase >= 0 && e.phase <= 2)
                    ? kPhaseNames[e.phase] : "?";
                auto row_lbl = std::make_shared<TextWidget>(
                    std::format("{} {}  {}m{:02d}s", tbuf, ph, m, s), 11.0f);
                row_lbl->setColor({0.70f, 0.70f, 0.75f, 1.0f});
                pomo_log_col->addChild(row_lbl);
            }
        };
        loadPomoLog();
        if (*rebuild_pomo_log) (*rebuild_pomo_log)();

        // Volume preset — restore highlight
        loadVolPreset();
        if (g_vol_preset >= 0 && g_vol_preset < kNumVolPresets)
            (*vol_preset_btns)[g_vol_preset]->setBackgroundColor(
                {0.22f, 0.44f, 0.88f, 0.95f});

        // Contact quick-search — wire the search function
        *do_contact_search = [contact_results_col, &xbus](const std::string& q) {
            ContactsSearchRequest req{};
            req.page_ = 0;
            std::strncpy(req.query_, q.c_str(), sizeof(req.query_) - 1);
            xbus.Call("com.freebsdx.Contacts",
                static_cast<uint32_t>(ContactsOpcode::Search),
                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                [contact_results_col, q, &xbus](const XBusMessage& r) {
                    ContactsListPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    contact_results_col->clearChildren();
                    if (!pg.valid_ || !pg.entry_.first_name_[0]) {
                        auto lbl = std::make_shared<TextWidget>("No results", 11.0f);
                        lbl->setColor({0.4f, 0.4f, 0.45f, 1.0f});
                        contact_results_col->addChild(lbl);
                        return;
                    }
                    // Show this result; more pages handled by has_more_ check
                    auto add_result = [&](const ContactEntry& e) {
                        std::string name = std::string(e.first_name_);
                        if (e.last_name_[0]) name += " " + std::string(e.last_name_);
                        std::string phone(e.phone_);
                        auto name_lbl = std::make_shared<TextWidget>(name, 12.0f);
                        name_lbl->setColor({0.90f, 0.90f, 0.95f, 1.0f});
                        auto phone_lbl = std::make_shared<TextWidget>(
                            phone.empty() ? "(no phone)" : phone, 11.0f);
                        phone_lbl->setColor({0.55f, 0.80f, 0.55f, 1.0f});
                        auto copy_btn = std::make_shared<ButtonWidget>("📋",
                            [phone]() {
                                if (phone.empty() || !g_shell_xbus) return;
                                char buf[256] = {};
                                std::strncpy(buf, phone.c_str(), sizeof(buf) - 1);
                                g_shell_xbus->Call("com.freebsdx.Clipboard", 0x01,
                                    {reinterpret_cast<const uint8_t*>(buf), sizeof(buf)},
                                    nullptr);
                            });
                        copy_btn->setFixedSize(28, 28);
                        copy_btn->setFontSize(11.0f);
                        auto row = std::make_shared<FlexRow>(
                            std::vector<std::shared_ptr<Widget>>{
                                std::make_shared<FlexColumn>(
                                    std::vector<std::shared_ptr<Widget>>{
                                        name_lbl, phone_lbl }),
                                std::make_shared<Spacer>(1, 1, true),
                                copy_btn
                            });
                        row->setBackgroundColor({0.12f, 0.14f, 0.22f, 1.0f});
                        row->setPadding(6);
                        contact_results_col->addChild(row);
                    };
                    add_result(pg.entry_);
                    // Fetch up to 3 more pages for top-4 results
                    if (pg.has_more_ && pg.total_ > 1) {
                        uint32_t pages = std::min(pg.total_, 4u);
                        for (uint32_t p = 1; p < pages; ++p) {
                            ContactsSearchRequest rq{};
                            rq.page_ = p;
                            std::strncpy(rq.query_, q.c_str(), sizeof(rq.query_) - 1);
                            xbus.Call("com.freebsdx.Contacts",
                                static_cast<uint32_t>(ContactsOpcode::Search),
                                {reinterpret_cast<const uint8_t*>(&rq), sizeof(rq)},
                                [contact_results_col, add_result](const XBusMessage& r2) {
                                    ContactsListPage pg2{};
                                    std::memcpy(&pg2, r2.payload_, sizeof(pg2));
                                    if (pg2.valid_) add_result(pg2.entry_);
                                });
                        }
                    }
                });
        };

        loadKeymap();
        kb_active_lbl->setText(kLayouts[g_keymap_idx].label);
        // Send active layout to compositor on connect
        {
            KeymapRequest kr{}; std::strncpy(kr.layout_, kLayouts[g_keymap_idx].code, 15);
            xbus.Call("com.freebsdx.Keymap",
                static_cast<uint32_t>(KeymapOpcode::SetLayout),
                {reinterpret_cast<const uint8_t*>(&kr), sizeof(kr)}, nullptr);
        }
        dnd_countdown_lbl->setText(dndCountdownText());
        loadAppHeatmap();
        heat_bar_lbl->setText(fmtHeatmap(g_app_hourly));
        g_heat_update_fn = [heat_bar_lbl]() {
            heat_bar_lbl->setText(fmtHeatmap(g_app_hourly));
        };
        loadSession();
        sess_switch->setValue(g_session_enabled);
        int saved_n = static_cast<int>(g_session_execs.size());
        if (saved_n > 0)
            sess_count_lbl->setText(std::format("{} app{} saved", saved_n,
                                                saved_n == 1 ? "" : "s"));
        // Restore previous session (once per process lifetime)
        if (g_session_enabled && !g_session_restored) {
            g_session_restored = true;
            for (const auto& exec : g_session_execs)
                launchApp(exec);
        }
        loadWinRules();
        if (*rebuild_win_rules) (*rebuild_win_rules)();
        // Wire workspace wallpaper picker to the real file-chooser objects
        *fc_wp_ws_idx = -1;
        *ws_wp_open_fn = [fc_filter_mime, fc_title_w, fc_overlay, fc_current_path,
                          rebuild_fc, fc_wp_ws_idx](int ws_idx) {
            *fc_filter_mime = "image/";
            fc_title_w->setText("Set Wallpaper — Workspace " + std::to_string(ws_idx + 1));
            *fc_wp_ws_idx = ws_idx;
            if (*rebuild_fc) (*rebuild_fc)(*fc_current_path);
            fc_overlay->setVisibility(true);
        };
        // Load and register custom hotkeys
        loadHotkeys();
        if (*rebuild_hotkeys) (*rebuild_hotkeys)();
        // Send stored custom bindings to compositor
        for (const auto& [act, km] : g_hotkey_overrides) {
            HotkeyRequest hkr{};
            hkr.action_id_  = act;
            hkr.keycode_    = km.first;
            hkr.modifiers_  = km.second;
            xbus.Call("com.freebsdx.Shell",
                static_cast<uint32_t>(ShellOpcode::RegisterHotkey),
                {reinterpret_cast<const uint8_t*>(&hkr), sizeof(hkr)}, nullptr);
        }
        // Wire weather forecast 3-day panel
        *forecast_rebuild = [forecast_day_lbls, forecast_icon_lbls, forecast_temp_lbls,
                             &xbus]() {
            static const char* kDayNames[3] = {"Today", "Tomorrow", "In 2 days"};
            for (uint32_t p = 0; p < 3; ++p) {
                uint32_t page = p;
                xbus.Call("com.freebsdx.Weather",
                    static_cast<uint32_t>(WeatherOpcode::GetForecast),
                    {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                    [p, forecast_day_lbls, forecast_icon_lbls, forecast_temp_lbls](
                            const XBusMessage& r) {
                        WeatherForecastPage pg{};
                        std::memcpy(&pg, r.payload_, sizeof(pg));
                        int i = static_cast<int>(p);
                        (*forecast_day_lbls)[i]->setText(kDayNames[i]);
                        (*forecast_icon_lbls)[i]->setText(
                            pg.count_ ? std::string(pg.day_.icon_) : "—");
                        (*forecast_temp_lbls)[i]->setText(
                            pg.count_ ? std::format("↑{}° ↓{}°", pg.day_.max_c_,
                                                    pg.day_.min_c_) : "--");
                    });
            }
        };
        if (*forecast_rebuild) (*forecast_rebuild)();

        if (g_theme_preset >= 0 && g_theme_preset < 5)
            apply_tp(g_theme_preset);
        // Sync DND schedule UI from loaded values
        dnd_sched_switch->setValue(g_dnd_sched_on);
        { char b[4]; std::snprintf(b, sizeof(b), "%02d", g_dnd_sched_start); dnd_from_lbl->setText(b); }
        { char b[4]; std::snprintf(b, sizeof(b), "%02d", g_dnd_sched_end);   dnd_to_lbl->setText(b); }
        // Sync Night Shift schedule UI from loaded values
        ns_sched_switch->setValue(g_ns_sched_on);
        { char b[4]; std::snprintf(b, sizeof(b), "%02d", g_ns_sched_start); ns_from_lbl->setText(b); }
        { char b[4]; std::snprintf(b, sizeof(b), "%02d", g_ns_sched_end);   ns_to_lbl->setText(b); }

        // ── Startup apps — wire panel and launch enabled apps ─────────────────
        *rebuild_startup_apps = [startup_apps_col, apps]() {
            startup_apps_col->clearChildren();
            if (apps.empty()) {
                auto lbl = std::make_shared<TextWidget>("No apps found", 12.0f);
                lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                lbl->setPadding(8);
                startup_apps_col->addChild(lbl);
                return;
            }
            for (const auto& app : apps) {
                std::string id   = app.identifier;
                std::string name = app.name;
                bool enabled = g_startup_apps.count(id) > 0;
                auto sw = std::make_shared<SwitchWidget>(enabled);
                sw->setOnChanged([id](bool on) {
                    if (on) g_startup_apps.insert(id);
                    else    g_startup_apps.erase(id);
                    saveStartupApps();
                });
                auto lbl = std::make_shared<TextWidget>(name, 12.0f);
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    sw
                });
                row->setPadding(4);
                startup_apps_col->addChild(row);
            }
        };
        (*rebuild_startup_apps)();
        // Launch any apps enabled for startup
        for (const auto& app : apps) {
            if (g_startup_apps.count(app.identifier) > 0)
                launchApp(app.executable_path);
        }

        // ── DND priority apps — wire rebuild and load persisted exceptions ────
        loadWpRotate();
        scanWpFolder();
        wp_rotate_switch->setValue(g_wp_rotate_on);
        wp_folder_lbl->setText(g_wp_rotate_folder.empty()
            ? "Folder: (none)" : "Folder: " + g_wp_rotate_folder);
        update_wp_interval_lbl();

        loadDndExceptions();
        *rebuild_dnd_exc = [dnd_exc_col, known_apps]() {
            dnd_exc_col->clearChildren();
            if (known_apps->empty()) {
                auto lbl = std::make_shared<TextWidget>("No apps seen yet", 12.0f);
                lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                lbl->setPadding(8);
                dnd_exc_col->addChild(lbl);
                return;
            }
            for (const auto& aname : *known_apps) {
                std::string an = aname;
                bool starred = g_dnd_exceptions.count(an) > 0;
                auto btn = std::make_shared<ButtonWidget>(starred ? "★" : "☆");
                btn->setFixedSize(28, 28);
                btn->setFontSize(14.0f);
                btn->setBackgroundColor(starred
                    ? Color{0.60f, 0.50f, 0.10f, 1.0f}
                    : Color{0.20f, 0.22f, 0.30f, 1.0f});
                btn->onClick([an, dnd_exc_col, known_apps]() {
                    if (g_dnd_exceptions.count(an))
                        g_dnd_exceptions.erase(an);
                    else
                        g_dnd_exceptions.insert(an);
                    saveDndExceptions();
                    // Rebuild to refresh star state
                    dnd_exc_col->clearChildren();
                    for (const auto& a2 : *known_apps) {
                        bool s2 = g_dnd_exceptions.count(a2) > 0;
                        auto b2 = std::make_shared<ButtonWidget>(s2 ? "★" : "☆");
                        b2->setFixedSize(28, 28);
                        b2->setFontSize(14.0f);
                        b2->setBackgroundColor(s2
                            ? Color{0.60f, 0.50f, 0.10f, 1.0f}
                            : Color{0.20f, 0.22f, 0.30f, 1.0f});
                        std::string a2c = a2;
                        b2->onClick([a2c, dnd_exc_col, known_apps]() {
                            if (g_dnd_exceptions.count(a2c))
                                g_dnd_exceptions.erase(a2c);
                            else
                                g_dnd_exceptions.insert(a2c);
                            saveDndExceptions();
                        });
                        auto lbl2 = std::make_shared<TextWidget>(a2, 12.0f);
                        auto row2 = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            b2,
                            std::make_shared<Spacer>(6, 1),
                            lbl2
                        });
                        row2->setPadding(4);
                        dnd_exc_col->addChild(row2);
                    }
                });
                auto lbl = std::make_shared<TextWidget>(aname, 12.0f);
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    btn,
                    std::make_shared<Spacer>(6, 1),
                    lbl
                });
                row->setPadding(4);
                dnd_exc_col->addChild(row);
            }
        };
        (*rebuild_dnd_exc)();

        // ── Display: build per-output rows ────────────────────────────────────
        auto rebuild_disp = [disp_col](const DisplayOutputList& list) {
            disp_col->clearChildren();
            for (uint32_t i = 0; i < list.count_; ++i) {
                const auto& o = list.outputs_[i];
                // Output header
                char hdr[48];
                std::snprintf(hdr, sizeof(hdr), "%s  %u×%u @ %u Hz",
                              o.name_, o.width_, o.height_, o.refresh_hz_);
                auto name_lbl = std::make_shared<TextWidget>(hdr, 11.0f);
                name_lbl->setColor({0.6f, 0.6f, 0.65f, 1.0f});
                disp_col->addChild(name_lbl);

                // Scale buttons
                auto make_scale_btn = [&o](float sc, const std::string& lbl) {
                    bool active = (std::abs(o.scale_ - sc) < 0.05f);
                    auto btn = std::make_shared<ButtonWidget>(lbl);
                    btn->setFixedSize(64, 28);
                    btn->setFontSize(11.0f);
                    btn->setBackgroundColor(active
                        ? Color{0.25f, 0.50f, 0.85f, 1.0f}
                        : Color{0.20f, 0.22f, 0.30f, 1.0f});
                    uint32_t oi = o.index_;
                    btn->onClick([sc, oi]() {
                        if (!g_shell_xbus) return;
                        DisplayScaleRequest req{};
                        req.output_idx_ = oi;
                        req.scale_       = sc;
                        g_shell_xbus->Call("com.freebsdx.Display",
                            static_cast<uint32_t>(DisplayOpcode::SetOutputScale),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                    });
                    return btn;
                };
                auto scale_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    make_scale_btn(1.0f, "1×"),
                    std::make_shared<Spacer>(4, 1),
                    make_scale_btn(1.5f, "1.5×"),
                    std::make_shared<Spacer>(4, 1),
                    make_scale_btn(2.0f, "2×"),
                });
                disp_col->addChild(std::make_shared<Spacer>(1, 4));
                disp_col->addChild(scale_row);

                // VRR (only if capable)
                if (o.vrr_capable_) {
                    auto vrr_sw = std::make_shared<SwitchWidget>(o.vrr_enabled_ != 0);
                    uint32_t oi2 = o.index_;
                    vrr_sw->setOnChanged([oi2](bool on) {
                        if (!g_shell_xbus) return;
                        DisplayVrrRequest req{};
                        req.output_idx_ = oi2;
                        req.enabled_     = on ? 1u : 0u;
                        g_shell_xbus->Call("com.freebsdx.Display",
                            static_cast<uint32_t>(DisplayOpcode::SetVrrEnabled),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                    });
                    auto vrr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                        std::make_shared<TextWidget>("VRR (FreeSync)", 11.0f),
                        std::make_shared<Spacer>(1, 1, true),
                        vrr_sw
                    });
                    disp_col->addChild(std::make_shared<Spacer>(1, 4));
                    disp_col->addChild(vrr_row);
                }

                // HDR (only if capable)
                if (o.hdr_capable_) {
                    auto hdr_sw = std::make_shared<SwitchWidget>(o.hdr_enabled_ != 0);
                    uint32_t oi3 = o.index_;
                    hdr_sw->setOnChanged([oi3](bool on) {
                        if (!g_shell_xbus) return;
                        DisplayHdrRequest req{};
                        req.output_idx_    = oi3;
                        req.enabled_       = on ? 1u : 0u;
                        req.max_luminance_ = 1000u;
                        g_shell_xbus->Call("com.freebsdx.Display",
                            static_cast<uint32_t>(DisplayOpcode::SetHdrEnabled),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                    });
                    auto hdr_row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                        std::make_shared<TextWidget>("HDR", 11.0f),
                        std::make_shared<Spacer>(1, 1, true),
                        hdr_sw
                    });
                    disp_col->addChild(std::make_shared<Spacer>(1, 4));
                    disp_col->addChild(hdr_row);
                }

                if (i + 1 < list.count_)
                    disp_col->addChild(std::make_shared<DividerWidget>(0.25f));
            }
            if (list.count_ == 0) {
                auto lbl = std::make_shared<TextWidget>("No outputs detected", 12.0f);
                lbl->setColor({0.45f, 0.45f, 0.5f, 1.0f});
                disp_col->addChild(lbl);
            }
        };

        xbus.Call("com.freebsdx.Display",
            static_cast<uint32_t>(DisplayOpcode::QueryOutputs), {},
            [rebuild_disp, qs_hdr_switch, qs_vrr_switch,
             qs_hdr_lbl, qs_vrr_lbl, qs_disp_row](const XBusMessage& r) {
                DisplayOutputList list{};
                std::memcpy(&list, r.payload_, sizeof(list));
                rebuild_disp(list);
                // Wire QS HDR/VRR quick row for primary output (index 0)
                if (list.count_ == 0) return;
                const auto& o = list.outputs_[0];
                bool has_hdr = o.hdr_capable_ != 0;
                bool has_vrr = o.vrr_capable_ != 0;
                if (!has_hdr && !has_vrr) return;
                qs_disp_row->setVisibility(true);
                if (has_hdr) {
                    qs_hdr_switch->setValue(o.hdr_enabled_ != 0);
                    uint32_t oi = o.index_;
                    qs_hdr_switch->setOnChanged([oi](bool on) {
                        if (!g_shell_xbus) return;
                        DisplayHdrRequest req{};
                        req.output_idx_    = oi;
                        req.enabled_       = on ? 1u : 0u;
                        req.max_luminance_ = 1000u;
                        g_shell_xbus->Call("com.freebsdx.Display",
                            static_cast<uint32_t>(DisplayOpcode::SetHdrEnabled),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                    });
                } else {
                    qs_hdr_lbl->setColor({0.35f, 0.35f, 0.40f, 1.0f});
                    qs_hdr_switch->setValue(false);
                }
                if (has_vrr) {
                    qs_vrr_switch->setValue(o.vrr_enabled_ != 0);
                    uint32_t oi = o.index_;
                    qs_vrr_switch->setOnChanged([oi](bool on) {
                        if (!g_shell_xbus) return;
                        DisplayVrrRequest req{};
                        req.output_idx_ = oi;
                        req.enabled_     = on ? 1u : 0u;
                        g_shell_xbus->Call("com.freebsdx.Display",
                            static_cast<uint32_t>(DisplayOpcode::SetVrrEnabled),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)}, nullptr);
                    });
                } else {
                    qs_vrr_lbl->setColor({0.35f, 0.35f, 0.40f, 1.0f});
                    qs_vrr_switch->setValue(false);
                }
            });

        // Audio player for notification chime (kept alive for the session)
        auto audio = std::make_shared<AudioPlayer>(xbus);

        if (load_tray && *load_tray) (*load_tray)(0);

        // ── Wallpaper: restore persisted path + fit mode ─────────────────────
        xbus.Call("com.freebsdx.Wallpaper",
            static_cast<uint32_t>(WallpaperOpcode::GetWallpaper), {},
            [wallpaper_img_slot](const XBusMessage& resp) {
                WallpaperSetRequest r{};
                std::memcpy(&r, resp.payload_, sizeof(r));
                if (r.path_[0]) {
                    static const ImageFitMode kFitMap[] = {
                        ImageFitMode::Cover, ImageFitMode::Contain,
                        ImageFitMode::Stretch, ImageFitMode::Center
                    };
                    auto fit = kFitMap[r.fit_mode_ < 4 ? r.fit_mode_ : 0];
                    auto img = std::make_shared<ImageWidget>(std::string(r.path_));
                    img->setFixedSize(10000.0f, 10000.0f);
                    img->setFitMode(fit);
                    wallpaper_img_slot->addChild(img);
                }
            });

        // ── Updates: check once on connect ───────────────────────────────────
        xbus.Call("com.freebsdx.Update",
            static_cast<uint32_t>(UpdateOpcode::CheckForUpdates), {},
            [update_upd_btn](const XBusMessage& r) {
                UpdateAvailableState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                update_upd_btn(st.count_);
            });

        // ── Lock: fetch initial timeout to sync auto-lock toggle ──────────────
        xbus.Call("com.freebsdx.Lock",
            static_cast<uint32_t>(LockOpcode::GetTimeout), {},
            [qs_autolock_switch](const XBusMessage& r) {
                LockTimeoutRequest rep{};
                std::memcpy(&rep, r.payload_, sizeof(rep));
                qs_autolock_switch->setValue(rep.seconds_ > 0);
            });

        // ── Battery: fetch initial state ─────────────────────────────────────
        xbus.Call("com.freebsdx.Power",
            static_cast<uint32_t>(PowerOpcode::GetPowerState), {},
            [update_battery](const XBusMessage& r) {
                PowerState ps{};
                std::memcpy(&ps, r.payload_, sizeof(ps));
                (*update_battery)(ps);
            });

        // ── Notification muted apps: fetch initial list ───────────────────────
        xbus.Call("com.freebsdx.Notification",
            static_cast<uint32_t>(NotificationOpcode::GetMutedApps), {},
            [muted_apps, rebuild_app_list](const XBusMessage& r) {
                NotifMutedAppList list{};
                std::memcpy(&list, r.payload_, sizeof(list));
                muted_apps->clear();
                for (uint32_t i = 0; i < list.count_ && i < 8; ++i)
                    muted_apps->insert(std::string(list.names_[i]));
                if (*rebuild_app_list) (*rebuild_app_list)();
            });

        // ── Notes: load first page for action center mini-grid ───────────────
        {
            NotesPageRequest notes_req{};
            notes_req.page_ = 0;
            xbus.Call("com.freebsdx.Notes",
                static_cast<uint32_t>(NotesOpcode::List),
                {reinterpret_cast<const uint8_t*>(&notes_req), sizeof(notes_req)},
                [rebuild_notes_mini](const XBusMessage& r) {
                    NotesListPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    std::vector<NoteEntry> entries;
                    if (pg.count_ > 0) entries.push_back(pg.entry_);
                    if (*rebuild_notes_mini) (*rebuild_notes_mini)(entries);
                });
        }

        // ── Night Shift: fetch initial state ─────────────────────────────────
        xbus.Call("com.freebsdx.NightShift",
            static_cast<uint32_t>(NightShiftOpcode::GetState), {},
            [qs_ns_switch, qs_ns_slider, qs_ns_val](const XBusMessage& r) {
                NightShiftState st{};
                std::memcpy(&st, r.payload_, sizeof(st));
                qs_ns_switch->setValue(st.enabled_ != 0);
                qs_ns_slider->setValue(static_cast<float>(st.intensity_));
                qs_ns_val->setText(std::to_string(st.intensity_) + "%");
            });

        // ── Weather: fetch initial state + wire button ────────────────────────
        xbus.Call("com.freebsdx.Weather",
            static_cast<uint32_t>(WeatherOpcode::GetCurrent), {},
            [weather_btn](const XBusMessage& r) {
                const auto* w = reinterpret_cast<const WeatherCondition*>(r.payload_);
                if (w->temp_c_ != 0 || w->condition_[0] != '\0') {
                    char buf[24];
                    std::snprintf(buf, sizeof(buf), "%s %d°", w->icon_, w->temp_c_);
                    weather_btn->setText(buf);
                }
            });
        weather_btn->onClick([]() {
            if (fork() == 0) {
                setenv("XBUS_SOCKET", "/tmp/freebsdx.sock", 1);
                if (access("./out/weather", X_OK) == 0)
                    execl("./out/weather", "weather", nullptr);
                execlp("weather", "weather", nullptr);
                exit(1);
            }
        });

        // ── Clipboard history: load initial entries ──────────────────────────
        auto rebuild_ch = std::make_shared<std::function<void()>>();
        *rebuild_ch = [ch_entries, ch_list_col_ptr, ch_overlay, rebuild_clip_ac,
                       ch_selected_text, ch_xform_status, &xbus]() {
            auto lc_base = *ch_list_col_ptr;
            if (!lc_base) return;
            auto lc = std::dynamic_pointer_cast<FlexColumn>(lc_base);
            if (!lc) return;
            lc->clearChildren();

            if (ch_entries->empty()) {
                auto empty_lbl = std::make_shared<TextWidget>("No clipboard history", 13.0f);
                empty_lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                empty_lbl->setPadding(16);
                lc->addChild(empty_lbl);
                return;
            }

            // Detect whether a clipboard entry is an image file path
            auto is_image_path = [](const std::string& t) -> bool {
                if (t.empty() || t[0] != '/') return false;
                if (t.find('\n') != std::string::npos) return false;
                const char* exts[] = { ".png", ".jpg", ".jpeg", ".gif",
                                       ".webp", ".bmp", nullptr };
                for (int i = 0; exts[i]; ++i) {
                    size_t el = std::strlen(exts[i]);
                    if (t.size() >= el &&
                        t.compare(t.size() - el, el, exts[i]) == 0)
                        return true;
                }
                return false;
            };

            for (const auto& [id, text] : *ch_entries) {
                std::string entry_text = text;
                auto paste_and_close = [entry_text, ch_overlay, ch_selected_text,
                                        ch_xform_status, &xbus]() {
                    *ch_selected_text = entry_text;
                    ch_xform_status->setText("");
                    char buf[256] = {};
                    std::strncpy(buf, entry_text.c_str(), sizeof(buf) - 1);
                    xbus.Call("com.freebsdx.Clipboard", 0x01,
                              {reinterpret_cast<const uint8_t*>(buf), sizeof(buf)},
                              [](const XBusMessage&){});
                    ch_overlay->setVisibility(false);
                };

                if (is_image_path(text)) {
                    // Render a thumbnail with the file path below it
                    auto thumb = std::make_shared<ImageWidget>(text);
                    thumb->setFixedSize(0, 100);
                    thumb->setFitMode(ImageFitMode::Contain);

                    auto path_lbl = std::make_shared<TextWidget>(
                        text.substr(text.rfind('/') + 1), 11.0f);
                    path_lbl->setColor(0.55f, 0.55f, 0.6f, 1.0f);
                    path_lbl->setPadding(4);

                    auto card_col = std::make_shared<FlexColumn>(
                        std::vector<std::shared_ptr<Widget>>{ thumb, path_lbl });
                    card_col->setBackgroundColor({0.14f, 0.15f, 0.21f, 1.0f});

                    auto row = std::make_shared<ButtonWidget>(card_col, paste_and_close);
                    row->setPadding(0);
                    lc->addChild(row);
                } else {
                    // Plain text preview — first 60 chars, newlines → spaces
                    std::string preview;
                    size_t count = 0;
                    for (char c : text) {
                        if (count >= 60) { preview += "..."; break; }
                        preview += (c == '\n') ? ' ' : c;
                        ++count;
                    }
                    auto row = std::make_shared<ButtonWidget>(preview, paste_and_close);
                    row->setFixedSize(0, 42);
                    row->setPadding(14);
                    row->setFontSize(13.0f);
                    lc->addChild(row);
                }
            }
            // Keep the action-center clipboard panel in sync
            if (*rebuild_clip_ac) (*rebuild_clip_ac)();
        };

        // Wire rebuild_clip_ac — compact view of the first 10 ch_entries
        *rebuild_clip_ac = [clip_ac_col, ch_entries, action_center, &xbus]() {
            clip_ac_col->clearChildren();
            if (ch_entries->empty()) {
                auto lbl = std::make_shared<TextWidget>("No clipboard history", 12.0f);
                lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                lbl->setPadding(8);
                clip_ac_col->addChild(lbl);
                return;
            }
            int shown = 0;
            for (const auto& [id, text] : *ch_entries) {
                if (shown >= 10) break;
                std::string entry_text = text;
                std::string preview;
                size_t count = 0;
                for (char c : entry_text) {
                    if (count >= 35) { preview += "\u2026"; break; }
                    preview += (c == '\n') ? ' ' : c;
                    ++count;
                }
                auto btn = std::make_shared<ButtonWidget>(preview,
                    [entry_text, action_center, &xbus]() {
                        char buf[256] = {};
                        std::strncpy(buf, entry_text.c_str(), sizeof(buf) - 1);
                        xbus.Call("com.freebsdx.Clipboard", 0x01,
                            {reinterpret_cast<const uint8_t*>(buf), sizeof(buf)},
                            [](const XBusMessage&){});
                        action_center->setVisibility(false);
                    });
                btn->setFixedSize(0, 36);
                btn->setPadding(10);
                btn->setFontSize(12.0f);
                clip_ac_col->addChild(btn);
                ++shown;
            }
        };

        // Wire Clear All button now that xbus is available
        ch_clear_btn->onClick([ch_entries, ch_list_col_ptr, ch_overlay, rebuild_ch, &xbus]() {
            xbus.Call("com.freebsdx.ClipboardHistory",
                      static_cast<uint32_t>(ClipboardHistoryOpcode::Clear), {},
                      [ch_entries, rebuild_ch](const XBusMessage&) {
                          ch_entries->clear();
                          if (*rebuild_ch) (*rebuild_ch)();
                      });
        });

        // Initial load of clipboard history
        auto load_ch_page = std::make_shared<std::function<void(uint32_t)>>();
        *load_ch_page = [ch_entries, rebuild_ch, load_ch_page, &xbus](uint32_t page) {
            ClipHistoryPageRequest req{};
            req.page_ = page;
            xbus.Call("com.freebsdx.ClipboardHistory",
                      static_cast<uint32_t>(ClipboardHistoryOpcode::GetPage),
                      {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                      [page, ch_entries, rebuild_ch, load_ch_page](const XBusMessage& ack) {
                          ClipHistoryPage pg{};
                          std::memcpy(&pg, ack.payload_, sizeof(pg));
                          if (pg.valid_) {
                              pg.entry_.text_[sizeof(pg.entry_.text_) - 1] = '\0';
                              ch_entries->emplace_back(pg.entry_.id_,
                                                       std::string(pg.entry_.text_));
                          }
                          if (pg.has_more_ && *load_ch_page)
                              (*load_ch_page)(page + 1);
                          else if (*rebuild_ch)
                              (*rebuild_ch)();
                      });
        };
        (*load_ch_page)(0);

        auto reload_ch = std::make_shared<std::function<void()>>();
        *reload_ch = [ch_entries, load_ch_page]() {
            ch_entries->clear();
            if (*load_ch_page) (*load_ch_page)(0);
        };

        // Wire rebuild_screen_time — per-app usage rows from g_app_usage_secs
        *rebuild_screen_time = [screen_time_col]() {
            screen_time_col->clearChildren();
            std::map<std::string, uint32_t> snapshot;
            {
                std::lock_guard<std::mutex> ul(g_usage_mtx);
                snapshot = g_app_usage_secs;
                // Add running-app elapsed time
                std::lock_guard<std::mutex> lk(g_child_pids_mtx);
                std::time_t now = std::time(nullptr);
                for (const auto& [pid, name] : g_child_pids) {
                    auto lt = g_child_launch_times.find(pid);
                    if (lt != g_child_launch_times.end())
                        snapshot[name] += static_cast<uint32_t>(now - lt->second);
                }
            }
            if (snapshot.empty()) {
                auto lbl = std::make_shared<TextWidget>("No usage recorded yet", 12.0f);
                lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                lbl->setPadding(8);
                screen_time_col->addChild(lbl);
                return;
            }
            // Sort descending by usage
            std::vector<std::pair<std::string,uint32_t>> sorted(snapshot.begin(), snapshot.end());
            std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b){ return a.second > b.second; });
            for (const auto& [name, secs] : sorted) {
                auto name_lbl = std::make_shared<TextWidget>(name, 12.0f);
                auto time_lbl = std::make_shared<TextWidget>(fmtUsage(secs), 12.0f);
                time_lbl->setColor({0.5f, 0.8f, 0.5f, 1.0f});
                auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                    name_lbl,
                    std::make_shared<Spacer>(1, 1, true),
                    time_lbl
                });
                row->setPadding(3);
                screen_time_col->addChild(row);
            }
        };
        (*rebuild_screen_time)();

        // Wire rebuild_audio_vol — one slider row per active audio stream
        *rebuild_audio_vol = [audio_vol_col]() {
            if (!g_shell_xbus) return;
            uint32_t page = 0;
            g_shell_xbus->Call("com.freebsdx.Audio",
                static_cast<uint32_t>(AudioOpcode::ListStreams),
                {reinterpret_cast<const uint8_t*>(&page), sizeof(page)},
                [audio_vol_col](const XBusMessage& r) {
                    AudioStreamPage pg{};
                    std::memcpy(&pg, r.payload_, sizeof(pg));
                    audio_vol_col->clearChildren();
                    if (!pg.valid_) {
                        auto lbl = std::make_shared<TextWidget>("No active streams", 12.0f);
                        lbl->setColor(0.4f, 0.4f, 0.4f, 1.0f);
                        lbl->setPadding(8);
                        audio_vol_col->addChild(lbl);
                        return;
                    }
                    // Iterate through all pages to collect streams
                    // For simplicity, add this stream and request next page if more
                    auto add_stream = [audio_vol_col](const AudioStreamInfo& s) {
                        std::string name = s.app_name_[0]
                            ? std::string(s.app_name_) : "Unknown";
                        uint32_t sid = s.stream_id_;
                        auto vol_lbl = std::make_shared<TextWidget>(
                            std::format("{}%", s.volume_), 11.0f);
                        vol_lbl->setColor({0.7f, 0.7f, 0.75f, 1.0f});
                        auto slider = std::make_shared<SliderWidget>(
                            static_cast<float>(s.volume_), 0.0f, 100.0f);
                        slider->setFixedSize(140, 20);
                        slider->setOnChanged([sid, vol_lbl](float v) {
                            int iv = static_cast<int>(v);
                            vol_lbl->setText(std::format("{}%", iv));
                            if (!g_shell_xbus) return;
                            AudioAppVolumeRequest req{};
                            req.stream_id_ = sid;
                            req.volume_    = static_cast<uint32_t>(iv);
                            g_shell_xbus->Call("com.freebsdx.Audio",
                                static_cast<uint32_t>(AudioOpcode::SetAppVolume),
                                {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                                nullptr);
                        });
                        auto name_lbl = std::make_shared<TextWidget>(name, 12.0f);
                        auto row = std::make_shared<FlexRow>(std::vector<std::shared_ptr<Widget>>{
                            name_lbl,
                            std::make_shared<Spacer>(1, 1, true),
                            slider,
                            std::make_shared<Spacer>(4, 1),
                            vol_lbl
                        });
                        row->setPadding(3);
                        audio_vol_col->addChild(row);
                    };
                    add_stream(pg.stream_);
                });
        };
        (*rebuild_audio_vol)();

        // ── Color picker: HSV→RGB helper and update lambda ────────────────────
        auto hsv_to_rgb = [](float h, float s, float v,
                             float& r, float& g, float& b) {
            float c = v * s;
            float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
            float m = v - c;
            if      (h < 60)  { r=c+m; g=x+m; b=m;   }
            else if (h < 120) { r=x+m; g=c+m; b=m;   }
            else if (h < 180) { r=m;   g=c+m; b=x+m; }
            else if (h < 240) { r=m;   g=x+m; b=c+m; }
            else if (h < 300) { r=x+m; g=m;   b=c+m; }
            else              { r=c+m; g=m;   b=x+m; }
        };

        *update_cp = [=, &hsv_to_rgb]() {
            float r = 0, g = 0, b = 0;
            hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);

            // Swatch background
            cp_swatch->setBackgroundColor({r, g, b, 1.0f});

            // Hex label
            uint8_t ri = static_cast<uint8_t>(r * 255.0f);
            uint8_t gi = static_cast<uint8_t>(g * 255.0f);
            uint8_t bi = static_cast<uint8_t>(b * 255.0f);
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", ri, gi, bi);
            cp_hex_lbl->setText(hex);

            // RGB label
            char rgb_buf[32];
            std::snprintf(rgb_buf, sizeof(rgb_buf), "rgb(%d, %d, %d)", ri, gi, bi);
            cp_rgb_lbl->setText(rgb_buf);

            // Slider labels
            char hlbl[12], slbl[12], vlbl[12];
            std::snprintf(hlbl, sizeof(hlbl), "H %3d", static_cast<int>(*cp_h));
            std::snprintf(slbl, sizeof(slbl), "S %3d%%", static_cast<int>(*cp_s * 100));
            std::snprintf(vlbl, sizeof(vlbl), "V %3d%%", static_cast<int>(*cp_v * 100));
            cp_h_lbl->setText(hlbl);
            cp_s_lbl->setText(slbl);
            cp_v_lbl->setText(vlbl);
        };

        // Initialise display with default values
        (*update_cp)();

        // Copy HEX to clipboard
        cp_copy_hex_btn->onClick([cp_h, cp_s, cp_v, cp_recent, cp_recent_row,
                                  &xbus, &hsv_to_rgb]() {
            float r = 0, g = 0, b = 0;
            hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);
            uint8_t ri = static_cast<uint8_t>(r * 255.0f);
            uint8_t gi = static_cast<uint8_t>(g * 255.0f);
            uint8_t bi = static_cast<uint8_t>(b * 255.0f);
            char hex[8];
            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", ri, gi, bi);

            // Write to clipboard
            xbus.Call("com.freebsdx.Clipboard", 0x01,
                      {reinterpret_cast<const uint8_t*>(hex), std::strlen(hex) + 1},
                      [](const XBusMessage&){});

            // Add to recent
            std::array<float,3> col = {r, g, b};
            auto it = std::find(cp_recent->begin(), cp_recent->end(), col);
            if (it != cp_recent->end()) cp_recent->erase(it);
            cp_recent->insert(cp_recent->begin(), col);
            if (cp_recent->size() > 8) cp_recent->resize(8);

            // Rebuild recent swatches
            cp_recent_row->clearChildren();
            for (const auto& c : *cp_recent) {
                auto sw = std::make_shared<FlexRow>();
                sw->setFixedSize(24, 24);
                sw->setBackgroundColor({c[0], c[1], c[2], 1.0f});
                cp_recent_row->addChild(sw);
            }
        });

        // Copy RGB to clipboard
        cp_copy_rgb_btn->onClick([cp_h, cp_s, cp_v, &xbus, &hsv_to_rgb]() {
            float r = 0, g = 0, b = 0;
            hsv_to_rgb(*cp_h, *cp_s, *cp_v, r, g, b);
            uint8_t ri = static_cast<uint8_t>(r * 255.0f);
            uint8_t gi = static_cast<uint8_t>(g * 255.0f);
            uint8_t bi = static_cast<uint8_t>(b * 255.0f);
            char rgb_buf[32];
            std::snprintf(rgb_buf, sizeof(rgb_buf), "rgb(%d, %d, %d)", ri, gi, bi);

            xbus.Call("com.freebsdx.Clipboard", 0x01,
                      {reinterpret_cast<const uint8_t*>(rgb_buf), std::strlen(rgb_buf) + 1},
                      [](const XBusMessage&){});
        });

        xbus.OnBroadcast([ch_overlay, ch_entries, ch_list_col_ptr, rebuild_ch, reload_ch,
                          cp_overlay,
                          search_portal, action_center,
                          notif_history, notif_list_col, notif_empty_lbl, make_notif_card,
                          ws_pips, switcher_chips, switcher_panel,
                          tbar_chips, tbar_ids, apps,
                          net_btn, qp_conn_w, qp_ip_w, qp_signal_w, qp_disc_btn,
                          do_scan_page, wifi_connect_col,
                          rebuild_bt_devices, update_bt_ui,
                          rebuild_usb,
                          ss_status_lbl, ss_last_path, ss_open_btn,
                          ss_recording, rec_toggle_btn, rec_status_lbl,
                          vpn_status_lbl, vpn_info_lbl, vpn_conn_btn, vpn_disc_btn, update_vpn_ui,
                          update_rd_ui, rebuild_print,
                          update_hs_ui,
                          update_focus_ui,
                          update_cast_ui,
                          rebuild_pkg,
                          update_ab_ui,
                          rebuild_svc,
                          update_gm_ui,
                          rebuild_sec,
                          update_a11y_ui,
                          update_cf_ui,
                          rebuild_lsess,
                          update_kb_bl_ui,
                          rebuild_bdrain,
                          update_ap_ui,
                          rebuild_cron,
                          update_loc_ui,
                          rebuild_aout,
                          update_stout_ui,
                          update_rot_ui,
                          update_mic_mute_ui,
                          rebuild_startup,
                          rebuild_syslog,
                          rebuild_dl,
                          update_auto_br_ui,
                          rebuild_sshkey,
                          update_rrate_ui,
                          rebuild_envvar,
                          update_scroll_dir_ui,
                          update_pres_ui,
                          update_mem_ui,
                          update_touchpad_ui,
                          rebuild_print_queue,
                          update_reduce_motion_ui,
                          rebuild_pkg_upd,
                          update_cursor_size_ui,
                          rebuild_zfs,
                          update_hotspot_ui,
                          rebuild_jails,
                          update_disp_scale_ui,
                          rebuild_perms,
                          update_dev_overlay_ui,
                          update_ft_ui,
                          update_color_prof_ui,
                          rebuild_im,
                          update_stylus_ui,
                          rebuild_captures,
                          update_ntp_ui,
                          rebuild_be,
                          update_upd_ch_ui,
                          update_text_size_ui,
                          rebuild_kmod,
                          update_tz_ui,
                          update_rl_ui,
                          update_a11y_ui_b,
                          update_proxy_ui,
                          update_dns_ui,
                          rebuild_audio_routes,
                          update_wl_ui,
                          rebuild_shares,
                          rebuild_vms,
                          rebuild_containers,
                          update_nl_ui,
                          rebuild_crash,
                          update_privacy_ui,
                          update_fw_ui,
                          update_pp_ui,
                          mp_playing, update_mp_ui, rebuild_storage,
                          qs_wifi_switch, qs_wifi_sub, qs_bt_switch, bt_btn,
                          qs_dark_switch,
                          load_tray, weather_btn,
                          fc_overlay, fc_request_id, fc_current_path, fc_filter_mime,
                          fc_save_mode, fc_title_w, fc_ok_btn, fc_save_field, fc_sel_w,
                          fc_selected, rebuild_fc,
                          wallpaper_img_slot, audio,
                          show_osd, notif_unread, notif_badge_lbl, notif_badge_box,
                          update_battery, shortcuts_overlay,
                          qs_ns_switch, qs_ns_slider, qs_ns_val,
                          known_apps, muted_apps, rebuild_app_list,
                          disp_col, rebuild_disp, qs_dnd_switch,
                          update_upd_btn, qs_autolock_switch,
                          notes_overlay, rebuild_notes_mini,
                          rebuild_dnd_exc,
                          rebuild_screen_time,
                          rebuild_audio_vol,
                          win_rules_col, rebuild_win_rules,
                          hotkey_status_lbl, rebuild_hotkeys,
                          ws_wp_update_hl,
                          forecast_rebuild,
                          live_titles,
                          dnd_countdown_lbl,
                          kb_active_lbl,
                          rebuild_tz, rebuild_usbdev,
                          rebuild_slog, rebuild_ns, update_dns_cfg_ui,
                          update_nsch_ui, rebuild_sandbox,
                          update_slk_ui, rebuild_btaudio,
                          update_loc_policy, rebuild_loc, rebuild_totp,
                          update_eq_ui, rebuild_bkp,
                          update_ptr_ui, rebuild_rem,
                          update_sa_ui, rebuild_xfr,
                          update_sndfx_ui, rebuild_gc,
                          update_captions_ui, rebuild_swifi,
                          update_dsaver_ui, rebuild_seckey,
                          update_fastchg_ui, rebuild_txtrep,
                          update_silence_ui, rebuild_applimit,
                          update_bhealth_ui, rebuild_notifsnd,
                          update_ttone_ui, rebuild_litem,
                          update_ostore_ui, rebuild_tdev,
                          update_monoaud_ui, rebuild_kbsc,
                          update_stagemgr_ui, rebuild_dapp,
                          arch_status_lbl, iapps_prog_lbl, rdp_status_lbl, rebuild_cam_dev,
                          rebuild_clip, rebuild_disk_usage, rebuild_disp_arr, rebuild_file_sync,
                          rebuild_fonts, rebuild_iapps, rebuild_indev, rebuild_keychain, rebuild_rdp,
                          rebuild_scap, rebuild_seats, rebuild_store, rebuild_term_sess, rebuild_uev,
                          unlock_inputs_row, unlock_locked, unlock_status_lbl, update_a11y_ui_c,
                          update_dock_ui, update_gest_ui, update_gpu_ui, update_locale_ui,
                          update_pkgsign_ui, update_ss_ui, update_xwl_ui,
                          store_search_input](const XBusMessage& msg)
        {
            // Per-workspace wallpaper change — update background if it's the active workspace
            if (std::strcmp(msg.service_name_, "com.freebsdx.Wallpaper") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WallpaperOpcode::SetWorkspaceWallpaper)) {
                WallpaperWorkspaceRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                if (static_cast<int>(req.workspace_idx_) == g_current_ws) {
                    wallpaper_img_slot->clearChildren();
                    if (req.path_[0]) {
                        static const ImageFitMode kFitMap[] = {
                            ImageFitMode::Cover, ImageFitMode::Contain,
                            ImageFitMode::Stretch, ImageFitMode::Center
                        };
                        auto fit = kFitMap[req.fit_mode_ < 4 ? req.fit_mode_ : 0];
                        auto img = std::make_shared<ImageWidget>(std::string(req.path_));
                        img->setFixedSize(10000.0f, 10000.0f);
                        img->setFitMode(fit);
                        wallpaper_img_slot->addChild(img);
                    }
                }
                return;
            }

            // Wallpaper change — swap image in the background layer
            if (std::strcmp(msg.service_name_, "com.freebsdx.Wallpaper") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WallpaperOpcode::SetWallpaper)) {
                WallpaperSetRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));
                wallpaper_img_slot->clearChildren();
                if (req.path_[0]) {
                    static const ImageFitMode kFitMap[] = {
                        ImageFitMode::Cover, ImageFitMode::Contain,
                        ImageFitMode::Stretch, ImageFitMode::Center
                    };
                    auto fit = kFitMap[req.fit_mode_ < 4 ? req.fit_mode_ : 0];
                    auto img = std::make_shared<ImageWidget>(std::string(req.path_));
                    img->setFixedSize(10000.0f, 10000.0f);
                    img->setFitMode(fit);
                    wallpaper_img_slot->addChild(img);
                }
                return;
            }

            if (std::strcmp(msg.service_name_, "com.freebsdx.Tray") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrayOpcode::TrayChanged)) {
                if (load_tray && *load_tray) (*load_tray)(0);
                return;
            }

            // Bluetooth state changes — update qs_bt_switch, bt_btn, and device list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Bluetooth") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceFound))) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Bluetooth",
                        static_cast<uint32_t>(BluetoothOpcode::GetState), {},
                        [qs_bt_switch, bt_btn](const XBusMessage& r) {
                            BluetoothState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            qs_bt_switch->setValue(st.enabled_ != 0);
                            bt_btn->setText(st.enabled_ ? "🔵" : "BT");
                        });
                    if (rebuild_bt_devices && *rebuild_bt_devices) (*rebuild_bt_devices)();
                }
                return;
            }

            // Network status changes — update taskbar indicator + quick panel header
            if (std::strcmp(msg.service_name_, "com.freebsdx.Network") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkOpcode::NetworkChanged)) {
                NetworkStatus n{};
                std::memcpy(&n, msg.payload_, sizeof(n));
                // Update taskbar button text
                if (!n.is_connected_) {
                    net_btn->setText("✕");
                    qp_conn_w->setText("○ Not connected");
                    qp_ip_w->setText("");
                    qp_signal_w->setText("");
                    qp_disc_btn->setVisibility(false);
                } else {
                    std::string bars = n.current_ssid_[0]
                        ? signalBars(n.signal_strength_)
                        : "⇄";
                    std::string iface_label = n.current_ssid_[0]
                        ? std::string(n.current_ssid_)
                        : "ETH";
                    net_btn->setText(bars + " " + iface_label);
                    qp_conn_w->setText("● " + iface_label);
                    qp_ip_w->setText(std::string(n.ip_address_));
                    if (n.current_ssid_[0])
                        qp_signal_w->setText(
                            signalBars(n.signal_strength_) +
                            std::format("  {} dBm", n.signal_strength_));
                    else
                        qp_signal_w->setText("");
                    qp_disc_btn->setVisibility(true);
                }
                // Update Wi-Fi switch, sub-label, and tracked SSID
                g_wifi_ssid = n.is_connected_ && n.current_ssid_[0]
                    ? std::string(n.current_ssid_) : "";
                qs_wifi_switch->setValue(n.is_connected_);
                qs_wifi_sub->setText(n.is_connected_ ? std::string(n.ip_address_) : "Not connected");
                // Refresh scan list so active network highlights update
                if (do_scan_page && *do_scan_page) (*do_scan_page)(0);
                return;
            }

            // Notification XBusMessages have the service name set
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notification") == 0) {
                if (msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::Post)) {
                    NotificationPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    // DND: suppress unless app is in priority exceptions list
                    if (g_dnd && g_dnd_exceptions.find(std::string(payload.app_name_)) == g_dnd_exceptions.end()) {
                        ++g_dnd_suppressed_count;
                        {
                            std::lock_guard<std::mutex> dlk(g_dnd_digest_mtx);
                            if (payload.app_name_[0])
                                ++g_dnd_suppressed_apps[std::string(payload.app_name_)];
                        }
                        return;
                    }
                    audio->playChime();
                    // Badge: increment unread count on the notification bell
                    ++(*notif_unread);
                    notif_badge_lbl->setText(std::to_string(*notif_unread));
                    notif_badge_box->setVisibility(true);

                    // Timestamp
                    auto t = std::chrono::system_clock::to_time_t(
                        std::chrono::system_clock::now());
                    char ts_buf[8];
                    std::strftime(ts_buf, sizeof(ts_buf), "%H:%M", std::localtime(&t));

                    auto card = make_notif_card(payload.id_,
                        std::string(payload.title_), std::string(payload.message_), ts_buf,
                        std::string(payload.app_name_));

                    // Cap history at 50 — drop oldest visible card from list
                    if (notif_history->size() >= 50) {
                        auto& oldest = notif_history->front();
                        oldest.card->setVisibility(false);
                        notif_history->erase(notif_history->begin());
                    }

                    NotifEntry entry;
                    entry.id       = payload.id_;
                    entry.title    = payload.title_;
                    entry.message  = payload.message_;
                    entry.time_str = ts_buf;
                    entry.card     = card;
                    notif_history->push_back(entry);

                    // Append to scroll list (newest at bottom)
                    notif_list_col->addChild(card);
                    notif_empty_lbl->setVisibility(false);
                    action_center->setVisibility(true);

                    // Track app name for per-app notification settings and DND exceptions
                    if (payload.app_name_[0]) {
                        std::string aname(payload.app_name_);
                        bool found = false;
                        for (const auto& a : *known_apps)
                            if (a == aname) { found = true; break; }
                        if (!found) {
                            known_apps->push_back(aname);
                            if (*rebuild_app_list) (*rebuild_app_list)();
                            if (*rebuild_dnd_exc)  (*rebuild_dnd_exc)();
                        }
                    }

                } else if (msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::Dismiss)) {
                    uint32_t id = 0;
                    std::memcpy(&id, msg.payload_, sizeof(id));
                    for (auto& e : *notif_history) {
                        if (e.id == id) { e.dismissed = true; e.card->setVisibility(false); break; }
                    }
                    bool any = false;
                    for (const auto& e : *notif_history) if (!e.dismissed) { any = true; break; }
                    notif_empty_lbl->setVisibility(!any);
                }
                return;
            }

            // Shell service broadcasts
            if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") == 0) {
                if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::LaunchWithFile)) {
                    ShellLaunchRequest req{};
                    std::memcpy(&req, msg.payload_, sizeof(req));
                    std::string app_id(req.app_id_);
                    std::string file_path(req.file_path_);
                    for (const auto& app : apps) {
                        if (app.identifier == app_id) {
                            launchApp(app.executable_path, file_path);
                            // Record in recent files
                            if (!file_path.empty()) {
                                std::string fname = file_path.substr(
                                    file_path.find_last_of("/\\") + 1);
                                g_recent_files.erase(
                                    std::remove_if(g_recent_files.begin(), g_recent_files.end(),
                                        [&file_path](const auto& e){ return e.second == file_path; }),
                                    g_recent_files.end());
                                g_recent_files.insert(g_recent_files.begin(), {fname, file_path});
                                if (g_recent_files.size() > 20) g_recent_files.resize(20);
                                saveRecentFiles();
                            }
                            break;
                        }
                    }
                    return;
                }
                if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::WorkspaceChanged)) {
                    WorkspaceChangedPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    for (int i = 0; i < kNumWorkspaces; ++i) {
                        (*ws_pips)[i]->setColor(
                            i == static_cast<int>(payload.index_)
                                ? Color{1.0f, 1.0f, 1.0f, 1.0f}
                                : Color{0.35f, 0.35f, 0.4f, 1.0f});
                    }
                    // Track current workspace, update WS wallpaper strip highlight, load wallpaper
                    g_current_ws = static_cast<int>(payload.index_);
                    if (*ws_wp_update_hl) (*ws_wp_update_hl)();
                    if (g_shell_xbus) {
                        WallpaperWorkspaceRequest ws_req{};
                        ws_req.workspace_idx_ = payload.index_;
                        g_shell_xbus->Call("com.freebsdx.Wallpaper",
                            static_cast<uint32_t>(WallpaperOpcode::GetWorkspaceWallpaper),
                            {reinterpret_cast<const uint8_t*>(&ws_req), sizeof(ws_req)},
                            [wallpaper_img_slot](const XBusMessage& resp) {
                                WallpaperWorkspaceRequest r{};
                                std::memcpy(&r, resp.payload_, sizeof(r));
                                wallpaper_img_slot->clearChildren();
                                if (r.path_[0]) {
                                    static const ImageFitMode kFitMap[] = {
                                        ImageFitMode::Cover, ImageFitMode::Contain,
                                        ImageFitMode::Stretch, ImageFitMode::Center
                                    };
                                    auto fit = kFitMap[r.fit_mode_ < 4 ? r.fit_mode_ : 0];
                                    auto img = std::make_shared<ImageWidget>(std::string(r.path_));
                                    img->setFixedSize(10000.0f, 10000.0f);
                                    img->setFitMode(fit);
                                    wallpaper_img_slot->addChild(img);
                                }
                            });
                    }
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::SwitcherUpdate)) {
                    SwitcherPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    for (int i = 0; i < kMaxSwitcherWindows; ++i) {
                        if (i < static_cast<int>(payload.count_)) {
                            auto& chip = (*switcher_chips)[i];
                            chip.surface_id = payload.entries_[i].surface_id_;
                            chip.label->setText(std::string(payload.entries_[i].title_));
                            bool focused = (payload.entries_[i].surface_id_ == payload.focused_id_);
                            chip.card->setBackgroundColor(focused
                                ? Color{0.22f, 0.44f, 0.88f, 0.97f}
                                : Color{0.2f, 0.2f, 0.28f, 0.95f});
                            chip.card->setVisibility(true);
                        } else {
                            (*switcher_chips)[i].card->setVisibility(false);
                        }
                    }
                    switcher_panel->setVisibility(true);
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::SwitcherDismiss)) {
                    for (auto& chip : *switcher_chips) chip.card->setVisibility(false);
                    switcher_panel->setVisibility(false);
                } else if (msg.opcode_ == static_cast<uint32_t>(ShellOpcode::WindowListUpdate)) {
                    WindowListPayload payload{};
                    std::memcpy(&payload, msg.payload_, sizeof(payload));
                    // Update live title list for session save
                    live_titles->clear();
                    for (uint32_t ti = 0; ti < payload.count_ && ti < 7; ++ti)
                        if (payload.entries_[ti].title_[0])
                            live_titles->emplace_back(payload.entries_[ti].title_);
                    for (int i = 0; i < kMaxTaskbarWindows; ++i) {
                        if (i < static_cast<int>(payload.count_)) {
                            (*tbar_ids)[i] = payload.entries_[i].surface_id_;
                            (*tbar_chips)[i].label->setText(
                                std::string(payload.entries_[i].title_));
                            (*tbar_chips)[i].card->setVisibility(true);

                            // Apply window rules to newly-seen surfaces
                            uint32_t sid = payload.entries_[i].surface_id_;
                            if (sid && g_rule_applied_ids.find(sid) == g_rule_applied_ids.end()) {
                                g_rule_applied_ids.insert(sid);
                                std::string title(payload.entries_[i].title_);
                                std::string title_lc = title;
                                std::transform(title_lc.begin(), title_lc.end(),
                                               title_lc.begin(), ::tolower);
                                for (const auto& rule : g_win_rules) {
                                    std::string kw = rule.title_match;
                                    std::transform(kw.begin(), kw.end(), kw.begin(), ::tolower);
                                    if (title_lc.find(kw) == std::string::npos) continue;
                                    if (rule.maximize && g_shell_xbus)
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::Maximize),
                                            {reinterpret_cast<const uint8_t*>(&sid), sizeof(sid)},
                                            nullptr);
                                    if (rule.always_on_top && g_shell_xbus) {
                                        WinAlwaysOnTopRequest atr{};
                                        atr.surface_id_ = sid; atr.enabled_ = 1u;
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::SetAlwaysOnTop),
                                            {reinterpret_cast<const uint8_t*>(&atr), sizeof(atr)},
                                            nullptr);
                                    }
                                    if (rule.workspace >= 0 && g_shell_xbus) {
                                        WinWorkspaceRequest wsr{};
                                        wsr.surface_id_    = sid;
                                        wsr.workspace_idx_ = static_cast<uint32_t>(rule.workspace);
                                        g_shell_xbus->Call("com.freebsdx.Window",
                                            static_cast<uint32_t>(WindowOpcode::SetWorkspace),
                                            {reinterpret_cast<const uint8_t*>(&wsr), sizeof(wsr)},
                                            nullptr);
                                    }
                                    break; // first matching rule wins
                                }
                            }
                        } else {
                            (*tbar_ids)[i] = 0;
                            (*tbar_chips)[i].card->setVisibility(false);
                        }
                    }
                    // Prune stale surface IDs (keep set bounded)
                    if (g_rule_applied_ids.size() > 256) {
                        auto it = g_rule_applied_ids.begin();
                        std::advance(it, g_rule_applied_ids.size() - 128);
                        g_rule_applied_ids.erase(g_rule_applied_ids.begin(), it);
                    }
                }
                return;
            }

            // File Chooser — service asks xshell to present a native picker
            if (std::strcmp(msg.service_name_, "com.freebsdx.FileChooser") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FileChooserOpcode::Show)) {
                FileChooserRequest req{};
                std::memcpy(&req, msg.payload_, sizeof(req));

                *fc_request_id = req.request_id_;
                *fc_filter_mime = req.filter_mime_;
                *fc_save_mode   = (req.save_mode_ != 0);

                fc_title_w->setText(req.title_[0] ? std::string(req.title_)
                                                   : (*fc_save_mode ? "Save File" : "Open File"));
                fc_ok_btn->setText(*fc_save_mode ? "Save" : "Open");
                fc_save_field->setVisibility(*fc_save_mode);
                fc_sel_w->setVisibility(!*fc_save_mode);

                std::string start = req.initial_path_[0]
                    ? std::string(req.initial_path_)
                    : (std::getenv("HOME") ? std::string(std::getenv("HOME")) : "/");
                *fc_current_path = start;
                *fc_selected = "";
                fc_sel_w->setText("");

                if (rebuild_fc && *rebuild_fc) (*rebuild_fc)(start);
                fc_overlay->setVisibility(true);
                return;
            }

            // Reset idle counter on actual user input (key, mouse, touch, scroll)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Shell") == 0) {
                const auto* im = reinterpret_cast<const InputMessage*>(msg.payload_);
                if (im->type_ == InputEventType::MouseMove   ||
                    im->type_ == InputEventType::MouseButton ||
                    im->type_ == InputEventType::KeyPress    ||
                    im->type_ == InputEventType::MouseWheel) {
                    g_idle_ticks.store(0);
                }
            }

            // Raw InputMessage broadcasts (SearchToggle, DashboardToggle, ThemeChanged …)
            const auto* input = reinterpret_cast<const InputMessage*>(msg.payload_);
            // Key capture for hotkey remapper — intercept before other handlers
            if (input->type_ == InputEventType::KeyPress && input->value_b_ == 1) {
                int cap = g_hotkey_capture_action.load();
                if (cap >= 0 && cap < kNumHotkeys) {
                    g_hotkey_capture_action.store(-1);
                    uint32_t kc   = static_cast<uint32_t>(input->value_a_);
                    uint32_t mods = input->modifiers_;
                    uint32_t aid  = kHotkeyDefs[cap].action_id;
                    g_hotkey_overrides[aid] = {kc, mods};
                    saveHotkeys();
                    hotkey_status_lbl->setText("Bound: " + fmtHotkey(kc, mods));
                    if (*rebuild_hotkeys) (*rebuild_hotkeys)();
                    if (g_shell_xbus) {
                        HotkeyRequest hkr{};
                        hkr.action_id_  = aid;
                        hkr.keycode_    = kc;
                        hkr.modifiers_  = mods;
                        g_shell_xbus->Call("com.freebsdx.Shell",
                            static_cast<uint32_t>(ShellOpcode::RegisterHotkey),
                            {reinterpret_cast<const uint8_t*>(&hkr), sizeof(hkr)}, nullptr);
                    }
                }
            }

            if (input->type_ == InputEventType::ThemeChanged) {
                g_accent_idx = static_cast<uint32_t>(input->value_b_);
                qs_dark_switch->setValue(input->value_a_ != 0);
            } else if (input->type_ == InputEventType::SearchToggle) {
                search_portal->setVisibility(!search_portal->isVisible());
            } else if (input->type_ == InputEventType::DashboardToggle) {
                action_center->setVisibility(!action_center->isVisible());
            } else if (input->type_ == InputEventType::ClipboardHistoryToggle) {
                ch_overlay->setVisibility(!ch_overlay->isVisible());
            } else if (input->type_ == InputEventType::ColorPickerToggle) {
                cp_overlay->setVisibility(!cp_overlay->isVisible());
            } else if (input->type_ == InputEventType::ShortcutHelpToggle) {
                shortcuts_overlay->setVisibility(!shortcuts_overlay->isVisible());
            } else if (input->type_ == InputEventType::QuickNoteToggle) {
                notes_overlay->setVisibility(!notes_overlay->isVisible());
            }
            // Notes changed broadcast — reload mini-grid in action center
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notes") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotesOpcode::NoteChanged)) {
                if (g_shell_xbus) {
                    NotesPageRequest notes_req{};
                    notes_req.page_ = 0;
                    g_shell_xbus->Call("com.freebsdx.Notes",
                        static_cast<uint32_t>(NotesOpcode::List),
                        {reinterpret_cast<const uint8_t*>(&notes_req), sizeof(notes_req)},
                        [rebuild_notes_mini](const XBusMessage& r) {
                            NotesListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));
                            std::vector<NoteEntry> entries;
                            if (pg.count_ > 0) entries.push_back(pg.entry_);
                            if (*rebuild_notes_mini) (*rebuild_notes_mini)(entries);
                        });
                }
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.ClipboardHistory") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ClipboardHistoryOpcode::HistoryChanged)) {
                if (*reload_ch) (*reload_ch)();
            }
            // Screenshot taken — update action center panel + show notification
            if (std::strcmp(msg.service_name_, "com.freebsdx.Screenshot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenshotOpcode::ScreenshotTaken)) {
                ScreenshotResponse resp{};
                std::memcpy(&resp, msg.payload_, sizeof(resp));
                if (resp.success_) {
                    // Update action center status
                    *ss_last_path = std::string(resp.path_);
                    std::string fname = *ss_last_path;
                    auto sl = fname.find_last_of("/\\");
                    if (sl != std::string::npos) fname = fname.substr(sl + 1);
                    ss_status_lbl->setText(fname);
                    ss_open_btn->setVisibility(true);
                    if (g_shell_xbus) {
                        NotificationPayload notif{};
                        notif.timeout_ms_ = 4000;
                        std::strncpy(notif.title_,   "Screenshot saved",  sizeof(notif.title_) - 1);
                        std::strncpy(notif.message_,  resp.path_,         sizeof(notif.message_) - 1);
                        std::strncpy(notif.app_name_, "Screenshot",       sizeof(notif.app_name_) - 1);
                        g_shell_xbus->Call("com.freebsdx.Notification",
                            static_cast<uint32_t>(NotificationOpcode::Post),
                            {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)}, nullptr);
                    }
                } else {
                    ss_status_lbl->setText("Capture failed");
                }
                return;
            }

            // Recording state changed — update toggle button and status label
            if (std::strcmp(msg.service_name_, "com.freebsdx.Screenshot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenshotOpcode::RecordingChanged)) {
                ScreenshotRecordingState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                *ss_recording = st.recording_ != 0;
                rec_toggle_btn->setText(*ss_recording ? "■ Stop" : "⏺ Record");
                rec_toggle_btn->setBackgroundColor(*ss_recording
                    ? Color{0.6f, 0.12f, 0.12f, 1.0f}
                    : Color{0.32f, 0.12f, 0.12f, 1.0f});
                if (*ss_recording) {
                    rec_status_lbl->setText(std::format("● {} fps", st.fps_));
                } else {
                    // Recording stopped — show output path filename
                    if (st.path_[0]) {
                        std::string fp = std::string(st.path_);
                        auto sl = fp.find_last_of("/\\");
                        if (sl != std::string::npos) fp = fp.substr(sl + 1);
                        rec_status_lbl->setText("Saved: " + fp);
                    } else {
                        rec_status_lbl->setText("");
                    }
                }
                return;
            }

            // Media player state changed — update QS panel controls
            if (std::strcmp(msg.service_name_, "com.freebsdx.MediaPlayer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MediaPlayerOpcode::StateChanged)) {
                MediaPlayerState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_mp_ui(st);
                return;
            }

            // Disk topology or mount changed — refresh storage quick-view
            if (std::strcmp(msg.service_name_, "com.freebsdx.Disk") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DiskOpcode::DiskChanged)) {
                if (rebuild_storage && *rebuild_storage) (*rebuild_storage)();
                return;
            }

            // VPN tunnel changed — requery state and update UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.VPN") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(VpnOpcode::VpnChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.VPN",
                        static_cast<uint32_t>(VpnOpcode::GetStatus), {},
                        [update_vpn_ui](const XBusMessage& r) {
                            VpnStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_vpn_ui(st);
                        });
                }
                return;
            }

            // Focus Mode session started, updated (remaining time tick), or ended
            if (std::strcmp(msg.service_name_, "com.freebsdx.FocusMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FocusModeOpcode::StateChanged)) {
                FocusModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_focus_ui(st);
                return;
            }

            // Cast session started, stopped, or receiver changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Cast") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CastOpcode::StateChanged)) {
                CastState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cast_ui(st);
                return;
            }

            // Package update started or finished — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Packages") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(PackageOpcode::UpdateStarted) ||
                 msg.opcode_ == static_cast<uint32_t>(PackageOpcode::UpdateFinished))) {
                if (rebuild_pkg && *rebuild_pkg) (*rebuild_pkg)();
                return;
            }

            // Ad-Blocker status changed (enabled/disabled or list refresh completed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.AdBlock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AdBlockOpcode::StatusChanged)) {
                AdBlockStatus st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ab_ui(st);
                return;
            }

            // Service daemon started or stopped
            if (std::strcmp(msg.service_name_, "com.freebsdx.Services") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ServiceOpcode::ServiceChanged)) {
                if (rebuild_svc && *rebuild_svc) (*rebuild_svc)();
                return;
            }

            // Accessibility settings changed from another app or schedule
            if (std::strcmp(msg.service_name_, "com.freebsdx.A11ySettings") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AccessibilitySettingsOpcode::SettingsChanged)) {
                AccessibilityQuickSettings st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_a11y_ui(st);
                return;
            }

            // Download progress update / completion / failure — refresh list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Downloads") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadProgress) ||
                 msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadComplete) ||
                 msg.opcode_ == static_cast<uint32_t>(DownloadOpcode::DownloadFailed))) {
                if (rebuild_dl && *rebuild_dl) (*rebuild_dl)();
                return;
            }

            // Game Mode toggled or foreground app changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.GameMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GameModeOpcode::StateChanged)) {
                GameModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_gm_ui(st);
                return;
            }

            // New security event (SSH / sudo / login / fail)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Security") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SecurityOpcode::EventAdded)) {
                if (rebuild_sec && *rebuild_sec) (*rebuild_sec)();
                return;
            }

            // Night Light state changed (schedule triggered or user toggled from another app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.NightLight") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NightLightOpcode::StateChanged)) {
                NightLightState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_nl_ui(st);
                return;
            }

            // New core dump arrived — refresh crash reporter list
            if (std::strcmp(msg.service_name_, "com.freebsdx.CrashReporter") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CrashOpcode::ReportAdded)) {
                if (rebuild_crash && *rebuild_crash) (*rebuild_crash)();
                return;
            }

            // Hotspot AP started/stopped or a client associated/disassociated
            if (std::strcmp(msg.service_name_, "com.freebsdx.Hotspot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(HotspotOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Hotspot",
                        static_cast<uint32_t>(HotspotOpcode::GetStatus), {},
                        [update_hs_ui](const XBusMessage& r) {
                            HotspotStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_hs_ui(st);
                        });
                }
                return;
            }

            // Remote Desktop server started/stopped or client connected/disconnected
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteDesktop") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteDesktopOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.RemoteDesktop",
                        static_cast<uint32_t>(RemoteDesktopOpcode::GetStatus), {},
                        [update_rd_ui](const XBusMessage& r) {
                            RemoteDesktopStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_rd_ui(st);
                        });
                }
                return;
            }

            // Bluetooth adapter toggled or device connection changed — refresh toggle + list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Bluetooth") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(BluetoothOpcode::DeviceFound))) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Bluetooth",
                        static_cast<uint32_t>(BluetoothOpcode::GetState), {},
                        [update_bt_ui](const XBusMessage& r) {
                            BluetoothState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_bt_ui(st);
                        });
                }
                return;
            }

            // USB device plugged or unplugged — refresh monitor
            if (std::strcmp(msg.service_name_, "com.freebsdx.Usb") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(UsbOpcode::DeviceAdded) ||
                 msg.opcode_ == static_cast<uint32_t>(UsbOpcode::DeviceRemoved))) {
                if (rebuild_usb && *rebuild_usb) (*rebuild_usb)();
                return;
            }

            // Power profile changed — sync toggle UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.PowerProfile") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PowerProfileOpcode::ProfileChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.PowerProfile",
                        static_cast<uint32_t>(PowerProfileOpcode::GetProfile), {},
                        [update_pp_ui](const XBusMessage& r) {
                            PowerProfileState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_pp_ui(st);
                        });
                }
                return;
            }

            // Camera or microphone access state changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Privacy") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PrivacyOpcode::DeviceStateChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Privacy",
                        static_cast<uint32_t>(PrivacyOpcode::GetStatus), {},
                        [update_privacy_ui](const XBusMessage& r) {
                            PrivacyStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_privacy_ui(st);
                        });
                }
                return;
            }

            // Firewall enabled/disabled or rules changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Firewall") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FirewallOpcode::StatusChanged)) {
                if (g_shell_xbus) {
                    g_shell_xbus->Call("com.freebsdx.Firewall",
                        static_cast<uint32_t>(FirewallOpcode::GetStatus), {},
                        [update_fw_ui](const XBusMessage& r) {
                            FirewallStatus st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_fw_ui(st);
                        });
                }
                return;
            }

            // Volume changed (keyboard keys or another client) — show OSD
            if (std::strcmp(msg.service_name_, "com.freebsdx.Audio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioOpcode::VolumeChanged)) {
                AudioVolumeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                const std::string icon = st.muted_ ? "🔇" : "🔊";
                (*show_osd)(icon, static_cast<int>(st.volume_));
                if (*rebuild_audio_vol) (*rebuild_audio_vol)();
            }

            // Power state changed — update battery indicator
            if (std::strcmp(msg.service_name_, "com.freebsdx.Power") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PowerOpcode::PowerChanged)) {
                PowerState ps{};
                std::memcpy(&ps, msg.payload_, sizeof(ps));
                (*update_battery)(ps);
            }

            // Updates available — refresh badge and post one notification
            if (std::strcmp(msg.service_name_, "com.freebsdx.Update") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UpdateOpcode::UpdatesAvailable)) {
                UpdateAvailableState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_upd_btn(st.count_);
                if (st.count_ > 0 && g_shell_xbus) {
                    NotificationPayload notif{};
                    notif.timeout_ms_ = 6000;
                    std::strncpy(notif.title_, "Software Updates Available",
                                 sizeof(notif.title_) - 1);
                    char body[128];
                    std::snprintf(body, sizeof(body),
                                  "%u package%s can be updated. Click ⬆ to open App Manager.",
                                  st.count_, st.count_ == 1 ? "" : "s");
                    std::strncpy(notif.message_,  body,          sizeof(notif.message_) - 1);
                    std::strncpy(notif.app_name_, "App Manager", sizeof(notif.app_name_) - 1);
                    g_shell_xbus->Call("com.freebsdx.Notification",
                        static_cast<uint32_t>(NotificationOpcode::Post),
                        {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                        nullptr);
                }
            }

            // Lock state changed — sync auto-lock toggle + admin unlock row visibility
            if (std::strcmp(msg.service_name_, "com.freebsdx.Lock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LockOpcode::LockStateChanged)) {
                LockState ls{};
                std::memcpy(&ls, msg.payload_, sizeof(ls));
                qs_autolock_switch->setValue(ls.timeout_secs_ > 0);
                *unlock_locked = (ls.locked_ != 0);
                unlock_inputs_row->setVisibility(*unlock_locked);
                if (!*unlock_locked) {
                    // Clear credentials and status on successful unlock
                    unlock_status_lbl->setText("");
                    unlock_status_lbl->setVisibility(false);
                }
            }

            // Muted apps list changed — re-sync local set and rebuild UI
            if (std::strcmp(msg.service_name_, "com.freebsdx.Notification") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotificationOpcode::MutedAppsChanged)) {
                NotifMutedAppList list{};
                std::memcpy(&list, msg.payload_, sizeof(list));
                muted_apps->clear();
                for (uint32_t i = 0; i < list.count_ && i < 8; ++i)
                    muted_apps->insert(std::string(list.names_[i]));
                if (*rebuild_app_list) (*rebuild_app_list)();
            }

            // Display output changed — rebuild display settings panel
            if (std::strcmp(msg.service_name_, "com.freebsdx.Display") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayOpcode::OutputChanged)) {
                DisplayOutputList list{};
                std::memcpy(&list, msg.payload_, sizeof(list));
                rebuild_disp(list);
            }

            // Night Shift state changed — sync QS panel controls
            if (std::strcmp(msg.service_name_, "com.freebsdx.NightShift") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NightShiftOpcode::StateChanged)) {
                NightShiftState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                qs_ns_switch->setValue(st.enabled_ != 0);
                qs_ns_slider->setValue(static_cast<float>(st.intensity_));
                qs_ns_val->setText(std::to_string(st.intensity_) + "%");
            }

            // Weather update — refresh taskbar button text and 3-day forecast
            if (std::strcmp(msg.service_name_, "com.freebsdx.Weather") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WeatherOpcode::WeatherChanged)) {
                const auto* w = reinterpret_cast<const WeatherCondition*>(msg.payload_);
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%s %d°", w->icon_, w->temp_c_);
                weather_btn->setText(buf);
                if (*forecast_rebuild) (*forecast_rebuild)();
            }

            // Keyboard layout confirmed by compositor
            if (std::strcmp(msg.service_name_, "com.freebsdx.Keymap") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeymapOpcode::LayoutChanged)) {
                KeymapRequest kr{};
                std::memcpy(&kr, msg.payload_, sizeof(kr));
                for (int i = 0; i < kNumLayouts; ++i) {
                    if (std::strcmp(kr.layout_, kLayouts[i].code) == 0) {
                        g_keymap_idx = i;
                        kb_active_lbl->setText(kLayouts[i].label);
                        break;
                    }
                }
            }

            // Color filter toggled, type or intensity changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.ColorFilter") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ColorFilterOpcode::StateChanged)) {
                ColorFilterState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cf_ui(st);
                return;
            }

            // Login session opened or closed — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Sessions") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SessionOpcode::SessionChanged)) {
                if (rebuild_lsess && *rebuild_lsess) (*rebuild_lsess)();
                return;
            }

            // Keyboard backlight brightness changed (Fn key or external tool)
            if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardBacklight") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeyboardBacklightOpcode::StateChanged)) {
                KeyboardBacklightState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_kb_bl_ui(st);
                return;
            }

            // Battery drain sample refreshed — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.BatteryDrain") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BatteryDrainOpcode::DrainChanged)) {
                if (rebuild_bdrain && *rebuild_bdrain) (*rebuild_bdrain)();
                return;
            }

            // Airplane mode toggled — sync switch + Wi-Fi/BT switches
            if (std::strcmp(msg.service_name_, "com.freebsdx.AirplaneMode") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AirplaneModeOpcode::StateChanged)) {
                AirplaneModeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ap_ui(st);
                return;
            }

            // Crontab edited externally or job toggled — refresh list
            if (std::strcmp(msg.service_name_, "com.freebsdx.CronJobs") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CronOpcode::JobChanged)) {
                if (rebuild_cron && *rebuild_cron) (*rebuild_cron)();
                return;
            }

            // Location grant/revoke — update QS switch + requester count
            if (std::strcmp(msg.service_name_, "com.freebsdx.LocationServices") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LocationOpcode::StateChanged)) {
                LocationState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_loc_ui(st);
                return;
            }

            // Mic mute toggled (by any client) — sync QS switch + status label
            if (std::strcmp(msg.service_name_, "com.freebsdx.MicMute") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MicMuteOpcode::StateChanged)) {
                MicMuteState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_mic_mute_ui(st);
                return;
            }

            // Startup items changed — rebuild list
            if (std::strcmp(msg.service_name_, "com.freebsdx.StartupItems") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StartupOpcode::ItemsChanged)) {
                if (rebuild_startup && *rebuild_startup) (*rebuild_startup)();
                return;
            }

            // Rotation lock toggled or sensor auto-rotated — sync QS row
            if (std::strcmp(msg.service_name_, "com.freebsdx.RotationLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RotationLockOpcode::StateChanged)) {
                RotationLockState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rot_ui(st);
                return;
            }

            // Audio sink plugged/unplugged — rebuild output pill row
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioOutput") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioOutputOpcode::SinkChanged)) {
                if (rebuild_aout && *rebuild_aout) (*rebuild_aout)();
                return;
            }

            // Screen timeout changed from Settings — sync QS preset buttons
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenTimeout") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenTimeoutOpcode::TimeoutChanged)) {
                ScreenTimeoutState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stout_ui(st);
                return;
            }

            // Auto-brightness toggled or sensor reading updated — sync QS row
            if (std::strcmp(msg.service_name_, "com.freebsdx.AutoBrightness") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AutoBrightnessOpcode::StateChanged)) {
                AutoBrightnessState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_auto_br_ui(st);
                return;
            }

            // SSH key removed — rebuild AC list
            if (std::strcmp(msg.service_name_, "com.freebsdx.SshKeys") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SshKeyOpcode::KeysChanged)) {
                if (rebuild_sshkey && *rebuild_sshkey) (*rebuild_sshkey)();
                return;
            }

            // Display refresh rate changed (hotplug or Settings) — sync preset buttons
            if (std::strcmp(msg.service_name_, "com.freebsdx.RefreshRate") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RefreshRateOpcode::RateChanged)) {
                RefreshRateState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rrate_ui(st);
                return;
            }

            // Env var set/unset — rebuild AC list
            if (std::strcmp(msg.service_name_, "com.freebsdx.EnvVars") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(EnvVarOpcode::EnvChanged)) {
                if (rebuild_envvar && *rebuild_envvar) (*rebuild_envvar)();
                return;
            }

            // Scroll direction changed from Settings — sync QS switch
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScrollDir") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScrollDirOpcode::DirChanged)) {
                ScrollDirState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_scroll_dir_ui(st);
                return;
            }

            // Presentation mode changed (e.g. ended by compositor on screen unlock)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Presentation") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PresentationOpcode::StateChanged)) {
                PresentationState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_pres_ui(st);
                return;
            }

            // Touchpad enabled/disabled externally (e.g. keyboard shortcut)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Touchpad") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TouchpadOpcode::StateChanged)) {
                TouchpadState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_touchpad_ui(st);
                return;
            }

            // Print job state changed — re-poll queue
            if (std::strcmp(msg.service_name_, "com.freebsdx.PrintQueue") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PrintOpcode::QueueChanged)) {
                if (rebuild_print_queue && *rebuild_print_queue) (*rebuild_print_queue)();
                return;
            }

            // Reduce motion toggled externally (e.g. Accessibility settings)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ReduceMotion") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ReduceMotionOpcode::StateChanged)) {
                ReduceMotionState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_reduce_motion_ui(st);
                return;
            }

            // Package database refreshed — re-poll update list
            if (std::strcmp(msg.service_name_, "com.freebsdx.PkgUpdates") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PkgOpcode::UpdatesChanged)) {
                if (rebuild_pkg_upd && *rebuild_pkg_upd) (*rebuild_pkg_upd)();
                return;
            }

            // Cursor size changed externally (e.g. Accessibility settings)
            if (std::strcmp(msg.service_name_, "com.freebsdx.CursorSize") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CursorSizeOpcode::SizeChanged)) {
                CursorSizeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_cursor_size_ui(st);
                return;
            }

            // ZFS pool state changed — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.ZfsPools") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ZfsOpcode::PoolsChanged)) {
                if (rebuild_zfs && *rebuild_zfs) (*rebuild_zfs)();
                return;
            }

            // Hotspot state changed (client joined/left or toggled externally)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Hotspot") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(HotspotToggleOpcode::StateChanged)) {
                HotspotState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_hotspot_ui(st);
                return;
            }

            // Jail started or stopped — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Jails") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(JailOpcode::JailsChanged)) {
                if (rebuild_jails && *rebuild_jails) (*rebuild_jails)();
                return;
            }

            // Display scale changed (e.g. from Settings or external display hotplug)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DisplayScale") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayScaleOpcode::ScaleChanged)) {
                DisplayScaleState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_disp_scale_ui(st);
                return;
            }

            // App permission granted or revoked
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppPerms") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppPermOpcode::PermsChanged)) {
                if (rebuild_perms && *rebuild_perms) (*rebuild_perms)();
                return;
            }

            // Dev overlay mode changed externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.DevOverlay") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DevOverlayOpcode::StateChanged)) {
                DevOverlayState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dev_overlay_ui(st);
                return;
            }

            // Color profile changed externally (e.g. via Settings app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ColorProfile") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ColorProfileOpcode::ProfileChanged)) {
                ColorProfileState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_color_prof_ui(st);
                return;
            }

            // Input method switched — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.InputMethod") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ImOpcode::MethodChanged)) {
                if (rebuild_im && *rebuild_im) (*rebuild_im)();
                return;
            }

            // Stylus connected/disconnected or settings changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Stylus") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StylusOpcode::StateChanged)) {
                StylusState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stylus_ui(st);
                return;
            }

            // New screenshot/recording taken or capture deleted — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.Captures") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CaptureOpcode::CapturesChanged)) {
                if (rebuild_captures && *rebuild_captures) (*rebuild_captures)();
                return;
            }

            // NTP sync state changed (stratum gained/lost, server switched)
            if (std::strcmp(msg.service_name_, "com.freebsdx.NTP") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NtpOpcode::StateChanged)) {
                NtpState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ntp_ui(st);
                return;
            }

            // Boot environment list changed — re-poll
            if (std::strcmp(msg.service_name_, "com.freebsdx.BootEnv") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BeOpcode::BEChanged)) {
                if (rebuild_be && *rebuild_be) (*rebuild_be)();
                return;
            }

            // Update channel changed (may carry new pending count)
            if (std::strcmp(msg.service_name_, "com.freebsdx.UpdateChannel") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UpdateChannelOpcode::ChannelChanged)) {
                UpdateChannelState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_upd_ch_ui(st);
                return;
            }

            // Text size changed externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.TextSize") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TextSizeOpcode::SizeChanged)) {
                TextSizeState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_text_size_ui(st);
                return;
            }

            // Kernel module loaded or unloaded — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.KernelModules") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KmodOpcode::ModulesChanged)) {
                if (rebuild_kmod && *rebuild_kmod) (*rebuild_kmod)();
                return;
            }

            // Time zone changed (user pick or system event)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Timezone") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TimezoneOpcode::TZChanged)) {
                TimezoneState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_tz_ui(st);
                return;
            }

            // sshd started, stopped, or session count changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteLogin") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteLoginOpcode::StateChanged)) {
                RemoteLoginState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_rl_ui(st);
                return;
            }

            // Accessibility feature toggled externally (e.g. keyboard shortcut)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Accessibility") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(A11yOpcode::StateChanged)) {
                A11yState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_a11y_ui_b(st);
                return;
            }

            // Proxy config changed (network switch, user edit, or MDM push)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Proxy") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ProxyOpcode::ConfigChanged)) {
                ProxyConfig st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_proxy_ui(st);
                return;
            }

            // DNS resolver changed (DHCP update, user pick, or network switch)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DNS") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DnsOpcode::ConfigChanged)) {
                DnsConfig st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dns_ui(st);
                return;
            }

            // Audio routing changed — app started/stopped or device plugged/unplugged
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioRouting") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioRoutingOpcode::RoutesChanged)) {
                if (rebuild_audio_routes && *rebuild_audio_routes) (*rebuild_audio_routes)();
                return;
            }

            // Wake lock expired or was cancelled externally
            if (std::strcmp(msg.service_name_, "com.freebsdx.WakeLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(WakeLockOpcode::StateChanged)) {
                WakeLockState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_wl_ui(st);
                return;
            }

            // Network share mounted or unmounted — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.NetworkShares") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkShareOpcode::SharesChanged)) {
                if (rebuild_shares && *rebuild_shares) (*rebuild_shares)();
                return;
            }

            // VM state transition — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.VirtualMachines") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(VmOpcode::VMChanged)) {
                if (rebuild_vms && *rebuild_vms) (*rebuild_vms)();
                return;
            }

            // Container state transition — re-poll list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Containers") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ContainerOpcode::ContainersChanged)) {
                if (rebuild_containers && *rebuild_containers) (*rebuild_containers)();
                return;
            }

            // Dock position or auto-hide changed (e.g. from Settings app)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Dock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DockOpcode::SettingsChanged)) {
                DockSettings st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dock_ui(st);
                return;
            }

            // Async disk usage scan finished — re-fetch sorted results
            if (std::strcmp(msg.service_name_, "com.freebsdx.DiskUsage") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DiskUsageOpcode::ScanComplete)) {
                if (rebuild_disk_usage && *rebuild_disk_usage) (*rebuild_disk_usage)();
                return;
            }

            // System locale changed (login.conf update or user preference)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Locale") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LocaleOpcode::LocaleChanged)) {
                LocaleState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_locale_ui(st);
                return;
            }

            // File sync job state change (sync started/finished/paused/errored)
            if (std::strcmp(msg.service_name_, "com.freebsdx.FileSync") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FileSyncOpcode::JobsChanged)) {
                if (rebuild_file_sync && *rebuild_file_sync) (*rebuild_file_sync)();
                return;
            }

            // Screen saver config changed (another client called SetConfig)
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenSaver") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenSaverOpcode::StateChanged)) {
                ScreenSaverState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ss_ui(st.timeout_secs_, st.mode_);
                return;
            }

            // Keychain item added, updated, or removed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Keychain") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeychainOpcode::Changed)) {
                if (rebuild_keychain && *rebuild_keychain) (*rebuild_keychain)();
                return;
            }

            // XWayland started or stopped
            if (std::strcmp(msg.service_name_, "com.freebsdx.XWayland") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(XWaylandOpcode::StatusChanged)) {
                XWaylandStatus st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_xwl_ui(st);
                return;
            }

            // Camera device hotplug (new device connected or removed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Camera") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CameraOpcode::DeviceChanged)) {
                if (rebuild_cam_dev && *rebuild_cam_dev) (*rebuild_cam_dev)();
                return;
            }

            // Package signing policy or trust store changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.PackageSigning") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PackageSigningOpcode::SigningChanged)) {
                g_shell_xbus->Call("com.freebsdx.PackageSigning",
                    static_cast<uint32_t>(PackageSigningOpcode::GetPolicy), {},
                    [update_pkgsign_ui](const XBusMessage& r) {
                        PkgSigningPolicy pol{};
                        std::memcpy(&pol, r.payload_, sizeof(pol));
                        update_pkgsign_ui(pol);
                    });
                return;
            }

            // App catalog refreshed — rebuild store list
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppManagerCatalogOpcode::CatalogUpdated)) {
                if (rebuild_store && *rebuild_store)
                    (*rebuild_store)(store_search_input->getText());
                return;
            }

            // Terminal session list changed (create/destroy/rename)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Terminal") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TerminalOpcode::SessionChanged)) {
                if (rebuild_term_sess && *rebuild_term_sess) (*rebuild_term_sess)();
                return;
            }

            // Font list changed after install or remove
            if (std::strcmp(msg.service_name_, "com.freebsdx.FontManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FontManagerOpcode::FontsChanged)) {
                if (rebuild_fonts && *rebuild_fonts) (*rebuild_fonts)();
                return;
            }

            // App install / uninstall progress
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppManagerOpcode::InstallProgress)) {
                AppInstallProgress prog{};
                std::memcpy(&prog, msg.payload_, sizeof(prog));
                if (prog.status_ == 0) {
                    // In-progress — show progress banner
                    iapps_prog_lbl->setText(
                        std::format("{} {}%", std::string(prog.message_), prog.percent_));
                    iapps_prog_lbl->setVisibility(true);
                } else {
                    // Completed or failed — rebuild the list
                    iapps_prog_lbl->setVisibility(false);
                    if (rebuild_iapps && *rebuild_iapps) (*rebuild_iapps)();
                }
                return;
            }

            // Seat topology changed (new seat attached or removed)
            if (std::strcmp(msg.service_name_, "com.freebsdx.Seat") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SeatOpcode::SeatChanged)) {
                if (rebuild_seats && *rebuild_seats) (*rebuild_seats)();
                return;
            }

            // Archive operation completed
            if (std::strcmp(msg.service_name_, "com.freebsdx.Archive") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ArchiveOpcode::OperationDone)) {
                ArchiveStatusResponse resp{};
                std::memcpy(&resp, msg.payload_, sizeof(resp));
                if (resp.success_) {
                    arch_status_lbl->setText(
                        std::format("\xe2\x9c\x93 {}", std::string(resp.message_)));
                    arch_status_lbl->setColor({0.30f, 0.75f, 0.45f, 1.0f});
                } else {
                    arch_status_lbl->setText(
                        std::format("\xe2\x9c\x97 {}", std::string(resp.message_)));
                    arch_status_lbl->setColor({0.80f, 0.30f, 0.30f, 1.0f});
                }
                return;
            }

            // Calendar event reminder — refresh upcoming events list
            if (std::strcmp(msg.service_name_, "com.freebsdx.Calendar") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(CalendarOpcode::EventReminder)) {
                if (rebuild_uev && *rebuild_uev) (*rebuild_uev)();
                return;
            }

            // GPU stats live update
            if (std::strcmp(msg.service_name_, "com.freebsdx.GpuMonitor") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GpuMonitorOpcode::StatsChanged)) {
                GpuStatsDetail st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_gpu_ui(st);
                return;
            }

            // New screenshot captured
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenCapture") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenCaptureOpcode::CaptureAdded)) {
                if (rebuild_scap && *rebuild_scap) (*rebuild_scap)();
                return;
            }

            // Display layout changed (monitor hotplug or primary switched)
            if (std::strcmp(msg.service_name_, "com.freebsdx.DisplayArrange") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DisplayArrangeOpcode::LayoutChanged)) {
                if (rebuild_disp_arr && *rebuild_disp_arr) (*rebuild_disp_arr)();
                return;
            }

            // Clipboard history: new entry added
            if (std::strcmp(msg.service_name_, "com.freebsdx.ClipboardHistory") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ClipHistoryOpcode::EntryAdded)) {
                if (rebuild_clip && *rebuild_clip) (*rebuild_clip)();
                return;
            }

            // Gesture config changed remotely
            if (std::strcmp(msg.service_name_, "com.freebsdx.Gesture") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GestureOpcode::ConfigChanged)) {
                GestureConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_gest_ui(cfg);
                return;
            }

            // Input device hotplug
            if (std::strcmp(msg.service_name_, "com.freebsdx.InputDevice") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(InputDeviceOpcode::DeviceChanged)) {
                if (rebuild_indev && *rebuild_indev) (*rebuild_indev)();
                return;
            }

            // Accessibility settings changed remotely
            if (std::strcmp(msg.service_name_, "com.freebsdx.Accessibility") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AccessibilityPanelOpcode::SettingsChanged)) {
                AccessibilitySettings cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_a11y_ui_c(cfg);
                return;
            }

            // Remote desktop session list changed
            if (std::strcmp(msg.service_name_, "com.freebsdx.RemoteDesktop") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(RemoteDesktopSessionOpcode::SessionChanged)) {
                if (rebuild_rdp && *rebuild_rdp) (*rebuild_rdp)();
                rdp_status_lbl->setText("");
                return;
            }

            // New system log entry — prepend to list
            if (std::strcmp(msg.service_name_, "com.freebsdx.SysLog") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SysLogOpcode::LogAdded)) {
                // Rebuild to show newest entries; level filter still applies
                if (rebuild_slog && *rebuild_slog) (*rebuild_slog)();
                return;
            }
            // DNS config updated by another process
            if (std::strcmp(msg.service_name_, "com.freebsdx.DnsConfig") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DnsConfigOpcode::ConfigChanged)) {
                DnsResolverConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_dns_cfg_ui(cfg);
                return;
            }
            // Network share mounted / unmounted elsewhere
            if (std::strcmp(msg.service_name_, "com.freebsdx.NetworkShare") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NetworkShareMountOpcode::ShareChanged)) {
                if (rebuild_ns && *rebuild_ns) (*rebuild_ns)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Thermal") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ThermalOpcode::ZonesChanged)) {
                if (rebuild_tz && *rebuild_tz) (*rebuild_tz)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.UsbDevice") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(UsbDeviceOpcode::DeviceChanged)) {
                if (rebuild_usbdev && *rebuild_usbdev) (*rebuild_usbdev)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.NotifSchedule") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotifScheduleOpcode::ScheduleChanged)) {
                NotifSchedule s{};
                std::memcpy(&s, msg.payload_, sizeof(s));
                update_nsch_ui(s);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppSandbox") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppSandboxOpcode::PermsChanged)) {
                if (rebuild_sandbox && *rebuild_sandbox) (*rebuild_sandbox)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.ScreenLock") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ScreenLockOpcode::PolicyChanged)) {
                ScreenLockPolicy p{};
                std::memcpy(&p, msg.payload_, sizeof(p));
                update_slk_ui(p);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.BtAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BtAudioOpcode::DeviceChanged)) {
                if (rebuild_btaudio && *rebuild_btaudio) (*rebuild_btaudio)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Location") == 0 &&
                (msg.opcode_ == static_cast<uint32_t>(LocationOpcode::StateChanged) ||
                 msg.opcode_ == static_cast<uint32_t>(LocationOpcode::PolicyChanged))) {
                // Re-fetch state and app list
                if (!g_shell_xbus) return;
                g_shell_xbus->Call("com.freebsdx.Location",
                    static_cast<uint32_t>(LocationOpcode::GetState), {},
                    [update_loc_policy](const XBusMessage& r) {
                        LocationState s{};
                        std::memcpy(&s, r.payload_, sizeof(s));
                        update_loc_policy(s);
                    });
                if (rebuild_loc && *rebuild_loc) (*rebuild_loc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Totp") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TotpOpcode::AccountAdded)) {
                if (rebuild_totp && *rebuild_totp) (*rebuild_totp)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AudioEq") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AudioEqOpcode::EqChanged)) {
                AudioEqConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_eq_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Backup") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BackupOpcode::JobStateChanged)) {
                if (rebuild_bkp && *rebuild_bkp) (*rebuild_bkp)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Pointer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(PointerOpcode::ConfigChanged)) {
                PointerConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_ptr_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Reminder") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(ReminderOpcode::ReminderChanged)) {
                if (rebuild_rem && *rebuild_rem) (*rebuild_rem)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SpatialAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SpatialAudioOpcode::ConfigChanged)) {
                SpatialAudioConfig cfg{};
                std::memcpy(&cfg, msg.payload_, sizeof(cfg));
                update_sa_ui(cfg);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.Transfer") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TransferOpcode::TransferChanged)) {
                if (rebuild_xfr && *rebuild_xfr) (*rebuild_xfr)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SystemSounds") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SystemSoundsOpcode::StateChanged)) {
                SystemSoundsState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_sndfx_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.GameController") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(GameControllerOpcode::ControllerChanged)) {
                if (rebuild_gc && *rebuild_gc) (*rebuild_gc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.LiveCaptions") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LiveCaptionsOpcode::StateChanged)) {
                LiveCaptionsState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_captions_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SavedWifi") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SavedWifiOpcode::SavedListChanged)) {
                if (rebuild_swifi && *rebuild_swifi) (*rebuild_swifi)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DataSaver") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DataSaverOpcode::StateChanged)) {
                DataSaverState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_dsaver_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.SecurityKey") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(SecurityKeyOpcode::KeysChanged)) {
                if (rebuild_seckey && *rebuild_seckey) (*rebuild_seckey)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.FastCharging") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(FastChargingOpcode::StateChanged)) {
                FastChargingState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_fastchg_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TextReplacement") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TextReplacementOpcode::ShortcutsChanged)) {
                if (rebuild_txtrep && *rebuild_txtrep) (*rebuild_txtrep)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DoNotDisturb") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DoNotDisturbOpcode::StateChanged)) {
                DoNotDisturbState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_silence_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.AppTimeLimit") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(AppTimeLimitOpcode::LimitsChanged)) {
                if (rebuild_applimit && *rebuild_applimit) (*rebuild_applimit)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.BatteryHealth") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(BatteryHealthOpcode::StateChanged)) {
                BatteryHealthState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_bhealth_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.NotifSound") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(NotifSoundOpcode::SoundsChanged)) {
                if (rebuild_notifsnd && *rebuild_notifsnd) (*rebuild_notifsnd)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TrueTone") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrueToneOpcode::StateChanged)) {
                TrueToneState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ttone_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.LoginItems") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(LoginItemOpcode::ItemsChanged)) {
                if (rebuild_litem && *rebuild_litem) (*rebuild_litem)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.OptStorage") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(OptStorageOpcode::StateChanged)) {
                OptStorageState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_ostore_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.TrustedDevices") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(TrustedDeviceOpcode::DevicesChanged)) {
                if (rebuild_tdev && *rebuild_tdev) (*rebuild_tdev)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.MonoAudio") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(MonoAudioOpcode::StateChanged)) {
                MonoAudioState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_monoaud_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardShortcuts") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(KeyboardShortcutOpcode::ShortcutsChanged)) {
                if (rebuild_kbsc && *rebuild_kbsc) (*rebuild_kbsc)();
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.StageManager") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(StageManagerOpcode::StateChanged)) {
                StageManagerState st{};
                std::memcpy(&st, msg.payload_, sizeof(st));
                update_stagemgr_ui(st);
                return;
            }
            if (std::strcmp(msg.service_name_, "com.freebsdx.DefaultApps") == 0 &&
                msg.opcode_ == static_cast<uint32_t>(DefaultAppsOpcode::DefaultsChanged)) {
                if (rebuild_dapp && *rebuild_dapp) (*rebuild_dapp)();
                return;
            }
        });

        auto prev_rx = std::make_shared<uint64_t>(0);
        auto prev_tx = std::make_shared<uint64_t>(0);

        std::thread([&xbus, clock_w, cpu_w, mem_w,
                     net_btn, qp_conn_w, qp_ip_w, qp_signal_w, qp_bw_w, qp_disc_btn,
                     qs_wifi_switch, qs_wifi_sub,
                     qs_bright_slider, qs_bright_val,
                     qs_vol_slider, qs_vol_val, vol_btn,
                     qs_bt_switch, bt_btn,
                     prev_rx, prev_tx,
                     file_results_ready, file_results_pending,
                     sec_files, sec_files_hdr, make_result_row,
                     pomo_phase_lbl, pomo_cd_lbl, pomo_tbar_w, pomo_start_lbl,
                     qs_dnd_switch,
                     update_upd_btn,
                     pinned_data, rebuild_pinned, apps,
                     qs_dark_switch,
                     update_battery,
                     qs_ns_switch,
                     rebuild_screen_time,
                     rebuild_audio_vol,
                     spark_cpu_lbl, spark_ram_lbl,
                     rebuild_therm, rebuild_conn, rebuild_proc, rebuild_diskio,
                     rebuild_syslog, rebuild_vol, rebuild_netconn, rebuild_usb,
                     rebuild_dl, update_gpu_ui, update_mem_ui,
                     rebuild_print_queue,
                     rebuild_pkg_upd,
                     rebuild_zfs,
                     rebuild_jails,
                     update_ft_ui,
                     rebuild_perms,
                     rebuild_im,
                     rebuild_captures,
                     rebuild_be,
                     rebuild_kmod,
                     update_ntp_ui,
                     rebuild_audio_routes,
                     update_tz_ui,
                     rebuild_energy,
                     update_rl_ui,
                     update_wl_ui,
                     rebuild_shares,
                     rebuild_vms,
                     rebuild_containers,
                     dock_pos_cur, dock_pos_btns, dock_autohide, update_dock_ui,
                     du_col, du_status_lbl, rebuild_disk_usage,
                     locale_cur, locale_btns, locale_sub_lbl, update_locale_ui,
                     fs_col, fs_count_lbl, rebuild_file_sync,
                     ss_on, ss_switch, ss_timeout_cur, ss_timeout_btns,
                     ss_mode_cur, ss_mode_btns, update_ss_ui,
                     kc_col, kc_count_lbl, kc_lock_btn, rebuild_keychain,
                     xwl_running, xwl_switch, xwl_info_lbl, update_xwl_ui,
                     iapps_col, iapps_count_lbl, iapps_prog_lbl, rebuild_iapps,
                     term_sess_col, term_sess_count_lbl, rebuild_term_sess,
                     fonts_col, fonts_count_lbl, rebuild_fonts,
                     pkgsign_req_switch, pkgsign_unk_switch, pkgsign_cur, update_pkgsign_ui,
                     store_col, store_status_lbl, store_search_input, rebuild_store,
                     cam_dev_col, cam_dev_count_lbl, rebuild_cam_dev,
                     bm_col, bm_count_lbl, rebuild_bm,
                     seat_count_lbl, seat_col, rebuild_seats,
                     arch_status_lbl,
                     unlock_locked, unlock_inputs_row, unlock_status_lbl,
                     uev_col, uev_count_lbl, rebuild_uev,
                     update_dns_cfg_ui,
                     rebuild_ns,
                     wol_col, wol_count_lbl, rebuild_wol,
                     slog_col, slog_level_mask, rebuild_slog,
                     a11y_cfg, update_a11y_ui,
                     rdp_col, rdp_count_lbl, rdp_status_lbl, rebuild_rdp,
                     gest_cfg, update_gest_ui,
                     indev_col, indev_count_lbl, rebuild_indev,
                     disp_arr_col, disp_arr_count_lbl, disp_mirror_switch, rebuild_disp_arr,
                     clip_col, clip_count_lbl, rebuild_clip,
                     update_gpu_ui_b,
                     scap_col, scap_count_lbl, rebuild_scap,
                     qp_data_w,
                     dnd_countdown_lbl,
                     rebuild_pomo_log,
                     net_rx_spark_lbl, net_tx_spark_lbl,
                     timer_cd_lbl, rebuild_storage,
                     rebuild_tz, rebuild_usbdev,
                     rebuild_sandbox, rebuild_btaudio,
                     rebuild_loc, rebuild_totp,
                     update_eq_ui, rebuild_bkp,
                     update_ptr_ui, rebuild_rem,
                     update_sa_ui, rebuild_xfr]() {
            // Initial state fetch (once at startup)
            xbus.Call("com.freebsdx.Network",
                static_cast<uint32_t>(NetworkOpcode::GetStatus), {},
                [qs_wifi_switch, qs_wifi_sub](const XBusMessage& r) {
                    NetworkStatus n{};
                    std::memcpy(&n, r.payload_, sizeof(n));
                    qs_wifi_switch->setValue(n.is_connected_);
                    qs_wifi_sub->setText(n.is_connected_ && n.current_ssid_[0]
                        ? std::string(n.current_ssid_) : (n.is_connected_ ? std::string(n.ip_address_) : "Not connected"));
                });

            uint32_t loop_tick = 0;
            while (true) {
                ++loop_tick;
                auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                char ts[16];
                std::strftime(ts, sizeof(ts), "%I:%M %p", std::localtime(&t));
                clock_w->setText(ts);

                // CPU / MEM taskbar indicators were removed; no System poll needed.

                xbus.Call("com.freebsdx.Network",
                    static_cast<uint32_t>(NetworkOpcode::GetStatus), {},
                    [net_btn, qp_conn_w, qp_ip_w, qp_signal_w, qp_bw_w, qp_disc_btn,
                     qs_wifi_switch, qs_wifi_sub,
                     prev_rx, prev_tx, qp_data_w,
                     net_rx_spark_lbl, net_tx_spark_lbl](const XBusMessage& r) {
                        NetworkStatus n{};
                        std::memcpy(&n, r.payload_, sizeof(n));

                        // Bandwidth delta (bytes since last poll ≈ bytes/sec)
                        uint64_t rx_d = (n.rx_bytes_ >= *prev_rx) ? n.rx_bytes_ - *prev_rx : 0;
                        uint64_t tx_d = (n.tx_bytes_ >= *prev_tx) ? n.tx_bytes_ - *prev_tx : 0;
                        *prev_rx = n.rx_bytes_;
                        *prev_tx = n.tx_bytes_;

                        // Update speed sparklines
                        g_net_rx_hist[static_cast<size_t>(g_net_spark_idx)] = rx_d;
                        g_net_tx_hist[static_cast<size_t>(g_net_spark_idx)] = tx_d;
                        g_net_spark_idx = (g_net_spark_idx + 1) % kNetSparkLen;
                        net_rx_spark_lbl->setText("↓ " + fmtNetSpark(g_net_rx_hist, g_net_spark_idx - 1));
                        net_tx_spark_lbl->setText("↑ " + fmtNetSpark(g_net_tx_hist, g_net_spark_idx - 1));

                        // Accumulate daily data usage
                        uint64_t delta = rx_d + tx_d;
                        if (delta > 0) {
                            uint64_t total = g_data_today_bytes.fetch_add(delta) + delta;
                            double mb = static_cast<double>(total) / (1024.0 * 1024.0);
                            std::string label = mb >= 1024.0
                                ? std::format("Today: {:.2f} GB", mb / 1024.0)
                                : std::format("Today: {:.1f} MB", mb);
                            qp_data_w->setText(label);
                        }

                        if (!n.is_connected_) {
                            net_btn->setText("✕");
                            qp_conn_w->setText("○ Not connected");
                            qp_ip_w->setText("");
                            qp_signal_w->setText("");
                            qp_bw_w->setText("");
                            qp_disc_btn->setVisibility(false);
                        } else {
                            std::string bars = n.current_ssid_[0]
                                ? signalBars(n.signal_strength_) : "⇄";
                            std::string name = n.current_ssid_[0]
                                ? std::string(n.current_ssid_) : "Ethernet";
                            net_btn->setText(bars + " " + name);
                            qp_conn_w->setText("● " + name);
                            qp_ip_w->setText(std::string(n.ip_address_));
                            if (n.current_ssid_[0])
                                qp_signal_w->setText(
                                    signalBars(n.signal_strength_) +
                                    std::format("  {} dBm", n.signal_strength_));
                            qp_bw_w->setText(std::format("↓ {}  ↑ {}",
                                fmtBps(rx_d), fmtBps(tx_d)));
                            qp_disc_btn->setVisibility(true);
                        }
                        // Update Wi-Fi switch and sub-label each poll cycle
                        qs_wifi_switch->setValue(n.is_connected_);
                        qs_wifi_sub->setText(n.is_connected_ ? std::string(n.ip_address_) : "Not connected");
                    });

                // Load persisted pinned apps from config
                {
                    char key_buf[32] = "shell.pinned";
                    xbus.Call("com.freebsdx.Configuration", 0x01,
                        {reinterpret_cast<const uint8_t*>(key_buf), sizeof(key_buf)},
                        [pinned_data, rebuild_pinned, apps](const XBusMessage& r) {
                            const char* val = reinterpret_cast<const char*>(r.payload_);
                            pinned_data->clear();
                            // Parse comma-separated identifiers, look up metadata in apps
                            std::string ids(val);
                            size_t pos = 0;
                            while (pos < ids.size()) {
                                size_t comma = ids.find(',', pos);
                                std::string id = ids.substr(pos, comma == std::string::npos
                                    ? std::string::npos : comma - pos);
                                if (!id.empty()) {
                                    auto it = std::find_if(apps.begin(), apps.end(),
                                        [&id](const auto& a){ return a.identifier == id; });
                                    if (it != apps.end())
                                        pinned_data->push_back({id, it->name, it->executable_path});
                                }
                                if (comma == std::string::npos) break;
                                pos = comma + 1;
                            }
                            (*rebuild_pinned)();
                        });
                }

                // Fetch current theme (dark mode + accent)
                xbus.Call("com.freebsdx.Theme",
                    static_cast<uint32_t>(ThemeOpcode::GetTheme), {},
                    [qs_dark_switch](const XBusMessage& r) {
                        ThemeState ts{};
                        std::memcpy(&ts, r.payload_, sizeof(ts));
                        g_accent_idx = ts.accent_idx_;
                        qs_dark_switch->setValue(ts.dark_mode_ != 0);
                    });

                // Fetch brightness
                xbus.Call("com.freebsdx.Backlight",
                    static_cast<uint32_t>(BacklightOpcode::GetBrightness), {},
                    [qs_bright_slider, qs_bright_val](const XBusMessage& r) {
                        BacklightState st{};
                        std::memcpy(&st, r.payload_, sizeof(st));
                        float v = static_cast<float>(st.level_);
                        qs_bright_slider->setValue(v);
                        qs_bright_val->setText(std::format("{}%", st.level_));
                    });

                // Fetch volume
                xbus.Call("com.freebsdx.Audio",
                    static_cast<uint32_t>(AudioOpcode::GetVolume), {},
                    [qs_vol_slider, qs_vol_val, vol_btn](const XBusMessage& r) {
                        AudioVolumeState st{};
                        std::memcpy(&st, r.payload_, sizeof(st));
                        float v = static_cast<float>(st.volume_);
                        qs_vol_slider->setValue(v);
                        qs_vol_val->setText(std::format("{}%", st.volume_));
                        vol_btn->setText(st.muted_ ? "🔇" : "🔊");
                    });

                // Fetch BT state
                xbus.Call("com.freebsdx.Bluetooth",
                    static_cast<uint32_t>(BluetoothOpcode::GetState), {},
                    [qs_bt_switch, bt_btn](const XBusMessage& r) {
                        BluetoothState st{};
                        std::memcpy(&st, r.payload_, sizeof(st));
                        qs_bt_switch->setValue(st.enabled_ != 0);
                        bt_btn->setText(st.enabled_ ? "🔵" : "BT");
                    });

                // Per-app volume — refresh every 30 seconds
                if (loop_tick % 30 == 0 && *rebuild_audio_vol)
                    (*rebuild_audio_vol)();

                // Snoozed notifications — re-fire expired entries every 30 seconds
                if (loop_tick % 30 == 0 && g_shell_xbus) {
                    std::vector<SnoozedNotif> fired;
                    {
                        std::lock_guard<std::mutex> lk(g_snooze_mtx);
                        std::time_t now = std::time(nullptr);
                        auto it = g_snoozed.begin();
                        while (it != g_snoozed.end()) {
                            if (it->wake_at <= now) {
                                fired.push_back(*it);
                                it = g_snoozed.erase(it);
                            } else {
                                ++it;
                            }
                        }
                    }
                    for (const auto& sn : fired) {
                        NotificationPayload notif{};
                        notif.timeout_ms_ = 8000;
                        std::strncpy(notif.title_,    sn.title.c_str(),
                            sizeof(notif.title_) - 1);
                        std::strncpy(notif.message_,  sn.message.c_str(),
                            sizeof(notif.message_) - 1);
                        std::strncpy(notif.app_name_, sn.app_name.c_str(),
                            sizeof(notif.app_name_) - 1);
                        notif.id_ = sn.id; // preserve original id
                        g_shell_xbus->Call("com.freebsdx.Notification",
                            static_cast<uint32_t>(NotificationOpcode::Post),
                            {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                            nullptr);
                    }
                }

                // CPU/RAM sparklines — sample every 10 seconds
                if (loop_tick % 10 == 0) {
                    int cpu = sampleCpuPct();
                    int ram = sampleRamPct();
                    if (cpu >= 0) {
                        g_cpu_history[g_spark_idx] = cpu;
                        g_ram_history[g_spark_idx] = ram;
                        g_spark_idx = (g_spark_idx + 1) % kSparkLen;
                    }
                    int ci = (g_spark_idx + kSparkLen - 1) % kSparkLen;
                    spark_cpu_lbl->setText(std::format("CPU  {}  {}%",
                        fmtSparkBar(g_cpu_history, g_spark_idx), g_cpu_history[ci]));
                    spark_ram_lbl->setText(std::format("RAM  {}  {}%",
                        fmtSparkBar(g_ram_history, g_spark_idx), g_ram_history[ci]));
                }

                // Energy impact — refresh every 5 seconds (alongside thermal)
                if (loop_tick % 5 == 0 && g_shell_xbus) {
                    if (rebuild_energy && *rebuild_energy) (*rebuild_energy)();
                }

                // Downloads + GPU stats — refresh every 2 seconds
                if (loop_tick % 2 == 0 && g_shell_xbus) {
                    if (rebuild_dl && *rebuild_dl) (*rebuild_dl)();
                    xbus.Call("com.freebsdx.GpuStats",
                        static_cast<uint32_t>(GpuOpcode::GetStats), {},
                        [update_gpu_ui_b](const XBusMessage& r) {
                            GpuStats st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_gpu_ui_b(st);
                        });
                    xbus.Call("com.freebsdx.FramePerf",
                        static_cast<uint32_t>(FramePerfOpcode::GetStats), {},
                        [update_ft_ui](const XBusMessage& r) {
                            FramePerfState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_ft_ui(st);
                        });
                }

                // Thermal readings, connections, processes, disk I/O — refresh every 5 seconds
                if (loop_tick % 5 == 0 && g_shell_xbus) {
                    if (rebuild_therm   && *rebuild_therm)   (*rebuild_therm)();
                    if (rebuild_conn    && *rebuild_conn)    (*rebuild_conn)();
                    if (rebuild_proc    && *rebuild_proc)    (*rebuild_proc)();
                    if (rebuild_diskio  && *rebuild_diskio)  (*rebuild_diskio)();
                    if (rebuild_syslog  && *rebuild_syslog)  (*rebuild_syslog)();
                    if (rebuild_vol     && *rebuild_vol)     (*rebuild_vol)();
                    if (rebuild_netconn && *rebuild_netconn) (*rebuild_netconn)();
                    if (rebuild_usb           && *rebuild_usb)           (*rebuild_usb)();
                    if (rebuild_print_queue   && *rebuild_print_queue)   (*rebuild_print_queue)();
                    if (rebuild_jails         && *rebuild_jails)         (*rebuild_jails)();
                    // Audio routing — refresh when apps start/stop audio
                    if (rebuild_audio_routes && *rebuild_audio_routes)   (*rebuild_audio_routes)();
                    // Network shares — refresh mount states
                    if (rebuild_shares && *rebuild_shares)               (*rebuild_shares)();
                    // VM list — refresh running state
                    if (rebuild_vms && *rebuild_vms)                     (*rebuild_vms)();
                    // Container list — refresh running state
                    if (rebuild_containers && *rebuild_containers)       (*rebuild_containers)();
                    // File sync job progress — refresh every 5 seconds
                    if (rebuild_file_sync && *rebuild_file_sync)         (*rebuild_file_sync)();
                    // Memory pressure — polled at same 5-second cadence
                    xbus.Call("com.freebsdx.MemPressure",
                        static_cast<uint32_t>(MemPressureOpcode::GetPressure), {},
                        [update_mem_ui](const XBusMessage& r) {
                            MemPressureState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_mem_ui(st);
                        });
                }

                // Package update list — poll every 30 seconds alongside battery
                if (loop_tick % 30 == 0 && rebuild_pkg_upd && *rebuild_pkg_upd)
                    (*rebuild_pkg_upd)();

                // Keychain — poll every 30 seconds (lock state / item count can change)
                if (loop_tick % 30 == 0 && rebuild_keychain && *rebuild_keychain)
                    (*rebuild_keychain)();

                // Installed apps — poll every 30 seconds (installs happen in bg)
                if (loop_tick % 30 == 0 && rebuild_iapps && *rebuild_iapps)
                    (*rebuild_iapps)();

                // Terminal sessions — poll every 5 seconds (sessions come and go quickly)
                if (loop_tick % 5 == 0 && rebuild_term_sess && *rebuild_term_sess)
                    (*rebuild_term_sess)();

                // Font list — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_fonts && *rebuild_fonts)
                    (*rebuild_fonts)();

                // App store catalog — refresh installed flags every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_store && *rebuild_store)
                    (*rebuild_store)(store_search_input->getText());

                // Camera device list — poll every 30 seconds (hotplug supplement)
                if (loop_tick % 30 == 0 && rebuild_cam_dev && *rebuild_cam_dev)
                    (*rebuild_cam_dev)();

                // Bookmarks — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_bm && *rebuild_bm)
                    (*rebuild_bm)();

                // Seat list — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_seats && *rebuild_seats)
                    (*rebuild_seats)();

                // Upcoming events — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_uev && *rebuild_uev)
                    (*rebuild_uev)();

                // Screenshot gallery — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_scap && *rebuild_scap)
                    (*rebuild_scap)();

                // Wake-on-LAN targets — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_wol && *rebuild_wol)
                    (*rebuild_wol)();

                // System log — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_slog && *rebuild_slog)
                    (*rebuild_slog)();

                // Thermal zones — poll every 10 seconds
                if (loop_tick % 10 == 0 && rebuild_tz && *rebuild_tz)
                    (*rebuild_tz)();

                // USB devices — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_usbdev && *rebuild_usbdev)
                    (*rebuild_usbdev)();

                // App sandbox permissions — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_sandbox && *rebuild_sandbox)
                    (*rebuild_sandbox)();

                // BT audio devices — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_btaudio && *rebuild_btaudio)
                    (*rebuild_btaudio)();

                // Location app list — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_loc && *rebuild_loc)
                    (*rebuild_loc)();

                // TOTP codes — refresh every 5 seconds to keep countdown current
                if (loop_tick % 10 == 0 && rebuild_totp && *rebuild_totp)
                    (*rebuild_totp)();

                // Backup jobs — poll every 60 seconds
                if (loop_tick % 120 == 0 && rebuild_bkp && *rebuild_bkp)
                    (*rebuild_bkp)();

                // Reminders — poll every 30 seconds
                if (loop_tick % 60 == 0 && rebuild_rem && *rebuild_rem)
                    (*rebuild_rem)();

                // Active transfers — poll every 5 seconds for live progress
                if (loop_tick % 10 == 0 && rebuild_xfr && *rebuild_xfr)
                    (*rebuild_xfr)();

                // Network shares — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_ns && *rebuild_ns)
                    (*rebuild_ns)();

                // Remote desktop sessions — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_rdp && *rebuild_rdp)
                    (*rebuild_rdp)();

                // Input devices — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_indev && *rebuild_indev)
                    (*rebuild_indev)();

                // Display layout — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_disp_arr && *rebuild_disp_arr)
                    (*rebuild_disp_arr)();

                // Clipboard history — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_clip && *rebuild_clip)
                    (*rebuild_clip)();

                // ZFS pool health — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_zfs && *rebuild_zfs)
                    (*rebuild_zfs)();

                // Screenshot & recording list — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_captures && *rebuild_captures)
                    (*rebuild_captures)();

                // Boot environments — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_be && *rebuild_be)
                    (*rebuild_be)();

                // Kernel module list — poll every 30 seconds
                if (loop_tick % 30 == 0 && rebuild_kmod && *rebuild_kmod)
                    (*rebuild_kmod)();

                // Time zone local-time sub-label — refresh every 30 seconds
                if (loop_tick % 30 == 0) {
                    xbus.Call("com.freebsdx.Timezone",
                        static_cast<uint32_t>(TimezoneOpcode::GetTZ), {},
                        [update_tz_ui](const XBusMessage& r) {
                            TimezoneState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_tz_ui(st);
                        });
                }

                // Remote login session count — refresh every 30 seconds
                if (loop_tick % 30 == 0) {
                    xbus.Call("com.freebsdx.RemoteLogin",
                        static_cast<uint32_t>(RemoteLoginOpcode::GetState), {},
                        [update_rl_ui](const XBusMessage& r) {
                            RemoteLoginState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_rl_ui(st);
                        });
                }

                // Wake lock remaining time — tick every 30 seconds
                if (loop_tick % 30 == 0) {
                    xbus.Call("com.freebsdx.WakeLock",
                        static_cast<uint32_t>(WakeLockOpcode::GetState), {},
                        [update_wl_ui](const XBusMessage& r) {
                            WakeLockState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_wl_ui(st);
                        });
                }

                // NTP sync state — poll every 30 seconds
                if (loop_tick % 30 == 0) {
                    xbus.Call("com.freebsdx.NTP",
                        static_cast<uint32_t>(NtpOpcode::GetState), {},
                        [update_ntp_ui](const XBusMessage& r) {
                            NtpState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_ntp_ui(st);
                        });
                }

                // Low-battery notifications — poll every 30 seconds
                if (loop_tick % 30 == 0) {
                    xbus.Call("com.freebsdx.Power",
                        static_cast<uint32_t>(PowerOpcode::GetPowerState), {},
                        [update_battery](const XBusMessage& r) {
                            PowerState ps{};
                            std::memcpy(&ps, r.payload_, sizeof(ps));
                            (*update_battery)(ps);
                            if (ps.battery_state_ == 1) {
                                // Charging — reset warning threshold
                                g_last_batt_warned.store(100);
                                return;
                            }
                            if (ps.battery_state_ != 0) return; // not discharging
                            int pct  = static_cast<int>(ps.battery_life_);
                            int prev = g_last_batt_warned.load();
                            int threshold = 0;
                            if      (pct <= 5  && prev > 5 ) threshold = 5;
                            else if (pct <= 10 && prev > 10) threshold = 10;
                            else if (pct <= 20 && prev > 20) threshold = 20;
                            if (threshold == 0 || !g_shell_xbus) return;
                            g_last_batt_warned.store(pct);
                            NotificationPayload notif{};
                            notif.timeout_ms_ = 8000;
                            std::snprintf(notif.title_,   sizeof(notif.title_),
                                "Battery Low: %d%%", pct);
                            std::snprintf(notif.message_, sizeof(notif.message_), "%s",
                                pct <= 5 ? "Plug in now to avoid shutdown."
                                         : "Consider plugging in soon.");
                            std::strncpy(notif.app_name_, "Battery",
                                sizeof(notif.app_name_) - 1);
                            g_shell_xbus->Call("com.freebsdx.Notification",
                                static_cast<uint32_t>(NotificationOpcode::Post),
                                {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                                nullptr);
                        });
                }

                // DND + Night Shift schedule auto-toggle — check every 60 seconds
                if (loop_tick % 60 == 0) {
                    std::time_t tnow = std::time(nullptr);
                    std::tm* ltnow = std::localtime(&tnow);
                    if (ltnow) {
                        // Screen time — reset daily, refresh panel
                        if (ltnow->tm_mday != g_usage_day) {
                            g_usage_day = ltnow->tm_mday;
                            std::lock_guard<std::mutex> ul(g_usage_mtx);
                            g_app_usage_secs.clear();
                        }
                        if (*rebuild_screen_time) (*rebuild_screen_time)();

                        // Network data usage — reset daily, warn at 1 GB and 5 GB
                        if (ltnow->tm_mday != g_data_day) {
                            g_data_day = ltnow->tm_mday;
                            g_data_today_bytes.store(0);
                            g_data_warned_gb = 0;
                            qp_data_w->setText("");
                        }
                        if (g_shell_xbus) {
                            uint64_t total = g_data_today_bytes.load();
                            auto check_gb = [&](int gb, int bit) {
                                uint64_t threshold = static_cast<uint64_t>(gb) * 1024 * 1024 * 1024;
                                if (total >= threshold && !(g_data_warned_gb & bit)) {
                                    g_data_warned_gb |= bit;
                                    NotificationPayload notif{};
                                    notif.timeout_ms_ = 8000;
                                    std::strncpy(notif.app_name_, "Network",
                                        sizeof(notif.app_name_) - 1);
                                    std::snprintf(notif.title_, sizeof(notif.title_),
                                        "Data usage: %d GB used today", gb);
                                    std::strncpy(notif.message_,
                                        "You may be on a metered connection.",
                                        sizeof(notif.message_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.Notification",
                                        static_cast<uint32_t>(NotificationOpcode::Post),
                                        {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                                        nullptr);
                                }
                            };
                            check_gb(1, 1);
                            check_gb(5, 2);
                        }

                        // DND schedule
                        bool should_dnd = dndSchedActive(ltnow->tm_hour);
                        if (should_dnd != g_dnd) {
                            g_dnd = should_dnd;
                            qs_dnd_switch->setValue(g_dnd);
                        }

                        // DND digest — post when DND transitions from on → off
                        if (g_dnd_was_on && !g_dnd) {
                            uint32_t cnt = g_dnd_suppressed_count.exchange(0);
                            if (cnt > 0 && g_shell_xbus) {
                                std::map<std::string, uint32_t> apps_snap;
                                { std::lock_guard<std::mutex> dlk(g_dnd_digest_mtx);
                                  apps_snap = std::move(g_dnd_suppressed_apps); }
                                std::string msg_str;
                                for (const auto& [aname, _] : apps_snap) {
                                    if (!msg_str.empty()) msg_str += ", ";
                                    msg_str += aname;
                                }
                                NotificationPayload notif{};
                                notif.timeout_ms_ = 8000;
                                std::strncpy(notif.app_name_, "System",
                                    sizeof(notif.app_name_) - 1);
                                std::snprintf(notif.title_, sizeof(notif.title_),
                                    "%u notification%s missed while DND was on",
                                    cnt, cnt == 1 ? "" : "s");
                                std::strncpy(notif.message_, msg_str.c_str(),
                                    sizeof(notif.message_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Notification",
                                    static_cast<uint32_t>(NotificationOpcode::Post),
                                    {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                                    nullptr);
                            }
                        }
                        g_dnd_was_on = g_dnd;

                        // Night Shift schedule
                        bool should_ns = nsSchedActive(ltnow->tm_hour);
                        if (g_ns_sched_on && g_shell_xbus) {
                            NightShiftConfig cfg{};
                            cfg.enabled_   = should_ns ? 1u : 0u;
                            cfg.intensity_ = should_ns
                                ? static_cast<uint32_t>(g_ns_sched_intensity) : 0u;
                            cfg.auto_mode_ = 0u;
                            g_shell_xbus->Call("com.freebsdx.NightShift",
                                static_cast<uint32_t>(NightShiftOpcode::SetConfig),
                                {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)},
                                nullptr);
                            qs_ns_switch->setValue(should_ns);
                        }

                        // DND countdown label
                        dnd_countdown_lbl->setText(dndCountdownText());
                    }
                }

                // Reap child processes — notify on crash (non-zero exit or signal)
                {
                    std::vector<std::string> crashed;
                    {
                        std::lock_guard<std::mutex> lk(g_child_pids_mtx);
                        int wstatus = 0;
                        pid_t done;
                        while ((done = waitpid(-1, &wstatus, WNOHANG)) > 0) {
                            auto it = g_child_pids.find(done);
                            if (it != g_child_pids.end()) {
                                bool ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
                                if (!ok) crashed.push_back(it->second);
                                // Accumulate screen time for completed session
                                auto lt = g_child_launch_times.find(done);
                                if (lt != g_child_launch_times.end()) {
                                    uint32_t elapsed = static_cast<uint32_t>(
                                        std::time(nullptr) - lt->second);
                                    std::lock_guard<std::mutex> ul(g_usage_mtx);
                                    g_app_usage_secs[it->second] += elapsed;
                                    g_child_launch_times.erase(lt);
                                }
                                g_child_pids.erase(it);
                            }
                        }
                    }
                    for (const auto& name : crashed) {
                        if (!g_shell_xbus) continue;
                        NotificationPayload notif{};
                        notif.timeout_ms_ = 6000;
                        std::strncpy(notif.app_name_, "System",
                            sizeof(notif.app_name_) - 1);
                        std::snprintf(notif.title_, sizeof(notif.title_),
                            "App crashed: %s", name.c_str());
                        std::strncpy(notif.message_,
                            "The application exited unexpectedly.",
                            sizeof(notif.message_) - 1);
                        g_shell_xbus->Call("com.freebsdx.Notification",
                            static_cast<uint32_t>(NotificationOpcode::Post),
                            {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                            nullptr);
                    }
                }

                // Storage quick-view refresh — every 5 minutes
                if (loop_tick % 300 == 0) {
                    if (rebuild_storage && *rebuild_storage) (*rebuild_storage)();
                }

                // Calendar event reminders — check every 5 minutes
                if (loop_tick % 300 == 0 && g_shell_xbus) {
                    std::time_t tnow = std::time(nullptr);
                    std::tm* lt = std::localtime(&tnow);
                    if (lt) {
                        // Reset notified set at midnight
                        if (lt->tm_mday != g_notif_day) {
                            g_notif_day = lt->tm_mday;
                            g_notified_event_ids.clear();
                        }
                        CalendarListRequest req{};
                        req.year_  = static_cast<uint16_t>(1900 + lt->tm_year);
                        req.month_ = static_cast<uint8_t>(1 + lt->tm_mon);
                        req.page_  = 0;
                        int now_mins = lt->tm_hour * 60 + lt->tm_min;
                        g_shell_xbus->Call("com.freebsdx.Calendar",
                            static_cast<uint32_t>(CalendarOpcode::ListEvents),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [now_mins](const XBusMessage& r) {
                                CalendarEventPage ep{};
                                std::memcpy(&ep, r.payload_, sizeof(ep));
                                if (!ep.valid_) return;
                                if (ep.all_day_) return; // skip all-day events
                                int evt_mins = static_cast<int>(ep.hour_) * 60
                                             + static_cast<int>(ep.minute_);
                                int diff = evt_mins - now_mins;
                                if (diff >= 0 && diff <= 15
                                    && g_notified_event_ids.find(ep.event_id_) == g_notified_event_ids.end()
                                    && g_shell_xbus) {
                                    g_notified_event_ids.insert(ep.event_id_);
                                    NotificationPayload notif{};
                                    notif.timeout_ms_ = 10000;
                                    std::strncpy(notif.app_name_, "Calendar",
                                        sizeof(notif.app_name_) - 1);
                                    if (diff == 0)
                                        std::snprintf(notif.title_, sizeof(notif.title_),
                                            "Event now: %s", ep.title_);
                                    else
                                        std::snprintf(notif.title_, sizeof(notif.title_),
                                            "Event in %d min: %s", diff, ep.title_);
                                    std::strncpy(notif.message_, ep.title_,
                                        sizeof(notif.message_) - 1);
                                    g_shell_xbus->Call("com.freebsdx.Notification",
                                        static_cast<uint32_t>(NotificationOpcode::Post),
                                        {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                                        nullptr);
                                }
                            });
                    }
                }

                // Disk space check — warn when any ZFS pool >= 90% full (hourly)
                if (loop_tick % 3600 == 0) {
                    xbus.Call("com.freebsdx.Disk",
                        static_cast<uint32_t>(DiskOpcode::ListZpools), {},
                        [](const XBusMessage& r) {
                            ZpoolListPage pg{};
                            std::memcpy(&pg, r.payload_, sizeof(pg));
                            if (!pg.count_) return;
                            const auto& pool = pg.pool_;
                            uint64_t total = pool.used_bytes_ + pool.free_bytes_;
                            if (total == 0) return;
                            int pct = static_cast<int>(pool.used_bytes_ * 100u / total);
                            std::string pname(pool.name_,
                                std::strlen(pool.name_) < sizeof(pool.name_)
                                    ? std::strlen(pool.name_) : sizeof(pool.name_) - 1);
                            if (pct >= 90 &&
                                g_warned_disks.find(pname) == g_warned_disks.end()) {
                                g_warned_disks.insert(pname);
                                if (!g_shell_xbus) return;
                                NotificationPayload notif{};
                                notif.timeout_ms_ = 8000;
                                std::strncpy(notif.app_name_, "Storage",
                                    sizeof(notif.app_name_) - 1);
                                std::snprintf(notif.title_, sizeof(notif.title_),
                                    "Low Disk Space: %s", pname.c_str());
                                std::snprintf(notif.message_, sizeof(notif.message_),
                                    "%d%% used — consider freeing space.", pct);
                                g_shell_xbus->Call("com.freebsdx.Notification",
                                    static_cast<uint32_t>(NotificationOpcode::Post),
                                    {reinterpret_cast<const uint8_t*>(&notif), sizeof(notif)},
                                    nullptr);
                            } else if (pct < 85) {
                                g_warned_disks.erase(pname);
                            }
                        });
                }

                // Check for pkg updates every 60 minutes (3600 loop ticks × 1 s sleep)
                if (loop_tick % 3600 == 0) {
                    xbus.Call("com.freebsdx.Update",
                        static_cast<uint32_t>(UpdateOpcode::CheckForUpdates), {},
                        [update_upd_btn](const XBusMessage& r) {
                            UpdateAvailableState st{};
                            std::memcpy(&st, r.payload_, sizeof(st));
                            update_upd_btn(st.count_);
                        });
                }

                // Flush file search results if ready
                if (file_results_ready->load()) {
                    file_results_ready->store(false);
                    sec_files->clearChildren();
                    for (const auto& path : *file_results_pending) {
                        std::string fname = std::filesystem::path(path).filename().string();
                        std::string dir   = std::filesystem::path(path).parent_path().string();
                        // Shorten home prefix
                        const char* home = std::getenv("HOME");
                        if (home && dir.starts_with(home))
                            dir = "~" + dir.substr(std::strlen(home));
                        std::string p = path;
                        sec_files->addChild(make_result_row("📄", fname, dir, [p, fname]() {
                            // Record in recent files
                            g_recent_files.erase(
                                std::remove_if(g_recent_files.begin(), g_recent_files.end(),
                                    [&p](const auto& e){ return e.second == p; }),
                                g_recent_files.end());
                            g_recent_files.insert(g_recent_files.begin(), {fname, p});
                            if (g_recent_files.size() > 20) g_recent_files.resize(20);
                            saveRecentFiles();
                            // Open via MIME service
                            if (g_shell_xbus) {
                                MimeQueryRequest qr{};
                                std::strncpy(qr.path_, p.c_str(), sizeof(qr.path_)-1);
                                qr.is_path_ = 1;
                                g_shell_xbus->Call("com.freebsdx.MIME",
                                    static_cast<uint32_t>(MimeOpcode::QueryHandler),
                                    {reinterpret_cast<const uint8_t*>(&qr), sizeof(qr)},
                                    [p](const XBusMessage& resp) {
                                        MimeHandlerInfo info{};
                                        std::memcpy(&info, resp.payload_, sizeof(info));
                                        if (info.app_id_[0]) launchApp(info.app_id_, p);
                                    });
                            }
                        }));
                    }
                    sec_files_hdr->setVisibility(!file_results_pending->empty());
                }

                // ── Pomodoro tick ─────────────────────────────────────────
                if (g_pomo_running.load()) {
                    int secs = g_pomo_secs.fetch_sub(1) - 1;
                    if (secs < 0) secs = 0;
                    std::string cd = pomoFmt(secs);
                    pomo_cd_lbl->setText(cd);
                    pomo_tbar_w->setText("🍅 " + cd);

                    if (secs == 0) {
                        g_pomo_running.store(false);
                        pomo_start_lbl->setText("▶ Start");

                        int phase = g_pomo_phase.load();
                        NotificationPayload pn{};
                        pn.timeout_ms_ = 8000;
                        std::strncpy(pn.app_name_, "Focus Timer",
                                     sizeof(pn.app_name_) - 1);

                        if (phase == 0) {
                            // Work session complete
                            int done = g_pomo_cycles.fetch_add(1) + 1;
                            bool long_break = (done % 4 == 0);
                            std::strncpy(pn.title_, "Focus Session Complete",
                                         sizeof(pn.title_) - 1);
                            std::strncpy(pn.message_,
                                long_break ? "Time for a long break! (15 min)"
                                           : "Time for a short break! (5 min)",
                                sizeof(pn.message_) - 1);
                            if (long_break) {
                                g_pomo_phase.store(2);
                                g_pomo_secs.store(kPomoLong);
                                pomo_phase_lbl->setText("Long Break");
                                pomo_phase_lbl->setColor({0.55f, 0.70f, 1.0f, 1.0f});
                                pomo_cd_lbl->setText("15:00");
                                pomo_tbar_w->setText("🍅 15:00");
                            } else {
                                g_pomo_phase.store(1);
                                g_pomo_secs.store(kPomoShort);
                                pomo_phase_lbl->setText("Short Break");
                                pomo_phase_lbl->setColor({0.55f, 0.70f, 1.0f, 1.0f});
                                pomo_cd_lbl->setText("05:00");
                                pomo_tbar_w->setText("🍅 05:00");
                            }
                        } else {
                            // Break over — back to work
                            std::strncpy(pn.title_, "Break Over",
                                         sizeof(pn.title_) - 1);
                            std::strncpy(pn.message_,
                                "Ready for another focus session?",
                                sizeof(pn.message_) - 1);
                            g_pomo_phase.store(0);
                            g_pomo_secs.store(kPomoWork);
                            pomo_phase_lbl->setText("Work");
                            pomo_phase_lbl->setColor({0.55f, 0.90f, 0.55f, 1.0f});
                            pomo_cd_lbl->setText("25:00");
                            pomo_tbar_w->setText("🍅 25:00");
                        }

                        // Record to session log
                        {
                            PomoLogEntry le{};
                            le.ts = std::time(nullptr);
                            le.phase = phase;
                            le.duration = (phase == 0) ? kPomoWork
                                        : (phase == 2) ? kPomoLong : kPomoShort;
                            { std::lock_guard<std::mutex> lk(g_pomo_log_mtx);
                              g_pomo_log.push_back(le);
                              if (g_pomo_log.size() > 200) g_pomo_log.erase(g_pomo_log.begin()); }
                            savePomoLog();
                            if (*rebuild_pomo_log) (*rebuild_pomo_log)();
                        }

                        if (g_shell_xbus)
                            g_shell_xbus->Call("com.freebsdx.Notification",
                                static_cast<uint32_t>(NotificationOpcode::Post),
                                {reinterpret_cast<const uint8_t*>(&pn), sizeof(pn)},
                                nullptr);
                    }
                }

                // Idle auto-lock — increment counter; lock when threshold reached
                {
                    uint32_t lock_secs = g_idle_lock_secs.load();
                    if (lock_secs > 0) {
                        uint32_t ticks = g_idle_ticks.fetch_add(1) + 1;
                        if (ticks >= lock_secs && g_shell_xbus) {
                            g_idle_ticks.store(0);
                            g_shell_xbus->Call("com.freebsdx.Lock",
                                static_cast<uint32_t>(LockOpcode::Lock), {}, nullptr);
                        }
                    }
                }

                // Quick countdown timer tick
                {
                    std::lock_guard<std::mutex> lk(g_countdown_mtx);
                    if (g_countdown.active) {
                        std::time_t now = std::time(nullptr);
                        int remain = static_cast<int>(g_countdown.end_at - now);
                        if (remain <= 0) {
                            g_countdown.active = false;
                            timer_cd_lbl->setText("✓ Done");
                            if (g_shell_xbus) {
                                NotificationPayload pn{};
                                pn.timeout_ms_ = 10000;
                                std::strncpy(pn.app_name_, "Timer",
                                             sizeof(pn.app_name_) - 1);
                                std::strncpy(pn.title_, g_countdown.label.c_str(),
                                             sizeof(pn.title_) - 1);
                                std::strncpy(pn.message_, "Time is up!",
                                             sizeof(pn.message_) - 1);
                                g_shell_xbus->Call("com.freebsdx.Notification",
                                    static_cast<uint32_t>(NotificationOpcode::Post),
                                    {reinterpret_cast<const uint8_t*>(&pn), sizeof(pn)},
                                    nullptr);
                            }
                        } else {
                            timer_cd_lbl->setText(fmtCountdown(remain));
                        }
                    }
                }

                // Wallpaper rotation — advance when countdown reaches zero
                if (g_wp_rotate_on && g_wp_files.size() > 1 && g_shell_xbus) {
                    if (g_wp_rotate_countdown > 0) {
                        --g_wp_rotate_countdown;
                    } else {
                        g_wp_rotate_countdown = g_wp_rotate_interval - 1;
                        g_wp_rotate_idx = (g_wp_rotate_idx + 1) % g_wp_files.size();
                        saveWpRotate();
                        WallpaperSetRequest req{};
                        std::strncpy(req.path_, g_wp_files[g_wp_rotate_idx].c_str(),
                            sizeof(req.path_) - 1);
                        req.fit_mode_ = 0u; // Cover
                        g_shell_xbus->Call("com.freebsdx.Wallpaper",
                            static_cast<uint32_t>(WallpaperOpcode::SetWallpaper),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            nullptr);
                    }
                }

                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    }, /*transparent_bg=*/true);
}
