#include "ProtocolError.h"

#include <utility>

namespace ocrservice::http::protocol {

std::string_view toString(const ErrorCode code) {
    switch (code) {
        case ErrorCode::invalidRequest:
            return "INVALID_REQUEST";
        case ErrorCode::imageInvalid:
            return "IMAGE_INVALID";
        case ErrorCode::authInvalidCredentials:
            return "AUTH_INVALID_CREDENTIALS";
        case ErrorCode::authTokenInvalid:
            return "AUTH_TOKEN_INVALID";
        case ErrorCode::authTokenExpired:
            return "AUTH_TOKEN_EXPIRED";
        case ErrorCode::deviceUnauthorized:
            return "DEVICE_UNAUTHORIZED";
        case ErrorCode::userDisabled:
            return "USER_DISABLED";
        case ErrorCode::deviceDisabled:
            return "DEVICE_DISABLED";
        case ErrorCode::deviceForbidden:
            return "DEVICE_FORBIDDEN";
        case ErrorCode::recognitionNotFound:
            return "RECOGNITION_NOT_FOUND";
        case ErrorCode::imageNotFound:
            return "IMAGE_NOT_FOUND";
        case ErrorCode::accessListNotFound:
            return "ACCESS_LIST_NOT_FOUND";
        case ErrorCode::captureIdConflict:
            return "CAPTURE_ID_CONFLICT";
        case ErrorCode::accessListConflictWhite:
            return "ACCESS_LIST_CONFLICT_WHITE";
        case ErrorCode::accessListConflictBlack:
            return "ACCESS_LIST_CONFLICT_BLACK";
        case ErrorCode::imageTooLarge:
            return "IMAGE_TOO_LARGE";
        case ErrorCode::imageStorageError:
            return "IMAGE_STORAGE_ERROR";
        case ErrorCode::internalError:
            return "INTERNAL_ERROR";
        case ErrorCode::recognitionQueueFull:
            return "RECOGNITION_QUEUE_FULL";
        case ErrorCode::databaseUnavailable:
            return "DATABASE_UNAVAILABLE";
        case ErrorCode::serviceUnavailable:
            return "SERVICE_UNAVAILABLE";
    }
    throw std::invalid_argument("invalid HTTP error code");
}

int httpStatusFor(const ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::invalidRequest:
        case ErrorCode::imageInvalid:
            return 400;
        case ErrorCode::authInvalidCredentials:
        case ErrorCode::authTokenInvalid:
        case ErrorCode::authTokenExpired:
        case ErrorCode::deviceUnauthorized:
            return 401;
        case ErrorCode::userDisabled:
        case ErrorCode::deviceDisabled:
        case ErrorCode::deviceForbidden:
            return 403;
        case ErrorCode::recognitionNotFound:
        case ErrorCode::imageNotFound:
        case ErrorCode::accessListNotFound:
            return 404;
        case ErrorCode::captureIdConflict:
        case ErrorCode::accessListConflictWhite:
        case ErrorCode::accessListConflictBlack:
            return 409;
        case ErrorCode::imageTooLarge:
            return 413;
        case ErrorCode::imageStorageError:
        case ErrorCode::internalError:
            return 500;
        case ErrorCode::recognitionQueueFull:
        case ErrorCode::databaseUnavailable:
        case ErrorCode::serviceUnavailable:
            return 503;
    }
    return 500;
}

ProtocolError::ProtocolError(const ErrorCode code, std::string message)
    : std::invalid_argument(std::move(message)), code_(code) {}

ErrorCode ProtocolError::code() const noexcept {
    return code_;
}

int ProtocolError::httpStatus() const noexcept {
    return httpStatusFor(code_);
}

[[noreturn]] void throwInvalidRequest(std::string message) {
    throw ProtocolError(ErrorCode::invalidRequest, std::move(message));
}

}  // namespace ocrservice::http::protocol
