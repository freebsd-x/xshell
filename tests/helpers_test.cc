// Unit tests for xshell pure helper functions (shell_helpers.h).
// No globals, no I/O, no XBus — runs anywhere gtest is available.

#include "shell_helpers.h"
#include <gtest/gtest.h>

// ── parseTimerInput ───────────────────────────────────────────────────────────

TEST(ParseTimerInputTest, Empty) {
    EXPECT_EQ(parseTimerInput(""), 0);
}

TEST(ParseTimerInputTest, SecondsUnit) {
    EXPECT_EQ(parseTimerInput("30s"), 30);
    EXPECT_EQ(parseTimerInput("90s"), 90);
}

TEST(ParseTimerInputTest, MinutesUnit) {
    EXPECT_EQ(parseTimerInput("10m"), 600);
    EXPECT_EQ(parseTimerInput("1m"),  60);
    EXPECT_EQ(parseTimerInput("60m"), 3600);
}

TEST(ParseTimerInputTest, HoursUnit) {
    EXPECT_EQ(parseTimerInput("1h"),  3600);
    EXPECT_EQ(parseTimerInput("2h"),  7200);
}

TEST(ParseTimerInputTest, CompoundHoursMinutes) {
    EXPECT_EQ(parseTimerInput("1h30m"), 5400);
    EXPECT_EQ(parseTimerInput("2h5m"),  7500);
}

TEST(ParseTimerInputTest, CompoundHoursMinutesSeconds) {
    EXPECT_EQ(parseTimerInput("1h2m3s"), 3600 + 120 + 3);
}

TEST(ParseTimerInputTest, ColonStyleMMSS) {
    // "1:30" → 1*60 + 30 = 90
    EXPECT_EQ(parseTimerInput("1:30"), 90);
}

TEST(ParseTimerInputTest, ColonStyleHHMM) {
    // "10:00" → 10*60 = 600
    EXPECT_EQ(parseTimerInput("10:00"), 600);
    // "1:05" → 65
    EXPECT_EQ(parseTimerInput("1:05"), 65);
}

TEST(ParseTimerInputTest, BareNumberTreatedAsSeconds) {
    // No unit suffix → treated as seconds by the 's' default
    EXPECT_EQ(parseTimerInput("45"), 45);
}

TEST(ParseTimerInputTest, InvalidStringReturnsZero) {
    EXPECT_EQ(parseTimerInput("abc"),  0);
    EXPECT_EQ(parseTimerInput("xyz"),  0);
}

// ── fmtCountdown ─────────────────────────────────────────────────────────────

TEST(FmtCountdownTest, Zero) {
    EXPECT_EQ(fmtCountdown(0), "Done");
}

TEST(FmtCountdownTest, Negative) {
    EXPECT_EQ(fmtCountdown(-1), "Done");
}

TEST(FmtCountdownTest, SecondsBelowMinute) {
    EXPECT_EQ(fmtCountdown(1),  "1s");
    EXPECT_EQ(fmtCountdown(59), "59s");
}

TEST(FmtCountdownTest, ExactlyOneMinute) {
    EXPECT_EQ(fmtCountdown(60), "1m 00s");
}

TEST(FmtCountdownTest, MinutesAndSeconds) {
    EXPECT_EQ(fmtCountdown(90),   "1m 30s");
    EXPECT_EQ(fmtCountdown(3599), "59m 59s");
}

TEST(FmtCountdownTest, ExactlyOneHour) {
    EXPECT_EQ(fmtCountdown(3600), "1h 00m 00s");
}

TEST(FmtCountdownTest, HoursMinutesSeconds) {
    EXPECT_EQ(fmtCountdown(3661), "1h 01m 01s");
    EXPECT_EQ(fmtCountdown(7384), "2h 03m 04s");
}

TEST(FmtCountdownTest, LeadingZeroPaddingOnMinutesAndSeconds) {
    // 3600 + 5 = 1h 00m 05s
    EXPECT_EQ(fmtCountdown(3605), "1h 00m 05s");
}

// ── parseCalInput ─────────────────────────────────────────────────────────────

TEST(ParseCalInputTest, EmptyReturnsFalse) {
    std::string t; int h = 0, m = 0; bool tom = false;
    EXPECT_FALSE(parseCalInput("", t, h, m, tom));
}

TEST(ParseCalInputTest, NoTimeTokenReturnsFalse) {
    std::string t; int h = 0, m = 0; bool tom = false;
    EXPECT_FALSE(parseCalInput("Lunch with Alice", t, h, m, tom));
}

TEST(ParseCalInputTest, SimplePm) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Call at 3pm", t, h, m, tom));
    EXPECT_EQ(t, "Call");
    EXPECT_EQ(h, 15);
    EXPECT_EQ(m, 0);
    EXPECT_FALSE(tom);
}

TEST(ParseCalInputTest, SimpleAm) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Standup at 9am", t, h, m, tom));
    EXPECT_EQ(t, "Standup");
    EXPECT_EQ(h, 9);
    EXPECT_EQ(m, 0);
    EXPECT_FALSE(tom);
}

TEST(ParseCalInputTest, Noon12pm) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Lunch 12pm", t, h, m, tom));
    EXPECT_EQ(h, 12); // 12pm stays 12
    EXPECT_EQ(m, 0);
}

TEST(ParseCalInputTest, Midnight12am) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Run 12am", t, h, m, tom));
    EXPECT_EQ(h, 0); // 12am → 0
}

TEST(ParseCalInputTest, WithColonTime) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Meeting at 14:30", t, h, m, tom));
    EXPECT_EQ(t, "Meeting");
    EXPECT_EQ(h, 14);
    EXPECT_EQ(m, 30);
}

TEST(ParseCalInputTest, TomorrowFlag) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Doctor tomorrow at 10am", t, h, m, tom));
    EXPECT_TRUE(tom);
    EXPECT_EQ(t, "Doctor");
    EXPECT_EQ(h, 10);
}

TEST(ParseCalInputTest, TitleIsEventWhenOnlyTimeGiven) {
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("15:00", t, h, m, tom));
    EXPECT_EQ(t, "Event");
}

TEST(ParseCalInputTest, AmbiguousBareDigitIsIgnored) {
    // Bare "3" (no am/pm, no colon, h < 10) should not be accepted as a time
    std::string t; int h = 0, m = 0; bool tom = false;
    EXPECT_FALSE(parseCalInput("meeting at 3", t, h, m, tom));
}

TEST(ParseCalInputTest, TwoDigitBareHourIsAccepted) {
    // "10" >= 10, so it's accepted even without am/pm
    std::string t; int h = 0, m = 0; bool tom = false;
    ASSERT_TRUE(parseCalInput("Review 10", t, h, m, tom));
    EXPECT_EQ(h, 10);
}

// ── fmtHeatmap ───────────────────────────────────────────────────────────────

TEST(FmtHeatmapTest, AllZeroReturnsAllLowest) {
    std::array<uint32_t, 24> h{};
    h.fill(0);
    auto s = fmtHeatmap(h);
    // When max == 0 every slot maps to kBlocks[0] = "▁"
    // Each block is 3 UTF-8 bytes; 24 × 3 = 72
    EXPECT_EQ(s.size(), 24u * 3u);
    // All chars should be "▁"
    for (size_t i = 0; i < 24; ++i)
        EXPECT_EQ(s.substr(i * 3, 3), "▁") << "slot " << i;
}

TEST(FmtHeatmapTest, AllEqualReturnsAllHighest) {
    std::array<uint32_t, 24> h{};
    h.fill(5);
    auto s = fmtHeatmap(h);
    // All equal → idx = 5*7/5 = 7 → "█"
    for (size_t i = 0; i < 24; ++i)
        EXPECT_EQ(s.substr(i * 3, 3), "█") << "slot " << i;
}

TEST(FmtHeatmapTest, SinglePeakAtMidnight) {
    std::array<uint32_t, 24> h{};
    h.fill(0);
    h[0] = 10; // midnight is the max
    auto s = fmtHeatmap(h);
    // slot 0 → 10*7/10 = 7 → "█"
    EXPECT_EQ(s.substr(0, 3), "█");
    // all others → 0 → "▁"
    for (size_t i = 1; i < 24; ++i)
        EXPECT_EQ(s.substr(i * 3, 3), "▁") << "slot " << i;
}

TEST(FmtHeatmapTest, OutputLength) {
    std::array<uint32_t, 24> h{};
    // Each block glyph is 3 UTF-8 bytes
    EXPECT_EQ(fmtHeatmap(h).size(), 24u * 3u);
}

// ── fmtNetSpark ──────────────────────────────────────────────────────────────

TEST(FmtNetSparkTest, AllZeroAllLowest) {
    std::array<uint64_t, kNetSparkLen> h{};
    h.fill(0);
    auto s = fmtNetSpark(h, kNetSparkLen - 1);
    EXPECT_EQ(s.size(), (size_t)kNetSparkLen * 3u);
    for (int i = 0; i < kNetSparkLen; ++i)
        EXPECT_EQ(s.substr((size_t)i * 3, 3), "▁") << "slot " << i;
}

TEST(FmtNetSparkTest, AllEqualAllHighest) {
    std::array<uint64_t, kNetSparkLen> h{};
    h.fill(100);
    auto s = fmtNetSpark(h, kNetSparkLen - 1);
    for (int i = 0; i < kNetSparkLen; ++i)
        EXPECT_EQ(s.substr((size_t)i * 3, 3), "█") << "slot " << i;
}

TEST(FmtNetSparkTest, OutputLength) {
    std::array<uint64_t, kNetSparkLen> h{};
    EXPECT_EQ(fmtNetSpark(h, 0).size(), (size_t)kNetSparkLen * 3u);
}

TEST(FmtNetSparkTest, RingOrderOldestToNewest) {
    // Fill with increasing values so the newest entry is largest.
    // With tail = kNetSparkLen-1, slot kNetSparkLen-1 is the newest (max → "█")
    // and slot 0 is the oldest (smallest → should produce a low block).
    std::array<uint64_t, kNetSparkLen> h{};
    for (int i = 0; i < kNetSparkLen; ++i) h[(size_t)i] = (uint64_t)(i + 1);
    auto s = fmtNetSpark(h, kNetSparkLen - 1);
    // Last rendered glyph (newest, largest) should be "█"
    EXPECT_EQ(s.substr((size_t)(kNetSparkLen - 1) * 3, 3), "█");
    // First rendered glyph (oldest, value=1 vs max=10) should be "▁"
    EXPECT_EQ(s.substr(0, 3), "▁");
}

// ── dndCountdownStr ───────────────────────────────────────────────────────────

TEST(DndCountdownStrTest, HoursAndMinutes) {
    EXPECT_EQ(dndCountdownStr(90),  "DND ends in 1h 30m");
    EXPECT_EQ(dndCountdownStr(121), "DND ends in 2h 1m");
}

TEST(DndCountdownStrTest, ExactHours) {
    EXPECT_EQ(dndCountdownStr(60),  "DND ends in 1h");
    EXPECT_EQ(dndCountdownStr(120), "DND ends in 2h");
}

TEST(DndCountdownStrTest, MinutesOnly) {
    EXPECT_EQ(dndCountdownStr(1),  "DND ends in 1m");
    EXPECT_EQ(dndCountdownStr(45), "DND ends in 45m");
    EXPECT_EQ(dndCountdownStr(59), "DND ends in 59m");
}

TEST(DndCountdownStrTest, FullDay) {
    // Maximum wrap-around value: 24*60 - 1 = 1439 minutes
    EXPECT_EQ(dndCountdownStr(1439), "DND ends in 23h 59m");
}
