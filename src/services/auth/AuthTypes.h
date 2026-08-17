#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "Identifiers.h"
#include "Recognition.h"

namespace ocrservice::services::auth {

enum class AuthFailure {
    invalidCredentials,
    userDisabled,
    tokenInvalid,
    tokenExpired,
    databaseUnavailable,
    internal
};

struct AuthSession final {
    std::uint64_t userId;
    std::string displayName;
    domain::Uuid clientId;
    domain::UtcTimePoint issuedAt;
    domain::UtcTimePoint expiresAt;
};

struct LoginSuccess final {
    std::string accessToken;
    AuthSession session;
};

struct Authenticated final {
    AuthSession session;
};

struct OperationSucceeded final {};

using LoginResult = std::variant<LoginSuccess, AuthFailure>;
using AuthenticationResult = std::variant<Authenticated, AuthFailure>;
using OperationResult = std::variant<OperationSucceeded, AuthFailure>;

class IAuthClock {
public:
    virtual ~IAuthClock() = default;
    virtual domain::UtcTimePoint nowUtc() = 0;
};

class SystemAuthClock final : public IAuthClock {
public:
    domain::UtcTimePoint nowUtc() override;
};

}  // namespace ocrservice::services::auth
