#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "AuthService.h"
#include "BcryptVerifier.h"
#include "OpaqueTokenGenerator.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::domain::AdminUserRecord;
using ocrservice::domain::IAdminUserRepository;
using ocrservice::domain::RepositoryFailure;
using ocrservice::domain::RepositoryResult;
using ocrservice::domain::UtcTimePoint;
using ocrservice::domain::Uuid;
using ocrservice::security::IPasswordVerifier;
using ocrservice::security::IRandomByteSource;
using ocrservice::security::ITokenGenerator;
using ocrservice::security::OpaqueTokenGenerator;
using ocrservice::security::PasswordVerification;
using ocrservice::security::TokenRandomBytes;
using ocrservice::services::auth::Authenticated;
using ocrservice::services::auth::AuthenticationResult;
using ocrservice::services::auth::AuthFailure;
using ocrservice::services::auth::AuthService;
using ocrservice::services::auth::IAuthClock;
using ocrservice::services::auth::LoginResult;
using ocrservice::services::auth::LoginSuccess;
using ocrservice::services::auth::OperationResult;
using ocrservice::services::auth::OperationSucceeded;
using ocrservice::services::auth::SessionStore;

constexpr std::string_view kAdminHash =
    "$2b$12$abcdefghijklmnopqrstuumj.RgFt55etWFiErc2FEn.hnLKiyjYi";

Uuid clientId(const std::uint64_t suffix) {
    char text[37]{};
    const int written = std::snprintf(
        text,
        sizeof(text),
        "11111111-2222-4333-8444-%012llx",
        static_cast<unsigned long long>(suffix));
    if (written != 36) {
        throw std::runtime_error("failed to create test clientId");
    }
    return Uuid::parse(text);
}

std::string token(const char value) {
    return std::string(ocrservice::security::kOpaqueTokenCharacters, value);
}

AdminUserRecord user(const bool enabled = true, const std::string& hash = std::string(kAdminHash)) {
    return AdminUserRecord(1U, "admin", "Demo Administrator", hash, enabled);
}

template <typename Result>
bool isFailure(const Result& result, const AuthFailure expected) {
    return std::holds_alternative<AuthFailure>(result) &&
           std::get<AuthFailure>(result) == expected;
}

class FakeUserRepository final : public IAdminUserRepository {
public:
    explicit FakeUserRepository(
        RepositoryResult<std::optional<AdminUserRecord>> result =
            std::optional<AdminUserRecord>(user()))
        : result_(std::move(result)) {}

    RepositoryResult<std::optional<AdminUserRecord>> findByUsername(
        const std::string_view username) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        lastUsername_ = username;
        if (throwOnCall_) {
            throw std::runtime_error("repository detail must not escape");
        }
        return result_;
    }

    void setResult(RepositoryResult<std::optional<AdminUserRecord>> result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = std::move(result);
    }

    void setThrowOnCall(const bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        throwOnCall_ = value;
    }

    unsigned int calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    RepositoryResult<std::optional<AdminUserRecord>> result_;
    std::string lastUsername_;
    unsigned int calls_ = 0U;
    bool throwOnCall_ = false;
};

class FakePasswordVerifier final : public IPasswordVerifier {
public:
    PasswordVerification verify(
        const std::string_view password,
        const std::string_view encodedHash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++calls_;
        lastPassword_ = password;
        lastHash_ = encodedHash;
        if (throwOnCall_) {
            throw std::runtime_error("bcrypt detail must not escape");
        }
        return result_;
    }

    void setResult(const PasswordVerification result) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_ = result;
    }

    void setThrowOnCall(const bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        throwOnCall_ = value;
    }

    std::string lastHash() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastHash_;
    }

    unsigned int calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    PasswordVerification result_ = PasswordVerification::match;
    std::string lastPassword_;
    std::string lastHash_;
    unsigned int calls_ = 0U;
    bool throwOnCall_ = false;
};

class FakeTokenGenerator final : public ITokenGenerator {
public:
    void append(std::optional<std::string> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        values_.push_back(std::move(value));
    }

    void setThrowOnCall(const bool value) {
        std::lock_guard<std::mutex> lock(mutex_);
        throwOnCall_ = value;
    }

    std::optional<std::string> generate() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (throwOnCall_) {
            throw std::runtime_error("random detail must not escape");
        }
        if (values_.empty()) {
            return std::nullopt;
        }
        auto value = std::move(values_.front());
        values_.pop_front();
        return value;
    }

private:
    std::mutex mutex_;
    std::deque<std::optional<std::string>> values_;
    bool throwOnCall_ = false;
};

class FakeClock final : public IAuthClock {
public:
    explicit FakeClock(const std::int64_t milliseconds) : now_(milliseconds) {}

    UtcTimePoint nowUtc() override {
        if (throwOnCall_.load()) {
            throw std::runtime_error("clock detail must not escape");
        }
        return UtcTimePoint(now_.load());
    }

    void set(const std::int64_t milliseconds) { now_.store(milliseconds); }
    void setThrowOnCall(const bool value) { throwOnCall_.store(value); }

private:
    std::atomic<std::int64_t> now_;
    std::atomic<bool> throwOnCall_{false};
};

class FakeRandomSource final : public IRandomByteSource {
public:
    bool fill(TokenRandomBytes& destination) noexcept override {
        if (!succeeds) {
            return false;
        }
        destination = bytes;
        return true;
    }

    TokenRandomBytes bytes{};
    bool succeeds = true;
};

struct AuthFixture final {
    FakeUserRepository users;
    FakePasswordVerifier passwords;
    FakeTokenGenerator tokens;
    FakeClock clock{1000000};
    SessionStore sessions;

    std::unique_ptr<AuthService> service(const std::chrono::seconds ttl = 8h) {
        return std::make_unique<AuthService>(
            users, passwords, tokens, clock, sessions, ttl);
    }
};

TEST(BcryptVerifierTest, VerifiesSupportedHashesAndRejectsBrokenInputs) {
    ocrservice::security::BcryptVerifier verifier;
    EXPECT_EQ(verifier.verify("plate-demo-2026", kAdminHash), PasswordVerification::match);
    EXPECT_EQ(verifier.verify("wrong", kAdminHash), PasswordVerification::mismatch);
    EXPECT_EQ(
        verifier.verify("ocrservice-dummy-password", ocrservice::security::kDummyBcryptHash),
        PasswordVerification::match);
    EXPECT_EQ(verifier.verify("password", "not-a-bcrypt-hash"), PasswordVerification::failure);
    const std::string embeddedNul("plate-demo-2026\0ignored", 23U);
    EXPECT_EQ(verifier.verify(embeddedNul, kAdminHash), PasswordVerification::failure);
}

TEST(OpaqueTokenGeneratorTest, EncodesAllThirtyTwoRandomBytesAndReportsFailure) {
    FakeRandomSource random;
    for (std::size_t index = 0U; index < random.bytes.size(); ++index) {
        random.bytes[index] = static_cast<std::uint8_t>(index);
    }
    OpaqueTokenGenerator generator(random);
    auto generated = generator.generate();
    ASSERT_TRUE(generated);
    EXPECT_EQ(
        *generated,
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    random.succeeds = false;
    EXPECT_FALSE(generator.generate());
}

TEST(AuthServiceTest, LoginUsesOneClockValueAndExpiresAtExactBoundary) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    auto service = fixture.service();
    auto login = service->login("admin", "correct", clientId(1U));
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(login));
    const auto& success = std::get<LoginSuccess>(login);
    EXPECT_EQ(success.accessToken, token('a'));
    EXPECT_EQ(success.session.issuedAt, UtcTimePoint(1000000));
    EXPECT_EQ(success.session.expiresAt, UtcTimePoint(29800000));

    fixture.clock.set(29799999);
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('a'))));
    EXPECT_TRUE(std::holds_alternative<OperationSucceeded>(
        service->heartbeat(token('a'), clientId(1U))));
    fixture.clock.set(29800000);
    EXPECT_TRUE(isFailure(service->authenticate(token('a')), AuthFailure::tokenExpired));
    EXPECT_TRUE(isFailure(service->authenticate(token('a')), AuthFailure::tokenInvalid));
}

TEST(AuthServiceTest, EnforcesCredentialUtf8LengthAndNulShapeBeforeRepository) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    auto service = fixture.service();
    EXPECT_TRUE(isFailure(
        service->login("", "p", clientId(1U)), AuthFailure::invalidCredentials));
    EXPECT_TRUE(isFailure(
        service->login("admin", "", clientId(1U)), AuthFailure::invalidCredentials));
    EXPECT_EQ(fixture.users.calls(), 0U);
    const std::string chinese = "\xE4\xB8\xAD";
    std::string username64;
    for (int index = 0; index < 64; ++index) {
        username64 += chinese;
    }
    EXPECT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login(username64, std::string(72U, 'p'), clientId(2U))));
    EXPECT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(3U))));
    EXPECT_TRUE(isFailure(
        service->login(username64 + chinese, "p", clientId(4U)),
        AuthFailure::invalidCredentials));
    EXPECT_TRUE(isFailure(
        service->login("admin", std::string(73U, 'p'), clientId(4U)),
        AuthFailure::invalidCredentials));
    const std::string nulPassword("p\0suffix", 8U);
    EXPECT_TRUE(isFailure(
        service->login("admin", nulPassword, clientId(4U)),
        AuthFailure::invalidCredentials));
    EXPECT_TRUE(isFailure(
        service->login(std::string("\xC3\x28", 2U), "p", clientId(4U)),
        AuthFailure::invalidCredentials));
    EXPECT_EQ(fixture.users.calls(), 2U);
}

TEST(AuthServiceTest, UnknownWrongAndDisabledUsersFollowBcryptFirstPolicy) {
    AuthFixture fixture;
    auto service = fixture.service();
    fixture.users.setResult(std::optional<AdminUserRecord>{});
    EXPECT_TRUE(isFailure(
        service->login("missing", "password", clientId(5U)),
        AuthFailure::invalidCredentials));
    EXPECT_EQ(fixture.passwords.lastHash(), ocrservice::security::kDummyBcryptHash);

    fixture.users.setResult(std::optional<AdminUserRecord>(user(false)));
    fixture.passwords.setResult(PasswordVerification::mismatch);
    EXPECT_TRUE(isFailure(
        service->login("admin", "wrong", clientId(5U)),
        AuthFailure::invalidCredentials));
    fixture.passwords.setResult(PasswordVerification::match);
    EXPECT_TRUE(isFailure(
        service->login("admin", "correct", clientId(5U)),
        AuthFailure::userDisabled));
    EXPECT_EQ(fixture.passwords.calls(), 3U);
}

TEST(AuthServiceTest, MapsRepositoryVerifierRandomAndClockFailuresWithoutThrowing) {
    for (const auto& [repositoryFailure, expected] :
         std::vector<std::pair<RepositoryFailure, AuthFailure>>{
             {RepositoryFailure::unavailable, AuthFailure::databaseUnavailable},
             {RepositoryFailure::internal, AuthFailure::internal},
             {RepositoryFailure::notFound, AuthFailure::internal},
             {RepositoryFailure::conflict, AuthFailure::internal},
             {RepositoryFailure::stateConflict, AuthFailure::internal}}) {
        AuthFixture fixture;
        fixture.users.setResult(repositoryFailure);
        auto service = fixture.service();
        EXPECT_TRUE(isFailure(service->login("admin", "p", clientId(6U)), expected));
    }
    AuthFixture fixture;
    auto service = fixture.service();
    fixture.passwords.setResult(PasswordVerification::failure);
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(6U)), AuthFailure::internal));
    fixture.passwords.setResult(PasswordVerification::match);
    fixture.tokens.append(std::nullopt);
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(6U)), AuthFailure::internal));
    fixture.tokens.append(std::string(64U, 'A'));
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(6U)), AuthFailure::internal));
    fixture.clock.setThrowOnCall(true);
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(6U)), AuthFailure::internal));
    fixture.users.setThrowOnCall(true);
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(6U)), AuthFailure::internal));
}

TEST(AuthServiceTest, RealVerifierMapsMalformedStoredBcryptHashToInternal) {
    FakeUserRepository users(std::optional<AdminUserRecord>(user(true, "broken-hash")));
    ocrservice::security::BcryptVerifier passwords;
    FakeTokenGenerator tokens;
    FakeClock clock(1000000);
    SessionStore sessions;
    AuthService service(users, passwords, tokens, clock, sessions, 8h);
    EXPECT_TRUE(isFailure(
        service.login("admin", "p", clientId(6U)), AuthFailure::internal));
}

TEST(AuthServiceTest, RetriesTokenCollisionWithoutOverwritingAnotherClient) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    auto service = fixture.service();
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(7U))));
    auto second = service->login("admin", "p", clientId(8U));
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(second));
    EXPECT_EQ(std::get<LoginSuccess>(second).accessToken, token('b'));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('a'))));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('b'))));
    EXPECT_EQ(fixture.sessions.size(), 2U);
}

TEST(AuthServiceTest, ExhaustedTokenCollisionsLeaveExistingSessionUntouched) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    for (unsigned int attempt = 0U; attempt < 16U; ++attempt) {
        fixture.tokens.append(token('a'));
    }
    auto service = fixture.service();
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(7U))));
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(8U)), AuthFailure::internal));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('a'))));
    EXPECT_EQ(fixture.sessions.size(), 1U);
}

TEST(AuthServiceTest, ReplacesOnlySameClientAndOldLogoutCannotRemoveNewMapping) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    fixture.tokens.append(token('c'));
    auto service = fixture.service();
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(9U))));
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(9U))));
    EXPECT_TRUE(isFailure(service->logout(token('a')), AuthFailure::tokenInvalid));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('b'))));
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(10U))));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('b'))));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('c'))));
    EXPECT_EQ(fixture.sessions.size(), 2U);
}

TEST(AuthServiceTest, LogoutHeartbeatAndExpiredCleanupHaveStableSemantics) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    auto service = fixture.service(1s);
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(11U))));
    EXPECT_TRUE(isFailure(
        service->heartbeat(token('a'), clientId(12U)), AuthFailure::tokenInvalid));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('a'))));
    EXPECT_TRUE(std::holds_alternative<OperationSucceeded>(service->logout(token('a'))));
    EXPECT_TRUE(isFailure(service->logout(token('a')), AuthFailure::tokenInvalid));

    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", clientId(11U))));
    fixture.clock.set(1001000);
    EXPECT_TRUE(isFailure(service->logout(token('b')), AuthFailure::tokenExpired));
    EXPECT_TRUE(isFailure(service->logout(token('b')), AuthFailure::tokenInvalid));
}

TEST(AuthServiceTest, ConcurrentSameClientLoginsLeaveExactlyOneTokenValid) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    auto service = fixture.service();
    const auto sameClient = clientId(13U);
    auto first = std::async(std::launch::async, [&] {
        return service->login("admin", "p", sameClient);
    });
    auto second = std::async(std::launch::async, [&] {
        return service->login("admin", "p", sameClient);
    });
    auto firstResult = first.get();
    auto secondResult = second.get();
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(firstResult));
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(secondResult));
    const auto firstToken = std::get<LoginSuccess>(firstResult).accessToken;
    const auto secondToken = std::get<LoginSuccess>(secondResult).accessToken;
    EXPECT_NE(firstToken, secondToken);
    const bool firstValid =
        std::holds_alternative<Authenticated>(service->authenticate(firstToken));
    const bool secondValid =
        std::holds_alternative<Authenticated>(service->authenticate(secondToken));
    EXPECT_NE(firstValid, secondValid);
    EXPECT_EQ(fixture.sessions.size(), 1U);
}

TEST(AuthServiceTest, ConcurrentOldLogoutNeverRevokesNewLogin) {
    AuthFixture fixture;
    fixture.tokens.append(token('a'));
    fixture.tokens.append(token('b'));
    auto service = fixture.service();
    const auto sameClient = clientId(14U);
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(
        service->login("admin", "p", sameClient)));
    auto newLogin = std::async(std::launch::async, [&] {
        return service->login("admin", "p", sameClient);
    });
    auto oldLogout = std::async(std::launch::async, [&] {
        return service->logout(token('a'));
    });
    auto loginResult = newLogin.get();
    auto logoutResult = oldLogout.get();
    ASSERT_TRUE(std::holds_alternative<LoginSuccess>(loginResult));
    EXPECT_TRUE(
        std::holds_alternative<OperationSucceeded>(logoutResult) ||
        isFailure(logoutResult, AuthFailure::tokenInvalid));
    EXPECT_TRUE(std::holds_alternative<Authenticated>(service->authenticate(token('b'))));
    EXPECT_EQ(fixture.sessions.size(), 1U);
}

TEST(AuthServiceTest, RejectsInvalidTokensTtlAndUnrepresentableExpiry) {
    AuthFixture fixture;
    EXPECT_THROW(fixture.service(0s), std::invalid_argument);
    auto service = fixture.service(1s);
    EXPECT_TRUE(isFailure(service->authenticate("short"), AuthFailure::tokenInvalid));
    EXPECT_TRUE(isFailure(service->logout(std::string(64U, 'A')), AuthFailure::tokenInvalid));
    fixture.clock.set(253402271999500LL);
    EXPECT_TRUE(isFailure(
        service->login("admin", "p", clientId(14U)), AuthFailure::internal));
}

}  // namespace
