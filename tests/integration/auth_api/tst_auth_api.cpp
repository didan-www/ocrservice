#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "AccessLog.h"
#include "AuthController.h"
#include "AuthService.h"
#include "ClientController.h"
#include "HttpServer.h"
#include "ProtocolTime.h"
#include "RequestId.h"

namespace ocrservice {
namespace {

using services::auth::Authenticated;
using services::auth::AuthenticationResult;
using services::auth::AuthFailure;
using services::auth::AuthSession;
using services::auth::IAuthService;
using services::auth::LoginResult;
using services::auth::LoginSuccess;
using services::auth::OperationResult;
using services::auth::OperationSucceeded;

constexpr std::string_view kClientId = "11111111-2222-4333-8444-555555555555";
constexpr std::string_view kAccessToken =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

struct ParsedResponse final {
    int status = 0;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

ParsedResponse parseResponse(const std::string& raw) {
    ParsedResponse response;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return response;
    }
    std::istringstream input(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(input, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    std::string line;
    while (std::getline(input, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] = trim(line.substr(colon + 1U));
        }
    }
    response.body = raw.substr(headerEnd + 4U);
    return response;
}

std::string socketExchange(const std::uint16_t port, const std::string& wireRequest) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(socket, asio::buffer(wireRequest));
    std::string raw;
    std::array<char, 16384> buffer{};
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error) {
            break;
        }
    }
    return raw;
}

std::string request(
    const std::string& target,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body = {}) {
    std::string result = "POST " + target + " HTTP/1.1\r\nHost: localhost\r\n";
    bool hasLength = false;
    for (const auto& [name, value] : headers) {
        result += name + ": " + value + "\r\n";
        hasLength = hasLength || lowercase(name) == "content-length";
    }
    if (!hasLength && !body.empty()) {
        result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    result += "Connection: close\r\n\r\n";
    result += body;
    return result;
}

nlohmann::json jsonBody(const ParsedResponse& response) {
    return nlohmann::json::parse(response.body);
}

void expectEnvelope(
    const ParsedResponse& response,
    const int status,
    const std::string_view code) {
    ASSERT_EQ(response.status, status);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    const auto body = jsonBody(response);
    EXPECT_EQ(body.size(), 5U);
    EXPECT_EQ(body.at("success"), status == 200);
    EXPECT_EQ(body.at("code"), code);
    EXPECT_TRUE(body.at("message").is_string());
    EXPECT_TRUE(body.at("requestId").is_string());
    EXPECT_EQ(domain::Uuid::parse(body.at("requestId").get<std::string>()).version(), 4U);
    if (status != 200) {
        EXPECT_TRUE(body.at("data").is_null());
    }
}

class RecordingAuthService final : public IAuthService {
public:
    RecordingAuthService()
        : clientId_(domain::Uuid::parse(kClientId)),
          expiresAt_(serialization::time::parseProtocolTime(
              "2099-08-23T20:30:00.000+08:00")) {}

    LoginResult login(
        const std::string_view username,
        const std::string_view password,
        const domain::Uuid& clientId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++loginCalls_;
        if (username.empty() || password.empty() || username == "wrong") {
            return AuthFailure::invalidCredentials;
        }
        if (username == "disabled") {
            return AuthFailure::userDisabled;
        }
        if (username == "database") {
            return AuthFailure::databaseUnavailable;
        }
        if (username == "internal") {
            return AuthFailure::internal;
        }
        return LoginSuccess{
            std::string(kAccessToken),
            AuthSession{1U, "演示管理员", clientId, expiresAt_, expiresAt_}};
    }

    AuthenticationResult authenticate(const std::string_view accessToken) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++authenticateCalls_;
        if (accessToken == "expired") {
            return AuthFailure::tokenExpired;
        }
        if (accessToken.empty() || accessToken == "invalid" || accessToken == "old") {
            return AuthFailure::tokenInvalid;
        }
        return Authenticated{AuthSession{1U, "演示管理员", clientId_, expiresAt_, expiresAt_}};
    }

    OperationResult logout(const std::string_view accessToken) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++logoutCalls_;
        return accessToken == "raced" ? OperationResult(AuthFailure::tokenInvalid) :
                                         OperationResult(OperationSucceeded{});
    }

    OperationResult heartbeat(
        std::string_view,
        const domain::Uuid& clientId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++heartbeatCalls_;
        return clientId == clientId_ ? OperationResult(OperationSucceeded{}) :
                                       OperationResult(AuthFailure::tokenInvalid);
    }

    std::size_t loginCalls() const { return calls(loginCalls_); }
    std::size_t authenticateCalls() const { return calls(authenticateCalls_); }
    std::size_t logoutCalls() const { return calls(logoutCalls_); }
    std::size_t heartbeatCalls() const { return calls(heartbeatCalls_); }

private:
    std::size_t calls(const std::size_t value) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return value;
    }

    mutable std::mutex mutex_;
    domain::Uuid clientId_;
    domain::UtcTimePoint expiresAt_;
    std::size_t loginCalls_ = 0U;
    std::size_t authenticateCalls_ = 0U;
    std::size_t logoutCalls_ = 0U;
    std::size_t heartbeatCalls_ = 0U;
};

class CapturingAccessLog final : public http::middleware::IAccessLogSink {
public:
    void write(const http::middleware::AccessLogEntry& entry) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
    }

    std::vector<http::middleware::AccessLogEntry> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<http::middleware::AccessLogEntry> entries_;
};

std::optional<std::vector<http::middleware::AccessLogEntry>> waitForAccessLogs(
    const std::shared_ptr<CapturingAccessLog>& accessLog,
    const std::size_t minimumCount) {
    constexpr std::size_t maxAttempts = 100U;
    for (std::size_t attempt = 0U; attempt < maxAttempts; ++attempt) {
        auto entries = accessLog->entries();
        if (entries.size() >= minimumCount) {
            return entries;
        }
        if (attempt + 1U < maxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return std::nullopt;
}

class AuthApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        mqtt_.publicHost = "192.0.2.10";
        mqtt_.port = 1883U;
        mqtt_.tls = false;
        mqtt_.managementUsername = "management-client";
        mqtt_.managementPassword = "sensitive-mqtt-password";
        accessLog_ = std::make_shared<CapturingAccessLog>();
        server_ = std::make_unique<http::server::HttpServer>(
            0U, http::middleware::makeRandomRequestIdGenerator(), accessLog_);
        authController_ = std::make_unique<http::controllers::auth::AuthController>(service_, mqtt_);
        clientController_ =
            std::make_unique<http::controllers::client::ClientController>(service_);
        authController_->registerRoutes(*server_);
        clientController_->registerRoutes(*server_);
        thread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        port_ = server_->port();
        ASSERT_NE(port_, 0U);
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ParsedResponse call(
        const std::string& target,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body = {}) const {
        return parseResponse(socketExchange(port_, request(target, headers, body)));
    }

    RecordingAuthService service_;
    app::config::MqttConfig mqtt_;
    std::shared_ptr<CapturingAccessLog> accessLog_;
    std::unique_ptr<http::server::HttpServer> server_;
    std::unique_ptr<http::controllers::auth::AuthController> authController_;
    std::unique_ptr<http::controllers::client::ClientController> clientController_;
    std::thread thread_;
    std::uint16_t port_ = 0U;
};

std::string loginBody(
    const std::string_view username = "admin",
    const std::string_view password = "plate-demo-2026") {
    return nlohmann::json{
        {"username", username}, {"password", password}, {"clientId", kClientId}}.dump();
}

std::string heartbeatBody(
    const std::string_view clientId = kClientId,
    const std::string_view appVersion = "0.1.0") {
    return nlohmann::json{{"clientId", clientId}, {"appVersion", appVersion}}.dump();
}

TEST_F(AuthApiTest, LoginReturnsExactDtoIgnoresAuthorizationAndMapsFailures) {
    const auto started = std::chrono::steady_clock::now();
    const auto success = call(
        "/api/v1/auth/login",
        {{"Content-Type", "application/json"},
         {"Authorization", "not bearer syntax"},
         {"Authorization", "still ignored"}},
        loginBody());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    expectEnvelope(success, 200, "OK");
    const auto data = jsonBody(success).at("data");
    EXPECT_EQ(data.size(), 4U);
    EXPECT_EQ(data.at("displayName"), "演示管理员");
    EXPECT_EQ(data.at("accessToken"), kAccessToken);
    EXPECT_EQ(data.at("expiresAt"), data.at("mqtt").at("expiresAt"));
    const auto expiresAt = serialization::time::parseProtocolTime(
        data.at("expiresAt").get<std::string>());
    const auto currentUtc = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    EXPECT_GT(expiresAt.unixMilliseconds(), currentUtc.count());
    EXPECT_EQ(data.at("mqtt").size(), 7U);
    EXPECT_EQ(data.at("mqtt").at("host"), "192.0.2.10");
    EXPECT_EQ(data.at("mqtt").at("port"), 1883);
    EXPECT_FALSE(data.at("mqtt").at("tls").get<bool>());
    EXPECT_EQ(data.at("mqtt").at("username"), "management-client");
    EXPECT_EQ(data.at("mqtt").at("password"), "sensitive-mqtt-password");
    EXPECT_EQ(
        data.at("mqtt").at("eventTopic"), "plate/management/recognition-events");

    for (const auto& [username, status, code] :
         std::vector<std::tuple<std::string, int, std::string>>{
             {"", 401, "AUTH_INVALID_CREDENTIALS"},
             {"wrong", 401, "AUTH_INVALID_CREDENTIALS"},
             {"disabled", 403, "USER_DISABLED"},
             {"database", 503, "DATABASE_UNAVAILABLE"},
             {"internal", 500, "INTERNAL_ERROR"}}) {
        expectEnvelope(
            call(
                "/api/v1/auth/login",
                {{"Content-Type", "application/json"}},
                loginBody(username, username.empty() ? "" : "password")),
            status,
            code);
    }

    const auto beforeInvalid = service_.loginCalls();
    expectEnvelope(
        call(
            "/api/v1/auth/login?unexpected=1",
            {{"Content-Type", "application/json"}},
            loginBody()),
        400,
        "INVALID_REQUEST");
    expectEnvelope(
        call(
            "/api/v1/auth/login",
            {{"Content-Type", "application/json"}},
            R"({"username":"admin","password":"p","clientId":"11111111-2222-4333-8444-555555555555","extra":true})"),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(service_.loginCalls(), beforeInvalid);

    expectEnvelope(
        call(
            "/api/v1/auth/login",
            {{"Content-Type", "application/json"},
             {"Content-Length", std::to_string(http::protocol::kMaximumJsonBytes + 1U)}}),
        413,
        "REQUEST_TOO_LARGE");
    EXPECT_EQ(service_.loginCalls(), beforeInvalid);
}

TEST_F(AuthApiTest, LogoutAuthenticatesBeforeDecoderAndNeverRevokesInvalidRequests) {
    expectEnvelope(call("/api/v1/auth/logout", {}), 401, "AUTH_TOKEN_INVALID");
    expectEnvelope(
        call(
            "/api/v1/auth/logout",
            {{"Authorization", "Bearer old"}}),
        401,
        "AUTH_TOKEN_INVALID");
    expectEnvelope(
        call(
            "/api/v1/auth/logout?unexpected=1",
            {{"Authorization", "Bearer invalid"}},
            "{}"),
        401,
        "AUTH_TOKEN_INVALID");
    EXPECT_EQ(service_.logoutCalls(), 0U);

    const auto authenticatedBeforeMalformed = service_.authenticateCalls();
    expectEnvelope(
        call(
            "/api/v1/auth/logout",
            {{"Content-Type", "text/plain"}, {"Authorization", "Bearer  malformed"}},
            "{}"),
        400,
        "INVALID_REQUEST");
    expectEnvelope(
        call(
            "/api/v1/auth/logout",
            {{"Content-Type", "text/plain"}, {"Authorization", "Bearer valid"}}),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(service_.authenticateCalls(), authenticatedBeforeMalformed);
    EXPECT_EQ(service_.logoutCalls(), 0U);

    expectEnvelope(
        call(
            "/api/v1/auth/logout",
            {{"Authorization", "Bearer  invalid"}},
            "{}"),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(service_.authenticateCalls(), authenticatedBeforeMalformed);

    expectEnvelope(
        call(
            "/api/v1/auth/logout?unexpected=1",
            {{"Authorization", "Bearer valid"}}),
        400,
        "INVALID_REQUEST");
    expectEnvelope(
        call(
            "/api/v1/auth/logout",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
            "{}"),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(service_.logoutCalls(), 0U);

    const auto started = std::chrono::steady_clock::now();
    const auto success = call(
        "/api/v1/auth/logout",
        {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}});
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    expectEnvelope(success, 200, "OK");
    EXPECT_TRUE(jsonBody(success).at("data").is_null());
    EXPECT_EQ(service_.logoutCalls(), 1U);
}

TEST_F(AuthApiTest, HeartbeatPrioritizesAuthenticationAndEnforcesSessionClientBinding) {
    expectEnvelope(
        call(
            "/api/v1/clients/heartbeat?unexpected=1",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer expired"}},
            "not json"),
        401,
        "AUTH_TOKEN_EXPIRED");
    expectEnvelope(
        call(
            "/api/v1/clients/heartbeat?unexpected=1",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
            heartbeatBody()),
        400,
        "INVALID_REQUEST");
    expectEnvelope(
        call(
            "/api/v1/clients/heartbeat",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
            heartbeatBody(kClientId, "")),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(service_.heartbeatCalls(), 0U);

    expectEnvelope(
        call(
            "/api/v1/clients/heartbeat",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
            heartbeatBody("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee")),
        401,
        "AUTH_TOKEN_INVALID");

    const auto started = std::chrono::steady_clock::now();
    const auto success = call(
        "/api/v1/clients/heartbeat",
        {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
        heartbeatBody());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    expectEnvelope(success, 200, "OK");
    EXPECT_TRUE(jsonBody(success).at("data").is_null());
}

TEST_F(AuthApiTest, ConcurrentHeartbeatsRemainIndependentAndAccessLogsAreSanitized) {
    constexpr std::size_t requestCount = 20U;
    expectEnvelope(
        call(
            "/api/v1/auth/login",
            {{"Content-Type", "application/json"}},
            loginBody()),
        200,
        "OK");
    std::vector<std::future<ParsedResponse>> requests;
    requests.reserve(requestCount);
    for (std::size_t index = 0U; index < requestCount; ++index) {
        requests.push_back(std::async(std::launch::async, [this] {
            return call(
                "/api/v1/clients/heartbeat",
                {{"Content-Type", "application/json"}, {"Authorization", "Bearer valid"}},
                heartbeatBody());
        }));
    }
    for (auto& pending : requests) {
        expectEnvelope(pending.get(), 200, "OK");
    }
    EXPECT_EQ(service_.heartbeatCalls(), requestCount);

    const auto entries = waitForAccessLogs(accessLog_, requestCount + 1U);
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(std::none_of(entries->begin(), entries->end(), [](const auto& entry) {
        const auto visible = entry.method + entry.routeTemplate + entry.code + entry.requestId;
        return visible.find("admin") != std::string::npos ||
               visible.find("plate-demo") != std::string::npos ||
               visible.find("Bearer") != std::string::npos ||
               visible.find("valid") != std::string::npos ||
               visible.find(std::string(kClientId)) != std::string::npos ||
               visible.find("sensitive-mqtt-password") != std::string::npos;
    }));
}

}  // namespace
}  // namespace ocrservice
