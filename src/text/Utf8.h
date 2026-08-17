#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ocrservice::text {

class Utf8Error final : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

std::vector<std::int32_t> decodeUtf8(std::string_view input);
std::size_t countCodePoints(std::string_view input);
bool isUnicodeWhitespace(std::int32_t codePoint) noexcept;

// Plate-like normalization is shared by non-empty plate numbers and
// optionally empty access-list keywords.
std::string normalizePlateLike(
    std::string_view input,
    bool allowEmpty,
    std::size_t maximumCodePoints = 16U);

}  // namespace ocrservice::text
