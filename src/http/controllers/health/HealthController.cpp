#include "HealthController.h"

#include <string>
#include <variant>

#include "ExactJson.h"
#include "ProtocolError.h"
#include "RequestDecoders.h"

namespace ocrservice::http::controllers::health {

HealthController::HealthController(services::health::IHealthService& service) noexcept
    : service_(service) {}

void HealthController::registerRoutes(server::IRouteRegistrar& registrar) {
    registrar.registerRoute(
        server::HttpMethod::get,
        "/health",
        {middleware::RequestBodyMode::none, false},
        [this](const server::HttpRequest& request) { return health(request); });
}

server::HttpResponse HealthController::health(const server::HttpRequest& request) {
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto result = service_.check();
    if (const auto* data = std::get_if<serialization::json::HealthData>(&result)) {
        return server::HttpResponse::json(
            200,
            serialization::json::serializeExact(
                serialization::json::EnvelopeWriter::success(
                    request.requestId, serialization::json::toJsonExact(*data))));
    }
    constexpr auto code = protocol::ErrorCode::serviceUnavailable;
    return server::HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(
                request.requestId, code, "服务暂不可用")),
        std::string(protocol::toString(code)));
}

}  // namespace ocrservice::http::controllers::health
