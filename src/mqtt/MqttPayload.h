#pragma once

#include <string>

#include "Recognition.h"

namespace ocrservice::mqtt {

inline constexpr const char* kManagementTopic =
    "plate/management/recognition-events";

struct SerializedMqttMessage final {
    std::string topic;
    std::string payload;
};

SerializedMqttMessage makeManagementMessage(
    const domain::RecognitionSnapshot& snapshot);
SerializedMqttMessage makeDeviceFinalMessage(
    const domain::RecognitionSnapshot& snapshot,
    domain::GateAction gateAction);

}  // namespace ocrservice::mqtt
