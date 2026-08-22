#pragma once

#include <chrono>
#include <string_view>

#include "AuthTypes.h"
#include "BcryptVerifier.h"
#include "OpaqueTokenGenerator.h"
#include "Ports.h"
#include "SessionStore.h"

namespace ocrservice::services::auth {

class IAuthService {
public:
    virtual ~IAuthService() = default;

    virtual LoginResult login(
        std::string_view username,
        std::string_view password,
        const domain::Uuid& clientId) = 0;
    virtual AuthenticationResult authenticate(std::string_view accessToken) = 0;
    virtual OperationResult logout(std::string_view accessToken) = 0;
    virtual OperationResult heartbeat(
        std::string_view accessToken,
        const domain::Uuid& clientId) = 0;
};

class AuthService final : public IAuthService {
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
        const domain::Uuid& clientId) override;
    AuthenticationResult authenticate(std::string_view accessToken) override;
    OperationResult logout(std::string_view accessToken) override;
    OperationResult heartbeat(
        std::string_view accessToken,
        const domain::Uuid& clientId) override;

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
