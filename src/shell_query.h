#pragma once

// Launcher / spotlight query helpers, carved out of the monolithic main.cc.
// Pure logic — no globals, no UI, no XBus — so it lives header-only (like
// shell_helpers.h) and is unit-testable in isolation.
//
//   quickMathEval(expr) — inline calculator for the search bar ("2+2" → "= 4")
//   searchFiles(query)  — recursive $HOME filename search for the launcher
//
// MathParser is the recursive-descent evaluator behind quickMathEval.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

// ── Recursive-descent math evaluator ─────────────────────────────────────────
// Supports: +  -  *  /  ^  unary-minus  ()
// Functions: sqrt abs floor ceil round sin cos tan log ln log2 pow ; consts pi e
struct MathParser {
    const char* p;
    bool        err{false};

    double parseExpr()  { return parseAddSub(); }
    double parseAddSub() {
        double v = parseMulDiv();
        while (!err && (*p == '+' || *p == '-')) {
            char op = *p++; skipWs();
            double r = parseMulDiv();
            v = (op == '+') ? v + r : v - r;
        }
        return v;
    }
    double parseMulDiv() {
        double v = parsePow();
        while (!err && (*p == '*' || *p == '/')) {
            char op = *p++; skipWs();
            double r = parsePow();
            if (op == '/') { if (r == 0.0) { err = true; return 0; } v /= r; }
            else v *= r;
        }
        return v;
    }
    double parsePow() {
        double base = parseUnary();
        if (!err && *p == '^') { ++p; skipWs(); double e = parseUnary(); base = std::pow(base, e); }
        return base;
    }
    double parseUnary() {
        if (*p == '-') { ++p; skipWs(); return -parsePrimary(); }
        if (*p == '+') { ++p; skipWs(); }
        return parsePrimary();
    }
    double parsePrimary() {
        skipWs();
        if (*p == '(') {
            ++p; skipWs();
            double v = parseExpr();
            skipWs();
            if (*p == ')') { ++p; skipWs(); } else err = true;
            return v;
        }
        // Named function or constant?
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            const char* start = p;
            while (std::isalpha(static_cast<unsigned char>(*p))) ++p;
            std::string name(start, p);
            skipWs();
            // Constants
            if (name == "pi")  return M_PI;
            if (name == "e")   return M_E;
            // Functions
            if (*p == '(') {
                ++p; skipWs();
                double a = parseExpr();
                double b = 0;
                bool two_arg = false;
                if (*p == ',') { ++p; skipWs(); b = parseExpr(); two_arg = true; }
                skipWs();
                if (*p == ')') { ++p; skipWs(); } else err = true;
                if (name == "sqrt")  return std::sqrt(a);
                if (name == "abs")   return std::fabs(a);
                if (name == "floor") return std::floor(a);
                if (name == "ceil")  return std::ceil(a);
                if (name == "round") return std::round(a);
                if (name == "sin")   return std::sin(a);
                if (name == "cos")   return std::cos(a);
                if (name == "tan")   return std::tan(a);
                if (name == "log")   return std::log10(a);
                if (name == "ln")    return std::log(a);
                if (name == "log2")  return std::log2(a);
                if (name == "pow" && two_arg) return std::pow(a, b);
                err = true; return 0;
            }
            err = true; return 0;
        }
        // Number
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) { err = true; return 0; }
        p = end; skipWs();
        return v;
    }
    void skipWs() { while (*p == ' ' || *p == '\t') ++p; }
};

// Evaluate a search-bar expression. Returns "= <result>" or "" if not a
// well-formed, math-only expression containing at least one digit.
inline std::string quickMathEval(const std::string& expr) noexcept {
    // Must contain at least one digit
    bool has_digit = false;
    for (char c : expr)
        if (std::isdigit(static_cast<unsigned char>(c))) { has_digit = true; break; }
    if (!has_digit) return {};
    // Must only contain math-safe characters
    for (char c : expr) {
        bool ok = std::isdigit(static_cast<unsigned char>(c))
               || std::isalpha(static_cast<unsigned char>(c))
               || c == '+' || c == '-' || c == '*' || c == '/'
               || c == '^' || c == '(' || c == ')' || c == '.'
               || c == ' ' || c == '\t' || c == ',';
        if (!ok) return {};
    }
    std::string s = expr;
    MathParser mp{s.c_str()};
    mp.skipWs();
    double result = mp.parseExpr();
    if (mp.err || *mp.p != '\0') return {};
    if (!std::isfinite(result)) return {};
    if (result == std::floor(result) && std::abs(result) < 1e15)
        return std::format("= {}", static_cast<long long>(result));
    return std::format("= {:.8g}", result);
}

// Searches $HOME recursively up to maxdepth=5 for files matching query.
// Stops after max_results or 300ms. Returns list of absolute paths.
inline std::vector<std::string> searchFiles(const std::string& query, int max_results = 8) noexcept {
    std::vector<std::string> results;
    const char* home = std::getenv("HOME");
    if (!home || query.size() < 2) return results;

    std::string ql = query;
    std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);

    try {
        for (auto it = std::filesystem::recursive_directory_iterator(
                home,
                std::filesystem::directory_options::skip_permission_denied);
             it != std::filesystem::recursive_directory_iterator(); ++it)
        {
            if (std::chrono::steady_clock::now() > deadline) break;
            if (static_cast<int>(it.depth()) > 5) { it.disable_recursion_pending(); continue; }

            const auto& path = it->path();
            std::string name = path.filename().string();
            std::string namel = name;
            std::transform(namel.begin(), namel.end(), namel.begin(), ::tolower);

            if (namel.find(ql) != std::string::npos) {
                results.push_back(path.string());
                if (static_cast<int>(results.size()) >= max_results) break;
            }
        }
    } catch (...) {}
    return results;
}
