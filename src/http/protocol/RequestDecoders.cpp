#include "RequestDecoders.h"

#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "HttpPolicy.h"
#include "ProtocolError.h"
#include "Utf8.h"

namespace ocrservice::http::protocol {
namespace {

using Json = nlohmann::json;

Json parseExactObject(
    const std::string_view body,
    const std::initializer_list<std::string_view> fields) {
    if (!isJsonSizeAllowed(body.size())) {
        throwInvalidRequest("request JSON exceeds 2 MiB");
    }
    bool duplicateKey = false;
    std::vector<std::set<std::string>> objectKeys;
    try {
        const auto callback = [&duplicateKey, &objectKeys](
                                  const int,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (event == Json::parse_event_t::object_start) {
                objectKeys.emplace_back();
            } else if (event == Json::parse_event_t::key && !objectKeys.empty()) {
                const auto key = parsed.get<std::string>();
                if (!objectKeys.back().insert(key).second) {
                    duplicateKey = true;
                }
            } else if (event == Json::parse_event_t::object_end && !objectKeys.empty()) {
                objectKeys.pop_back();
            }
            return true;
        };
        auto document = Json::parse(body.begin(), body.end(), callback, true, false);
        if (duplicateKey) {
            throwInvalidRequest("JSON object contains a duplicate field");
        }
        if (!document.is_object() || document.size() != fields.size()) {
            throwInvalidRequest("JSON root must contain the exact required fields");
        }
        for (const auto field : fields) {
            if (!document.contains(std::string(field))) {
                throwInvalidRequest("JSON root must contain the exact required fields");
            }
        }
        return document;
    } catch (const ProtocolError&) {
        throw;
    } catch (const nlohmann::json::exception&) {
        throwInvalidRequest("request body is not valid UTF-8 JSON");
    }
}

std::string requireString(const Json& document, const char* field, const bool allowEmpty) {
    const auto& value = document.at(field);
    if (!value.is_string()) {
        throwInvalidRequest(std::string(field) + " must be a string");
    }
    auto result = value.get<std::string>();
    try {
        const auto length = text::countCodePoints(result);
        if (!allowEmpty && length == 0U) {
            throwInvalidRequest(std::string(field) + " must not be empty");
        }
    } catch (const text::Utf8Error&) {
        throwInvalidRequest(std::string(field) + " must be valid UTF-8");
    }
    return result;
}

domain::Uuid requireUuid(const Json& document, const char* field) {
    const auto value = requireString(document, field, false);
    try {
        return domain::Uuid::parse(value);
    } catch (const domain::DomainError&) {
        throwInvalidRequest(std::string(field) + " must be a canonical non-nil UUID");
    }
}

domain::AccessListType requireAccessListType(const Json& document) {
    const auto value = requireString(document, "listType", false);
    if (value == "WHITE") {
        return domain::AccessListType::white;
    }
    if (value == "BLACK") {
        return domain::AccessListType::black;
    }
    throwInvalidRequest("listType must be WHITE or BLACK");
}

}  // namespace

LoginRequest decodeLoginRequest(const std::string_view body) {
    const auto document = parseExactObject(body, {"username", "password", "clientId"});
    return LoginRequest{
        requireString(document, "username", false),
        requireString(document, "password", false),
        requireUuid(document, "clientId")};
}

HeartbeatRequest decodeHeartbeatRequest(const std::string_view body) {
    const auto document = parseExactObject(body, {"clientId", "appVersion"});
    return HeartbeatRequest{
        requireUuid(document, "clientId"),
        requireString(document, "appVersion", false)};
}

AccessListCreateRequest decodeAccessListCreateRequest(const std::string_view body) {
    const auto document =
        parseExactObject(body, {"listType", "plateNumber", "remark"});
    const auto plateText = requireString(document, "plateNumber", false);
    auto remark = requireString(document, "remark", true);
    try {
        if (text::countCodePoints(remark) > 200U) {
            throwInvalidRequest("remark must contain at most 200 Unicode code points");
        }
        return AccessListCreateRequest{
            requireAccessListType(document), domain::PlateNumber::parse(plateText), std::move(remark)};
    } catch (const ProtocolError&) {
        throw;
    } catch (const domain::DomainError&) {
        throwInvalidRequest("plateNumber is invalid");
    } catch (const text::Utf8Error&) {
        throwInvalidRequest("remark must be valid UTF-8");
    }
}

void requireEmptyRequestBody(const std::string_view body) {
    if (!body.empty()) {
        throwInvalidRequest("request body must be empty");
    }
}

}  // namespace ocrservice::http::protocol
