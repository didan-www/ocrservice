#include "AuthController.h"

#include <string>
#include <utility>
#include <variant>

#include "AuthResponses.h"
#include "ExactJson.h"
#include "ProtocolError.h"
#include "RequestDecoders.h"

namespace ocrservice::http::controllers::auth {
namespace {

constexpr std::string_view kManagementEventTopic =
    "plate/management/recognition-events";

std::string_view token(const server::HttpRequest& request) noexcept {
    return request.bearerToken ? std::string_view(*request.bearerToken) : std::string_view{};
}

}  // namespace

AuthController::AuthController(
    services::auth::IAuthService& service,
    const app::config::MqttConfig& mqttConfig)
    : service_(service), mqttConfig_(mqttConfig) {}

void AuthController::registerRoutes(server::IRouteRegistrar& registrar) {
    registrar.registerRoute(
        server::HttpMethod::post,
        "/api/v1/auth/login",
        {middleware::RequestBodyMode::json, false},
        [this](const server::HttpRequest& request) { return login(request); });
    registrar.registerRoute(
        server::HttpMethod::post,
        "/api/v1/auth/logout",
        {middleware::RequestBodyMode::optionalJsonContentType, true},
        [this](const server::HttpRequest& request) { return logout(request); });
}

server::HttpResponse AuthController::login(const server::HttpRequest& request) {
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto decoded = protocol::decodeLoginRequest(request.body);
    auto result = service_.login(decoded.username, decoded.password, decoded.clientId);
    if (std::holds_alternative<services::auth::AuthFailure>(result)) {
        return failureResponse(
            request.requestId, std::get<services::auth::AuthFailure>(result));
    }
    auto success = std::get<services::auth::LoginSuccess>(std::move(result));
    serialization::json::LoginData data{
        success.session.displayName,
        std::move(success.accessToken),
        success.session.expiresAt,
        {mqttConfig_.publicHost,
         mqttConfig_.port,
         mqttConfig_.tls,
         mqttConfig_.managementUsername,
         mqttConfig_.managementPassword,
         success.session.expiresAt,
         std::string(kManagementEventTopic)}};
    return server::HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(
                request.requestId, serialization::json::toJsonExact(data))));
}

server::HttpResponse AuthController::logout(const server::HttpRequest& request) {
    const auto accessToken = token(request);
    auto authentication = service_.authenticate(accessToken);
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    protocol::requireEmptyRequestBody(request.body);
    auto result = service_.logout(accessToken);
    if (std::holds_alternative<services::auth::AuthFailure>(result)) {
        return failureResponse(
            request.requestId, std::get<services::auth::AuthFailure>(result));
    }
    return emptySuccessResponse(request.requestId);
}

}  // namespace ocrservice::http::controllers::auth
