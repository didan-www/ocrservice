#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace ocrservice::security {

inline constexpr std::size_t kOpaqueTokenBytes = 32U;
inline constexpr std::size_t kOpaqueTokenCharacters = kOpaqueTokenBytes * 2U;
using TokenRandomBytes = std::array<std::uint8_t, kOpaqueTokenBytes>;

class IRandomByteSource {
public:
    virtual ~IRandomByteSource() = default;
    virtual bool fill(TokenRandomBytes& destination) noexcept = 0;
};

class OpenSslRandomByteSource final : public IRandomByteSource {
public:
    bool fill(TokenRandomBytes& destination) noexcept override;
};

class ITokenGenerator {
public:
    virtual ~ITokenGenerator() = default;
    virtual std::optional<std::string> generate() = 0;
};

class OpaqueTokenGenerator final : public ITokenGenerator {
public:
    explicit OpaqueTokenGenerator(IRandomByteSource& randomSource) noexcept;
    std::optional<std::string> generate() override;

private:
    IRandomByteSource& randomSource_;
};

}  // namespace ocrservice::security
