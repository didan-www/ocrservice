#include "BcryptVerifier.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include <crypt.h>
#include <openssl/crypto.h>

namespace ocrservice::security {
namespace {

constexpr std::size_t kBcryptHashLength = 60U;

bool isBcryptCharacter(const char value) noexcept {
    return value == '.' || value == '/' || (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool isSupportedHash(const std::string_view hash) noexcept {
    constexpr std::string_view kPrefix = "$2b$12$";
    return hash.size() == kBcryptHashLength && hash.substr(0U, kPrefix.size()) == kPrefix &&
           std::all_of(hash.begin() + static_cast<std::ptrdiff_t>(kPrefix.size()),
                       hash.end(), isBcryptCharacter);
}

}  // namespace

PasswordVerification BcryptVerifier::verify(
    const std::string_view password,
    const std::string_view encodedHash) {
    if (!isSupportedHash(encodedHash) || password.find('\0') != std::string_view::npos) {
        return PasswordVerification::failure;
    }
    const std::string passwordText(password);
    const std::string hashText(encodedHash);
    crypt_data state{};
    const char* const calculated = crypt_r(passwordText.c_str(), hashText.c_str(), &state);
    if (calculated == nullptr || calculated[0] == '*' ||
        std::strlen(calculated) != encodedHash.size()) {
        return PasswordVerification::failure;
    }
    return CRYPTO_memcmp(calculated, encodedHash.data(), encodedHash.size()) == 0
               ? PasswordVerification::match
               : PasswordVerification::mismatch;
}

}  // namespace ocrservice::security
