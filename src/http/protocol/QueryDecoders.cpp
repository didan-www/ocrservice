#include "QueryDecoders.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include "Identifiers.h"
#include "ProtocolError.h"
#include "ProtocolTime.h"
#include "Utf8.h"

namespace ocrservice::http::protocol {
namespace {

int hexValue(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

void requireExactKeys(
    const QueryParameters& parameters,
    const std::initializer_list<std::string_view> required,
    const std::initializer_list<std::string_view> optional = {}) {
    std::set<std::string, std::less<>> allowed;
    for (const auto key : required) {
        allowed.emplace(key);
        if (parameters.find(key) == parameters.end()) {
            throwInvalidRequest("query is missing required parameter " + std::string(key));
        }
    }
    for (const auto key : optional) {
        allowed.emplace(key);
    }
    for (const auto& [key, value] : parameters) {
        (void)value;
        if (allowed.find(key) == allowed.end()) {
            throwInvalidRequest("query contains unknown parameter " + key);
        }
    }
}

serialization::time::UtcTimePoint parseTime(
    const QueryParameters& parameters,
    const std::string_view field) {
    try {
        return serialization::time::parseProtocolTime(parameters.at(std::string(field)));
    } catch (const serialization::time::TimeError&) {
        throwInvalidRequest(std::string(field) + " is not a valid protocol time");
    }
}

domain::HistoryFilter historyFilter(const QueryParameters& parameters) {
    const auto start = parseTime(parameters, "startTime");
    const auto end = parseTime(parameters, "endTime");
    std::optional<domain::DeviceId> device;
    const auto found = parameters.find("deviceId");
    try {
        if (found != parameters.end()) {
            device = domain::DeviceId::parse(found->second);
        }
        return domain::HistoryFilter(start, end, std::move(device));
    } catch (const domain::DomainError&) {
        throwInvalidRequest("history filter is invalid");
    }
}

domain::PageRequest pageRequest(const QueryParameters& parameters) {
    if (parameters.at("pageSize") != "100") {
        throwInvalidRequest("pageSize must be 100");
    }
    try {
        return domain::PageRequest(parsePositiveSafeInteger(parameters.at("page"), "page"));
    } catch (const domain::DomainError&) {
        throwInvalidRequest("page is invalid");
    }
}

domain::AccessListType parseAccessListType(const std::string_view value) {
    if (value == "WHITE") {
        return domain::AccessListType::white;
    }
    if (value == "BLACK") {
        return domain::AccessListType::black;
    }
    throwInvalidRequest("listType must be WHITE or BLACK");
}

}  // namespace

std::string percentDecodeOnce(const std::string_view encoded) {
    std::string result;
    result.reserve(encoded.size());
    for (std::size_t index = 0U; index < encoded.size(); ++index) {
        if (encoded[index] != '%') {
            result.push_back(encoded[index]);
            continue;
        }
        if (index + 2U >= encoded.size()) {
            throwInvalidRequest("query contains a malformed percent escape");
        }
        const int high = hexValue(encoded[index + 1U]);
        const int low = hexValue(encoded[index + 2U]);
        if (high < 0 || low < 0) {
            throwInvalidRequest("query contains a malformed percent escape");
        }
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2U;
    }
    try {
        (void)text::countCodePoints(result);
    } catch (const text::Utf8Error&) {
        throwInvalidRequest("query contains invalid UTF-8");
    }
    return result;
}

QueryParameters decodeQueryParameters(std::string_view rawQuery) {
    if (!rawQuery.empty() && rawQuery.front() == '?') {
        rawQuery.remove_prefix(1U);
    }
    QueryParameters parameters;
    if (rawQuery.empty()) {
        return parameters;
    }
    std::size_t start = 0U;
    while (start <= rawQuery.size()) {
        const auto end = rawQuery.find('&', start);
        const auto pair = rawQuery.substr(start, end - start);
        if (pair.empty()) {
            throwInvalidRequest("query contains an empty parameter");
        }
        const auto equals = pair.find('=');
        if (equals == std::string_view::npos) {
            throwInvalidRequest("query parameter must contain '='");
        }
        auto key = percentDecodeOnce(pair.substr(0U, equals));
        auto value = percentDecodeOnce(pair.substr(equals + 1U));
        if (key.empty() || !parameters.emplace(std::move(key), std::move(value)).second) {
            throwInvalidRequest("query contains an empty or duplicate parameter name");
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return parameters;
}

std::uint64_t parsePositiveSafeInteger(
    const std::string_view value,
    const std::string_view fieldName) {
    if (value.empty()) {
        throwInvalidRequest(std::string(fieldName) + " must be a positive integer");
    }
    std::uint64_t result = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            throwInvalidRequest(std::string(fieldName) + " must be a positive integer");
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (result > (domain::kJsonSafeIntegerMaximum - digit) / 10U) {
            throwInvalidRequest(std::string(fieldName) + " exceeds the JSON safe integer range");
        }
        result = result * 10U + digit;
    }
    if (result == 0U) {
        throwInvalidRequest(std::string(fieldName) + " must be a positive integer");
    }
    return result;
}

HistoryQuery decodeHistoryQuery(const std::string_view rawQuery) {
    const auto parameters = decodeQueryParameters(rawQuery);
    requireExactKeys(parameters, {"startTime", "endTime", "page", "pageSize"}, {"deviceId"});
    return HistoryQuery{historyFilter(parameters), pageRequest(parameters)};
}

domain::HistoryFilter decodeHistoryExportQuery(const std::string_view rawQuery) {
    const auto parameters = decodeQueryParameters(rawQuery);
    requireExactKeys(parameters, {"startTime", "endTime"}, {"deviceId"});
    return historyFilter(parameters);
}

AccessListPageQuery decodeAccessListPageQuery(const std::string_view rawQuery) {
    const auto parameters = decodeQueryParameters(rawQuery);
    requireExactKeys(parameters, {"listType", "keyword", "page", "pageSize"});
    try {
        return AccessListPageQuery{
            domain::AccessListFilter(
                parseAccessListType(parameters.at("listType")),
                domain::PlateKeyword::parse(parameters.at("keyword"))),
            pageRequest(parameters)};
    } catch (const ProtocolError&) {
        throw;
    } catch (const domain::DomainError&) {
        throwInvalidRequest("access-list query is invalid");
    }
}

domain::PlateNumber decodeAccessListLookupQuery(const std::string_view rawQuery) {
    const auto parameters = decodeQueryParameters(rawQuery);
    requireExactKeys(parameters, {"plateNumber"});
    try {
        return domain::PlateNumber::parse(parameters.at("plateNumber"));
    } catch (const domain::DomainError&) {
        throwInvalidRequest("plateNumber is invalid");
    }
}

}  // namespace ocrservice::http::protocol
