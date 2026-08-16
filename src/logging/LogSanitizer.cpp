#include "LogSanitizer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

namespace ocrservice::logging {
namespace {

constexpr std::size_t kMaximumDetailLength = 512;

std::string trimCopy(const std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char byte) {
        return std::isspace(byte) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char byte) {
        return std::isspace(byte) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

bool looksLikeStructuredPayload(const std::string_view value) {
    const auto trimmed = trimCopy(value);
    return trimmed.size() >= 2 &&
           ((trimmed.front() == '{' && trimmed.back() == '}') ||
            (trimmed.front() == '[' && trimmed.back() == ']'));
}

std::string redactSensitiveValues(std::string value) {
    static const std::regex bearer(
        R"((Bearer[[:space:]]+)[^[:space:],;"']+)",
        std::regex_constants::icase);
    static const std::regex keyedSecret(
        R"(((?:"?)(?:[A-Za-z0-9_-]*(?:password|token)[A-Za-z0-9_-]*|authorization)(?:"?)[[:space:]]*[:=][[:space:]]*)(?:"[^"]*"|'[^']*'|[^[:space:],;]+))",
        std::regex_constants::icase);
    value = std::regex_replace(value, bearer, "$1[REDACTED]");
    return std::regex_replace(value, keyedSecret, "$1[REDACTED]");
}

std::string redactAbsolutePaths(std::string value) {
    static const std::regex absolutePath(
        R"((^|[[:space:]=:])((?:/[A-Za-z0-9._-]+){2,}|[A-Za-z]:\\[^[:space:],;]+))");
    return std::regex_replace(value, absolutePath, "$1[REDACTED_PATH]");
}

std::string escapeControlCharacters(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char rawByte : value) {
        const auto byte = static_cast<unsigned char>(rawByte);
        switch (byte) {
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                if (byte < 0x20U || byte == 0x7FU) {
                    char escaped[7] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04X", byte);
                    result += escaped;
                } else {
                    result.push_back(static_cast<char>(byte));
                }
        }
    }
    return result;
}

}  // namespace

std::string LogSanitizer::sanitize(const std::string_view value) {
    if (looksLikeStructuredPayload(value)) {
        return "[REDACTED_STRUCTURED_DATA]";
    }
    auto sanitized = escapeControlCharacters(redactAbsolutePaths(redactSensitiveValues(
        std::string(value))));
    if (sanitized.size() > kMaximumDetailLength) {
        sanitized.resize(kMaximumDetailLength);
        sanitized += "[TRUNCATED]";
    }
    return sanitized;
}

}  // namespace ocrservice::logging
