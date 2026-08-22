#include "RecognitionResponses.h"

#include <string>

#include "ExactJson.h"
#include "ProtocolError.h"

namespace ocrservice::http::controllers::recognition {
namespace {

protocol::ErrorCode errorCode(const services::history::HistoryFailure failure) noexcept {
    switch (failure) {
        case services::history::HistoryFailure::recognitionNotFound:
            return protocol::ErrorCode::recognitionNotFound;
        case services::history::HistoryFailure::imageNotFound:
            return protocol::ErrorCode::imageNotFound;
        case services::history::HistoryFailure::databaseUnavailable:
            return protocol::ErrorCode::databaseUnavailable;
        case services::history::HistoryFailure::internal:
            return protocol::ErrorCode::internalError;
    }
    return protocol::ErrorCode::internalError;
}

protocol::ErrorCode errorCode(const services::csv::CsvFailure failure) noexcept {
    return failure == services::csv::CsvFailure::databaseUnavailable ?
               protocol::ErrorCode::databaseUnavailable : protocol::ErrorCode::internalError;
}

std::string message(const protocol::ErrorCode code) {
    switch (code) {
        case protocol::ErrorCode::recognitionNotFound:
            return "识别记录不存在";
        case protocol::ErrorCode::imageNotFound:
            return "识别图片不存在";
        case protocol::ErrorCode::databaseUnavailable:
            return "数据库暂不可用";
        default:
            return "服务器内部错误";
    }
}

server::HttpResponse response(
    const domain::Uuid& requestId,
    const protocol::ErrorCode code) {
    return server::HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(requestId, code, message(code))),
        std::string(protocol::toString(code)));
}

}  // namespace

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    const services::history::HistoryFailure failure) {
    return response(requestId, errorCode(failure));
}

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    const services::csv::CsvFailure failure) {
    return response(requestId, errorCode(failure));
}

}  // namespace ocrservice::http::controllers::recognition
