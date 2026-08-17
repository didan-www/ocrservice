#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ProtocolTime.h"

namespace {

using ocrservice::serialization::time::TimeError;
using ocrservice::serialization::time::UtcTimePoint;
using ocrservice::serialization::time::formatProtocolTime;
using ocrservice::serialization::time::parseProtocolTime;

TEST(ProtocolTimeTest, ParsesToUtcMillisecondsAndFormatsFixedOffset) {
    EXPECT_EQ(parseProtocolTime("1970-01-01T08:00:00.000+08:00").unixMilliseconds(), 0);
    EXPECT_EQ(parseProtocolTime("1970-01-01T00:00:00.001+08:00").unixMilliseconds(),
              -8LL * 60LL * 60LL * 1000LL + 1LL);
    EXPECT_EQ(
        formatProtocolTime(UtcTimePoint(0)), "1970-01-01T08:00:00.000+08:00");
}

TEST(ProtocolTimeTest, AcceptsLeapDaysAndYearBoundaries) {
    for (const std::string value : {
             "1000-01-01T08:00:00.000+08:00",
             "2000-02-29T23:59:59.999+08:00",
             "2024-02-29T12:00:00.123+08:00",
             "9999-12-31T23:59:59.999+08:00",
         }) {
        EXPECT_EQ(formatProtocolTime(parseProtocolTime(value)), value);
    }
    EXPECT_THROW(parseProtocolTime("1900-02-29T00:00:00.000+08:00"), TimeError);
    EXPECT_THROW(parseProtocolTime("2023-02-29T00:00:00.000+08:00"), TimeError);
}

TEST(ProtocolTimeTest, RejectsNonExactFormatOffsetAndLeapSecond) {
    const std::vector<std::string> invalid = {
        "2026-08-15T12:30:45+08:00",
        "2026-08-15T12:30:45.12+08:00",
        "2026-08-15T12:30:45.1234+08:00",
        "2026-08-15T12:30:45.123Z",
        "2026-08-15T12:30:45.123+00:00",
        "2026-08-15 12:30:45.123+08:00",
        "2026-08-15T12:30:60.000+08:00",
        "0999-12-31T23:59:59.999+08:00",
    };
    for (const auto& value : invalid) {
        EXPECT_THROW(parseProtocolTime(value), TimeError) << value;
    }
}

TEST(ProtocolTimeTest, LowerBoundMatchesMySqlDatetimeUtcRange) {
    constexpr std::int64_t kMySqlDatetimeMinimumUtcMilliseconds = -30610224000000LL;
    const auto minimum = parseProtocolTime("1000-01-01T08:00:00.000+08:00");
    EXPECT_EQ(minimum.unixMilliseconds(), kMySqlDatetimeMinimumUtcMilliseconds);
    EXPECT_EQ(formatProtocolTime(minimum), "1000-01-01T08:00:00.000+08:00");

    EXPECT_THROW(parseProtocolTime("1000-01-01T07:59:59.999+08:00"), TimeError);
    EXPECT_THROW(parseProtocolTime("1000-01-01T00:00:00.000+08:00"), TimeError);
}

TEST(ProtocolTimeTest, RejectsInvalidCalendarAndClockFields) {
    for (const std::string value : {
             "2026-00-01T00:00:00.000+08:00",
             "2026-13-01T00:00:00.000+08:00",
             "2026-04-31T00:00:00.000+08:00",
             "2026-01-00T00:00:00.000+08:00",
             "2026-01-01T24:00:00.000+08:00",
             "2026-01-01T00:60:00.000+08:00",
         }) {
        EXPECT_THROW(parseProtocolTime(value), TimeError) << value;
    }
}

TEST(ProtocolTimeTest, FormattingRejectsLocalYearsOutsideContract) {
    const auto first = parseProtocolTime("1000-01-01T08:00:00.000+08:00");
    EXPECT_THROW(formatProtocolTime(UtcTimePoint(first.unixMilliseconds() - 1)), TimeError);
    const auto last = parseProtocolTime("9999-12-31T23:59:59.999+08:00");
    EXPECT_THROW(formatProtocolTime(UtcTimePoint(last.unixMilliseconds() + 1)), TimeError);
    EXPECT_THROW(
        formatProtocolTime(UtcTimePoint(std::numeric_limits<std::int64_t>::min())), TimeError);
    EXPECT_THROW(
        formatProtocolTime(UtcTimePoint(std::numeric_limits<std::int64_t>::max())), TimeError);
}

}  // namespace
