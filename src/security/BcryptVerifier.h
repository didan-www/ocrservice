#pragma once

#include <string_view>

namespace ocrservice::security {

inline constexpr std::string_view kDummyBcryptHash =
    "$2b$12$abcdefghijklmnopqrstuu/gGjSuxxO4N5/tBA2nf7MXxaENB080u";

enum class PasswordVerification { match, mismatch, failure };

class IPasswordVerifier {
public:
    virtual ~IPasswordVerifier() = default;
    virtual PasswordVerification verify(
        std::string_view password,
        std::string_view encodedHash) = 0;
};

class BcryptVerifier final : public IPasswordVerifier {
public:
    PasswordVerification verify(
        std::string_view password,
        std::string_view encodedHash) override;
};

}  // namespace ocrservice::security
