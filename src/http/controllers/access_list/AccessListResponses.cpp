#include "AccessListResponses.h"

#include <string>
#include <utility>

#include "ExactJson.h"
#include "ProtocolError.h"

namespace ocrservice::http::controllers::access_list {
namespace {

protocol::ErrorCode errorCode(
    const services::access_list::AccessListFailure failure) noexcept {
    switch (failure) {
        case services::access_list::AccessListFailure::notFound:
            return protocol::ErrorCode::accessListNotFound;
        case services::access_list::AccessListFailure::conflictWhite:
            return protocol::ErrorCode::accessListConflictWhite;
        case services::access_list::AccessListFailure::conflictBlack:
            return protocol::ErrorCode::accessListConflictBlack;
        case services::access_list::AccessListFailure::databaseUnavailable:
            return protocol::ErrorCode::databaseUnavailable;
        case services::access_list::AccessListFailure::internal:
            return protocol::ErrorCode::internalError;
    }
    return protocol::ErrorCode::internalError;
}

std::string message(const protocol::ErrorCode code) {
    switch (code) {
        case protocol::ErrorCode::accessListNotFound:
            return "名单记录不存在";
        case protocol::ErrorCode::accessListConflictWhite:
            return "车牌已在白名单";
        case protocol::ErrorCode::accessListConflictBlack:
            return "车牌已在黑名单";
        case protocol::ErrorCode::databaseUnavailable:
            return "数据库暂不可用";
        default:
            return "服务器内部错误";
    }
}

server::HttpResponse success(
    const domain::Uuid& requestId,
    serialization::json::Json data = nullptr) {
    return server::HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(requestId, std::move(data))));
}

}  // namespace

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    const services::access_list::AccessListFailure failure) {
    const auto code = errorCode(failure);
    return server::HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(requestId, code, message(code))),
        std::string(protocol::toString(code)));
}

server::HttpResponse recordSuccessResponse(
    const domain::Uuid& requestId,
    const domain::AccessListRecord& record) {
    return success(requestId, serialization::json::toJsonExact(record));
}

server::HttpResponse pageSuccessResponse(
    const domain::Uuid& requestId,
    const domain::PageResult<domain::AccessListRecord>& page) {
    return success(requestId, serialization::json::toJsonExact(page));
}

server::HttpResponse emptySuccessResponse(const domain::Uuid& requestId) {
    return success(requestId);
}

}  // namespace ocrservice::http::controllers::access_list
