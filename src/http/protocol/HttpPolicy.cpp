#include "HttpPolicy.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace ocrservice::http::protocol {
namespace {

std::string_view trimAscii(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

std::string lowercaseAscii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::vector<std::string_view> splitParameters(const std::string_view value) {
    std::vector<std::string_view> result;
    std::size_t start = 0U;
    while (start <= value.size()) {
        const auto end = value.find(';', start);
        result.push_back(trimAscii(value.substr(start, end - start)));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return result;
}

}  // namespace

std::string_view contentTypeFor(const ResponseBodyKind kind) {
    switch (kind) {
        case ResponseBodyKind::json:
            return "application/json";
        case ResponseBodyKind::jpeg:
            return "image/jpeg";
        case ResponseBodyKind::png:
            return "image/png";
        case ResponseBodyKind::csv:
            return "text/csv; charset=utf-8";
    }
    throw HttpPolicyError("invalid response body kind");
}

bool isJsonContentType(const std::string_view contentType) {
    const auto parts = splitParameters(contentType);
    if (parts.empty() || lowercaseAscii(parts.front()) != "application/json" || parts.size() > 2U) {
        return false;
    }
    return parts.size() == 1U || lowercaseAscii(parts[1]) == "charset=utf-8";
}

std::optional<std::string> multipartBoundary(const std::string_view contentType) {
    const auto parts = splitParameters(contentType);
    if (parts.size() != 2U || lowercaseAscii(parts.front()) != "multipart/form-data") {
        return std::nullopt;
    }
    const auto parameter = parts[1];
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos ||
        lowercaseAscii(trimAscii(parameter.substr(0U, equals))) != "boundary") {
        return std::nullopt;
    }
    auto value = trimAscii(parameter.substr(equals + 1U));
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        value = value.substr(1U, value.size() - 2U);
    }
    if (value.empty() || value.size() > 70U) {
        return std::nullopt;
    }
    for (const char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (character <= 0x20U || character >= 0x7FU || character == '"' || character == ';') {
            return std::nullopt;
        }
    }
    return std::string(value);
}

bool isAllowedImageContentType(const std::string_view contentType) noexcept {
    return contentType == "image/jpeg" || contentType == "image/png";
}

bool isRedirectStatus(const int status) noexcept {
    return status >= 300 && status <= 399;
}

bool isJsonSizeAllowed(const std::size_t bytes) noexcept {
    return bytes <= kMaximumJsonBytes;
}

void requireNonRedirectStatus(const int status) {
    if (isRedirectStatus(status)) {
        throw HttpPolicyError("HTTP redirects are forbidden");
    }
}

void requireJsonSizeAllowed(const std::size_t bytes) {
    if (!isJsonSizeAllowed(bytes)) {
        throw HttpPolicyError("JSON body exceeds 2 MiB");
    }
}

}  // namespace ocrservice::http::protocol
