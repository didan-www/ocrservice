#include "DeviceRecognitionResponses.h"

#include <string>

#include "ExactJson.h"
#include "ProtocolError.h"

namespace ocrservice::http::controllers::device_recognition {
namespace {

using services::recognition::acceptance::AcceptanceFailure;

protocol::ErrorCode errorCode(const AcceptanceFailure failure) noexcept {
    switch (failure) {
        case AcceptanceFailure::deviceUnauthorized:
            return protocol::ErrorCode::deviceUnauthorized;
        case AcceptanceFailure::deviceForbidden:
            return protocol::ErrorCode::deviceForbidden;
        case AcceptanceFailure::deviceDisabled:
            return protocol::ErrorCode::deviceDisabled;
        case AcceptanceFailure::captureIdConflict:
            return protocol::ErrorCode::captureIdConflict;
        case AcceptanceFailure::imageTooLarge:
            return protocol::ErrorCode::imageTooLarge;
        case AcceptanceFailure::imageInvalid:
            return protocol::ErrorCode::imageInvalid;
        case AcceptanceFailure::recognitionQueueFull:
            return protocol::ErrorCode::recognitionQueueFull;
        case AcceptanceFailure::databaseUnavailable:
            return protocol::ErrorCode::databaseUnavailable;
        case AcceptanceFailure::imageStorageError:
            return protocol::ErrorCode::imageStorageError;
        case AcceptanceFailure::serviceUnavailable:
            return protocol::ErrorCode::serviceUnavailable;
        case AcceptanceFailure::internal:
            return protocol::ErrorCode::internalError;
    }
    return protocol::ErrorCode::internalError;
}

std::string message(const protocol::ErrorCode code) {
    switch (code) {
        case protocol::ErrorCode::deviceUnauthorized:
            return "设备认证失败";
        case protocol::ErrorCode::deviceForbidden:
            return "设备无权访问该路径";
        case protocol::ErrorCode::deviceDisabled:
            return "设备已禁用";
        case protocol::ErrorCode::captureIdConflict:
            return "captureId 与已有上传冲突";
        case protocol::ErrorCode::imageTooLarge:
            return "图片超过大小限制";
        case protocol::ErrorCode::imageInvalid:
            return "图片格式或内容非法";
        case protocol::ErrorCode::recognitionQueueFull:
            return "识别队列已满";
        case protocol::ErrorCode::databaseUnavailable:
            return "数据库暂不可用";
        case protocol::ErrorCode::imageStorageError:
            return "图片保存失败";
        case protocol::ErrorCode::serviceUnavailable:
            return "服务正在停止";
        default:
            return "服务器内部错误";
    }
}

}  // namespace

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    const AcceptanceFailure failure) {
    const auto code = errorCode(failure);
    return server::HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(requestId, code, message(code))),
        std::string(protocol::toString(code)));
}

server::HttpResponse successResponse(
    const domain::Uuid& requestId,
    const services::recognition::acceptance::AcceptanceSucceeded& result) {
    const serialization::json::DeviceAcceptanceData data{
        result.recognitionId, result.captureId, result.status};
    const int status = result.status == domain::RecognitionStatus::processing ? 202 : 200;
    return server::HttpResponse::json(
        status,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(
                requestId, serialization::json::toJsonExact(data))));
}

}  // namespace ocrservice::http::controllers::device_recognition
