#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
#include "AccessListService.h"
#include "AuthService.h"
#include "HttpServer.h"
#include "ProtocolTime.h"
#include "RequestId.h"

namespace ocrservice {
namespace {

using namespace std::chrono_literals;
using services::access_list::AccessListService;
using services::auth::Authenticated;
using services::auth::AuthenticationResult;
using services::auth::AuthFailure;
using services::auth::AuthSession;
using services::auth::IAuthService;
using services::auth::LoginResult;
using services::auth::OperationResult;

constexpr std::string_view kValidToken = "valid";

domain::UtcTimePoint protocolTime(const std::string_view value) {
    return serialization::time::parseProtocolTime(value);
}

domain::AccessListRecord accessRecord(
    const std::uint64_t id,
    const domain::AccessListType type,
    const std::string_view plate,
    const std::string_view remark,
    const std::string_view createdAt) {
    return domain::AccessListRecord(
        id,
        type,
        domain::PlateNumber::parse(plate),
        std::string(remark),
        "演示管理员",
        protocolTime(createdAt));
}

class FakeAccessListRepository final : public domain::IAccessListRepository {
public:
    FakeAccessListRepository()
        : records_{
              accessRecord(
                  3U,
                  domain::AccessListType::white,
                  "ABC123",
                  "较新白名单",
                  "2026-08-15T12:03:00.000+08:00"),
              accessRecord(
                  2U,
                  domain::AccessListType::black,
                  "A%_\\1",
                  "字面通配符",
                  "2026-08-15T12:02:00.000+08:00"),
              accessRecord(
                  1U,
                  domain::AccessListType::white,
                  "京A12345",
                  "教学测试车辆",
                  "2026-08-15T12:01:00.000+08:00")} {}

    domain::RepositoryResult<domain::PageResult<domain::AccessListRecord>> query(
        const domain::AccessListFilter& filter,
        const domain::PageRequest& page) override {
        ++queryCalls;
        lastQueryKeyword = filter.keyword().value();
        if (queryFailure) {
            const auto failure = *std::exchange(queryFailure, std::nullopt);
            return failure;
        }
        std::vector<domain::AccessListRecord> matches;
        for (const auto& record : records_) {
            if (record.listType() == filter.listType() &&
                record.plateNumber().value().find(filter.keyword().value()) != std::string::npos) {
                matches.push_back(record);
            }
        }
        const auto total = static_cast<std::uint64_t>(matches.size());
        if (page.page() != 1U) {
            matches.clear();
        }
        return domain::PageResult<domain::AccessListRecord>(
            std::move(matches), page, total);
    }

    domain::RepositoryResult<std::optional<domain::AccessListRecord>> lookup(
        const domain::PlateNumber& plateNumber) override {
        ++lookupCalls;
        lastLookupPlate = plateNumber.value();
        if (lookupFailure) {
            const auto failure = *std::exchange(lookupFailure, std::nullopt);
            return failure;
        }
        const auto found = findPlate(plateNumber.value());
        return found == records_.end() ? std::optional<domain::AccessListRecord>{} :
                                         std::optional<domain::AccessListRecord>(*found);
    }

    domain::AccessListInsertResult insert(const domain::NewAccessListRecord& record) override {
        ++insertCalls;
        lastCreatorUserId = record.createdByUserId();
        lastCreatorDisplayName = record.createdBy();
        lastCreatedAt = record.createdAt();
        lastInsertedPlate = record.plateNumber().value();
        lastInsertedRemark = record.remark();
        if (insertFailure) {
            const auto failure = *std::exchange(insertFailure, std::nullopt);
            return failure;
        }
        const auto existing = findPlate(record.plateNumber().value());
        if (existing != records_.end()) {
            return domain::AccessListConflict(existing->listType());
        }
        domain::AccessListRecord inserted(
            nextId_++,
            record.listType(),
            record.plateNumber(),
            record.remark(),
            record.createdBy(),
            record.createdAt());
        records_.insert(records_.begin(), inserted);
        return inserted;
    }

    domain::RepositoryResult<bool> remove(const std::uint64_t id) override {
        ++removeCalls;
        lastRemovedId = id;
        if (removeFailure) {
            const auto failure = *std::exchange(removeFailure, std::nullopt);
            return failure;
        }
        const auto found = std::find_if(records_.begin(), records_.end(), [id](const auto& record) {
            return record.id() == id;
        });
        if (found == records_.end()) {
            return false;
        }
        records_.erase(found);
        return true;
    }

    std::optional<domain::RepositoryFailure> queryFailure;
    std::optional<domain::RepositoryFailure> lookupFailure;
    std::optional<domain::RepositoryFailure> insertFailure;
    std::optional<domain::RepositoryFailure> removeFailure;
    std::string lastQueryKeyword;
    std::string lastLookupPlate;
    std::string lastInsertedPlate;
    std::string lastInsertedRemark;
    std::optional<std::uint64_t> lastCreatorUserId;
    std::string lastCreatorDisplayName;
    std::optional<domain::UtcTimePoint> lastCreatedAt;
    std::optional<std::uint64_t> lastRemovedId;
    unsigned int queryCalls = 0U;
    unsigned int lookupCalls = 0U;
    unsigned int insertCalls = 0U;
    unsigned int removeCalls = 0U;

private:
    std::vector<domain::AccessListRecord>::iterator findPlate(const std::string_view plate) {
        return std::find_if(records_.begin(), records_.end(), [plate](const auto& record) {
            return record.plateNumber().value() == plate;
        });
    }

    std::vector<domain::AccessListRecord> records_;
    std::uint64_t nextId_ = 4U;
};

class FakeClock final : public services::access_list::IAccessListClock {
public:
    domain::UtcTimePoint nowUtc() override {
        ++calls;
        if (std::exchange(failNext, false)) {
            throw std::runtime_error("injected clock failure");
        }
        return now;
    }

    domain::UtcTimePoint now = protocolTime("2026-08-15T13:14:15.123+08:00");
    unsigned int calls = 0U;
    bool failNext = false;
};

class FakeAuthService final : public IAuthService {
public:
    FakeAuthService()
        : clientId_(domain::Uuid::parse("11111111-2222-4333-8444-555555555555")),
          expiresAt_(protocolTime("2099-08-23T20:30:00.000+08:00")) {}

    LoginResult login(std::string_view, std::string_view, const domain::Uuid&) override {
        return AuthFailure::internal;
    }

    AuthenticationResult authenticate(const std::string_view token) override {
        ++authenticateCalls;
        if (token == "expired") {
            return AuthFailure::tokenExpired;
        }
        if (token != kValidToken) {
            return AuthFailure::tokenInvalid;
        }
        return Authenticated{AuthSession{
            7U,
            "会话管理员",
            clientId_,
            expiresAt_,
            expiresAt_}};
    }

    OperationResult logout(std::string_view) override { return AuthFailure::internal; }
    OperationResult heartbeat(std::string_view, const domain::Uuid&) override {
        return AuthFailure::internal;
    }

    unsigned int authenticateCalls = 0U;

private:
    domain::Uuid clientId_;
    domain::UtcTimePoint expiresAt_;
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
};

ParsedResponse parseResponse(const std::string& raw) {
    ParsedResponse response;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        throw std::runtime_error("HTTP response has no complete headers");
    }
    std::istringstream headers(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(headers, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    if (!status) {
        throw std::runtime_error("HTTP response has an invalid status line");
    }
    std::string line;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] =
                trim(line.substr(colon + 1U));
        }
    }
    response.body = raw.substr(headerEnd + 4U);
    const auto length = response.headers.find("content-length");
    if (length == response.headers.end() ||
        response.body.size() != static_cast<std::size_t>(std::stoull(length->second))) {
        throw std::runtime_error("HTTP response body length is incomplete");
    }
    return response;
}

std::string socketExchange(const std::uint16_t port, const std::string& request) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(socket, asio::buffer(request));
    socket.non_blocking(true);
    std::string raw;
    std::array<char, 16384> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error == asio::error::would_block || error == asio::error::try_again) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("HTTP socket response timed out");
            }
            std::this_thread::sleep_for(2ms);
            continue;
        }
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            break;
        }
        if (error) {
            throw std::runtime_error("HTTP socket read failed");
        }
    }
    return raw;
}

std::string request(
    const std::string_view method,
    const std::string& target,
    const std::string_view token = kValidToken,
    const std::string& body = {},
    const std::optional<std::string_view> contentType = std::nullopt) {
    std::string result = std::string(method) + " " + target +
                         " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " +
                         std::string(token) + "\r\nConnection: close\r\n";
    if (contentType) {
        result += "Content-Type: " + std::string(*contentType) + "\r\n";
    }
    if (!body.empty() || method == "POST") {
        result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    result += "\r\n";
    result += body;
    return result;
}

nlohmann::json jsonBody(const ParsedResponse& response) {
    return nlohmann::json::parse(response.body);
}

std::set<std::string> keys(const nlohmann::json& object) {
    std::set<std::string> result;
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        result.insert(iterator.key());
    }
    return result;
}

void expectFailure(
    const ParsedResponse& response,
    const int status,
    const std::string_view code) {
    ASSERT_EQ(response.status, status);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    const auto body = jsonBody(response);
    EXPECT_EQ(keys(body),
              (std::set<std::string>{"success", "code", "message", "requestId", "data"}));
    EXPECT_FALSE(body.at("success").get<bool>());
    EXPECT_EQ(body.at("code"), code);
    EXPECT_FALSE(body.at("message").get<std::string>().empty());
    EXPECT_NO_THROW(domain::Uuid::parse(body.at("requestId").get<std::string>()));
    EXPECT_TRUE(body.at("data").is_null());
}

class AccessListApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<http::server::HttpServer>(
            0U, http::middleware::makeRandomRequestIdGenerator());
        controller_ =
            std::make_unique<http::controllers::access_list::AccessListController>(auth_, service_);
        controller_->registerRoutes(*server_);
        serverThread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        port_ = server_->port();
        ASSERT_NE(port_, 0U);
    }

    void TearDown() override {
        server_->stop();
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    ParsedResponse call(const std::string& wireRequest) const {
        return parseResponse(socketExchange(port_, wireRequest));
    }

    FakeAccessListRepository repository_;
    FakeClock clock_;
    FakeAuthService auth_;
    AccessListService service_{repository_, clock_};
    std::unique_ptr<http::server::HttpServer> server_;
    std::unique_ptr<http::controllers::access_list::AccessListController> controller_;
    std::thread serverThread_;
    std::uint16_t port_ = 0U;
};

TEST_F(AccessListApiTest, TransportAndAuthenticationPrecedeRouteDecoders) {
    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists?bad",
            "invalid")),
        401,
        "AUTH_TOKEN_INVALID");
    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists/lookup?plateNumber=bad%20plate",
            "expired")),
        401,
        "AUTH_TOKEN_EXPIRED");
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists?debug=1",
            "invalid",
            "not-json",
            "application/json")),
        401,
        "AUTH_TOKEN_INVALID");
    expectFailure(
        call(request("DELETE", "/api/v1/access-lists/lookup", "expired")),
        401,
        "AUTH_TOKEN_EXPIRED");

    const auto authenticatedBeforeBody = auth_.authenticateCalls;
    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists?bad",
            "invalid",
            "{}",
            "text/plain")),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call(request(
            "DELETE",
            "/api/v1/access-lists/1",
            "invalid",
            "{}",
            "text/plain")),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(auth_.authenticateCalls, authenticatedBeforeBody);

    const auto authenticatedBeforeContentType = auth_.authenticateCalls;
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            "{}",
            "text/plain")),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(auth_.authenticateCalls, authenticatedBeforeContentType);
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            "not-json",
            "application/json")),
        400,
        "INVALID_REQUEST");
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists?debug=1",
            kValidToken,
            "{}",
            "application/json")),
        400,
        "INVALID_REQUEST");
    EXPECT_EQ(repository_.queryCalls, 0U);
    EXPECT_EQ(repository_.lookupCalls, 0U);
    EXPECT_EQ(repository_.insertCalls, 0U);
    EXPECT_EQ(repository_.removeCalls, 0U);
    EXPECT_EQ(clock_.calls, 0U);
}

TEST_F(AccessListApiTest, QueryAndLookupReturnExactNormalizedDtos) {
    auto started = std::chrono::steady_clock::now();
    const auto page = call(request(
        "GET",
        "/api/v1/access-lists?listType=BLACK&keyword=%25_%5C&page=1&pageSize=100"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(page.status, 200);
    const auto pageEnvelope = jsonBody(page);
    EXPECT_EQ(keys(pageEnvelope),
              (std::set<std::string>{"success", "code", "message", "requestId", "data"}));
    EXPECT_TRUE(pageEnvelope.at("success").get<bool>());
    EXPECT_EQ(pageEnvelope.at("code"), "OK");
    const auto& data = pageEnvelope.at("data");
    EXPECT_EQ(keys(data), (std::set<std::string>{"items", "page", "pageSize", "total"}));
    EXPECT_EQ(data.at("page"), 1U);
    EXPECT_EQ(data.at("pageSize"), 100U);
    EXPECT_EQ(data.at("total"), 1U);
    ASSERT_EQ(data.at("items").size(), 1U);
    EXPECT_EQ(repository_.lastQueryKeyword, "%_\\");
    EXPECT_EQ(repository_.queryCalls, 1U);
    const auto& item = data.at("items").at(0);
    EXPECT_EQ(keys(item),
              (std::set<std::string>{
                  "id", "listType", "plateNumber", "remark", "createdBy", "createdAt"}));
    EXPECT_EQ(item.at("plateNumber"), "A%_\\1");
    EXPECT_EQ(item.at("listType"), "BLACK");

    const auto lookup = call(request(
        "GET",
        "/api/v1/access-lists/lookup?plateNumber=%E3%80%80%E4%BA%ACa12345%C2%85"));
    ASSERT_EQ(lookup.status, 200);
    EXPECT_EQ(repository_.lastLookupPlate, "京A12345");
    EXPECT_EQ(repository_.lookupCalls, 1U);
    EXPECT_EQ(jsonBody(lookup).at("data").at("plateNumber"), "京A12345");

    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists/lookup?plateNumber=%25E4%25BA%25ACA12345")),
        404,
        "ACCESS_LIST_NOT_FOUND");
    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists?listType=WHITE&keyword=&page=1&pageSize=100&debug=1")),
        400,
        "INVALID_REQUEST");
}

TEST_F(AccessListApiTest, CreateUsesSessionCreatorSingleClockSampleAndStableConflicts) {
    const nlohmann::json createBody{
        {"listType", "BLACK"},
        {"plateNumber", "　newa123\u0085"},
        {"remark", "新增教学车辆"}};
    auto started = std::chrono::steady_clock::now();
    const auto created = call(request(
        "POST",
        "/api/v1/access-lists",
        kValidToken,
        createBody.dump(),
        "Application/JSON; Charset=UTF-8"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(created.status, 200);
    EXPECT_EQ(clock_.calls, 1U);
    EXPECT_EQ(repository_.insertCalls, 1U);
    ASSERT_TRUE(repository_.lastCreatorUserId);
    EXPECT_EQ(*repository_.lastCreatorUserId, 7U);
    EXPECT_EQ(repository_.lastCreatorDisplayName, "会话管理员");
    ASSERT_TRUE(repository_.lastCreatedAt);
    EXPECT_EQ(*repository_.lastCreatedAt, clock_.now);
    EXPECT_EQ(repository_.lastInsertedPlate, "NEWA123");
    EXPECT_EQ(repository_.lastInsertedRemark, "新增教学车辆");
    const auto data = jsonBody(created).at("data");
    EXPECT_EQ(data.at("listType"), "BLACK");
    EXPECT_EQ(data.at("plateNumber"), "NEWA123");
    EXPECT_EQ(data.at("createdBy"), "会话管理员");
    EXPECT_EQ(data.at("createdAt"), "2026-08-15T13:14:15.123+08:00");

    const auto callsBeforeConflict = clock_.calls;
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            nlohmann::json{
                {"listType", "BLACK"},
                {"plateNumber", "京a12345"},
                {"remark", ""}}.dump(),
            "application/json")),
        409,
        "ACCESS_LIST_CONFLICT_WHITE");
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            nlohmann::json{
                {"listType", "WHITE"},
                {"plateNumber", "A%_\\1"},
                {"remark", ""}}.dump(),
            "application/json")),
        409,
        "ACCESS_LIST_CONFLICT_BLACK");
    EXPECT_EQ(clock_.calls, callsBeforeConflict + 2U);

    const auto insertsBeforeClockFailure = repository_.insertCalls;
    clock_.failNext = true;
    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            nlohmann::json{
                {"listType", "WHITE"},
                {"plateNumber", "CLOCK1"},
                {"remark", ""}}.dump(),
            "application/json")),
        500,
        "INTERNAL_ERROR");
    EXPECT_EQ(repository_.insertCalls, insertsBeforeClockFailure);

    expectFailure(
        call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            R"({"listType":"WHITE","plateNumber":"ABC","remark":"","createdBy":"x"})",
            "application/json")),
        400,
        "INVALID_REQUEST");
}

TEST_F(AccessListApiTest, DeleteUsesSafeIntegerAndReturnsStrictEmptyEnvelope) {
    auto started = std::chrono::steady_clock::now();
    const auto removed = call(request(
        "DELETE",
        "/api/v1/access-lists/0000002",
        kValidToken,
        {},
        "text/plain"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_EQ(removed.status, 200);
    ASSERT_TRUE(repository_.lastRemovedId);
    EXPECT_EQ(*repository_.lastRemovedId, 2U);
    EXPECT_EQ(repository_.removeCalls, 1U);
    const auto envelope = jsonBody(removed);
    EXPECT_TRUE(envelope.at("success").get<bool>());
    EXPECT_EQ(envelope.at("code"), "OK");
    EXPECT_EQ(envelope.at("message"), "");
    EXPECT_TRUE(envelope.at("data").is_null());

    expectFailure(
        call(request("DELETE", "/api/v1/access-lists/0000002")),
        404,
        "ACCESS_LIST_NOT_FOUND");
    expectFailure(
        call(request("DELETE", "/api/v1/access-lists/lookup")),
        400,
        "INVALID_REQUEST");
    for (const std::string id : {
             "0", "+1", "-1", "1.0", "%31", "9007199254740992"}) {
        expectFailure(
            call(request("DELETE", "/api/v1/access-lists/" + id)),
            400,
            "INVALID_REQUEST");
    }
    expectFailure(
        call(request("DELETE", "/api/v1/access-lists/1?force=true")),
        400,
        "INVALID_REQUEST");
}

TEST_F(AccessListApiTest, MapsEveryRepositoryFailureWithoutLeakingDetails) {
    repository_.queryFailure = domain::RepositoryFailure::unavailable;
    expectFailure(
        call(request(
            "GET",
            "/api/v1/access-lists?listType=WHITE&keyword=&page=1&pageSize=100")),
        503,
        "DATABASE_UNAVAILABLE");
    for (const auto failure : {
             domain::RepositoryFailure::conflict,
             domain::RepositoryFailure::notFound,
             domain::RepositoryFailure::stateConflict,
             domain::RepositoryFailure::internal}) {
        repository_.queryFailure = failure;
        expectFailure(
            call(request(
                "GET",
                "/api/v1/access-lists?listType=WHITE&keyword=&page=1&pageSize=100")),
            500,
            "INTERNAL_ERROR");
    }

    expectFailure(
        call(request("GET", "/api/v1/access-lists/lookup?plateNumber=MISSING")),
        404,
        "ACCESS_LIST_NOT_FOUND");
    repository_.lookupFailure = domain::RepositoryFailure::unavailable;
    expectFailure(
        call(request("GET", "/api/v1/access-lists/lookup?plateNumber=ABC123")),
        503,
        "DATABASE_UNAVAILABLE");
    for (const auto failure : {
             domain::RepositoryFailure::conflict,
             domain::RepositoryFailure::notFound,
             domain::RepositoryFailure::stateConflict,
             domain::RepositoryFailure::internal}) {
        repository_.lookupFailure = failure;
        expectFailure(
            call(request("GET", "/api/v1/access-lists/lookup?plateNumber=ABC123")),
            500,
            "INTERNAL_ERROR");
    }

    const auto post = [this](const domain::RepositoryFailure failure) {
        repository_.insertFailure = failure;
        return call(request(
            "POST",
            "/api/v1/access-lists",
            kValidToken,
            nlohmann::json{
                {"listType", "WHITE"},
                {"plateNumber", "FAIL"},
                {"remark", ""}}.dump(),
            "application/json"));
    };
    expectFailure(post(domain::RepositoryFailure::unavailable), 503, "DATABASE_UNAVAILABLE");
    for (const auto failure : {
             domain::RepositoryFailure::conflict,
             domain::RepositoryFailure::notFound,
             domain::RepositoryFailure::stateConflict,
             domain::RepositoryFailure::internal}) {
        expectFailure(post(failure), 500, "INTERNAL_ERROR");
    }

    repository_.removeFailure = domain::RepositoryFailure::unavailable;
    expectFailure(
        call(request("DELETE", "/api/v1/access-lists/1")),
        503,
        "DATABASE_UNAVAILABLE");
    for (const auto failure : {
             domain::RepositoryFailure::conflict,
             domain::RepositoryFailure::notFound,
             domain::RepositoryFailure::stateConflict,
             domain::RepositoryFailure::internal}) {
        repository_.removeFailure = failure;
        expectFailure(
            call(request("DELETE", "/api/v1/access-lists/1")),
            500,
            "INTERNAL_ERROR");
    }
}

}  // namespace
}  // namespace ocrservice
