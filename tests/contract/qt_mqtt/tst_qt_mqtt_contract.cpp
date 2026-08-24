#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "MqttPayload.h"
#include "ProtocolTime.h"
#include "QtStrictJson.h"

namespace ocrservice {
namespace {

domain::UtcTimePoint at(const std::string_view value) {
    return serialization::time::parseProtocolTime(value);
}

domain::RecognitionSnapshot snapshot(const domain::RecognitionStatus status) {
    const auto captured = at("2026-08-15T12:30:44.000+08:00");
    const auto started = at("2026-08-15T12:30:45.000+08:00");
    if (status == domain::RecognitionStatus::processing) {
        return domain::RecognitionSnapshot(
            domain::RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910"),
            1U,
            domain::DeviceId::parse("device-001"),
            status,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            captured,
            started,
            std::nullopt,
            std::nullopt);
    }
    if (status == domain::RecognitionStatus::succeeded) {
        return domain::RecognitionSnapshot(
            domain::RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910"),
            2U,
            domain::DeviceId::parse("device-001"),
            status,
            domain::PlateNumber::parse("京A12345"),
            std::nullopt,
            std::nullopt,
            captured,
            started,
            at("2026-08-15T12:30:45.120+08:00"),
            120U);
    }
    return domain::RecognitionSnapshot(
        domain::RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910"),
        2U,
        domain::DeviceId::parse("device-001"),
        status,
        std::nullopt,
        domain::RecognitionFailureCode::modelInferenceError,
        std::string("模型推理失败"),
        captured,
        started,
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

TEST(QtMqttFixtureContractTest, AcceptsLegalFixtureAndRejectsManagementGateAction) {
    EXPECT_NO_THROW(qt_contract::parse(
        qt_contract::loadFixture("mqtt/legal/processing-event.json"),
        qt_contract::PayloadKind::managementEvent));
    EXPECT_THROW(
        qt_contract::parse(
            qt_contract::loadFixture("mqtt/invalid/gate-action-event.json"),
            qt_contract::PayloadKind::managementEvent),
        qt_contract::ParseError);
}

TEST(QtMqttContractTest, RealManagementMessagesForAllStatesPassQtStrictParser) {
    const std::vector<domain::RecognitionStatus> states = {
        domain::RecognitionStatus::processing,
        domain::RecognitionStatus::succeeded,
        domain::RecognitionStatus::failed};
    for (const auto state : states) {
        const auto message = mqtt::makeManagementMessage(snapshot(state));
        EXPECT_EQ(message.topic, mqtt::kManagementTopic);
        EXPECT_NO_THROW(qt_contract::parseText(
            message.payload, qt_contract::PayloadKind::managementEvent));
        const auto document = qt_contract::Json::parse(message.payload);
        EXPECT_FALSE(document.contains("gateAction"));
        EXPECT_EQ(document.size(), 12U);
    }
}

TEST(QtMqttContractTest, DeviceGateActionCannotBeAcceptedAsManagementPayload) {
    const auto message = mqtt::makeDeviceFinalMessage(
        snapshot(domain::RecognitionStatus::succeeded), domain::GateAction::open);
    const auto document = qt_contract::Json::parse(message.payload);
    ASSERT_TRUE(document.contains("gateAction"));
    EXPECT_THROW(
        qt_contract::parse(document, qt_contract::PayloadKind::managementEvent),
        qt_contract::ParseError);
}

}  // namespace
}  // namespace ocrservice
