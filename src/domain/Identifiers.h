#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ocrservice::domain {

constexpr std::uint64_t kJsonSafeIntegerMaximum = 9007199254740991ULL;

class DomainError final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

class Uuid final {
public:
    static Uuid parse(std::string_view input);
    static Uuid v4(std::array<std::uint8_t, 16> randomBytes);

    const std::array<std::uint8_t, 16>& bytes() const noexcept;
    std::uint8_t version() const noexcept;
    std::string toString() const;

    friend bool operator==(const Uuid& left, const Uuid& right) noexcept;
    friend bool operator!=(const Uuid& left, const Uuid& right) noexcept;

private:
    explicit Uuid(std::array<std::uint8_t, 16> bytes) noexcept;

    std::array<std::uint8_t, 16> bytes_;
};

class RecognitionId final {
public:
    static RecognitionId parse(std::string_view input);
    explicit RecognitionId(Uuid value) noexcept;
    const Uuid& value() const noexcept;
    std::string toString() const;

    friend bool operator==(const RecognitionId& left, const RecognitionId& right) noexcept;

private:
    Uuid value_;
};

class CaptureId final {
public:
    static CaptureId parse(std::string_view input);
    explicit CaptureId(Uuid value) noexcept;
    const Uuid& value() const noexcept;
    std::string toString() const;

    friend bool operator==(const CaptureId& left, const CaptureId& right) noexcept;

private:
    Uuid value_;
};

class DeviceId final {
public:
    static DeviceId parse(std::string_view input);
    const std::string& value() const noexcept;

    friend bool operator==(const DeviceId& left, const DeviceId& right) noexcept;

private:
    explicit DeviceId(std::string value);
    std::string value_;
};

class PlateNumber final {
public:
    static PlateNumber parse(std::string_view input);
    const std::string& value() const noexcept;

    friend bool operator==(const PlateNumber& left, const PlateNumber& right) noexcept;

private:
    explicit PlateNumber(std::string value);
    std::string value_;
};

class PlateKeyword final {
public:
    static PlateKeyword parse(std::string_view input);
    const std::string& value() const noexcept;
    bool empty() const noexcept;

private:
    explicit PlateKeyword(std::string value);
    std::string value_;
};

class RelativeImagePath final {
public:
    static RelativeImagePath parseGenerated(std::string_view input);
    const std::string& value() const noexcept;

    friend bool operator==(
        const RelativeImagePath& left,
        const RelativeImagePath& right) noexcept;

private:
    explicit RelativeImagePath(std::string value);
    std::string value_;
};

class Sha256Digest final {
public:
    explicit Sha256Digest(std::array<std::uint8_t, 32> bytes) noexcept;
    static Sha256Digest parseHex(std::string_view input);

    const std::array<std::uint8_t, 32>& bytes() const noexcept;
    std::string toHex() const;

    friend bool operator==(const Sha256Digest& left, const Sha256Digest& right) noexcept;

private:
    std::array<std::uint8_t, 32> bytes_;
};

bool isJsonSafeInteger(std::int64_t value) noexcept;
bool isJsonSafeUnsigned(std::uint64_t value) noexcept;
void requirePositiveJsonSafe(std::uint64_t value, std::string_view fieldName);
void requireNonNegativeJsonSafe(std::uint64_t value, std::string_view fieldName);

}  // namespace ocrservice::domain
