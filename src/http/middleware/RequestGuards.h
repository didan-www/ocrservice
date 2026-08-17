#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace crow {
struct request;
}

namespace ocrservice::http::middleware {

enum class RequestBodyMode { none, json, optionalEmptyJson, multipart };

struct RequestMetadata final {
    std::optional<std::string> bearerToken;
    std::optional<std::string> multipartBoundary;
};

std::size_t receiveBodyLimit(const crow::request& request) noexcept;
RequestMetadata validateRequest(
    const crow::request& request,
    RequestBodyMode bodyMode,
    bool parseBearer);

}  // namespace ocrservice::http::middleware
