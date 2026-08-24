#include "HttpUploader.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "Identifiers.h"

#ifndef _WIN32
#include <sys/socket.h>
#endif

namespace ocrservice::tests::embedded {
namespace {

using Json = nlohmann::json;

struct Authority final {
    std::string host;
    std::string port;
    std::string header;
};

Authority parseAuthority(const std::string& baseUrl) {
    constexpr std::string_view prefix = "http://";
    if (baseUrl.compare(0U, prefix.size(), prefix) != 0 || baseUrl.size() <= prefix.size()) {
        throw std::runtime_error("HTTP base URL is invalid");
    }
    const auto authority = baseUrl.substr(prefix.size());
    Authority result;
    result.header = authority;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            throw std::runtime_error("HTTP authority is invalid");
        }
        result.host = authority.substr(1U, close - 1U);
        if (close + 1U < authority.size()) {
            if (authority[close + 1U] != ':' || close + 2U == authority.size()) {
                throw std::runtime_error("HTTP authority is invalid");
            }
            result.port = authority.substr(close + 2U);
        }
    } else {
        const auto firstColon = authority.find(':');
        const auto lastColon = authority.rfind(':');
        if (firstColon != std::string::npos && firstColon == lastColon) {
            result.host = authority.substr(0U, firstColon);
            result.port = authority.substr(firstColon + 1U);
        } else {
            result.host = authority;
        }
    }
    if (result.host.empty()) {
        throw std::runtime_error("HTTP authority is invalid");
    }
    if (result.port.empty()) {
        result.port = "80";
    }
    return result;
}

const char* imageMime(const std::vector<std::uint8_t>& image) {
    static constexpr std::array<std::uint8_t, 8> png = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    if (image.size() >= png.size() && std::equal(png.begin(), png.end(), image.begin())) {
        return "image/png";
    }
    if (image.size() >= 3U && image[0] == 0xffU && image[1] == 0xd8U &&
        image[2] == 0xffU) {
        return "image/jpeg";
    }
    throw std::runtime_error("image must be JPEG or PNG");
}

Json parseStrict(const std::string& value) {
    bool duplicate = false;
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&duplicate, &objectKeys](
                              int depth,
                              Json::parse_event_t event,
                              Json& parsed) {
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            objectKeys[index].clear();
        } else if (event == Json::parse_event_t::key) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            duplicate = !objectKeys[index].insert(parsed.get<std::string>()).second || duplicate;
        }
        return true;
    };
    auto result = Json::parse(value, callback, true, false);
    if (duplicate) {
        throw std::runtime_error("HTTP response contains duplicate fields");
    }
    return result;
}

std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    if (!value.is_object()) {
        return result;
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        result.insert(key);
    }
    return result;
}

std::string lowercase(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::map<std::string, std::string> parseHeaders(
    const std::string& response,
    const std::size_t statusEnd,
    const std::size_t headerEnd) {
    std::map<std::string, std::string> result;
    auto cursor = statusEnd + 2U;
    while (cursor < headerEnd) {
        const auto lineEnd = response.find("\r\n", cursor);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            throw std::runtime_error("HTTP response header is malformed");
        }
        const auto separator = response.find(':', cursor);
        if (separator == std::string::npos || separator >= lineEnd) {
            throw std::runtime_error("HTTP response header is malformed");
        }
        auto name = lowercase(response.substr(cursor, separator - cursor));
        auto valueStart = separator + 1U;
        while (valueStart < lineEnd &&
               (response[valueStart] == ' ' || response[valueStart] == '\t')) {
            ++valueStart;
        }
        auto value = response.substr(valueStart, lineEnd - valueStart);
        if (!result.emplace(std::move(name), std::move(value)).second) {
            throw std::runtime_error("HTTP response contains a duplicate header");
        }
        cursor = lineEnd + 2U;
    }
    return result;
}

#ifndef _WIN32
void setSocketTimeout(asio::ip::tcp::socket& socket, const int timeoutSeconds) {
    const timeval timeout{timeoutSeconds, 0};
    if (::setsockopt(
            socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        ::setsockopt(
            socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        throw std::runtime_error("failed to set HTTP socket timeout");
    }
}
#else
void setSocketTimeout(asio::ip::tcp::socket&, int) {}
#endif

}  // namespace

AcceptedUpload uploadImage(
    const DeviceConfig& config,
    const std::vector<std::uint8_t>& image,
    const std::string& captureId,
    const std::string& capturedAt,
    const int timeoutSeconds) {
    if (image.empty() || image.size() > 10U * 1024U * 1024U || timeoutSeconds < 1) {
        throw std::runtime_error("upload arguments are invalid");
    }
    const auto mime = imageMime(image);
    const auto authority = parseAuthority(config.http.baseUrl);
    const std::string boundary = "ocrservice-" + captureId;

    std::string body;
    body.reserve(image.size() + 1024U);
    const auto field = [&body, &boundary](const char* name, const std::string& value) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name +
                "\"\r\n\r\n" + value + "\r\n";
    };
    field("captureId", captureId);
    field("capturedAt", capturedAt);
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"capture";
    body += std::string(mime) == "image/png" ? ".png\"\r\n" : ".jpg\"\r\n";
    body += "Content-Type: ";
    body += mime;
    body += "\r\n\r\n";
    body.append(reinterpret_cast<const char*>(image.data()), image.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::ostringstream request;
    request << "POST " << config.http.uploadPath << " HTTP/1.1\r\n"
            << "Host: " << authority.header << "\r\n"
            << "Authorization: Bearer " << config.http.bearerToken << "\r\n"
            << "Content-Type: multipart/form-data; boundary=" << boundary << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n";
    auto serialized = request.str();
    serialized += body;

    asio::io_context context;
    asio::ip::tcp::resolver resolver(context);
    const auto endpoints = resolver.resolve(authority.host, authority.port);
    asio::ip::tcp::socket socket(context);
    socket.open(endpoints.begin()->endpoint().protocol());
    setSocketTimeout(socket, timeoutSeconds);
    asio::connect(socket, endpoints);
    asio::write(socket, asio::buffer(serialized));

    std::string response;
    std::error_code error;
    asio::read(socket, asio::dynamic_buffer(response, 2U * 1024U * 1024U + 81920U), error);
    if (error != asio::error::eof) {
        throw std::runtime_error("HTTP response read failed");
    }
    const auto headerEnd = response.find("\r\n\r\n");
    const auto statusEnd = response.find("\r\n");
    if (headerEnd == std::string::npos || statusEnd == std::string::npos) {
        throw std::runtime_error("HTTP response is malformed");
    }
    const std::regex statusPattern("^HTTP/1\\.[01] ([0-9]{3}) .*$");
    std::smatch match;
    const auto statusLine = response.substr(0U, statusEnd);
    if (!std::regex_match(statusLine, match, statusPattern) || match[1].str() != "202") {
        throw std::runtime_error("upload was not accepted with HTTP 202");
    }
    const auto responseBody = response.substr(headerEnd + 4U);
    if (responseBody.size() > 2U * 1024U * 1024U) {
        throw std::runtime_error("HTTP response exceeds 2 MiB");
    }
    const auto headers = parseHeaders(response, statusEnd, headerEnd);
    const auto contentType = headers.find("content-type");
    const auto contentLength = headers.find("content-length");
    if (contentType == headers.end() || contentType->second != "application/json" ||
        contentLength == headers.end() || headers.count("transfer-encoding") != 0U) {
        throw std::runtime_error("HTTP 202 response headers violate the contract");
    }
    std::size_t consumed = 0U;
    std::uint64_t declaredLength = 0U;
    try {
        declaredLength = std::stoull(contentLength->second, &consumed, 10);
    } catch (...) {
        throw std::runtime_error("HTTP Content-Length is invalid");
    }
    if (consumed != contentLength->second.size() || declaredLength != responseBody.size()) {
        throw std::runtime_error("HTTP Content-Length does not match the response body");
    }
    const auto envelope = parseStrict(responseBody);
    if (keys(envelope) !=
            std::set<std::string>{"success", "code", "message", "requestId", "data"} ||
        !envelope.at("success").is_boolean() || !envelope.at("success").get<bool>() ||
        !envelope.at("code").is_string() || envelope.at("code").get<std::string>() != "OK" ||
        !envelope.at("message").is_string() || !envelope.at("requestId").is_string() ||
        keys(envelope.at("data")) !=
            std::set<std::string>{"recognitionId", "captureId", "status"}) {
        throw std::runtime_error("HTTP 202 response violates the exact envelope contract");
    }
    const auto& data = envelope.at("data");
    if (!data.at("recognitionId").is_string() || !data.at("captureId").is_string() ||
        !data.at("status").is_string() || data.at("captureId").get<std::string>() != captureId ||
        data.at("status").get<std::string>() != "PROCESSING") {
        throw std::runtime_error("HTTP 202 response has invalid acceptance data");
    }
    try {
        (void)ocrservice::domain::Uuid::parse(envelope.at("requestId").get<std::string>());
        (void)ocrservice::domain::RecognitionId::parse(
            data.at("recognitionId").get<std::string>());
    } catch (const ocrservice::domain::DomainError&) {
        throw std::runtime_error("HTTP 202 response identifiers are invalid");
    }
    return {data.at("recognitionId").get<std::string>(), captureId};
}

}  // namespace ocrservice::tests::embedded
