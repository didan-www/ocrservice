#include "ProtocolTime.h"

#include <array>
#include <cstdio>
#include <limits>

namespace ocrservice::serialization::time {
namespace {

constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr std::int64_t kSecondsPerDay = 86400;
constexpr std::int64_t kOffsetSeconds = 8 * 60 * 60;

int parseDigits(const std::string_view input, const std::size_t offset, const std::size_t count) {
    int result = 0;
    for (std::size_t index = 0U; index < count; ++index) {
        const char value = input[offset + index];
        if (value < '0' || value > '9') {
            throw TimeError("protocol time contains a non-digit");
        }
        result = result * 10 + (value - '0');
    }
    return result;
}

bool isLeapYear(const int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(const int year, const int month) noexcept {
    constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && isLeapYear(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
}

// Howard Hinnant's civil calendar conversion, relative to 1970-01-01.
std::int64_t daysFromCivil(int year, const unsigned month, const unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const auto shiftedMonth = month > 2U ? month - 3U : month + 9U;
    const auto dayOfYear = (153U * shiftedMonth + 2U) / 5U + day - 1U;
    const auto dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) - 719468;
}

void civilFromDays(
    std::int64_t days,
    int& year,
    unsigned& month,
    unsigned& day) noexcept {
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const auto yearOfEra =
        (dayOfEra - dayOfEra / 1460U + dayOfEra / 36524U - dayOfEra / 146096U) / 365U;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400;
    const auto dayOfYear = dayOfEra - (365U * yearOfEra + yearOfEra / 4U - yearOfEra / 100U);
    const auto monthPrime = (5U * dayOfYear + 2U) / 153U;
    day = dayOfYear - (153U * monthPrime + 2U) / 5U + 1U;
    month = monthPrime < 10U ? monthPrime + 3U : monthPrime - 9U;
    year += month <= 2U ? 1 : 0;
}

std::int64_t floorDivide(const std::int64_t value, const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

}  // namespace

UtcTimePoint parseProtocolTime(const std::string_view input) {
    constexpr std::string_view kSuffix = "+08:00";
    if (input.size() != 29U || input[4] != '-' || input[7] != '-' || input[10] != 'T' ||
        input[13] != ':' || input[16] != ':' || input[19] != '.' ||
        input.substr(23) != kSuffix) {
        throw TimeError("protocol time must use YYYY-MM-DDTHH:mm:ss.SSS+08:00");
    }

    const int year = parseDigits(input, 0U, 4U);
    const int month = parseDigits(input, 5U, 2U);
    const int day = parseDigits(input, 8U, 2U);
    const int hour = parseDigits(input, 11U, 2U);
    const int minute = parseDigits(input, 14U, 2U);
    const int second = parseDigits(input, 17U, 2U);
    const int millisecond = parseDigits(input, 20U, 3U);

    if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) {
        throw TimeError("protocol time contains an invalid calendar value");
    }

    const auto localSeconds =
        daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) *
            kSecondsPerDay +
        hour * 3600 + minute * 60 + second;
    const auto utcMilliseconds =
        (localSeconds - kOffsetSeconds) * kMillisecondsPerSecond + millisecond;
    const auto minimumUtcMilliseconds =
        daysFromCivil(1000, 1U, 1U) * kSecondsPerDay * kMillisecondsPerSecond;
    if (utcMilliseconds < minimumUtcMilliseconds) {
        throw TimeError("protocol time is earlier than the supported UTC range");
    }
    return UtcTimePoint(utcMilliseconds);
}

std::string formatProtocolTime(const UtcTimePoint timePoint) {
    constexpr std::int64_t kMillisecondsPerDay =
        kSecondsPerDay * kMillisecondsPerSecond;
    const auto minimumUtcMilliseconds =
        daysFromCivil(1000, 1U, 1U) * kMillisecondsPerDay;
    const auto maximumUtcMillisecondsExclusive =
        daysFromCivil(10000, 1U, 1U) * kMillisecondsPerDay -
        kOffsetSeconds * kMillisecondsPerSecond;
    if (timePoint.unixMilliseconds() < minimumUtcMilliseconds ||
        timePoint.unixMilliseconds() >= maximumUtcMillisecondsExclusive) {
        throw TimeError("protocol time year is outside 1000..9999");
    }
    const auto localMilliseconds =
        timePoint.unixMilliseconds() + kOffsetSeconds * kMillisecondsPerSecond;
    const auto localSeconds = floorDivide(localMilliseconds, kMillisecondsPerSecond);
    const auto millisecond = static_cast<int>(
        localMilliseconds - localSeconds * kMillisecondsPerSecond);
    const auto days = floorDivide(localSeconds, kSecondsPerDay);
    const auto secondOfDay = localSeconds - days * kSecondsPerDay;

    int year = 0;
    unsigned month = 0U;
    unsigned day = 0U;
    civilFromDays(days, year, month, day);
    const auto hour = static_cast<int>(secondOfDay / 3600);
    const auto minute = static_cast<int>((secondOfDay % 3600) / 60);
    const auto second = static_cast<int>(secondOfDay % 60);
    std::array<char, 30> output{};
    const int written = std::snprintf(
        output.data(),
        output.size(),
        "%04d-%02u-%02uT%02d:%02d:%02d.%03d+08:00",
        year,
        month,
        day,
        hour,
        minute,
        second,
        millisecond);
    if (written != 29) {
        throw TimeError("failed to format protocol time");
    }
    return std::string(output.data(), static_cast<std::size_t>(written));
}

}  // namespace ocrservice::serialization::time
