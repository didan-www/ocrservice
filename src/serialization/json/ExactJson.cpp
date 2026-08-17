#include "ExactJson.h"

#include <string_view>
#include <utility>

#include "HttpPolicy.h"
#include "ProtocolTime.h"
#include "Utf8.h"

namespace ocrservice::serialization::json {
namespace {

void requireUtf8(
    const std::string_view value,
    const std::string_view field,
    const bool allowEmpty = false) {
    try {
        const auto length = text::countCodePoints(value);
        if (!allowEmpty && length == 0U) {
            throw std::invalid_argument(std::string(field) + " must not be empty");
        }
    } catch (const text::Utf8Error&) {
        throw std::invalid_argument(std::string(field) + " must be valid UTF-8");
    }
}

std::string_view toString(const HealthStatus status) {
    switch (status) {
        case HealthStatus::up:
            return "UP";
        case HealthStatus::degraded:
            return "DEGRADED";
    }
    throw std::invalid_argument("invalid health status");
}

std::string_view toString(const ComponentStatus status) {
    switch (status) {
        case ComponentStatus::up:
            return "UP";
        case ComponentStatus::down:
            return "DOWN";
    }
    throw std::invalid_argument("invalid component status");
}

template <typename T, typename Serializer>
Json pageToJson(const domain::PageResult<T>& page, Serializer serializer) {
    Json items = Json::array();
    for (const auto& item : page.items()) {
        items.push_back(serializer(item));
    }
    return Json{
        {"items", std::move(items)},
        {"page", page.request().page()},
        {"pageSize", domain::PageRequest::pageSize()},
        {"total", page.total()}};
}

void requireSafeJsonNumbers(const Json& value) {
    if (value.is_number_unsigned()) {
        if (!domain::isJsonSafeUnsigned(value.get<std::uint64_t>())) {
            throw std::invalid_argument("public JSON contains an unsafe unsigned integer");
        }
        return;
    }
    if (value.is_number_integer()) {
        if (!domain::isJsonSafeInteger(value.get<std::int64_t>())) {
            throw std::invalid_argument("public JSON contains an unsafe signed integer");
        }
        return;
    }
    if (value.is_number_float()) {
        throw std::invalid_argument("public JSON contract does not contain floating-point fields");
    }
    if (value.is_array() || value.is_object()) {
        for (const auto& child : value) {
            requireSafeJsonNumbers(child);
        }
    }
}

}  // namespace

Json toJsonExact(const LoginData& value) {
    requireUtf8(value.displayName, "displayName");
    requireUtf8(value.accessToken, "accessToken");
    requireUtf8(value.mqtt.host, "mqtt.host");
    requireUtf8(value.mqtt.username, "mqtt.username");
    requireUtf8(value.mqtt.password, "mqtt.password");
    requireUtf8(value.mqtt.eventTopic, "mqtt.eventTopic");
    if (value.mqtt.port == 0U || value.mqtt.tls ||
        value.mqtt.eventTopic != "plate/management/recognition-events" ||
        value.expiresAt != value.mqtt.expiresAt) {
        throw std::invalid_argument("login MQTT data violates the public contract");
    }
    const auto expiresAt = time::formatProtocolTime(value.expiresAt);
    return Json{
        {"displayName", value.displayName},
        {"accessToken", value.accessToken},
        {"expiresAt", expiresAt},
        {"mqtt",
         Json{
             {"host", value.mqtt.host},
             {"port", value.mqtt.port},
             {"tls", false},
             {"username", value.mqtt.username},
             {"password", value.mqtt.password},
             {"expiresAt", expiresAt},
             {"eventTopic", value.mqtt.eventTopic}}}};
}

Json toJsonExact(const domain::RecognitionSnapshot& value) {
    Json result{
        {"schemaVersion", domain::RecognitionSnapshot::schemaVersion()},
        {"recognitionId", value.recognitionId().toString()},
        {"revision", value.revision()},
        {"deviceId", value.deviceId().value()},
        {"status", domain::toString(value.status())},
        {"plateNumber", nullptr},
        {"errorCode", nullptr},
        {"errorMessage", nullptr},
        {"capturedAt", time::formatProtocolTime(value.capturedAt())},
        {"startedAt", time::formatProtocolTime(value.startedAt())},
        {"completedAt", nullptr},
        {"durationMs", nullptr}};
    if (value.plateNumber().has_value()) {
        result["plateNumber"] = value.plateNumber()->value();
    }
    if (value.errorCode().has_value()) {
        result["errorCode"] = domain::toString(*value.errorCode());
    }
    if (value.errorMessage().has_value()) {
        result["errorMessage"] = *value.errorMessage();
    }
    if (value.completedAt().has_value()) {
        result["completedAt"] = time::formatProtocolTime(*value.completedAt());
    }
    if (value.durationMs().has_value()) {
        result["durationMs"] = *value.durationMs();
    }
    return result;
}

Json toJsonExact(const domain::RecognitionRecord& value) {
    return toJsonExact(value.snapshot());
}

Json toJsonExact(const domain::PageResult<domain::RecognitionSnapshot>& value) {
    return pageToJson(value, [](const auto& item) { return toJsonExact(item); });
}

Json toJsonExact(const domain::PageResult<domain::RecognitionRecord>& value) {
    return pageToJson(value, [](const auto& item) { return toJsonExact(item); });
}

Json toJsonExact(const domain::AccessListRecord& value) {
    return Json{
        {"id", value.id()},
        {"listType", domain::toString(value.listType())},
        {"plateNumber", value.plateNumber().value()},
        {"remark", value.remark()},
        {"createdBy", value.createdBy()},
        {"createdAt", time::formatProtocolTime(value.createdAt())}};
}

Json toJsonExact(const domain::PageResult<domain::AccessListRecord>& value) {
    return pageToJson(value, [](const auto& item) { return toJsonExact(item); });
}

Json toJsonExact(const DeviceAcceptanceData& value) {
    return Json{
        {"recognitionId", value.recognitionId.toString()},
        {"captureId", value.captureId.toString()},
        {"status", domain::toString(value.status)}};
}

Json toJsonExact(const HealthData& value) {
    domain::requireNonNegativeJsonSafe(value.queueDepth, "queueDepth");
    domain::requirePositiveJsonSafe(value.queueCapacity, "queueCapacity");
    if (value.queueDepth > value.queueCapacity || value.model != ComponentStatus::up ||
        value.mysql != ComponentStatus::up ||
        (value.status == HealthStatus::up && value.mqtt != ComponentStatus::up) ||
        (value.status == HealthStatus::degraded && value.mqtt != ComponentStatus::down)) {
        throw std::invalid_argument("health data contains an invalid public state combination");
    }
    return Json{
        {"status", toString(value.status)},
        {"model", toString(value.model)},
        {"mysql", toString(value.mysql)},
        {"mqtt", toString(value.mqtt)},
        {"queueDepth", value.queueDepth},
        {"queueCapacity", value.queueCapacity}};
}

Json managementRecognitionEvent(const domain::RecognitionSnapshot& value) {
    return toJsonExact(value);
}

Json deviceRecognitionResult(const domain::RecognitionSnapshot& value) {
    const auto action = domain::gateActionFor(value.status());
    if (!action.has_value()) {
        throw std::invalid_argument("device result requires a final recognition state");
    }
    auto result = toJsonExact(value);
    result["gateAction"] = domain::toString(*action);
    return result;
}

Json EnvelopeWriter::success(
    const domain::Uuid& requestId,
    Json data,
    std::string message) {
    requireUtf8(message, "message", true);
    if (!data.is_null() && !data.is_object()) {
        throw std::invalid_argument("success envelope data must be an object or null");
    }
    return Json{
        {"success", true},
        {"code", "OK"},
        {"message", std::move(message)},
        {"requestId", requestId.toString()},
        {"data", std::move(data)}};
}

Json EnvelopeWriter::failure(
    const domain::Uuid& requestId,
    const http::protocol::ErrorCode code,
    std::string message) {
    requireUtf8(message, "message", true);
    return Json{
        {"success", false},
        {"code", http::protocol::toString(code)},
        {"message", std::move(message)},
        {"requestId", requestId.toString()},
        {"data", nullptr}};
}

std::string serializeExact(const Json& document) {
    if (!document.is_object()) {
        throw std::invalid_argument("public JSON root must be an object");
    }
    requireSafeJsonNumbers(document);
    std::string body;
    try {
        body = document.dump();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("public JSON contains invalid UTF-8");
    }
    http::protocol::requireJsonSizeAllowed(body.size());
    return body;
}

}  // namespace ocrservice::serialization::json
