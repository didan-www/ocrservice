#include "OpaqueTokenGenerator.h"

#include <openssl/rand.h>

namespace ocrservice::security {

bool OpenSslRandomByteSource::fill(TokenRandomBytes& destination) noexcept {
    return RAND_bytes(destination.data(), static_cast<int>(destination.size())) == 1;
}

OpaqueTokenGenerator::OpaqueTokenGenerator(IRandomByteSource& randomSource) noexcept
    : randomSource_(randomSource) {}

std::optional<std::string> OpaqueTokenGenerator::generate() {
    TokenRandomBytes randomBytes{};
    if (!randomSource_.fill(randomBytes)) {
        return std::nullopt;
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(kOpaqueTokenCharacters);
    for (const auto value : randomBytes) {
        token.push_back(kHex[value >> 4U]);
        token.push_back(kHex[value & 0x0FU]);
    }
    return token;
}

}  // namespace ocrservice::security
