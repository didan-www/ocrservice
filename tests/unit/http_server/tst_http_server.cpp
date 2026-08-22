#include <string>

#include <crow.h>
#include <gtest/gtest.h>

#include "HttpPolicy.h"
#include "HttpRuntime.h"
#include "ProtocolError.h"
#include "RequestGuards.h"

namespace ocrservice {
namespace {

using http::middleware::RequestBodyMode;
using http::protocol::ProtocolError;

crow::request requestWith(std::string body = {}) {
    crow::request request;
    request.body = std::move(body);
    return request;
}

TEST(RequestGuardsTest, NoneModeIgnoresContentTypeButRejectsAnyBodyByte) {
    auto request = requestWith();
    request.headers.emplace("Content-Type", "text/plain");
    request.headers.emplace("Content-Type", "application/json");
    EXPECT_NO_THROW(http::middleware::validateRequest(
        request, RequestBodyMode::none, false));

    request.body = " ";
    EXPECT_THROW(
        http::middleware::validateRequest(request, RequestBodyMode::none, false),
        ProtocolError);
}

TEST(RequestGuardsTest, EnforcesJsonAndOptionalEmptyJsonContentTypes) {
    auto json = requestWith("{}");
    EXPECT_THROW(
        http::middleware::validateRequest(json, RequestBodyMode::json, false),
        ProtocolError);
    json.headers.emplace("Content-Type", "Application/JSON; CHARSET=UTF-8");
    EXPECT_NO_THROW(http::middleware::validateRequest(json, RequestBodyMode::json, false));

    auto deferredBody = requestWith("not empty");
    EXPECT_NO_THROW(http::middleware::validateRequest(
        deferredBody, RequestBodyMode::optionalJsonContentType, false));
    deferredBody.headers.emplace("Content-Type", "application/json; charset=utf-8");
    EXPECT_NO_THROW(http::middleware::validateRequest(
        deferredBody, RequestBodyMode::optionalJsonContentType, false));

    auto deferredInvalid = requestWith();
    deferredInvalid.headers.emplace("Content-Type", "text/plain");
    EXPECT_THROW(
        http::middleware::validateRequest(
            deferredInvalid, RequestBodyMode::optionalJsonContentType, false),
        http::protocol::ProtocolError);

    auto deferredRepeated = requestWith();
    deferredRepeated.headers.emplace("Content-Type", "application/json");
    deferredRepeated.headers.emplace("Content-Type", "application/json");
    EXPECT_THROW(
        http::middleware::validateRequest(
            deferredRepeated, RequestBodyMode::optionalJsonContentType, false),
        http::protocol::ProtocolError);

    auto empty = requestWith();
    EXPECT_NO_THROW(http::middleware::validateRequest(
        empty, RequestBodyMode::optionalEmptyJson, false));
    empty.headers.emplace("Content-Type", "text/plain");
    EXPECT_THROW(
        http::middleware::validateRequest(
            empty, RequestBodyMode::optionalEmptyJson, false),
        ProtocolError);
}

TEST(RequestGuardsTest, ParsesOnlyStrictSingleBearerHeader) {
    auto missing = requestWith();
    const auto missingMetadata = http::middleware::validateRequest(
        missing, RequestBodyMode::none, true);
    EXPECT_FALSE(missingMetadata.bearerToken.has_value());

    auto valid = requestWith();
    valid.headers.emplace("Authorization", "bEaReR token-123");
    const auto metadata = http::middleware::validateRequest(
        valid, RequestBodyMode::none, true);
    ASSERT_TRUE(metadata.bearerToken.has_value());
    EXPECT_EQ(*metadata.bearerToken, "token-123");

    for (const std::string value : {"", "Bearer", "Bearer  token", "Bearer token value",
                                    "Basic token", "Bearer token,other"}) {
        auto invalid = requestWith();
        invalid.headers.emplace("Authorization", value);
        EXPECT_THROW(
            http::middleware::validateRequest(
                invalid, RequestBodyMode::none, true),
            ProtocolError)
            << value;
    }

    auto repeated = requestWith();
    repeated.headers.emplace("Authorization", "Bearer first");
    repeated.headers.emplace("Authorization", "Bearer second");
    EXPECT_THROW(
        http::middleware::validateRequest(repeated, RequestBodyMode::none, true),
        ProtocolError);
}

TEST(RequestGuardsTest, GrantsMultipartLimitOnlyToOneStrictContentType) {
    auto valid = requestWith();
    valid.headers.emplace("Content-Type", "multipart/form-data; boundary=abc");
    EXPECT_EQ(
        http::middleware::receiveBodyLimit(valid),
        http::protocol::kMaximumMultipartBytes);

    for (const std::string value : {
             "multipart/form-datajunk; boundary=abc",
             "multipart/form-data",
             "multipart/form-data; boundary=",
             "multipart/form-data; boundary=abc; extra=x"}) {
        auto invalid = requestWith();
        invalid.headers.emplace("Content-Type", value);
        EXPECT_EQ(
            http::middleware::receiveBodyLimit(invalid),
            http::protocol::kMaximumJsonBytes)
            << value;
    }

    auto repeated = valid;
    repeated.headers.emplace("Content-Type", "application/json");
    EXPECT_EQ(
        http::middleware::receiveBodyLimit(repeated),
        http::protocol::kMaximumJsonBytes);
}

TEST(HttpResponseTest, RejectsInvalidImageKindsAndDefinesResponseModes) {
    EXPECT_THROW(
        http::server::HttpResponse::image(
            http::protocol::ResponseBodyKind::csv, 1U,
            [](http::server::IStreamWriter&) {}),
        std::invalid_argument);
    const auto json = http::server::HttpResponse::json(202, "{}", "OK");
    EXPECT_EQ(json.status(), 202);
    EXPECT_EQ(json.kind(), http::protocol::ResponseBodyKind::json);
    EXPECT_EQ(json.body(), "{}");
    EXPECT_EQ(json.code(), "OK");
}

}  // namespace
}  // namespace ocrservice
