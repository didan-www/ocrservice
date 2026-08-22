#include "ClientController.h"

#include <string_view>
#include <variant>

#include "AuthResponses.h"
#include "ProtocolError.h"
#include "RequestDecoders.h"

namespace ocrservice::http::controllers::client {
namespace {

std::string_view token(const server::HttpRequest& request) noexcept {
    return request.bearerToken ? std::string_view(*request.bearerToken) : std::string_view{};
}

}  // namespace

ClientController::ClientController(services::auth::IAuthService& service) : service_(service) {}

void ClientController::registerRoutes(server::IRouteRegistrar& registrar) {
    registrar.registerRoute(
        server::HttpMethod::post,
        "/api/v1/clients/heartbeat",
        {middleware::RequestBodyMode::json, true},
        [this](const server::HttpRequest& request) { return heartbeat(request); });
}

server::HttpResponse ClientController::heartbeat(const server::HttpRequest& request) {
    const auto accessToken = token(request);
    auto authentication = service_.authenticate(accessToken);
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    const auto decoded = protocol::decodeHeartbeatRequest(request.body);
    auto result = service_.heartbeat(accessToken, decoded.clientId);
    if (std::holds_alternative<services::auth::AuthFailure>(result)) {
        return auth::failureResponse(
            request.requestId, std::get<services::auth::AuthFailure>(result));
    }
    return auth::emptySuccessResponse(request.requestId);
}

}  // namespace ocrservice::http::controllers::client
