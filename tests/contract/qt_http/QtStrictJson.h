#pragma once

// Mirrors the strict field and combination checks in the completed Qt client:
//   src/infrastructure/http/JsonCodecs.cpp
//   src/infrastructure/mqtt/RecognitionEventCodec.cpp
// Keep this dependency-free mirror and tests/fixtures/qt in sync with those files.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "Identifiers.h"
#include "ProtocolTime.h"
#include "Utf8.h"

namespace qt_contract {

using Json = nlohmann::json;

enum class PayloadKind {
    loginEnvelope,
    recognitionEnvelope,
    recognitionPageEnvelope,
    accessListEnvelope,
    accessListPageEnvelope,
    emptyEnvelope,
    failureEnvelope,
    managementEvent,
};

class ParseError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline Json loadFixture(const std::filesystem::path& relativePath) {
    const auto path = std::filesystem::path(OCR_QT_FIXTURE_ROOT) / relativePath;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw ParseError("cannot open Qt fixture: " + path.string());
    }
    try {
        return Json::parse(stream);
    } catch (const Json::exception&) {
        throw ParseError("malformed Qt fixture: " + path.string());
    }
}

inline void fail(const std::string_view path, const std::string_view reason) {
    throw ParseError(std::string(path) + ": " + std::string(reason));
}

inline void requireObject(const Json& value, const std::string_view path) {
    if (!value.is_object()) {
        fail(path, "object required");
    }
}

inline void requireExactKeys(
    const Json& value,
    const std::initializer_list<std::string_view> expected,
    const std::string_view path) {
    requireObject(value, path);
    std::set<std::string, std::less<>> expectedKeys;
    for (const auto key : expected) {
        expectedKeys.emplace(key);
    }
    if (value.size() != expectedKeys.size()) {
        fail(path, "field count differs");
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (expectedKeys.find(key) == expectedKeys.end()) {
            fail(path, "unknown field");
        }
    }
    for (const auto& key : expectedKeys) {
        if (!value.contains(key)) {
            fail(path, "required field missing");
        }
    }
}

inline const std::string& requireString(
    const Json& value,
    const std::string_view path,
    const bool allowEmpty = false,
    const std::optional<std::size_t> maximumCodePoints = std::nullopt) {
    if (!value.is_string()) {
        fail(path, "string required");
    }
    const auto& text = value.get_ref<const std::string&>();
    std::size_t length = 0U;
    try {
        length = ocrservice::text::countCodePoints(text);
    } catch (const ocrservice::text::Utf8Error&) {
        fail(path, "valid UTF-8 required");
    }
    if ((!allowEmpty && length == 0U) ||
        (maximumCodePoints.has_value() && length > *maximumCodePoints)) {
        fail(path, "string length out of range");
    }
    return text;
}

inline std::int64_t requireInteger(
    const Json& value,
    const std::string_view path,
    const std::int64_t minimum,
    const std::int64_t maximum) {
    constexpr std::int64_t safe = 9007199254740991LL;
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(safe) ||
            number < static_cast<std::uint64_t>(minimum) ||
            number > static_cast<std::uint64_t>(maximum)) {
            fail(path, "integer out of range");
        }
        return static_cast<std::int64_t>(number);
    }
    if (!value.is_number_integer()) {
        fail(path, "integer required");
    }
    const auto number = value.get<std::int64_t>();
    if (number < minimum || number > maximum || number < -safe || number > safe) {
        fail(path, "integer out of range");
    }
    return number;
}

inline ocrservice::serialization::time::UtcTimePoint requireTime(
    const Json& value,
    const std::string_view path) {
    const auto& text = requireString(value, path);
    static const std::regex exact(
        R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\+08:00$)",
        std::regex::ECMAScript);
    if (!std::regex_match(text, exact)) {
        fail(path, "exact +08:00 millisecond time required");
    }
    try {
        return ocrservice::serialization::time::parseProtocolTime(text);
    } catch (const ocrservice::serialization::time::TimeError&) {
        fail(path, "valid protocol time required");
    }
    throw ParseError("unreachable time parser state");
}

inline void requireUuid(const Json& value, const std::string_view path) {
    try {
        (void)ocrservice::domain::Uuid::parse(requireString(value, path));
    } catch (const ocrservice::domain::DomainError&) {
        fail(path, "canonical UUID required");
    }
}

inline void requireRecognitionId(const Json& value, const std::string_view path) {
    try {
        (void)ocrservice::domain::RecognitionId::parse(requireString(value, path));
    } catch (const ocrservice::domain::DomainError&) {
        fail(path, "canonical recognition UUID required");
    }
}

inline void requireDeviceId(const Json& value, const std::string_view path) {
    try {
        (void)ocrservice::domain::DeviceId::parse(requireString(value, path));
    } catch (const ocrservice::domain::DomainError&) {
        fail(path, "device ID required");
    }
}

inline void parseRecognition(const Json& value, const std::string_view path) {
    constexpr std::int64_t safe = 9007199254740991LL;
    requireExactKeys(
        value,
        {"schemaVersion", "recognitionId", "revision", "deviceId", "status",
         "plateNumber", "errorCode", "errorMessage", "capturedAt", "startedAt",
         "completedAt", "durationMs"},
        path);
    (void)requireInteger(value.at("schemaVersion"), "$.schemaVersion", 1, 1);
    requireRecognitionId(value.at("recognitionId"), "$.recognitionId");
    (void)requireInteger(value.at("revision"), "$.revision", 1, safe);
    requireDeviceId(value.at("deviceId"), "$.deviceId");
    const auto& status = requireString(value.at("status"), "$.status");
    (void)requireTime(value.at("capturedAt"), "$.capturedAt");
    const auto started = requireTime(value.at("startedAt"), "$.startedAt");

    const bool plateNull = value.at("plateNumber").is_null();
    const bool errorCodeNull = value.at("errorCode").is_null();
    const bool errorMessageNull = value.at("errorMessage").is_null();
    const bool completedNull = value.at("completedAt").is_null();
    const bool durationNull = value.at("durationMs").is_null();

    if (status == "PROCESSING") {
        if (!plateNull || !errorCodeNull || !errorMessageNull || !completedNull || !durationNull) {
            fail(path, "PROCESSING null combination required");
        }
        return;
    }

    if (completedNull || durationNull) {
        fail(path, "final time and duration required");
    }
    const auto completed = requireTime(value.at("completedAt"), "$.completedAt");
    if (completed < started) {
        fail(path, "completedAt precedes startedAt");
    }
    (void)requireInteger(value.at("durationMs"), "$.durationMs", 0, safe);

    if (status == "SUCCEEDED") {
        if (plateNull || !errorCodeNull || !errorMessageNull) {
            fail(path, "SUCCEEDED field combination required");
        }
        try {
            (void)ocrservice::domain::PlateNumber::parse(
                requireString(value.at("plateNumber"), "$.plateNumber", false, 16U));
        } catch (const ocrservice::domain::DomainError&) {
            fail(path, "normalized plate required");
        }
        return;
    }

    if (status == "FAILED") {
        if (!plateNull || errorCodeNull || errorMessageNull) {
            fail(path, "FAILED field combination required");
        }
        (void)requireString(value.at("errorCode"), "$.errorCode", false, 64U);
        (void)requireString(value.at("errorMessage"), "$.errorMessage", false, 512U);
        return;
    }
    fail(path, "unknown recognition status");
}

inline void parseAccessListRecord(const Json& value, const std::string_view path) {
    constexpr std::int64_t safe = 9007199254740991LL;
    requireExactKeys(
        value,
        {"id", "listType", "plateNumber", "remark", "createdBy", "createdAt"},
        path);
    (void)requireInteger(value.at("id"), "$.id", 1, safe);
    const auto& listType = requireString(value.at("listType"), "$.listType");
    if (listType != "WHITE" && listType != "BLACK") {
        fail(path, "unknown list type");
    }
    try {
        (void)ocrservice::domain::PlateNumber::parse(
            requireString(value.at("plateNumber"), "$.plateNumber", false, 16U));
    } catch (const ocrservice::domain::DomainError&) {
        fail(path, "normalized plate required");
    }
    (void)requireString(value.at("remark"), "$.remark", true, 200U);
    (void)requireString(value.at("createdBy"), "$.createdBy", false, 64U);
    (void)requireTime(value.at("createdAt"), "$.createdAt");
}

template <typename ItemParser>
inline void parsePage(
    const Json& value,
    const std::string_view path,
    ItemParser parseItem) {
    constexpr std::int64_t safe = 9007199254740991LL;
    requireExactKeys(value, {"items", "page", "pageSize", "total"}, path);
    if (!value.at("items").is_array()) {
        fail(path, "items array required");
    }
    if (value.at("items").size() > 100U) {
        fail(path, "too many page items");
    }
    for (const auto& item : value.at("items")) {
        parseItem(item, "$.data.items[]");
    }
    (void)requireInteger(value.at("page"), "$.data.page", 1, 2147483647LL);
    (void)requireInteger(value.at("pageSize"), "$.data.pageSize", 100, 100);
    const auto total = requireInteger(value.at("total"), "$.data.total", 0, safe);
    if (value.at("items").size() > static_cast<std::size_t>(total)) {
        fail(path, "item count exceeds total");
    }
}

inline void parseLogin(const Json& value, const std::string_view path) {
    requireExactKeys(value, {"displayName", "accessToken", "expiresAt", "mqtt"}, path);
    (void)requireString(value.at("displayName"), "$.data.displayName", false, 64U);
    (void)requireString(value.at("accessToken"), "$.data.accessToken");
    const auto& expiresText = requireString(value.at("expiresAt"), "$.data.expiresAt");
    (void)requireTime(value.at("expiresAt"), "$.data.expiresAt");
    const auto& mqtt = value.at("mqtt");
    requireExactKeys(
        mqtt,
        {"host", "port", "tls", "username", "password", "expiresAt", "eventTopic"},
        "$.data.mqtt");
    (void)requireString(mqtt.at("host"), "$.data.mqtt.host");
    (void)requireInteger(mqtt.at("port"), "$.data.mqtt.port", 1, 65535);
    if (!mqtt.at("tls").is_boolean() || mqtt.at("tls").get<bool>()) {
        fail("$.data.mqtt.tls", "false required");
    }
    (void)requireString(mqtt.at("username"), "$.data.mqtt.username");
    (void)requireString(mqtt.at("password"), "$.data.mqtt.password");
    (void)requireTime(mqtt.at("expiresAt"), "$.data.mqtt.expiresAt");
    if (requireString(mqtt.at("expiresAt"), "$.data.mqtt.expiresAt") != expiresText) {
        fail("$.data.mqtt.expiresAt", "login expiry values differ");
    }
    if (requireString(mqtt.at("eventTopic"), "$.data.mqtt.eventTopic") !=
        "plate/management/recognition-events") {
        fail("$.data.mqtt.eventTopic", "management topic required");
    }
}

inline const Json& parseEnvelope(const Json& value, const PayloadKind kind) {
    requireExactKeys(value, {"success", "code", "message", "requestId", "data"}, "$");
    if (!value.at("success").is_boolean()) {
        fail("$.success", "boolean required");
    }
    const bool success = value.at("success").get<bool>();
    const auto& code = requireString(value.at("code"), "$.code");
    (void)requireString(value.at("message"), "$.message", true);
    requireUuid(value.at("requestId"), "$.requestId");
    if (!success) {
        if (code == "OK" || !value.at("data").is_null()) {
            fail("$", "failure envelope combination required");
        }
        if (kind != PayloadKind::failureEnvelope) {
            fail("$", "unexpected failure envelope");
        }
        return value.at("data");
    }
    if (code != "OK" || kind == PayloadKind::failureEnvelope) {
        fail("$", "success envelope combination required");
    }
    return value.at("data");
}

inline void parse(const Json& value, const PayloadKind kind) {
    if (kind == PayloadKind::managementEvent) {
        parseRecognition(value, "$");
        return;
    }
    const auto& data = parseEnvelope(value, kind);
    switch (kind) {
        case PayloadKind::loginEnvelope:
            parseLogin(data, "$.data");
            return;
        case PayloadKind::recognitionEnvelope:
            parseRecognition(data, "$.data");
            return;
        case PayloadKind::recognitionPageEnvelope:
            parsePage(data, "$.data", parseRecognition);
            return;
        case PayloadKind::accessListEnvelope:
            parseAccessListRecord(data, "$.data");
            return;
        case PayloadKind::accessListPageEnvelope:
            parsePage(data, "$.data", parseAccessListRecord);
            return;
        case PayloadKind::emptyEnvelope:
            if (!data.is_null()) {
                fail("$.data", "null required");
            }
            return;
        case PayloadKind::failureEnvelope:
            return;
        case PayloadKind::managementEvent:
            break;
    }
    throw ParseError("unknown Qt payload kind");
}

inline void parseText(const std::string_view text, const PayloadKind kind) {
    try {
        parse(Json::parse(text), kind);
    } catch (const Json::exception&) {
        throw ParseError("malformed JSON");
    }
}

}  // namespace qt_contract
