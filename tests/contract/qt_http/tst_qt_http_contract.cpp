#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "AccessListController.h"
#include "AuthController.h"
#include "AuthService.h"
#include "CsvService.h"
#include "HistoryService.h"
#include "HttpServer.h"
#include "ProtocolTime.h"
#include "QtStrictJson.h"
#include "RecognitionController.h"
#include "ServerConfig.h"

namespace ocrservice {
namespace {

using namespace std::chrono_literals;
using services::access_list::AccessListCreator;
using services::access_list::AccessListFailure;
using services::access_list::IAccessListService;
using services::auth::Authenticated;
using services::auth::AuthenticationResult;
using services::auth::AuthFailure;
using services::auth::AuthSession;
using services::auth::IAuthService;
using services::auth::LoginResult;
using services::auth::LoginSuccess;
using services::history::HistoryFailure;
using services::history::IHistoryService;

constexpr std::string_view kClientId = "11111111-2222-4333-8444-555555555555";
constexpr std::string_view kToken = "valid-token";

domain::UtcTimePoint at(const std::string_view value) {
    return serialization::time::parseProtocolTime(value);
}

domain::RecognitionId recognitionId(const std::string_view value) {
    return domain::RecognitionId::parse(value);
}

domain::RecognitionRecord recognitionRecord() {
    const auto id = recognitionId("a15c7268-b211-4a4c-a765-49b922af1910");
    return domain::RecognitionRecord(
        domain::RecognitionSnapshot(
            id,
            2U,
            domain::DeviceId::parse("device-001"),
            domain::RecognitionStatus::succeeded,
            domain::PlateNumber::parse("京A12345"),
            std::nullopt,
            std::nullopt,
            at("2026-08-15T12:30:44.000+08:00"),
            at("2026-08-15T12:30:45.000+08:00"),
            at("2026-08-15T12:30:45.120+08:00"),
            120U),
        domain::CaptureId::parse("11111111-2222-4333-8444-555555555556"),
        domain::Sha256Digest::parseHex(std::string(64U, 'a')),
        domain::RelativeImagePath::parseGenerated(
            "2026/08/15/a15c7268-b211-4a4c-a765-49b922af1910.png"),
        domain::ImageMime::png,
        4U);
}

domain::AccessListRecord accessRecord() {
    return domain::AccessListRecord(
        5U,
        domain::AccessListType::white,
        domain::PlateNumber::parse("京A12345"),
        "教学测试车辆",
        "演示管理员",
        at("2026-08-15T12:00:00.000+08:00"));
}

class MemoryImageReader final : public domain::ImageReader {
public:
    std::size_t read(std::uint8_t* destination, const std::size_t capacity) override {
        const auto count = std::min(capacity, bytes_.size() - offset_);
        if (count != 0U) {
            std::memcpy(destination, bytes_.data() + offset_, count);
            offset_ += count;
        }
        return count;
    }

private:
    const std::array<std::uint8_t, 4U> bytes_{0x89U, 0x50U, 0x4eU, 0x47U};
    std::size_t offset_ = 0U;
};

class ContractAuthService final : public IAuthService {
public:
    ContractAuthService()
        : clientId_(domain::Uuid::parse(kClientId)),
          issuedAt_(at("2026-08-15T12:30:00.000+08:00")),
          expiresAt_(at("2026-08-15T20:30:00.000+08:00")) {}

    LoginResult login(
        const std::string_view,
        const std::string_view,
        const domain::Uuid& clientId) override {
        return LoginSuccess{
            std::string(kToken),
            AuthSession{7U, "演示管理员", clientId, issuedAt_, expiresAt_}};
    }

    AuthenticationResult authenticate(const std::string_view token) override {
        if (token != kToken) {
            return AuthFailure::tokenInvalid;
        }
        return Authenticated{
            AuthSession{7U, "演示管理员", clientId_, issuedAt_, expiresAt_}};
    }

    services::auth::OperationResult logout(const std::string_view token) override {
        return token == kToken ? services::auth::OperationResult(
                                    services::auth::OperationSucceeded{}) :
                                 services::auth::OperationResult(AuthFailure::tokenInvalid);
    }

    services::auth::OperationResult heartbeat(
        const std::string_view token,
        const domain::Uuid&) override {
        return logout(token);
    }

private:
    domain::Uuid clientId_;
    domain::UtcTimePoint issuedAt_;
    domain::UtcTimePoint expiresAt_;
};

class ContractHistoryService final : public IHistoryService {
public:
    services::history::DetailResult detail(const domain::RecognitionId&) override {
        return recognitionRecord();
    }

    services::history::PageResult query(
        const domain::HistoryFilter& filter,
        const domain::PageRequest& page) override {
        lastStart = filter.startInclusiveUtc();
        lastEnd = filter.endExclusiveUtc();
        lastPage = page.page();
        if (page.page() == domain::PageRequest::maximumPage()) {
            return domain::PageResult<domain::RecognitionRecord>({}, page, 0U);
        }
        return domain::PageResult<domain::RecognitionRecord>(
            std::vector<domain::RecognitionRecord>{recognitionRecord()}, page, 1U);
    }

    services::history::ImageResult openImage(const domain::RecognitionId& id) override {
        if (id == recognitionId("00000000-0000-4000-8000-000000000099")) {
            return HistoryFailure::imageNotFound;
        }
        return domain::ImageFile(
            std::make_unique<MemoryImageReader>(), domain::ImageMime::png, 4U);
    }

    std::optional<domain::UtcTimePoint> lastStart;
    std::optional<domain::UtcTimePoint> lastEnd;
    std::optional<std::uint64_t> lastPage;
};

class ContractCursor final : public domain::IHistoryCursor {
public:
    explicit ContractCursor(const bool failAfterFirst) : failAfterFirst_(failAfterFirst) {}

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> next() override {
        if (index_ == 0U) {
            ++index_;
            return std::optional<domain::RecognitionRecord>(recognitionRecord());
        }
        if (failAfterFirst_ && index_ == 1U) {
            ++index_;
            return domain::RepositoryFailure::internal;
        }
        return std::optional<domain::RecognitionRecord>{};
    }

private:
    bool failAfterFirst_;
    std::size_t index_ = 0U;
};

class ContractCsvService final : public services::csv::ICsvService {
public:
    services::csv::CsvPrepareResult prepare(const domain::HistoryFilter& filter) override {
        const auto device = filter.deviceId().has_value() ?
                                filter.deviceId()->value() : std::string{};
        if (device == "db-down") {
            return services::csv::CsvFailure::databaseUnavailable;
        }
        return services::csv::CsvExport(
            std::make_unique<ContractCursor>(device == "mid-fail"));
    }
};

class ContractAccessListService final : public IAccessListService {
public:
    services::access_list::PageResult query(
        const domain::AccessListFilter& filter,
        const domain::PageRequest& page) override {
        lastKeyword = filter.keyword().value();
        lastPage = page.page();
        if (page.page() == domain::PageRequest::maximumPage()) {
            return domain::PageResult<domain::AccessListRecord>({}, page, 0U);
        }
        return domain::PageResult<domain::AccessListRecord>(
            std::vector<domain::AccessListRecord>{accessRecord()}, page, 1U);
    }

    services::access_list::RecordResult lookup(const domain::PlateNumber&) override {
        return accessRecord();
    }

    services::access_list::RecordResult create(
        const domain::AccessListType,
        domain::PlateNumber,
        std::string,
        AccessListCreator) override {
        return accessRecord();
    }

    services::access_list::OperationResult remove(const std::uint64_t) override {
        return services::access_list::OperationSucceeded{};
    }

    std::string lastKeyword;
    std::optional<std::uint64_t> lastPage;
};

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
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

struct ParsedResponse final {
    int status = 0;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    bool complete = false;
};

ParsedResponse parseResponse(const std::string& raw) {
    ParsedResponse response;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return response;
    }
    std::istringstream headers(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(headers, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    std::string line;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] =
                trim(line.substr(colon + 1U));
        }
    }
    const std::string wireBody = raw.substr(headerEnd + 4U);
    const auto length = response.headers.find("content-length");
    if (length != response.headers.end()) {
        const auto expected = static_cast<std::size_t>(std::stoull(length->second));
        response.complete = wireBody.size() >= expected;
        response.body = wireBody.substr(0U, std::min(wireBody.size(), expected));
        return response;
    }
    const auto transfer = response.headers.find("transfer-encoding");
    if (transfer == response.headers.end() || lowercase(transfer->second) != "chunked") {
        response.body = wireBody;
        response.complete = true;
        return response;
    }
    std::size_t position = 0U;
    while (position < wireBody.size()) {
        const auto lineEnd = wireBody.find("\r\n", position);
        if (lineEnd == std::string::npos) {
            return response;
        }
        std::size_t chunkSize = 0U;
        std::istringstream input(wireBody.substr(position, lineEnd - position));
        input >> std::hex >> chunkSize;
        if (!input) {
            return response;
        }
        position = lineEnd + 2U;
        if (chunkSize == 0U) {
            response.complete = wireBody.size() >= position + 2U &&
                                wireBody.compare(position, 2U, "\r\n") == 0;
            return response;
        }
        if (wireBody.size() < position + chunkSize + 2U) {
            return response;
        }
        response.body.append(wireBody, position, chunkSize);
        position += chunkSize;
        if (wireBody.compare(position, 2U, "\r\n") != 0) {
            return response;
        }
        position += 2U;
    }
    return response;
}

std::string socketExchange(
    const std::uint16_t port,
    const std::string& request,
    const std::chrono::seconds totalBudget) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(socket, asio::buffer(request));
    socket.non_blocking(true);
    std::string raw;
    std::array<char, 16384U> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + totalBudget;
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("Qt total request budget exceeded");
            }
            std::this_thread::sleep_for(2ms);
            continue;
        }
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            return raw;
        }
        if (error) {
            throw std::runtime_error("socket response failed");
        }
    }
}

std::string request(
    const std::string_view method,
    const std::string_view target,
    const std::optional<std::string_view> token = kToken,
    const std::string_view body = {},
    const std::optional<std::string_view> contentType = std::nullopt) {
    std::string result = std::string(method) + " " + std::string(target) +
                         " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    if (token.has_value()) {
        result += "Authorization: Bearer " + std::string(*token) + "\r\n";
    }
    if (contentType.has_value()) {
        result += "Content-Type: " + std::string(*contentType) + "\r\n";
    }
    result += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    result += body;
    return result;
}

class QtHttpContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        mqtt_.publicHost = "192.168.137.128";
        mqtt_.port = 1883U;
        mqtt_.tls = false;
        mqtt_.managementUsername = "management-client";
        mqtt_.managementPassword = "contract-password";
        server_ = std::make_unique<http::server::HttpServer>(0U);
        authController_ =
            std::make_unique<http::controllers::auth::AuthController>(auth_, mqtt_);
        recognitionController_ =
            std::make_unique<http::controllers::recognition::RecognitionController>(
                auth_, history_, csv_);
        accessController_ =
            std::make_unique<http::controllers::access_list::AccessListController>(
                auth_, accessLists_);
        authController_->registerRoutes(*server_);
        recognitionController_->registerRoutes(*server_);
        accessController_->registerRoutes(*server_);
        thread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        ASSERT_NE(server_->port(), 0U);
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ParsedResponse call(
        const std::string& wire,
        const std::chrono::seconds budget = 10s) const {
        return parseResponse(socketExchange(server_->port(), wire, budget));
    }

    ContractAuthService auth_;
    ContractHistoryService history_;
    ContractCsvService csv_;
    ContractAccessListService accessLists_;
    app::config::MqttConfig mqtt_;
    std::unique_ptr<http::server::HttpServer> server_;
    std::unique_ptr<http::controllers::auth::AuthController> authController_;
    std::unique_ptr<http::controllers::recognition::RecognitionController>
        recognitionController_;
    std::unique_ptr<http::controllers::access_list::AccessListController>
        accessController_;
    std::thread thread_;
};

void expectJson(
    const ParsedResponse& response,
    const int status,
    const qt_contract::PayloadKind kind) {
    ASSERT_EQ(response.status, status);
    ASSERT_TRUE(response.complete);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    EXPECT_NO_THROW(qt_contract::parseText(response.body, kind));
}

TEST(QtFixtureContractTest, AcceptsLegalQtFixturesAndRejectsIllegalFixtures) {
    using qt_contract::PayloadKind;
    const std::vector<std::pair<std::filesystem::path, PayloadKind>> legal = {
        {"http/legal/login-envelope.json", PayloadKind::loginEnvelope},
        {"http/legal/processing-envelope.json", PayloadKind::recognitionEnvelope},
        {"http/legal/recognition-page-envelope.json", PayloadKind::recognitionPageEnvelope},
        {"http/legal/access-list-page-envelope.json", PayloadKind::accessListPageEnvelope},
        {"http/legal/empty-envelope.json", PayloadKind::emptyEnvelope},
        {"http/legal/failure-envelope.json", PayloadKind::failureEnvelope}};
    for (const auto& [path, kind] : legal) {
        EXPECT_NO_THROW(qt_contract::parse(qt_contract::loadFixture(path), kind)) << path;
    }

    const std::vector<std::pair<std::filesystem::path, PayloadKind>> invalid = {
        {"http/invalid/envelope-extra-field.json", PayloadKind::emptyEnvelope},
        {"http/invalid/login-extra-field.json", PayloadKind::loginEnvelope},
        {"http/invalid/login-mismatched-expiry.json", PayloadKind::loginEnvelope},
        {"http/invalid/snapshot-missing-null.json", PayloadKind::recognitionEnvelope},
        {"http/invalid/snapshot-unsafe-revision.json", PayloadKind::recognitionEnvelope},
        {"http/invalid/snapshot-utc-time.json", PayloadKind::recognitionEnvelope},
        {"http/invalid/page-size.json", PayloadKind::recognitionPageEnvelope},
        {"http/invalid/page-overflow.json", PayloadKind::recognitionPageEnvelope},
        {"http/invalid/access-list-extra-field.json", PayloadKind::accessListEnvelope}};
    for (const auto& [path, kind] : invalid) {
        EXPECT_THROW(qt_contract::parse(qt_contract::loadFixture(path), kind),
                     qt_contract::ParseError)
            << path;
    }
}

TEST_F(QtHttpContractTest, AuthResponsesPassQtParserIncludingJsonContentTypeEmptyLogout) {
    const auto login = call(request(
        "POST",
        "/api/v1/auth/login",
        std::nullopt,
        nlohmann::json{
            {"username", "admin"}, {"password", "fixture"}, {"clientId", kClientId}}
            .dump(),
        "application/json"));
    expectJson(login, 200, qt_contract::PayloadKind::loginEnvelope);
    const auto loginJson = nlohmann::json::parse(login.body);
    EXPECT_EQ(loginJson.at("data").at("expiresAt"),
              loginJson.at("data").at("mqtt").at("expiresAt"));

    const auto logout = call(request(
        "POST", "/api/v1/auth/logout", kToken, {}, "application/json"));
    expectJson(logout, 200, qt_contract::PayloadKind::emptyEnvelope);
}

TEST_F(QtHttpContractTest, RecognitionResponsesUseRawPlusAndQtTenThirtySecondBudgets) {
    const auto detail = call(request(
        "GET", "/api/v1/recognitions/a15c7268-b211-4a4c-a765-49b922af1910"));
    expectJson(detail, 200, qt_contract::PayloadKind::recognitionEnvelope);

    const auto history = call(request(
        "GET",
        "/api/v1/recognitions?startTime=2026-08-15T12:00:00.000+08:00&endTime=2026-08-15T13:00:00.000+08:00&page=1&pageSize=100"));
    expectJson(history, 200, qt_contract::PayloadKind::recognitionPageEnvelope);
    ASSERT_TRUE(history_.lastStart.has_value());
    ASSERT_TRUE(history_.lastEnd.has_value());
    EXPECT_EQ(*history_.lastStart, at("2026-08-15T12:00:00.000+08:00"));
    EXPECT_EQ(*history_.lastEnd, at("2026-08-15T13:00:00.000+08:00"));

    const auto maximumPage = call(request(
        "GET",
        "/api/v1/recognitions?startTime=2026-08-15T12:00:00.000+08:00&endTime=2026-08-15T13:00:00.000+08:00&page=2147483647&pageSize=100"));
    expectJson(maximumPage, 200, qt_contract::PayloadKind::recognitionPageEnvelope);
    ASSERT_TRUE(history_.lastPage.has_value());
    EXPECT_EQ(*history_.lastPage, domain::PageRequest::maximumPage());
    EXPECT_EQ(nlohmann::json::parse(maximumPage.body).at("data").at("page"), 2147483647U);

    const auto pageOverflow = call(request(
        "GET",
        "/api/v1/recognitions?startTime=2026-08-15T12:00:00.000+08:00&endTime=2026-08-15T13:00:00.000+08:00&page=2147483648&pageSize=100"));
    expectJson(pageOverflow, 400, qt_contract::PayloadKind::failureEnvelope);
    EXPECT_EQ(nlohmann::json::parse(pageOverflow.body).at("code"), "INVALID_REQUEST");

    const auto image = call(
        request(
            "GET",
            "/api/v1/recognitions/a15c7268-b211-4a4c-a765-49b922af1910/image"),
        30s);
    ASSERT_EQ(image.status, 200);
    ASSERT_TRUE(image.complete);
    EXPECT_EQ(image.headers.at("content-type"), "image/png");
    EXPECT_EQ(image.body, std::string("\x89PNG", 4U));
}

TEST_F(QtHttpContractTest, ImageAndCsvPreHeaderErrorsRemainStrictJson) {
    const auto missingImage = call(
        request(
            "GET",
            "/api/v1/recognitions/00000000-0000-4000-8000-000000000099/image"),
        30s);
    expectJson(missingImage, 404, qt_contract::PayloadKind::failureEnvelope);
    EXPECT_EQ(nlohmann::json::parse(missingImage.body).at("code"), "IMAGE_NOT_FOUND");

    const auto csvFailure = call(
        request(
            "GET",
            "/api/v1/recognitions/export?startTime=2026-08-15T12:00:00.000%2B08:00&endTime=2026-08-15T13:00:00.000%2B08:00&deviceId=db-down"),
        120s);
    expectJson(csvFailure, 503, qt_contract::PayloadKind::failureEnvelope);
    EXPECT_EQ(nlohmann::json::parse(csvFailure.body).at("code"), "DATABASE_UNAVAILABLE");
}

TEST_F(QtHttpContractTest, CsvMidStreamFailureAbortsWithinQtOneHundredTwentySecondBudget) {
    const auto response = call(
        request(
            "GET",
            "/api/v1/recognitions/export?startTime=2026-08-15T12:00:00.000%2B08:00&endTime=2026-08-15T13:00:00.000%2B08:00&deviceId=mid-fail"),
        120s);
    ASSERT_EQ(response.status, 200);
    ASSERT_EQ(response.headers.at("content-type"), "text/csv; charset=utf-8");
    EXPECT_FALSE(response.complete);
    ASSERT_GE(response.body.size(), 3U);
    EXPECT_EQ(response.body.substr(0U, 3U), std::string("\xEF\xBB\xBF", 3U));
}

TEST_F(QtHttpContractTest, AccessListResponsesPreserveChinesePercentAndExactDtos) {
    const auto page = call(request(
        "GET",
        "/api/v1/access-lists?listType=WHITE&keyword=%E4%B8%AD%E6%96%87%25&page=1&pageSize=100"));
    expectJson(page, 200, qt_contract::PayloadKind::accessListPageEnvelope);
    EXPECT_EQ(accessLists_.lastKeyword, "中文%");

    const auto maximumPage = call(request(
        "GET",
        "/api/v1/access-lists?listType=WHITE&keyword=&page=2147483647&pageSize=100"));
    expectJson(maximumPage, 200, qt_contract::PayloadKind::accessListPageEnvelope);
    ASSERT_TRUE(accessLists_.lastPage.has_value());
    EXPECT_EQ(*accessLists_.lastPage, domain::PageRequest::maximumPage());
    EXPECT_EQ(nlohmann::json::parse(maximumPage.body).at("data").at("page"), 2147483647U);

    const auto pageOverflow = call(request(
        "GET",
        "/api/v1/access-lists?listType=WHITE&keyword=&page=2147483648&pageSize=100"));
    expectJson(pageOverflow, 400, qt_contract::PayloadKind::failureEnvelope);
    EXPECT_EQ(nlohmann::json::parse(pageOverflow.body).at("code"), "INVALID_REQUEST");

    const auto lookup = call(request(
        "GET", "/api/v1/access-lists/lookup?plateNumber=%E4%BA%ACA12345"));
    expectJson(lookup, 200, qt_contract::PayloadKind::accessListEnvelope);

    const auto created = call(request(
        "POST",
        "/api/v1/access-lists",
        kToken,
        nlohmann::json{
            {"listType", "WHITE"}, {"plateNumber", "京A12345"}, {"remark", "中文%"}}
            .dump(),
        "application/json"));
    expectJson(created, 200, qt_contract::PayloadKind::accessListEnvelope);

    const auto removed = call(request("DELETE", "/api/v1/access-lists/5"));
    expectJson(removed, 200, qt_contract::PayloadKind::emptyEnvelope);
}

}  // namespace
}  // namespace ocrservice
