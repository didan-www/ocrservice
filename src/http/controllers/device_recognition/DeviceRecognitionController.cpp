#include "DeviceRecognitionController.h"

#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "DeviceRecognitionResponses.h"
#include "ProtocolError.h"
#include "ProtocolTime.h"
#include "StrictMultipart.h"

namespace ocrservice::http::controllers::device_recognition {
namespace {

using namespace services::recognition::acceptance;

std::string_view bearerToken(const server::HttpRequest& request) noexcept {
    return request.bearerToken ? std::string_view(*request.bearerToken) : std::string_view{};
}

domain::DeviceId pathDeviceId(const server::HttpRequest& request) {
    const auto found = request.pathParameters.find("deviceId");
    if (found == request.pathParameters.end()) {
        throw std::logic_error("device recognition route parameter is missing");
    }
    try {
        return domain::DeviceId::parse(found->second);
    } catch (const domain::DomainError&) {
        protocol::throwInvalidRequest("deviceId 格式非法");
    }
}

domain::CaptureId captureId(const std::string_view value) {
    try {
        return domain::CaptureId::parse(value);
    } catch (const domain::DomainError&) {
        protocol::throwInvalidRequest("captureId 格式非法");
    }
}

}  // namespace

DeviceRecognitionController::DeviceRecognitionController(
    IRecognitionAcceptanceService& acceptance,
    IUploadImageValidator& images)
    : acceptance_(acceptance), images_(images) {}

void DeviceRecognitionController::registerRoutes(server::IRouteRegistrar& registrar) {
    registrar.registerRoute(
        server::HttpMethod::post,
        "/api/v1/devices/{deviceId}/recognitions",
        {middleware::RequestBodyMode::multipart, true},
        [this](const server::HttpRequest& request) { return upload(request); });
}

server::HttpResponse DeviceRecognitionController::upload(const server::HttpRequest& request) {
    auto authentication = acceptance_.authenticateToken(bearerToken(request));
    if (const auto* failure = std::get_if<AcceptanceFailure>(&authentication)) {
        return failureResponse(request.requestId, *failure);
    }

    const auto deviceId = pathDeviceId(request);
    auto authorization = acceptance_.authorizeDevice(
        std::get<DevicePrincipal>(authentication), deviceId);
    if (const auto* failure = std::get_if<AcceptanceFailure>(&authorization)) {
        return failureResponse(request.requestId, *failure);
    }
    if (!request.rawQuery.empty()) {
        protocol::throwInvalidRequest("设备上传接口不接受 query");
    }
    if (!request.multipartBoundary) {
        protocol::throwInvalidRequest("multipart boundary 缺失");
    }

    const auto parts = decodeUploadMultipart(request.body, *request.multipartBoundary);
    auto decodedCaptureId = captureId(parts.captureId);
    domain::UtcTimePoint capturedAtUtc(0);
    try {
        capturedAtUtc = serialization::time::parseProtocolTime(parts.capturedAt);
    } catch (const serialization::time::TimeError&) {
        protocol::throwInvalidRequest("capturedAt 格式非法");
    }

    auto image = images_.validate(parts.image);
    if (const auto* failure = std::get_if<AcceptanceFailure>(&image)) {
        return failureResponse(request.requestId, *failure);
    }
    auto result = acceptance_.acceptUpload(
        std::get<AuthorizedDevice>(authorization),
        decodedCaptureId,
        capturedAtUtc,
        std::get<ValidatedUploadImage>(image),
        request.requestId);
    if (const auto* failure = std::get_if<AcceptanceFailure>(&result)) {
        return failureResponse(request.requestId, *failure);
    }
    return successResponse(request.requestId, std::get<AcceptanceSucceeded>(result));
}

}  // namespace ocrservice::http::controllers::device_recognition
