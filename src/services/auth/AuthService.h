#pragma once

#include <chrono>
#include <string_view>

#include "AuthTypes.h"
#include "BcryptVerifier.h"
#include "OpaqueTokenGenerator.h"
#include "Ports.h"
#include "SessionStore.h"

namespace ocrservice::services::auth {

class AuthService final {
public:
    AuthService(
        domain::IAdminUserRepository& users,
        security::IPasswordVerifier& passwordVerifier,
        security::ITokenGenerator& tokenGenerator,
        IAuthClock& clock,
        SessionStore& sessions,
        std::chrono::seconds tokenTtl);

    LoginResult login(
        std::string_view username,
        std::string_view password,
        const domain::Uuid& clientId);
    AuthenticationResult authenticate(std::string_view accessToken);
    OperationResult logout(std::string_view accessToken);
    OperationResult heartbeat(
        std::string_view accessToken,
        const domain::Uuid& clientId);

private:
    static constexpr unsigned int kTokenGenerationAttempts = 16U;

    domain::IAdminUserRepository& users_;
    security::IPasswordVerifier& passwordVerifier_;
    security::ITokenGenerator& tokenGenerator_;
    IAuthClock& clock_;
    SessionStore& sessions_;
    std::chrono::seconds tokenTtl_;
};

}  // namespace ocrservice::services::auth
