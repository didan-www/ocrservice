#include "AuthService.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "ProtocolTime.h"
#include "Utf8.h"

namespace ocrservice::services::auth {
namespace {

bool hasValidCredentialShape(
    const std::string_view username,
    const std::string_view password) noexcept {
    if (password.empty() || password.size() > 72U ||
        password.find('\0') != std::string_view::npos) {
        return false;
    }
    try {
        const auto usernameLength = text::countCodePoints(username);
        (void)text::countCodePoints(password);
        return usernameLength >= 1U && usernameLength <= 64U;
    } catch (const text::Utf8Error&) {
        return false;
    }
}

bool isOpaqueToken(const std::string_view token) noexcept {
    if (token.size() != security::kOpaqueTokenCharacters) {
        return false;
    }
    for (const char value : token) {
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

AuthFailure mapRepositoryFailure(const domain::RepositoryFailure failure) noexcept {
    return failure == domain::RepositoryFailure::unavailable
               ? AuthFailure::databaseUnavailable
               : AuthFailure::internal;
}

AuthFailure mapLookupFailure(const SessionLookupStatus status) noexcept {
    return status == SessionLookupStatus::expired ? AuthFailure::tokenExpired
                                                   : AuthFailure::tokenInvalid;
}

}  // namespace

AuthService::AuthService(
    domain::IAdminUserRepository& users,
    security::IPasswordVerifier& passwordVerifier,
    security::ITokenGenerator& tokenGenerator,
    IAuthClock& clock,
    SessionStore& sessions,
    const std::chrono::seconds tokenTtl)
    : users_(users),
      passwordVerifier_(passwordVerifier),
      tokenGenerator_(tokenGenerator),
      clock_(clock),
      sessions_(sessions),
      tokenTtl_(tokenTtl) {
    if (tokenTtl_.count() <= 0 ||
        tokenTtl_.count() > std::numeric_limits<std::int64_t>::max() / 1000) {
        throw std::invalid_argument("token TTL must be positive and representable in milliseconds");
    }
}

LoginResult AuthService::login(
    const std::string_view username,
    const std::string_view password,
    const domain::Uuid& clientId) {
    if (!hasValidCredentialShape(username, password)) {
        return AuthFailure::invalidCredentials;
    }
    try {
        auto repositoryResult = users_.findByUsername(username);
        if (std::holds_alternative<domain::RepositoryFailure>(repositoryResult)) {
            return mapRepositoryFailure(std::get<domain::RepositoryFailure>(repositoryResult));
        }
        const auto& user = std::get<std::optional<domain::AdminUserRecord>>(repositoryResult);
        const std::string_view hash =
            user ? std::string_view(user->passwordHash()) : security::kDummyBcryptHash;
        const auto verification = passwordVerifier_.verify(password, hash);
        if (verification == security::PasswordVerification::failure) {
            return AuthFailure::internal;
        }
        if (!user || verification != security::PasswordVerification::match) {
            return AuthFailure::invalidCredentials;
        }
        if (!user->enabled()) {
            return AuthFailure::userDisabled;
        }

        const auto issuedAt = clock_.nowUtc();
        const auto ttlMilliseconds = tokenTtl_.count() * 1000;
        if (issuedAt.unixMilliseconds() >
            std::numeric_limits<std::int64_t>::max() - ttlMilliseconds) {
            return AuthFailure::internal;
        }
        const domain::UtcTimePoint expiresAt(
            issuedAt.unixMilliseconds() + ttlMilliseconds);
        (void)serialization::time::formatProtocolTime(issuedAt);
        (void)serialization::time::formatProtocolTime(expiresAt);
        const AuthSession session{
            user->id(), user->displayName(), clientId, issuedAt, expiresAt};
        for (unsigned int attempt = 0U; attempt < kTokenGenerationAttempts; ++attempt) {
            auto token = tokenGenerator_.generate();
            if (!token || !isOpaqueToken(*token)) {
                return AuthFailure::internal;
            }
            if (sessions_.replace(*token, session)) {
                return LoginSuccess{std::move(*token), session};
            }
        }
        return AuthFailure::internal;
    } catch (...) {
        return AuthFailure::internal;
    }
}

AuthenticationResult AuthService::authenticate(const std::string_view accessToken) {
    if (!isOpaqueToken(accessToken)) {
        return AuthFailure::tokenInvalid;
    }
    try {
        auto lookup = sessions_.lookup(accessToken, clock_.nowUtc());
        if (lookup.status != SessionLookupStatus::valid || !lookup.session) {
            return mapLookupFailure(lookup.status);
        }
        return Authenticated{std::move(*lookup.session)};
    } catch (...) {
        return AuthFailure::internal;
    }
}

OperationResult AuthService::logout(const std::string_view accessToken) {
    if (!isOpaqueToken(accessToken)) {
        return AuthFailure::tokenInvalid;
    }
    try {
        const auto lookup = sessions_.revoke(accessToken, clock_.nowUtc());
        if (lookup.status != SessionLookupStatus::valid) {
            return mapLookupFailure(lookup.status);
        }
        return OperationSucceeded{};
    } catch (...) {
        return AuthFailure::internal;
    }
}

OperationResult AuthService::heartbeat(
    const std::string_view accessToken,
    const domain::Uuid& clientId) {
    auto authenticated = authenticate(accessToken);
    if (std::holds_alternative<AuthFailure>(authenticated)) {
        return std::get<AuthFailure>(authenticated);
    }
    if (std::get<Authenticated>(authenticated).session.clientId != clientId) {
        return AuthFailure::tokenInvalid;
    }
    return OperationSucceeded{};
}

}  // namespace ocrservice::services::auth
