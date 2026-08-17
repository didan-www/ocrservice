#include "RequestGuards.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include <crow.h>

#include "HttpPolicy.h"
#include "ProtocolError.h"

namespace ocrservice::http::middleware {
namespace {

std::string lowercaseAscii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::optional<std::string> parseBearerToken(const crow::request& request) {
    const auto range = request.headers.equal_range("Authorization");
    if (range.first == range.second) {
        return std::nullopt;
    }
    auto current = range.first;
    const std::string value = current->second;
    ++current;
    if (current != range.second || value.empty()) {
        protocol::throwInvalidRequest("Authorization header is empty or repeated");
    }
    constexpr std::string_view prefix = "bearer ";
    if (value.size() <= prefix.size() ||
        lowercaseAscii(std::string_view(value).substr(0U, prefix.size())) != prefix) {
        protocol::throwInvalidRequest("Authorization header does not use Bearer syntax");
    }
    const std::string token = value.substr(prefix.size());
    if (std::any_of(token.begin(), token.end(), [](const unsigned char character) {
            return std::isspace(character) != 0 || character == ',';
        })) {
        protocol::throwInvalidRequest("Bearer token contains a forbidden character");
    }
    return token;
}

std::optional<std::string> onlyContentType(const crow::request& request) {
    const auto range = request.headers.equal_range("Content-Type");
    if (range.first == range.second) {
        return std::nullopt;
    }
    auto current = range.first;
    std::string value = current->second;
    ++current;
    if (current != range.second) {
        protocol::throwInvalidRequest("Content-Type header is repeated");
    }
    return value;
}

}  // namespace

std::size_t receiveBodyLimit(const crow::request& request) noexcept {
    try {
        const auto range = request.headers.equal_range("Content-Type");
        if (range.first == range.second) {
            return protocol::kMaximumJsonBytes;
        }
        auto current = range.first;
        const std::string value = current->second;
        ++current;
        if (current == range.second && protocol::multipartBoundary(value)) {
            return protocol::kMaximumMultipartBytes;
        }
    } catch (...) {
    }
    return protocol::kMaximumJsonBytes;
}

RequestMetadata validateRequest(
    const crow::request& request,
    const RequestBodyMode bodyMode,
    const bool parseBearer) {
    RequestMetadata metadata;
    switch (bodyMode) {
        case RequestBodyMode::none:
            if (!request.body.empty()) {
                protocol::throwInvalidRequest("request body must be empty");
            }
            break;
        case RequestBodyMode::json: {
            const auto contentType = onlyContentType(request);
            if (!contentType || !protocol::isJsonContentType(*contentType)) {
                protocol::throwInvalidRequest("Content-Type must be application/json");
            }
            break;
        }
        case RequestBodyMode::optionalEmptyJson: {
            const auto contentType = onlyContentType(request);
            if (!request.body.empty()) {
                protocol::throwInvalidRequest("request body must be empty");
            }
            if (contentType && !protocol::isJsonContentType(*contentType)) {
                protocol::throwInvalidRequest("Content-Type must be application/json when present");
            }
            break;
        }
        case RequestBodyMode::multipart: {
            const auto contentType = onlyContentType(request);
            if (!contentType) {
                protocol::throwInvalidRequest("multipart Content-Type is required");
            }
            metadata.multipartBoundary = protocol::multipartBoundary(*contentType);
            if (!metadata.multipartBoundary) {
                protocol::throwInvalidRequest("multipart boundary is invalid");
            }
            break;
        }
    }
    if (parseBearer) {
        metadata.bearerToken = parseBearerToken(request);
    }
    return metadata;
}

}  // namespace ocrservice::http::middleware
