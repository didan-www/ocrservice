#include "AuthResponses.h"

#include <string>

#include "ExactJson.h"
#include "ProtocolError.h"

namespace ocrservice::http::controllers::auth {
namespace {

protocol::ErrorCode errorCode(const services::auth::AuthFailure failure) noexcept {
    switch (failure) {
        case services::auth::AuthFailure::invalidCredentials:
            return protocol::ErrorCode::authInvalidCredentials;
        case services::auth::AuthFailure::userDisabled:
            return protocol::ErrorCode::userDisabled;
        case services::auth::AuthFailure::tokenInvalid:
            return protocol::ErrorCode::authTokenInvalid;
        case services::auth::AuthFailure::tokenExpired:
            return protocol::ErrorCode::authTokenExpired;
        case services::auth::AuthFailure::databaseUnavailable:
            return protocol::ErrorCode::databaseUnavailable;
        case services::auth::AuthFailure::internal:
            return protocol::ErrorCode::internalError;
    }
    return protocol::ErrorCode::internalError;
}

std::string message(const protocol::ErrorCode code) {
    switch (code) {
        case protocol::ErrorCode::authInvalidCredentials:
            return "用户名或密码错误";
        case protocol::ErrorCode::userDisabled:
            return "用户已禁用";
        case protocol::ErrorCode::authTokenInvalid:
            return "登录状态无效";
        case protocol::ErrorCode::authTokenExpired:
            return "登录状态已过期";
        case protocol::ErrorCode::databaseUnavailable:
            return "数据库暂不可用";
        default:
            return "服务器内部错误";
    }
}

}  // namespace

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    const services::auth::AuthFailure failure) {
    const auto code = errorCode(failure);
    return server::HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(requestId, code, message(code))),
        std::string(protocol::toString(code)));
}

server::HttpResponse emptySuccessResponse(const domain::Uuid& requestId) {
    return server::HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(requestId)));
}

}  // namespace ocrservice::http::controllers::auth
