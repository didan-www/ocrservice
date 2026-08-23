#include "StrictMultipart.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "ImageValidator.h"
#include "ProtocolError.h"

namespace ocrservice::http::controllers::device_recognition {
namespace {

constexpr std::size_t kMaximumPartHeaderBytes = 8U * 1024U;

[[noreturn]] void invalidMultipart() {
    throw protocol::ProtocolError(
        protocol::ErrorCode::invalidRequest, "multipart 请求格式非法");
}

[[noreturn]] void imageTooLarge() {
    throw protocol::ProtocolError(
        protocol::ErrorCode::imageTooLarge, "图片超过 10 MiB");
}

bool isTokenCharacter(const unsigned char character) noexcept {
    if (std::isalnum(character) != 0) {
        return true;
    }
    constexpr std::string_view kPunctuation = "!#$%&'*+-.^_`|~";
    return kPunctuation.find(static_cast<char>(character)) != std::string_view::npos;
}

std::string lowercaseAscii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::string_view trimOws(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return value;
}

class ParameterParser final {
public:
    explicit ParameterParser(const std::string_view input) : input_(input) {}

    std::string token() {
        const auto start = position_;
        while (position_ < input_.size() &&
               isTokenCharacter(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start) {
            invalidMultipart();
        }
        return std::string(input_.substr(start, position_ - start));
    }

    std::string value() {
        if (position_ >= input_.size()) {
            invalidMultipart();
        }
        if (input_[position_] != '"') {
            return token();
        }
        ++position_;
        std::string result;
        while (position_ < input_.size()) {
            const auto character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                return result;
            }
            if (character == '\\') {
                if (position_ >= input_.size() ||
                    (input_[position_] != '"' && input_[position_] != '\\')) {
                    invalidMultipart();
                }
                result.push_back(input_[position_++]);
                continue;
            }
            if (character < 0x20U || character > 0x7EU) {
                invalidMultipart();
            }
            result.push_back(static_cast<char>(character));
        }
        invalidMultipart();
    }

    void skipOws() noexcept {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    bool consume(const char expected) noexcept {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool atEnd() const noexcept { return position_ == input_.size(); }

private:
    std::string_view input_;
    std::size_t position_ = 0U;
};

using Parameters = std::map<std::string, std::string, std::less<>>;

Parameters parseParameters(ParameterParser& parser) {
    Parameters result;
    while (!parser.atEnd()) {
        parser.skipOws();
        if (!parser.consume(';')) {
            invalidMultipart();
        }
        parser.skipOws();
        auto name = lowercaseAscii(parser.token());
        parser.skipOws();
        if (!parser.consume('=')) {
            invalidMultipart();
        }
        parser.skipOws();
        auto value = parser.value();
        parser.skipOws();
        if (!result.emplace(std::move(name), std::move(value)).second) {
            invalidMultipart();
        }
    }
    return result;
}

struct Disposition final {
    std::string name;
    bool hasFilename = false;
};

Disposition parseDisposition(const std::string_view value) {
    ParameterParser parser(value);
    if (lowercaseAscii(parser.token()) != "form-data") {
        invalidMultipart();
    }
    auto parameters = parseParameters(parser);
    for (const auto& parameter : parameters) {
        if (parameter.first != "name" && parameter.first != "filename") {
            invalidMultipart();
        }
    }
    const auto name = parameters.find("name");
    if (name == parameters.end() || name->second.empty()) {
        invalidMultipart();
    }
    return Disposition{name->second, parameters.find("filename") != parameters.end()};
}

void validateContentType(
    const std::string_view value,
    const bool textPart) {
    ParameterParser parser(value);
    const auto type = lowercaseAscii(parser.token());
    if (!parser.consume('/')) {
        invalidMultipart();
    }
    const auto subtype = lowercaseAscii(parser.token());
    auto parameters = parseParameters(parser);
    if (!textPart) {
        if (!parameters.empty()) {
            invalidMultipart();
        }
        return;
    }
    if (type != "text" || subtype != "plain" || parameters.size() > 1U) {
        invalidMultipart();
    }
    if (!parameters.empty()) {
        const auto charset = parameters.find("charset");
        if (charset == parameters.end() || lowercaseAscii(charset->second) != "utf-8") {
            invalidMultipart();
        }
    }
}

struct PartHeaders final {
    Disposition disposition;
    std::optional<std::string_view> contentType;
};

PartHeaders parseHeaders(const std::string_view block) {
    std::optional<std::string_view> disposition;
    std::optional<std::string_view> contentType;
    std::size_t start = 0U;
    while (start < block.size()) {
        const auto end = block.find("\r\n", start);
        const auto lineEnd = end == std::string_view::npos ? block.size() : end;
        const auto line = block.substr(start, lineEnd - start);
        if (line.empty() || line.find('\r') != std::string_view::npos ||
            line.find('\n') != std::string_view::npos) {
            invalidMultipart();
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            invalidMultipart();
        }
        const auto rawName = line.substr(0U, colon);
        for (const char rawCharacter : rawName) {
            if (!isTokenCharacter(static_cast<unsigned char>(rawCharacter))) {
                invalidMultipart();
            }
        }
        const auto name = lowercaseAscii(rawName);
        const auto value = trimOws(line.substr(colon + 1U));
        if (value.empty()) {
            invalidMultipart();
        }
        if (name == "content-disposition") {
            if (disposition) {
                invalidMultipart();
            }
            disposition = value;
        } else if (name == "content-type") {
            if (contentType) {
                invalidMultipart();
            }
            contentType = value;
        } else {
            invalidMultipart();
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 2U;
    }
    if (!disposition) {
        invalidMultipart();
    }
    return PartHeaders{parseDisposition(*disposition), contentType};
}

struct Delimiter final {
    std::size_t position;
    bool closing;
    std::size_t next;
};

Delimiter findDelimiter(
    const std::string_view body,
    const std::string_view marker,
    const std::size_t from) {
    auto position = body.find(marker, from);
    while (position != std::string_view::npos) {
        const auto suffix = position + marker.size();
        if (body.substr(suffix, 2U) == "\r\n") {
            return Delimiter{position, false, suffix + 2U};
        }
        if (body.substr(suffix, 2U) == "--") {
            const auto tail = body.substr(suffix + 2U);
            if (tail.empty() || tail == "\r\n") {
                return Delimiter{position, true, body.size()};
            }
        }
        position = body.find(marker, position + 1U);
    }
    invalidMultipart();
}

}  // namespace

UploadParts decodeUploadMultipart(
    const std::string_view body,
    const std::string_view boundary) {
    if (boundary.empty()) {
        invalidMultipart();
    }
    const std::string initial = "--" + std::string(boundary) + "\r\n";
    const std::string marker = "\r\n--" + std::string(boundary);
    if (body.size() < initial.size() || body.substr(0U, initial.size()) != initial) {
        invalidMultipart();
    }

    UploadParts result{};
    bool hasImage = false;
    bool hasCaptureId = false;
    bool hasCapturedAt = false;
    std::size_t position = initial.size();
    for (;;) {
        const auto headerEnd = body.find("\r\n\r\n", position);
        if (headerEnd == std::string_view::npos || headerEnd - position > kMaximumPartHeaderBytes) {
            invalidMultipart();
        }
        const auto headers = parseHeaders(body.substr(position, headerEnd - position));
        const auto dataStart = headerEnd + 4U;
        const auto delimiter = findDelimiter(body, marker, dataStart);
        const auto value = body.substr(dataStart, delimiter.position - dataStart);

        if (headers.disposition.name == "image") {
            if (hasImage) {
                invalidMultipart();
            }
            hasImage = true;
            if (value.empty()) {
                invalidMultipart();
            }
            if (value.size() > storage::kMaximumCompressedImageBytes) {
                imageTooLarge();
            }
            if (headers.contentType) {
                validateContentType(*headers.contentType, false);
            }
            result.image = value;
        } else if (headers.disposition.name == "captureId") {
            if (hasCaptureId || headers.disposition.hasFilename) {
                invalidMultipart();
            }
            hasCaptureId = true;
            if (headers.contentType) {
                validateContentType(*headers.contentType, true);
            }
            result.captureId = value;
        } else if (headers.disposition.name == "capturedAt") {
            if (hasCapturedAt || headers.disposition.hasFilename) {
                invalidMultipart();
            }
            hasCapturedAt = true;
            if (headers.contentType) {
                validateContentType(*headers.contentType, true);
            }
            result.capturedAt = value;
        } else {
            invalidMultipart();
        }

        if (delimiter.closing) {
            if (!hasImage || !hasCaptureId || !hasCapturedAt || result.captureId.empty() ||
                result.capturedAt.empty()) {
                invalidMultipart();
            }
            return result;
        }
        position = delimiter.next;
    }
}

}  // namespace ocrservice::http::controllers::device_recognition
