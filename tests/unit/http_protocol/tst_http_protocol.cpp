#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ExactJson.h"
#include "HttpPolicy.h"
#include "Ports.h"
#include "ProtocolError.h"
#include "ProtocolTime.h"
#include "QueryDecoders.h"
#include "RequestDecoders.h"

namespace {

using namespace ocrservice;
using http::protocol::ProtocolError;
using serialization::json::Json;

domain::Uuid requestId() {
    return domain::Uuid::parse("4a4911ce-041c-42ad-bff9-1dd5dc70de66");
}

domain::RecognitionId recognitionId() {
    return domain::RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910");
}

domain::CaptureId captureId() {
    return domain::CaptureId::parse("11111111-2222-4333-8444-555555555555");
}

domain::UtcTimePoint at(const std::string_view value) {
    return serialization::time::parseProtocolTime(value);
}

std::string chinesePlate() {
    return "\xE4\xBA\xAC" "A12345";
}

domain::RecognitionSnapshot processingSnapshot() {
    return domain::RecognitionSnapshot(
        recognitionId(),
        1U,
        domain::DeviceId::parse("device-001"),
        domain::RecognitionStatus::processing,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        std::nullopt,
        std::nullopt);
}

domain::RecognitionSnapshot succeededSnapshot() {
    return domain::RecognitionSnapshot(
        recognitionId(),
        2U,
        domain::DeviceId::parse("device-001"),
        domain::RecognitionStatus::succeeded,
        domain::PlateNumber::parse(chinesePlate()),
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

domain::RecognitionSnapshot failedSnapshot() {
    return domain::RecognitionSnapshot(
        recognitionId(),
        3U,
        domain::DeviceId::parse("device-001"),
        domain::RecognitionStatus::failed,
        std::nullopt,
        domain::RecognitionFailureCode::plateNotFound,
        std::string("plate not found"),
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        result.insert(iterator.key());
    }
    return result;
}

TEST(ProtocolErrorTest, MapsEveryStableCodeToItsRequiredHttpStatus) {
    using http::protocol::ErrorCode;
    const std::vector<std::tuple<ErrorCode, std::string_view, int>> cases = {
        {ErrorCode::invalidRequest, "INVALID_REQUEST", 400},
        {ErrorCode::imageInvalid, "IMAGE_INVALID", 400},
        {ErrorCode::authInvalidCredentials, "AUTH_INVALID_CREDENTIALS", 401},
        {ErrorCode::authTokenInvalid, "AUTH_TOKEN_INVALID", 401},
        {ErrorCode::authTokenExpired, "AUTH_TOKEN_EXPIRED", 401},
        {ErrorCode::deviceUnauthorized, "DEVICE_UNAUTHORIZED", 401},
        {ErrorCode::userDisabled, "USER_DISABLED", 403},
        {ErrorCode::deviceDisabled, "DEVICE_DISABLED", 403},
        {ErrorCode::deviceForbidden, "DEVICE_FORBIDDEN", 403},
        {ErrorCode::recognitionNotFound, "RECOGNITION_NOT_FOUND", 404},
        {ErrorCode::imageNotFound, "IMAGE_NOT_FOUND", 404},
        {ErrorCode::accessListNotFound, "ACCESS_LIST_NOT_FOUND", 404},
        {ErrorCode::routeNotFound, "ROUTE_NOT_FOUND", 404},
        {ErrorCode::methodNotAllowed, "METHOD_NOT_ALLOWED", 405},
        {ErrorCode::captureIdConflict, "CAPTURE_ID_CONFLICT", 409},
        {ErrorCode::accessListConflictWhite, "ACCESS_LIST_CONFLICT_WHITE", 409},
        {ErrorCode::accessListConflictBlack, "ACCESS_LIST_CONFLICT_BLACK", 409},
        {ErrorCode::imageTooLarge, "IMAGE_TOO_LARGE", 413},
        {ErrorCode::requestTooLarge, "REQUEST_TOO_LARGE", 413},
        {ErrorCode::imageStorageError, "IMAGE_STORAGE_ERROR", 500},
        {ErrorCode::internalError, "INTERNAL_ERROR", 500},
        {ErrorCode::recognitionQueueFull, "RECOGNITION_QUEUE_FULL", 503},
        {ErrorCode::databaseUnavailable, "DATABASE_UNAVAILABLE", 503},
        {ErrorCode::serviceUnavailable, "SERVICE_UNAVAILABLE", 503}};
    for (const auto& [code, text, status] : cases) {
        EXPECT_EQ(http::protocol::toString(code), text);
        EXPECT_EQ(http::protocol::httpStatusFor(code), status);
    }
}

TEST(RequestDecoderTest, DecodesExactLoginAndHeartbeatObjects) {
    const auto login = http::protocol::decodeLoginRequest(
        R"({"username":"admin","password":"plate-demo-2026","clientId":"11111111-2222-4333-8444-555555555555"})");
    EXPECT_EQ(login.username, "admin");
    EXPECT_EQ(login.password, "plate-demo-2026");
    EXPECT_EQ(login.clientId.toString(), "11111111-2222-4333-8444-555555555555");

    const auto heartbeat = http::protocol::decodeHeartbeatRequest(
        R"({"clientId":"11111111-2222-4333-8444-555555555555","appVersion":"0.1.0"})");
    EXPECT_EQ(heartbeat.appVersion, "0.1.0");
}

TEST(RequestDecoderTest, RejectsSyntaxShapeTypeDuplicateAndUuidErrors) {
    const std::vector<std::string> invalid = {
        "[]",
        R"({"username":"admin","password":"p"})",
        R"({"username":"admin","password":"p","clientId":"11111111-2222-4333-8444-555555555555","extra":1})",
        R"({"username":7,"password":"p","clientId":"11111111-2222-4333-8444-555555555555"})",
        R"({"username":"admin","username":"again","password":"p","clientId":"11111111-2222-4333-8444-555555555555"})",
        R"({"username":"admin","password":"p","clientId":"00000000-0000-0000-0000-000000000000"})",
        "{"};
    for (const auto& body : invalid) {
        EXPECT_THROW(http::protocol::decodeLoginRequest(body), ProtocolError) << body;
    }
}

TEST(RequestDecoderTest, UsesUnicodeCodePointLimitsForAccessListFields) {
    const std::string supplementary = "\xF0\x90\x90\x80";
    std::string chineseRemark;
    for (int index = 0; index < 200; ++index) {
        chineseRemark += "\xE4\xB8\xAD";
    }
    const auto acceptedBody = Json{
        {"listType", "WHITE"},
        {"plateNumber", supplementary + std::string(15U, 'A')},
        {"remark", chineseRemark}}.dump();
    const auto accepted = http::protocol::decodeAccessListCreateRequest(acceptedBody);
    EXPECT_EQ(accepted.listType, domain::AccessListType::white);
    EXPECT_EQ(accepted.plateNumber.value(), supplementary + std::string(15U, 'A'));

    const auto supplementaryRemark = Json{
        {"listType", "BLACK"},
        {"plateNumber", chinesePlate()},
        {"remark", std::string()}};
    auto exact200 = supplementaryRemark;
    exact200["remark"] = std::string();
    for (int index = 0; index < 200; ++index) {
        exact200["remark"] = exact200["remark"].get<std::string>() + supplementary;
    }
    EXPECT_NO_THROW(http::protocol::decodeAccessListCreateRequest(exact200.dump()));
    auto tooLong = exact200;
    tooLong["remark"] = tooLong["remark"].get<std::string>() + supplementary;
    EXPECT_THROW(http::protocol::decodeAccessListCreateRequest(tooLong.dump()), ProtocolError);

    const auto tooLongPlate = Json{
        {"listType", "WHITE"},
        {"plateNumber", supplementary + std::string(16U, 'A')},
        {"remark", ""}}.dump();
    EXPECT_THROW(http::protocol::decodeAccessListCreateRequest(tooLongPlate), ProtocolError);
}

TEST(RequestDecoderTest, EnforcesTwoMiBRequestBoundaryBeforeParsing) {
    std::string exact(http::protocol::kMaximumJsonBytes, ' ');
    EXPECT_THROW(http::protocol::decodeHeartbeatRequest(exact), ProtocolError);
    exact.push_back(' ');
    try {
        (void)http::protocol::decodeHeartbeatRequest(exact);
        FAIL() << "expected size rejection";
    } catch (const ProtocolError& error) {
        EXPECT_EQ(error.code(), http::protocol::ErrorCode::invalidRequest);
        EXPECT_STREQ(error.what(), "request JSON exceeds 2 MiB");
    }
}

TEST(RequestDecoderTest, AcceptsOnlyAByteEmptyLogoutBody) {
    EXPECT_NO_THROW(http::protocol::requireEmptyRequestBody(""));
    EXPECT_THROW(http::protocol::requireEmptyRequestBody("{}"), ProtocolError);
    EXPECT_THROW(http::protocol::requireEmptyRequestBody(" "), ProtocolError);
}

TEST(QueryDecoderTest, PreservesLiteralPlusAndDecodesPercentExactlyOnce) {
    const auto literal = http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T12:30:44.000+08:00&endTime=2026-08-15T13:30:44.000+08:00&page=1&pageSize=100");
    const auto encoded = http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T12%3A30%3A44.000%2B08%3A00&endTime=2026-08-15T13%3A30%3A44.000%2B08%3A00&page=2&pageSize=100");
    EXPECT_EQ(literal.page.page(), 1U);
    EXPECT_EQ(encoded.page.page(), 2U);
    EXPECT_EQ(literal.filter.startInclusiveUtc(), encoded.filter.startInclusiveUtc());
    EXPECT_EQ(http::protocol::percentDecodeOnce("%2525"), "%25");
    EXPECT_EQ(http::protocol::percentDecodeOnce("A+B"), "A+B");
}

TEST(QueryDecoderTest, RetainsSqlMetacharactersForRepositoryLiteralEscaping) {
    const auto query = http::protocol::decodeAccessListPageQuery(
        "listType=WHITE&keyword=%25_%5C&page=1&pageSize=100");
    EXPECT_EQ(query.filter.keyword().value(), "%_\\");
    const auto lookup = http::protocol::decodeAccessListLookupQuery(
        "plateNumber=%E4%BA%ACA12345");
    EXPECT_EQ(lookup.value(), chinesePlate());
}

TEST(QueryDecoderTest, RejectsDuplicateUnknownMissingMalformedAndInvalidUtf8) {
    const std::vector<std::string> invalid = {
        "listType=WHITE&keyword=&page=1&page=2&pageSize=100",
        "listType=WHITE&keyword=&page=1&pageSize=100&debug=1",
        "listType=WHITE&keyword=&page=1",
        "listType=WHITE&keyword=%GG&page=1&pageSize=100",
        "listType=WHITE&keyword=%C3%28&page=1&pageSize=100",
        "listType=WHITE&keyword=&page=0&pageSize=100",
        "listType=WHITE&keyword=&page=1&pageSize=99"};
    for (const auto& raw : invalid) {
        EXPECT_THROW(http::protocol::decodeAccessListPageQuery(raw), ProtocolError) << raw;
    }
    EXPECT_THROW(
        http::protocol::decodeHistoryExportQuery(
            "startTime=2026-08-15T12:30:44.000+08:00&endTime=2026-08-15T13:30:44.000+08:00&page=1"),
        ProtocolError);
}

TEST(QueryDecoderTest, EnforcesTimeOrderingDeviceAndSafeInteger) {
    EXPECT_NO_THROW(http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T12:30:44.000+08:00&endTime=2026-08-15T13:30:44.000+08:00&deviceId=device_1&page=9007199254740991&pageSize=100"));
    EXPECT_THROW(http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T13:30:44.000+08:00&endTime=2026-08-15T12:30:44.000+08:00&page=1&pageSize=100"), ProtocolError);
    EXPECT_THROW(http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T12:30:44.000+08:00&endTime=2026-08-15T13:30:44.000+08:00&deviceId=&page=1&pageSize=100"), ProtocolError);
    EXPECT_THROW(http::protocol::decodeHistoryQuery(
        "startTime=2026-08-15T12:30:44.000+08:00&endTime=2026-08-15T13:30:44.000+08:00&page=9007199254740992&pageSize=100"), ProtocolError);
}

TEST(HttpPolicyTest, DefinesExactContentTypesLimitsAndNoRedirects) {
    using http::protocol::ResponseBodyKind;
    EXPECT_EQ(http::protocol::contentTypeFor(ResponseBodyKind::json), "application/json");
    EXPECT_EQ(http::protocol::contentTypeFor(ResponseBodyKind::csv), "text/csv; charset=utf-8");
    EXPECT_TRUE(http::protocol::isJsonContentType("application/json"));
    EXPECT_TRUE(http::protocol::isJsonContentType("Application/JSON; charset=utf-8"));
    EXPECT_TRUE(http::protocol::isJsonContentType("APPLICATION/JSON; CHARSET=UTF-8"));
    EXPECT_FALSE(http::protocol::isJsonContentType("application/json; charset=utf-16"));
    EXPECT_FALSE(http::protocol::isJsonContentType(
        "application/json; charset=utf-8; charset=utf-8"));
    EXPECT_FALSE(http::protocol::isJsonContentType("text/json"));
    EXPECT_EQ(http::protocol::multipartBoundary("multipart/form-data; boundary=abc-123"), "abc-123");
    EXPECT_EQ(http::protocol::multipartBoundary("multipart/form-data; boundary=\"abc-123\""), "abc-123");
    EXPECT_FALSE(http::protocol::multipartBoundary("multipart/form-data").has_value());
    EXPECT_TRUE(http::protocol::isAllowedImageContentType("image/jpeg"));
    EXPECT_TRUE(http::protocol::isAllowedImageContentType("image/png"));
    EXPECT_FALSE(http::protocol::isAllowedImageContentType("image/gif"));
    EXPECT_TRUE(http::protocol::isJsonSizeAllowed(http::protocol::kMaximumJsonBytes));
    EXPECT_EQ(http::protocol::kMaximumRawUrlBytes, 8U * 1024U);
    EXPECT_EQ(http::protocol::kMaximumRequestHeadBytes, 80U * 1024U);
    EXPECT_EQ(http::protocol::kMaximumMultipartBytes, 11U * 1024U * 1024U);
    EXPECT_EQ(http::protocol::kMaximumUploadImageBytes, 10U * 1024U * 1024U);
    EXPECT_FALSE(http::protocol::isJsonSizeAllowed(http::protocol::kMaximumJsonBytes + 1U));
    EXPECT_NO_THROW(http::protocol::requireNonRedirectStatus(202));
    EXPECT_THROW(http::protocol::requireNonRedirectStatus(301), http::protocol::HttpPolicyError);
    EXPECT_THROW(http::protocol::requireNonRedirectStatus(399), http::protocol::HttpPolicyError);
}

TEST(ExactJsonTest, WritesExactFiveFieldSuccessAndFailureEnvelopes) {
    const auto success = serialization::json::EnvelopeWriter::success(requestId());
    EXPECT_EQ(keys(success), (std::set<std::string>{"success", "code", "message", "requestId", "data"}));
    EXPECT_TRUE(success.at("success").get<bool>());
    EXPECT_EQ(success.at("code"), "OK");
    EXPECT_EQ(success.at("message"), "");
    EXPECT_TRUE(success.at("data").is_null());

    const auto failure = serialization::json::EnvelopeWriter::failure(
        requestId(), http::protocol::ErrorCode::databaseUnavailable, "database unavailable");
    EXPECT_EQ(keys(failure), keys(success));
    EXPECT_FALSE(failure.at("success").get<bool>());
    EXPECT_EQ(failure.at("code"), "DATABASE_UNAVAILABLE");
    EXPECT_TRUE(failure.at("data").is_null());
    EXPECT_THROW(
        serialization::json::EnvelopeWriter::success(requestId(), Json::array()),
        std::invalid_argument);
}

TEST(ExactJsonTest, WritesAllSnapshotNullCombinationsAndMqttRoots) {
    const auto processing = serialization::json::toJsonExact(processingSnapshot());
    const auto succeeded = serialization::json::toJsonExact(succeededSnapshot());
    const auto failed = serialization::json::toJsonExact(failedSnapshot());
    const std::set<std::string> expected = {
        "schemaVersion", "recognitionId", "revision", "deviceId", "status", "plateNumber",
        "errorCode", "errorMessage", "capturedAt", "startedAt", "completedAt", "durationMs"};
    EXPECT_EQ(keys(processing), expected);
    EXPECT_TRUE(processing.at("plateNumber").is_null());
    EXPECT_TRUE(processing.at("errorCode").is_null());
    EXPECT_TRUE(processing.at("completedAt").is_null());
    EXPECT_EQ(succeeded.at("plateNumber"), chinesePlate());
    EXPECT_TRUE(succeeded.at("errorCode").is_null());
    EXPECT_TRUE(succeeded.at("errorMessage").is_null());
    EXPECT_FALSE(succeeded.at("completedAt").is_null());
    EXPECT_TRUE(failed.at("plateNumber").is_null());
    EXPECT_EQ(failed.at("errorCode"), "PLATE_NOT_FOUND");
    EXPECT_EQ(failed.at("errorMessage"), "plate not found");

    const auto management = serialization::json::managementRecognitionEvent(succeededSnapshot());
    EXPECT_EQ(keys(management), expected);
    EXPECT_FALSE(management.contains("gateAction"));
    const auto deviceSuccess = serialization::json::deviceRecognitionResult(succeededSnapshot());
    const auto deviceFailure = serialization::json::deviceRecognitionResult(failedSnapshot());
    EXPECT_EQ(deviceSuccess.size(), 13U);
    EXPECT_EQ(deviceSuccess.at("gateAction"), "OPEN");
    EXPECT_EQ(deviceFailure.at("gateAction"), "KEEP_CLOSED");
    EXPECT_THROW(
        serialization::json::deviceRecognitionResult(processingSnapshot()),
        std::invalid_argument);
}

TEST(ExactJsonTest, WritesLoginPagesAccessListAcceptanceAndHealthDtos) {
    using serialization::json::ComponentStatus;
    using serialization::json::HealthStatus;
    const auto expires = at("2026-08-15T20:30:00.000+08:00");
    const serialization::json::LoginData login{
        "Demo Administrator",
        "opaque-random-token",
        expires,
        {"192.168.137.128", 1883U, false, "management-client", "mqtt-password", expires,
         "plate/management/recognition-events"}};
    const auto loginJson = serialization::json::toJsonExact(login);
    EXPECT_EQ(keys(loginJson), (std::set<std::string>{"displayName", "accessToken", "expiresAt", "mqtt"}));
    EXPECT_EQ(keys(loginJson.at("mqtt")),
              (std::set<std::string>{"host", "port", "tls", "username", "password", "expiresAt", "eventTopic"}));
    EXPECT_EQ(loginJson.at("expiresAt"), loginJson.at("mqtt").at("expiresAt"));
    auto invalidLogin = login;
    invalidLogin.mqtt.tls = true;
    EXPECT_THROW(serialization::json::toJsonExact(invalidLogin), std::invalid_argument);

    domain::PageResult<domain::RecognitionSnapshot> recognitionPage(
        {succeededSnapshot()}, domain::PageRequest(2U), 101U);
    const auto page = serialization::json::toJsonExact(recognitionPage);
    EXPECT_EQ(keys(page), (std::set<std::string>{"items", "page", "pageSize", "total"}));
    EXPECT_EQ(page.at("page"), 2U);
    EXPECT_EQ(page.at("pageSize"), 100U);
    EXPECT_EQ(page.at("total"), 101U);

    const domain::AccessListRecord record(
        1U, domain::AccessListType::white, domain::PlateNumber::parse(chinesePlate()), "demo car",
        "Demo Administrator", at("2026-08-15T12:00:00.000+08:00"));
    const auto recordJson = serialization::json::toJsonExact(record);
    EXPECT_EQ(keys(recordJson),
              (std::set<std::string>{"id", "listType", "plateNumber", "remark", "createdBy", "createdAt"}));
    domain::PageResult<domain::AccessListRecord> accessPage(
        {record}, domain::PageRequest(1U), 1U);
    EXPECT_EQ(serialization::json::toJsonExact(accessPage).at("items").size(), 1U);

    const serialization::json::DeviceAcceptanceData acceptance{
        recognitionId(), captureId(), domain::RecognitionStatus::processing};
    EXPECT_EQ(keys(serialization::json::toJsonExact(acceptance)),
              (std::set<std::string>{"recognitionId", "captureId", "status"}));

    const serialization::json::HealthData health{
        HealthStatus::degraded, ComponentStatus::up, ComponentStatus::up,
        ComponentStatus::down, 3U, 20U};
    const auto healthJson = serialization::json::toJsonExact(health);
    EXPECT_EQ(keys(healthJson),
              (std::set<std::string>{"status", "model", "mysql", "mqtt", "queueDepth", "queueCapacity"}));
    EXPECT_EQ(healthJson.at("status"), "DEGRADED");
    EXPECT_THROW(
        serialization::json::toJsonExact(serialization::json::HealthData{
            HealthStatus::up, ComponentStatus::up, ComponentStatus::up,
            ComponentStatus::down, 0U, 20U}),
        std::invalid_argument);
    EXPECT_THROW(
        serialization::json::toJsonExact(serialization::json::HealthData{
            HealthStatus::up, ComponentStatus::up, ComponentStatus::up,
            ComponentStatus::up, domain::kJsonSafeIntegerMaximum + 1U,
            domain::kJsonSafeIntegerMaximum + 1U}),
        domain::DomainError);
}

TEST(ExactJsonTest, EnforcesExactTwoMiBSerializedBoundary) {
    const std::size_t objectOverhead = std::string(R"({"x":""})").size();
    const Json exact{{"x", std::string(http::protocol::kMaximumJsonBytes - objectOverhead, 'a')}};
    EXPECT_EQ(serialization::json::serializeExact(exact).size(), http::protocol::kMaximumJsonBytes);
    const Json tooLarge{{"x", std::string(http::protocol::kMaximumJsonBytes - objectOverhead + 1U, 'a')}};
    EXPECT_THROW(serialization::json::serializeExact(tooLarge), http::protocol::HttpPolicyError);
    EXPECT_THROW(
        serialization::json::serializeExact(
            Json{{"unsafe", domain::kJsonSafeIntegerMaximum + 1U}}),
        std::invalid_argument);
    EXPECT_THROW(serialization::json::serializeExact(Json{{"float", 1.5}}), std::invalid_argument);
    EXPECT_THROW(serialization::json::serializeExact(Json::array()), std::invalid_argument);
}

}  // namespace
