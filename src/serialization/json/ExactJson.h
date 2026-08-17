#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "Identifiers.h"
#include "Ports.h"
#include "ProtocolError.h"
#include "Recognition.h"

namespace ocrservice::serialization::json {

using Json = nlohmann::json;

struct MqttLoginData final {
    std::string host;
    std::uint16_t port;
    bool tls;
    std::string username;
    std::string password;
    domain::UtcTimePoint expiresAt;
    std::string eventTopic;
};

struct LoginData final {
    std::string displayName;
    std::string accessToken;
    domain::UtcTimePoint expiresAt;
    MqttLoginData mqtt;
};

struct DeviceAcceptanceData final {
    domain::RecognitionId recognitionId;
    domain::CaptureId captureId;
    domain::RecognitionStatus status;
};

enum class HealthStatus { up, degraded };
enum class ComponentStatus { up, down };

struct HealthData final {
    HealthStatus status;
    ComponentStatus model;
    ComponentStatus mysql;
    ComponentStatus mqtt;
    std::uint64_t queueDepth;
    std::uint64_t queueCapacity;
};

Json toJsonExact(const LoginData& value);
Json toJsonExact(const domain::RecognitionSnapshot& value);
Json toJsonExact(const domain::RecognitionRecord& value);
Json toJsonExact(const domain::PageResult<domain::RecognitionSnapshot>& value);
Json toJsonExact(const domain::PageResult<domain::RecognitionRecord>& value);
Json toJsonExact(const domain::AccessListRecord& value);
Json toJsonExact(const domain::PageResult<domain::AccessListRecord>& value);
Json toJsonExact(const DeviceAcceptanceData& value);
Json toJsonExact(const HealthData& value);

// MQTT payload roots deliberately bypass the HTTP envelope.
Json managementRecognitionEvent(const domain::RecognitionSnapshot& value);
Json deviceRecognitionResult(const domain::RecognitionSnapshot& value);

class EnvelopeWriter final {
public:
    static Json success(
        const domain::Uuid& requestId,
        Json data = nullptr,
        std::string message = {});
    static Json failure(
        const domain::Uuid& requestId,
        http::protocol::ErrorCode code,
        std::string message);

private:
    EnvelopeWriter() = delete;
};

std::string serializeExact(const Json& document);

}  // namespace ocrservice::serialization::json
