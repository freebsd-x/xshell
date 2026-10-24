// Unit tests for xshell launcher/spotlight query helpers (shell_query.h):
//   quickMathEval — the search-bar inline calculator (recursive-descent parser)
//   searchFiles   — recursive $HOME filename search
//
// quickMathEval is pure (no I/O). searchFiles touches the filesystem, so those
// tests point $HOME at an isolated temp tree to stay deterministic and fast.

#include "shell_query.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// ── quickMathEval: rejection of non-expressions ──────────────────────────────

TEST(QuickMathEvalTest, EmptyReturnsEmpty) {
    EXPECT_EQ(quickMathEval(""), "");
}

TEST(QuickMathEvalTest, NoDigitReturnsEmpty) {
    // Must contain at least one digit to be treated as a calculation.
    EXPECT_EQ(quickMathEval("hello"), "");
    EXPECT_EQ(quickMathEval("pi"), "");   // constant but no digit → not a calc
    EXPECT_EQ(quickMathEval("+-*/"), "");
}

TEST(QuickMathEvalTest, DisallowedCharactersReturnEmpty) {
    EXPECT_EQ(quickMathEval("2 $ 3"), "");   // '$' not math-safe
    EXPECT_EQ(quickMathEval("rm 2"), "");    // space ok, but not a valid expr
    EXPECT_EQ(quickMathEval("2 & 3"), "");
}

// ── quickMathEval: basic arithmetic ──────────────────────────────────────────

TEST(QuickMathEvalTest, Addition) {
    EXPECT_EQ(quickMathEval("2+2"), "= 4");
    EXPECT_EQ(quickMathEval("40+2"), "= 42");
}

TEST(QuickMathEvalTest, Subtraction) {
    EXPECT_EQ(quickMathEval("10-3"), "= 7");
    EXPECT_EQ(quickMathEval("3-10"), "= -7");
}

TEST(QuickMathEvalTest, WhitespaceIsIgnored) {
    EXPECT_EQ(quickMathEval("2 + 2"), "= 4");
    EXPECT_EQ(quickMathEval("  7  *  6  "), "= 42");
}

TEST(QuickMathEvalTest, OperatorPrecedence) {
    // multiply binds tighter than add: 2 + 3*4 = 14, not 20
    EXPECT_EQ(quickMathEval("2+3*4"), "= 14");
    EXPECT_EQ(quickMathEval("2*3+4"), "= 10");
}

TEST(QuickMathEvalTest, Parentheses) {
    EXPECT_EQ(quickMathEval("(2+3)*4"), "= 20");
    EXPECT_EQ(quickMathEval("2*(3+4)"), "= 14");
    EXPECT_EQ(quickMathEval("((1+1))"), "= 2");
}

TEST(QuickMathEvalTest, UnaryMinus) {
    EXPECT_EQ(quickMathEval("-5+3"), "= -2");
    EXPECT_EQ(quickMathEval("-(2+3)"), "= -5");
}

TEST(QuickMathEvalTest, Power) {
    EXPECT_EQ(quickMathEval("2^10"), "= 1024");
    EXPECT_EQ(quickMathEval("3^2"), "= 9");
}

TEST(QuickMathEvalTest, ChainedPowerIsNotSupported) {
    // parsePow consumes a single exponent; the trailing "^2" is leftover input,
    // which quickMathEval treats as a malformed expression → empty.
    EXPECT_EQ(quickMathEval("2^3^2"), "");
}

// ── quickMathEval: division & float formatting ───────────────────────────────

TEST(QuickMathEvalTest, IntegerDivisionResult) {
    EXPECT_EQ(quickMathEval("8/4"), "= 2");
}

TEST(QuickMathEvalTest, NonIntegerResultUses8SigFigs) {
    EXPECT_EQ(quickMathEval("10/4"), "= 2.5");
    EXPECT_EQ(quickMathEval("1/8"), "= 0.125");
}

TEST(QuickMathEvalTest, DivideByZeroReturnsEmpty) {
    EXPECT_EQ(quickMathEval("1/0"), "");
    EXPECT_EQ(quickMathEval("5/(3-3)"), "");
}

TEST(QuickMathEvalTest, FloatValuedButWholeIsPrintedAsInteger) {
    // 2.5 + 2.5 == 5.0 → printed without decimals
    EXPECT_EQ(quickMathEval("2.5+2.5"), "= 5");
    EXPECT_EQ(quickMathEval("2.0+2.0"), "= 4");
}

// ── quickMathEval: functions & constants ─────────────────────────────────────

TEST(QuickMathEvalTest, Sqrt) {
    EXPECT_EQ(quickMathEval("sqrt(16)"), "= 4");
    EXPECT_EQ(quickMathEval("sqrt(2)"), "= 1.4142136");  // {:.8g}
}

TEST(QuickMathEvalTest, AbsFloorCeilRound) {
    EXPECT_EQ(quickMathEval("abs(-5)"), "= 5");
    EXPECT_EQ(quickMathEval("floor(3.7)"), "= 3");
    EXPECT_EQ(quickMathEval("ceil(3.2)"), "= 4");
    EXPECT_EQ(quickMathEval("round(2.5)"), "= 3");  // round-half-away-from-zero
}

TEST(QuickMathEvalTest, TwoArgPow) {
    EXPECT_EQ(quickMathEval("pow(2,10)"), "= 1024");
}

TEST(QuickMathEvalTest, PowWithoutSecondArgIsError) {
    // "pow" requires two args; one-arg form is rejected → empty.
    EXPECT_EQ(quickMathEval("pow(2)"), "");
}

TEST(QuickMathEvalTest, UnknownFunctionReturnsEmpty) {
    EXPECT_EQ(quickMathEval("frobnicate(2)"), "");
}

TEST(QuickMathEvalTest, ConstantPiInExpression) {
    // pi ≈ 3.14159265 → 8 sig figs → "3.1415927"
    auto r = quickMathEval("pi*1");
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.substr(0, 9), "= 3.14159");
}

TEST(QuickMathEvalTest, ConstantEInExpression) {
    // e ≈ 2.71828 → check "2*e" ≈ 5.4365637
    auto r = quickMathEval("e*2");
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.substr(0, 7), "= 5.436");
}

// ── quickMathEval: malformed input ───────────────────────────────────────────

TEST(QuickMathEvalTest, TrailingGarbageReturnsEmpty) {
    EXPECT_EQ(quickMathEval("2+2)"), "");    // unbalanced close paren
    EXPECT_EQ(quickMathEval("2+2x"), "");    // trailing identifier
}

TEST(QuickMathEvalTest, IncompleteExpressionReturnsEmpty) {
    EXPECT_EQ(quickMathEval("2+"), "");
    EXPECT_EQ(quickMathEval("*3"), "");
    EXPECT_EQ(quickMathEval("(2+3"), "");    // missing close paren
}

TEST(QuickMathEvalTest, LargeIntegerResult) {
    EXPECT_EQ(quickMathEval("1000000*1000000"), "= 1000000000000");
}

// ── searchFiles: filesystem search over an isolated temp $HOME ────────────────

namespace {
// RAII helper: build a temp directory tree, point $HOME at it for the test's
// duration, and restore the previous $HOME on destruction.
class ScopedTempHome {
public:
    ScopedTempHome() {
        const char* prev = std::getenv("HOME");
        prev_home_ = prev ? prev : "";
        root_ = fs::temp_directory_path() /
                ("fbx_query_test_" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
        ::setenv("HOME", root_.c_str(), 1);
    }
    ~ScopedTempHome() {
        if (prev_home_.empty()) ::unsetenv("HOME");
        else                    ::setenv("HOME", prev_home_.c_str(), 1);
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    const fs::path& root() const { return root_; }
    void touch(const std::string& rel) const {
        fs::path p = root_ / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    }
private:
    fs::path    root_;
    std::string prev_home_;
};

bool contains_name(const std::vector<std::string>& v, const std::string& name) {
    for (const auto& p : v)
        if (fs::path(p).filename().string() == name) return true;
    return false;
}
}  // namespace

TEST(SearchFilesTest, ShortQueryReturnsEmpty) {
    ScopedTempHome home;
    home.touch("alpha.txt");
    // Query shorter than 2 chars is rejected outright.
    EXPECT_TRUE(searchFiles("a").empty());
    EXPECT_TRUE(searchFiles("").empty());
}

TEST(SearchFilesTest, FindsMatchingFile) {
    ScopedTempHome home;
    home.touch("alpha.txt");
    home.touch("beta.log");
    auto r = searchFiles("alpha");
    ASSERT_FALSE(r.empty());
    EXPECT_TRUE(contains_name(r, "alpha.txt"));
    EXPECT_FALSE(contains_name(r, "beta.log"));
}

TEST(SearchFilesTest, CaseInsensitive) {
    ScopedTempHome home;
    home.touch("ReadMe.md");
    auto r = searchFiles("readme");
    EXPECT_TRUE(contains_name(r, "ReadMe.md"));
}

TEST(SearchFilesTest, RecursesIntoSubdirectories) {
    ScopedTempHome home;
    home.touch("top.txt");
    home.touch("sub/nested/deep.txt");
    auto r = searchFiles("txt");
    EXPECT_TRUE(contains_name(r, "top.txt"));
    EXPECT_TRUE(contains_name(r, "deep.txt"));
}

TEST(SearchFilesTest, NoMatchReturnsEmpty) {
    ScopedTempHome home;
    home.touch("alpha.txt");
    EXPECT_TRUE(searchFiles("zzzznomatch").empty());
}

TEST(SearchFilesTest, RespectsMaxResults) {
    ScopedTempHome home;
    for (int i = 0; i < 10; ++i)
        home.touch("match_" + std::to_string(i) + ".dat");
    auto r = searchFiles("match", /*max_results=*/3);
    EXPECT_LE(r.size(), 3u);
    EXPECT_FALSE(r.empty());
}
