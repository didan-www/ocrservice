#include "Identifiers.h"

#include <algorithm>
#include <utility>

#include "Utf8.h"

namespace ocrservice::domain {
namespace {

int hexValue(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool isDeviceCharacter(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-';
}

bool isDeviceFirstCharacter(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9');
}

}  // namespace

Uuid::Uuid(std::array<std::uint8_t, 16> bytes) noexcept : bytes_(bytes) {}

Uuid Uuid::parse(const std::string_view input) {
    if (input.size() != 36U || input[8] != '-' || input[13] != '-' || input[18] != '-' ||
        input[23] != '-') {
        throw DomainError("UUID must use canonical 8-4-4-4-12 form");
    }
    std::array<std::uint8_t, 16> bytes{};
    std::size_t byteIndex = 0U;
    for (std::size_t index = 0U; index < input.size();) {
        if (input[index] == '-') {
            ++index;
            continue;
        }
        if (index + 1U >= input.size() || input[index + 1U] == '-') {
            throw DomainError("UUID contains a misplaced separator");
        }
        const int high = hexValue(input[index]);
        const int low = hexValue(input[index + 1U]);
        if (high < 0 || low < 0 || byteIndex >= bytes.size()) {
            throw DomainError("UUID contains invalid hexadecimal digits");
        }
        bytes[byteIndex++] = static_cast<std::uint8_t>((high << 4) | low);
        index += 2U;
    }
    if (byteIndex != bytes.size() ||
        std::all_of(bytes.begin(), bytes.end(), [](const std::uint8_t value) { return value == 0U; })) {
        throw DomainError("UUID must not be nil");
    }
    const auto version = static_cast<std::uint8_t>(bytes[6] >> 4U);
    if (version < 1U || version > 8U) {
        throw DomainError("UUID version must be in 1..8");
    }
    if ((bytes[8] & 0xC0U) != 0x80U) {
        throw DomainError("UUID must use the RFC variant");
    }
    return Uuid(bytes);
}

Uuid Uuid::v4(std::array<std::uint8_t, 16> randomBytes) {
    randomBytes[6] = static_cast<std::uint8_t>((randomBytes[6] & 0x0FU) | 0x40U);
    randomBytes[8] = static_cast<std::uint8_t>((randomBytes[8] & 0x3FU) | 0x80U);
    return Uuid(randomBytes);
}

const std::array<std::uint8_t, 16>& Uuid::bytes() const noexcept {
    return bytes_;
}

std::uint8_t Uuid::version() const noexcept {
    return static_cast<std::uint8_t>(bytes_[6] >> 4U);
}

std::string Uuid::toString() const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36U);
    for (std::size_t index = 0U; index < bytes_.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            result.push_back('-');
        }
        result.push_back(kHex[bytes_[index] >> 4U]);
        result.push_back(kHex[bytes_[index] & 0x0FU]);
    }
    return result;
}

bool operator==(const Uuid& left, const Uuid& right) noexcept {
    return left.bytes_ == right.bytes_;
}

bool operator!=(const Uuid& left, const Uuid& right) noexcept {
    return !(left == right);
}

RecognitionId::RecognitionId(Uuid value) noexcept : value_(std::move(value)) {}
RecognitionId RecognitionId::parse(const std::string_view input) {
    return RecognitionId(Uuid::parse(input));
}
const Uuid& RecognitionId::value() const noexcept {
    return value_;
}
std::string RecognitionId::toString() const {
    return value_.toString();
}
bool operator==(const RecognitionId& left, const RecognitionId& right) noexcept {
    return left.value_ == right.value_;
}

CaptureId::CaptureId(Uuid value) noexcept : value_(std::move(value)) {}
CaptureId CaptureId::parse(const std::string_view input) {
    return CaptureId(Uuid::parse(input));
}
const Uuid& CaptureId::value() const noexcept {
    return value_;
}
std::string CaptureId::toString() const {
    return value_.toString();
}
bool operator==(const CaptureId& left, const CaptureId& right) noexcept {
    return left.value_ == right.value_;
}

DeviceId::DeviceId(std::string value) : value_(std::move(value)) {}
DeviceId DeviceId::parse(const std::string_view input) {
    if (input.empty() || input.size() > 64U || !isDeviceFirstCharacter(input.front()) ||
        !std::all_of(input.begin(), input.end(), isDeviceCharacter)) {
        throw DomainError("deviceId does not match the required ASCII syntax");
    }
    return DeviceId(std::string(input));
}
const std::string& DeviceId::value() const noexcept {
    return value_;
}
bool operator==(const DeviceId& left, const DeviceId& right) noexcept {
    return left.value_ == right.value_;
}

PlateNumber::PlateNumber(std::string value) : value_(std::move(value)) {}
PlateNumber PlateNumber::parse(const std::string_view input) {
    try {
        return PlateNumber(text::normalizePlateLike(input, false));
    } catch (const text::Utf8Error& error) {
        throw DomainError(error.what());
    }
}
const std::string& PlateNumber::value() const noexcept {
    return value_;
}
bool operator==(const PlateNumber& left, const PlateNumber& right) noexcept {
    return left.value_ == right.value_;
}

PlateKeyword::PlateKeyword(std::string value) : value_(std::move(value)) {}
PlateKeyword PlateKeyword::parse(const std::string_view input) {
    try {
        return PlateKeyword(text::normalizePlateLike(input, true));
    } catch (const text::Utf8Error& error) {
        throw DomainError(error.what());
    }
}
const std::string& PlateKeyword::value() const noexcept {
    return value_;
}
bool PlateKeyword::empty() const noexcept {
    return value_.empty();
}

RelativeImagePath::RelativeImagePath(std::string value) : value_(std::move(value)) {}
RelativeImagePath RelativeImagePath::parseGenerated(const std::string_view input) {
    if (input.empty() || input.front() == '/' || input.back() == '/' ||
        input.find('\\') != std::string_view::npos || input.find('\0') != std::string_view::npos) {
        throw DomainError("relative image path is invalid");
    }
    std::size_t segmentStart = 0U;
    for (std::size_t index = 0U; index <= input.size(); ++index) {
        if (index != input.size() && input[index] != '/') {
            const char value = input[index];
            if (!isDeviceCharacter(value)) {
                throw DomainError("relative image path contains an invalid character");
            }
            continue;
        }
        const auto segment = input.substr(segmentStart, index - segmentStart);
        if (segment.empty() || segment == "." || segment == "..") {
            throw DomainError("relative image path contains an invalid segment");
        }
        segmentStart = index + 1U;
    }
    return RelativeImagePath(std::string(input));
}
const std::string& RelativeImagePath::value() const noexcept {
    return value_;
}
bool operator==(
    const RelativeImagePath& left,
    const RelativeImagePath& right) noexcept {
    return left.value_ == right.value_;
}

Sha256Digest::Sha256Digest(std::array<std::uint8_t, 32> bytes) noexcept : bytes_(bytes) {}
Sha256Digest Sha256Digest::parseHex(const std::string_view input) {
    if (input.size() != 64U) {
        throw DomainError("SHA-256 digest must contain 64 hexadecimal digits");
    }
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const int high = hexValue(input[index * 2U]);
        const int low = hexValue(input[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            throw DomainError("SHA-256 digest contains invalid hexadecimal digits");
        }
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return Sha256Digest(bytes);
}
const std::array<std::uint8_t, 32>& Sha256Digest::bytes() const noexcept {
    return bytes_;
}
std::string Sha256Digest::toHex() const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64U);
    for (const auto value : bytes_) {
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    return result;
}
bool operator==(const Sha256Digest& left, const Sha256Digest& right) noexcept {
    return left.bytes_ == right.bytes_;
}

bool isJsonSafeInteger(const std::int64_t value) noexcept {
    return value >= -static_cast<std::int64_t>(kJsonSafeIntegerMaximum) &&
           value <= static_cast<std::int64_t>(kJsonSafeIntegerMaximum);
}
bool isJsonSafeUnsigned(const std::uint64_t value) noexcept {
    return value <= kJsonSafeIntegerMaximum;
}
void requirePositiveJsonSafe(const std::uint64_t value, const std::string_view fieldName) {
    if (value == 0U || !isJsonSafeUnsigned(value)) {
        throw DomainError(std::string(fieldName) + " must be a positive JSON safe integer");
    }
}
void requireNonNegativeJsonSafe(const std::uint64_t value, const std::string_view fieldName) {
    if (!isJsonSafeUnsigned(value)) {
        throw DomainError(std::string(fieldName) + " must be a JSON safe integer");
    }
}

}  // namespace ocrservice::domain
