#include "Utf8.h"

#include <limits>

#include <utf8proc.h>

namespace ocrservice::text {
namespace {

std::string encodeCodePoint(const std::int32_t codePoint) {
    utf8proc_uint8_t encoded[4]{};
    const auto length = utf8proc_encode_char(codePoint, encoded);
    if (length <= 0) {
        throw Utf8Error("failed to encode Unicode code point");
    }
    return std::string(
        reinterpret_cast<const char*>(encoded), static_cast<std::size_t>(length));
}

}  // namespace

std::vector<std::int32_t> decodeUtf8(const std::string_view input) {
    std::vector<std::int32_t> result;
    result.reserve(input.size());

    std::size_t offset = 0U;
    while (offset < input.size()) {
        utf8proc_int32_t codePoint = 0;
        const auto remaining = input.size() - offset;
        if (remaining > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max())) {
            throw Utf8Error("UTF-8 input is too large");
        }
        const auto consumed = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(input.data() + offset),
            static_cast<utf8proc_ssize_t>(remaining),
            &codePoint);
        if (consumed <= 0) {
            throw Utf8Error("invalid UTF-8 input");
        }
        result.push_back(static_cast<std::int32_t>(codePoint));
        offset += static_cast<std::size_t>(consumed);
    }
    return result;
}

std::size_t countCodePoints(const std::string_view input) {
    return decodeUtf8(input).size();
}

bool isUnicodeWhitespace(const std::int32_t codePoint) noexcept {
    if ((codePoint >= 0x0009 && codePoint <= 0x000D) || codePoint == 0x0085) {
        return true;
    }
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codePoint));
    return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
           category == UTF8PROC_CATEGORY_ZP;
}

std::string normalizePlateLike(
    const std::string_view input,
    const bool allowEmpty,
    const std::size_t maximumCodePoints) {
    if (maximumCodePoints == 0U) {
        throw Utf8Error("maximum code point count must be positive");
    }
    const auto codePoints = decodeUtf8(input);
    std::size_t first = 0U;
    while (first < codePoints.size() && isUnicodeWhitespace(codePoints[first])) {
        ++first;
    }
    std::size_t last = codePoints.size();
    while (last > first && isUnicodeWhitespace(codePoints[last - 1U])) {
        --last;
    }

    const auto normalizedLength = last - first;
    if ((!allowEmpty && normalizedLength == 0U) || normalizedLength > maximumCodePoints) {
        throw Utf8Error("plate-like value has invalid length");
    }

    std::string result;
    result.reserve(input.size());
    for (std::size_t index = first; index < last; ++index) {
        auto codePoint = codePoints[index];
        if (isUnicodeWhitespace(codePoint)) {
            throw Utf8Error("plate-like value contains internal whitespace");
        }
        if (codePoint >= 'a' && codePoint <= 'z') {
            codePoint -= ('a' - 'A');
        }
        result += encodeCodePoint(codePoint);
    }
    return result;
}

}  // namespace ocrservice::text
