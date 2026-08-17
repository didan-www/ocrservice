#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ocrservice::serialization::time {

class TimeError final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class UtcTimePoint final {
public:
    explicit constexpr UtcTimePoint(const std::int64_t unixMilliseconds) noexcept
        : unixMilliseconds_(unixMilliseconds) {}

    constexpr std::int64_t unixMilliseconds() const noexcept {
        return unixMilliseconds_;
    }

private:
    std::int64_t unixMilliseconds_;
};

constexpr bool operator==(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return left.unixMilliseconds() == right.unixMilliseconds();
}
constexpr bool operator!=(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return !(left == right);
}
constexpr bool operator<(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return left.unixMilliseconds() < right.unixMilliseconds();
}
constexpr bool operator<=(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return left.unixMilliseconds() <= right.unixMilliseconds();
}
constexpr bool operator>(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return right < left;
}
constexpr bool operator>=(const UtcTimePoint left, const UtcTimePoint right) noexcept {
    return right <= left;
}

UtcTimePoint parseProtocolTime(std::string_view input);
std::string formatProtocolTime(UtcTimePoint timePoint);

}  // namespace ocrservice::serialization::time
