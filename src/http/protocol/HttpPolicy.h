#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ocrservice::http::protocol {

constexpr std::size_t kMaximumJsonBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumRawUrlBytes = 8U * 1024U;
constexpr std::size_t kMaximumRequestHeadBytes = 80U * 1024U;
constexpr std::size_t kMaximumMultipartBytes = 11U * 1024U * 1024U;
constexpr std::size_t kMaximumUploadImageBytes = 10U * 1024U * 1024U;
constexpr std::size_t kMaximumImageResponseBytes = 20U * 1024U * 1024U;

enum class ResponseBodyKind { json, jpeg, png, csv };

class HttpPolicyError final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

std::string_view contentTypeFor(ResponseBodyKind kind);
bool isJsonContentType(std::string_view contentType);
std::optional<std::string> multipartBoundary(std::string_view contentType);
bool isAllowedImageContentType(std::string_view contentType) noexcept;
bool isRedirectStatus(int status) noexcept;
bool isJsonSizeAllowed(std::size_t bytes) noexcept;
void requireNonRedirectStatus(int status);
void requireJsonSizeAllowed(std::size_t bytes);

}  // namespace ocrservice::http::protocol
