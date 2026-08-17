#include "MqttPayload.h"

#include <stdexcept>

#include "ExactJson.h"

namespace ocrservice::mqtt {

SerializedMqttMessage makeManagementMessage(
    const domain::RecognitionSnapshot& snapshot) {
    return {
        kManagementTopic,
        serialization::json::serializeExact(
            serialization::json::managementRecognitionEvent(snapshot)),
    };
}

SerializedMqttMessage makeDeviceFinalMessage(
    const domain::RecognitionSnapshot& snapshot,
    const domain::GateAction gateAction) {
    const auto expected = domain::gateActionFor(snapshot.status());
    if (!expected.has_value() || *expected != gateAction) {
        throw std::invalid_argument(
            "device MQTT result requires a matching final gate action");
    }
    return {
        "plate/devices/" + snapshot.deviceId().value() + "/recognition-results",
        serialization::json::serializeExact(
            serialization::json::deviceRecognitionResult(snapshot)),
    };
}

}  // namespace ocrservice::mqtt
