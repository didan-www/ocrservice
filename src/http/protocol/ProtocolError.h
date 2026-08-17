#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace ocrservice::http::protocol {

enum class ErrorCode {
    invalidRequest,
    imageInvalid,
    authInvalidCredentials,
    authTokenInvalid,
    authTokenExpired,
    deviceUnauthorized,
    userDisabled,
    deviceDisabled,
    deviceForbidden,
    recognitionNotFound,
    imageNotFound,
    accessListNotFound,
    captureIdConflict,
    accessListConflictWhite,
    accessListConflictBlack,
    imageTooLarge,
    imageStorageError,
    internalError,
    recognitionQueueFull,
    databaseUnavailable,
    serviceUnavailable
};

std::string_view toString(ErrorCode code);
int httpStatusFor(ErrorCode code) noexcept;

class ProtocolError final : public std::invalid_argument {
public:
    ProtocolError(ErrorCode code, std::string message);

    ErrorCode code() const noexcept;
    int httpStatus() const noexcept;

private:
    ErrorCode code_;
};

[[noreturn]] void throwInvalidRequest(std::string message);

}  // namespace ocrservice::http::protocol
