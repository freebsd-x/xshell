#pragma once
// Pure, stateless helper functions for xshell.
// No globals, no I/O, no OS calls — safe to include in unit tests.

#include <string>
#include <array>
#include <algorithm>
#include <format>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// ── Timer input parser ────────────────────────────────────────────────────────
// Accepts: "10m", "1h30m", "90s", "1:30", "HH:MM", bare number (treated as s)
// Returns total seconds, or 0 on failure.
inline int parseTimerInput(const std::string& raw) noexcept {
    if (raw.empty()) return 0;
    // "HH:MM" or "M:SS" style — left of colon * 60 + right
    auto colon = raw.find(':');
    if (colon != std::string::npos) {
        int a = std::atoi(raw.c_str());
        int b = std::atoi(raw.c_str() + colon + 1);
        return a * 60 + b;
    }
    int total = 0;
    const char* p = raw.c_str();
    while (*p) {
        int v = 0;
        bool has_digit = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); has_digit = true; }
        if (!has_digit) break;
        char unit = *p ? static_cast<char>(::tolower(*p++)) : 's';
        if      (unit == 'h') total += v * 3600;
        else if (unit == 'm') total += v * 60;
        else                  total += v;
    }
    return total;
}

// ── Countdown formatter ───────────────────────────────────────────────────────
// Formats seconds as "Xh MM:SS" / "Xm XXs" / "Xs" / "Done"
inline std::string fmtCountdown(int secs) noexcept {
    if (secs <= 0) return "Done";
    int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    if (h > 0) return std::format("{}h {:02d}m {:02d}s", h, m, s);
    if (m > 0) return std::format("{}m {:02d}s", m, s);
    return std::format("{}s", s);
}

// ── Calendar quick-add parser ─────────────────────────────────────────────────
// Extracts title / hour / minute / tomorrow-flag from free-text like "Call at 3pm"
// Returns false if no recognisable time token is found.
inline bool parseCalInput(const std::string& raw,
                          std::string& out_title,
                          int& out_hour, int& out_min,
                          bool& out_tomorrow) noexcept {
    if (raw.empty()) return false;
    std::string sl(raw.size(), '\0');
    std::transform(raw.begin(), raw.end(), sl.begin(), ::tolower);

    out_tomorrow = (sl.find("tomorrow") != std::string::npos);

    int    best_h = -1, best_m = 0;
    size_t best_start = std::string::npos, best_end = 0;

    for (size_t i = 0; i < raw.size(); ) {
        if (!std::isdigit(static_cast<unsigned char>(raw[i]))) { ++i; continue; }
        size_t start = i;
        int n = 0;
        while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i])))
            n = n * 10 + (raw[i++] - '0');
        int h = n, m = 0;
        size_t end = i;
        bool has_colon = false;
        if (i < raw.size() && raw[i] == ':') {
            ++i; has_colon = true;
            if (i + 1 < raw.size() &&
                std::isdigit(static_cast<unsigned char>(raw[i])) &&
                std::isdigit(static_cast<unsigned char>(raw[i + 1]))) {
                m = (raw[i] - '0') * 10 + (raw[i + 1] - '0');
                i += 2; end = i;
            } else { continue; }
        }
        size_t k = i;
        while (k < raw.size() && raw[k] == ' ') ++k;
        bool has_ampm = false;
        if (k + 1 < sl.size()) {
            std::string ap = sl.substr(k, 2);
            if (ap == "am" || ap == "pm") {
                if (ap == "pm" && h != 12) h += 12;
                if (ap == "am" && h == 12) h = 0;
                end = k + 2; has_ampm = true;
            }
        }
        if (h > 23 || m > 59) continue;
        if (!has_ampm && !has_colon && h < 10) continue; // bare "3" is ambiguous
        best_h = h; best_m = m; best_start = start; best_end = end;
    }
    (void)best_end;
    if (best_h < 0) return false;
    out_hour = best_h; out_min = best_m;

    // Build title from text before the time token
    std::string pre = (best_start == std::string::npos) ? raw : raw.substr(0, best_start);
    // Strip trailing " at" or " at "
    {
        std::string prel(pre.size(), '\0');
        std::transform(pre.begin(), pre.end(), prel.begin(), ::tolower);
        auto at = prel.rfind(" at");
        if (at != std::string::npos && at + 3 >= prel.size() - 1)
            pre = pre.substr(0, at);
    }
    // Strip "tomorrow"
    {
        std::string prel(pre.size(), '\0');
        std::transform(pre.begin(), pre.end(), prel.begin(), ::tolower);
        auto tp = prel.find("tomorrow");
        if (tp != std::string::npos) pre.erase(tp, 8);
    }
    while (!pre.empty() && (pre.back() == ' ' || pre.back() == ',' || pre.back() == '-'))
        pre.pop_back();
    while (!pre.empty() && pre.front() == ' ') pre.erase(0, 1);
    out_title = pre.empty() ? "Event" : pre;
    return true;
}

// ── App heatmap formatter ─────────────────────────────────────────────────────
// Renders 24 hourly counts as a sparkline using block characters ▁▂▃▄▅▆▇█
inline std::string fmtHeatmap(const std::array<uint32_t, 24>& h) noexcept {
    static constexpr const char* kBlocks[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    uint32_t mx = *std::max_element(h.begin(), h.end());
    std::string s;
    s.reserve(24 * 4);
    for (int i = 0; i < 24; ++i) {
        int idx = mx > 0 ? static_cast<int>(h[i] * 7u / mx) : 0;
        s += kBlocks[idx];
    }
    return s;
}

// ── Network sparkline formatter ───────────────────────────────────────────────
static constexpr int kNetSparkLen = 10;

// Renders kNetSparkLen samples from a ring buffer h[] with write head at tail.
// Oldest sample is at index (tail - kNetSparkLen + 1) % kNetSparkLen.
inline std::string fmtNetSpark(const std::array<uint64_t, kNetSparkLen>& h, int tail) noexcept {
    static constexpr const char* kBlocks[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
    uint64_t mx = *std::max_element(h.begin(), h.end());
    std::string s;
    s.reserve(kNetSparkLen * 4);
    for (int i = 0; i < kNetSparkLen; ++i) {
        int idx_i = (tail - kNetSparkLen + 1 + i + kNetSparkLen) % kNetSparkLen;
        int b = mx > 0 ? static_cast<int>(h[static_cast<size_t>(idx_i)] * 7u / mx) : 0;
        s += kBlocks[b];
    }
    return s;
}

// ── DND countdown formatter ───────────────────────────────────────────────────
// Pure inner function: given diff_minutes until DND ends, formats the string.
// Called by dndCountdownText() in main.cc after resolving globals and wall time.
inline std::string dndCountdownStr(int diff_minutes) noexcept {
    int h = diff_minutes / 60, m = diff_minutes % 60;
    if (h > 0 && m > 0) return std::format("DND ends in {}h {}m", h, m);
    if (h > 0)           return std::format("DND ends in {}h", h);
    return std::format("DND ends in {}m", m);
}
