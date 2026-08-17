#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "MqttPayload.h"
#include "ProtocolTime.h"
#include "Recognition.h"

namespace {

using ocrservice::domain::DeviceId;
using ocrservice::domain::GateAction;
using ocrservice::domain::PlateNumber;
using ocrservice::domain::RecognitionFailureCode;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionSnapshot;
using ocrservice::domain::RecognitionStatus;
using ocrservice::serialization::time::parseProtocolTime;

RecognitionId recognitionId() {
    return RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910");
}

DeviceId deviceId() { return DeviceId::parse("device-001"); }

auto at(const std::string& value) { return parseProtocolTime(value); }

RecognitionSnapshot processing() {
    return RecognitionSnapshot(
        recognitionId(),
        1U,
        deviceId(),
        RecognitionStatus::processing,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        std::nullopt,
        std::nullopt);
}

RecognitionSnapshot succeeded() {
    return RecognitionSnapshot(
        recognitionId(),
        2U,
        deviceId(),
        RecognitionStatus::succeeded,
        PlateNumber::parse(u8"京A12345"),
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

RecognitionSnapshot failed() {
    return RecognitionSnapshot(
        recognitionId(),
        2U,
        deviceId(),
        RecognitionStatus::failed,
        std::nullopt,
        RecognitionFailureCode::plateNotFound,
        std::string(u8"未检测到车牌"),
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

std::set<std::string> keys(const nlohmann::json& value) {
    std::set<std::string> result;
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        result.insert(key);
    }
    return result;
}

const std::set<std::string> snapshotKeys = {
    "schemaVersion", "recognitionId", "revision",   "deviceId",
    "status",        "plateNumber",   "errorCode",  "errorMessage",
    "capturedAt",    "startedAt",     "completedAt", "durationMs",
};

TEST(MqttPayloadTest, ManagementPayloadIsTheExactSnapshotRootForEveryState) {
    for (const auto& snapshot : {processing(), succeeded(), failed()}) {
        const auto message = ocrservice::mqtt::makeManagementMessage(snapshot);
        EXPECT_EQ(message.topic, ocrservice::mqtt::kManagementTopic);
        const auto document = nlohmann::json::parse(message.payload);
        EXPECT_EQ(keys(document), snapshotKeys);
        EXPECT_FALSE(document.contains("gateAction"));
        EXPECT_FALSE(document.contains("success"));
        EXPECT_FALSE(document.contains("captureId"));
    }
}

TEST(MqttPayloadTest, DevicePayloadAddsOnlyTheDerivedGateAction) {
    const auto success = ocrservice::mqtt::makeDeviceFinalMessage(
        succeeded(), GateAction::open);
    const auto failure = ocrservice::mqtt::makeDeviceFinalMessage(
        failed(), GateAction::keepClosed);

    EXPECT_EQ(success.topic, "plate/devices/device-001/recognition-results");
    auto expectedKeys = snapshotKeys;
    expectedKeys.insert("gateAction");
    const auto successJson = nlohmann::json::parse(success.payload);
    const auto failureJson = nlohmann::json::parse(failure.payload);
    EXPECT_EQ(keys(successJson), expectedKeys);
    EXPECT_EQ(keys(failureJson), expectedKeys);
    EXPECT_EQ(successJson.at("gateAction"), "OPEN");
    EXPECT_EQ(failureJson.at("gateAction"), "KEEP_CLOSED");
    EXPECT_EQ(successJson.at("plateNumber"), u8"京A12345");
    EXPECT_EQ(failureJson.at("plateNumber"), nullptr);
}

TEST(MqttPayloadTest, RejectsProcessingAndMismatchedActionsBeforeTransport) {
    EXPECT_THROW(
        ocrservice::mqtt::makeDeviceFinalMessage(processing(), GateAction::keepClosed),
        std::invalid_argument);
    EXPECT_THROW(
        ocrservice::mqtt::makeDeviceFinalMessage(succeeded(), GateAction::keepClosed),
        std::invalid_argument);
    EXPECT_THROW(
        ocrservice::mqtt::makeDeviceFinalMessage(failed(), GateAction::open),
        std::invalid_argument);
}

}  // namespace
