#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "DeviceConfig.h"
#include "DeviceResult.h"

namespace {

using ocrservice::tests::embedded::DeviceResult;
using ocrservice::tests::embedded::GateAction;
using ocrservice::tests::embedded::ResultDeduplicator;
using Json = nlohmann::json;

class SummaryFile final {
public:
    explicit SummaryFile(const std::string& contents) {
        static std::size_t sequence = 0U;
        path_ = std::filesystem::temp_directory_path() /
                ("ocrservice-embedded-summary-" + std::to_string(++sequence) + ".json");
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << contents;
        output.close();
        std::filesystem::permissions(
            path_,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);
    }

    ~SummaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

Json validSummary() {
    return {
        {"deviceId", "device-task-022"},
        {"http",
         {{"baseUrl", "http://127.0.0.1:8080"},
          {"bearerToken", "http-token-visible"},
          {"uploadPath", "/api/v1/devices/device-task-022/recognitions"}}},
        {"mqtt",
         {{"host", "127.0.0.1"},
          {"port", 1883},
          {"tls", false},
          {"username", "mqtt-device-task-022"},
          {"password", "mqtt-password-visible"},
          {"clientId", "device-task-022"},
          {"cleanSession", false},
          {"qos", 1},
          {"resultTopic", "plate/devices/device-task-022/recognition-results"}}}};
}

Json successfulResult() {
    return {
        {"schemaVersion", 1},
        {"recognitionId", "a15c7268-b211-4a4c-a765-49b922af1910"},
        {"revision", 2},
        {"deviceId", "device-task-022"},
        {"status", "SUCCEEDED"},
        {"plateNumber", "ABC123"},
        {"errorCode", nullptr},
        {"errorMessage", nullptr},
        {"capturedAt", "2026-08-25T10:00:00.000+08:00"},
        {"startedAt", "2026-08-25T10:00:00.100+08:00"},
        {"completedAt", "2026-08-25T10:00:00.200+08:00"},
        {"durationMs", 100},
        {"gateAction", "OPEN"}};
}

TEST(DeviceConfigTest, ReadsExactProvisioningSummaryWithoutServerConfiguration) {
    SummaryFile file(validSummary().dump());
    const auto config = ocrservice::tests::embedded::loadDeviceConfig(file.path());
    EXPECT_EQ(config.deviceId, "device-task-022");
    EXPECT_EQ(config.http.uploadPath, "/api/v1/devices/device-task-022/recognitions");
    EXPECT_EQ(config.mqtt.clientId, config.deviceId);
    EXPECT_FALSE(config.mqtt.cleanSession);
    EXPECT_EQ(config.mqtt.qos, 1);
}

TEST(DeviceConfigTest, RejectsUnknownDuplicateAndMisboundFields) {
    auto unknown = validSummary();
    unknown["serverConfig"] = "/app/config/server.json";
    SummaryFile unknownFile(unknown.dump());
    EXPECT_THROW(
        ocrservice::tests::embedded::loadDeviceConfig(unknownFile.path()),
        std::runtime_error);

    auto misbound = validSummary();
    misbound["mqtt"]["resultTopic"] = "plate/devices/other/recognition-results";
    SummaryFile misboundFile(misbound.dump());
    EXPECT_THROW(
        ocrservice::tests::embedded::loadDeviceConfig(misboundFile.path()),
        std::runtime_error);

    const auto duplicate = validSummary().dump();
    SummaryFile duplicateFile(
        duplicate.substr(0U, duplicate.size() - 1U) + ",\"deviceId\":\"other\"}");
    EXPECT_THROW(
        ocrservice::tests::embedded::loadDeviceConfig(duplicateFile.path()),
        std::runtime_error);
}

TEST(DeviceConfigTest, RequiresExact0600AndFixedMqttPolicy) {
    SummaryFile file(validSummary().dump());
    std::filesystem::permissions(
        file.path(), std::filesystem::perms::owner_read,
        std::filesystem::perm_options::replace);
    EXPECT_THROW(
        ocrservice::tests::embedded::loadDeviceConfig(file.path()),
        std::runtime_error);

    auto wrongPolicy = validSummary();
    wrongPolicy["mqtt"]["cleanSession"] = true;
    SummaryFile policyFile(wrongPolicy.dump());
    EXPECT_THROW(
        ocrservice::tests::embedded::loadDeviceConfig(policyFile.path()),
        std::runtime_error);
}

TEST(DeviceConfigTest, ValidationErrorsNeverContainSecrets) {
    auto invalid = validSummary();
    invalid["mqtt"]["qos"] = 0;
    SummaryFile file(invalid.dump());
    try {
        (void)ocrservice::tests::embedded::loadDeviceConfig(file.path());
        FAIL() << "invalid summary was accepted";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_EQ(message.find("http-token-visible"), std::string::npos);
        EXPECT_EQ(message.find("mqtt-password-visible"), std::string::npos);
    }
}

TEST(DeviceResultTest, StrictlyMapsFinalStatusToGateAction) {
    const auto success = ocrservice::tests::embedded::parseDeviceResult(
        successfulResult().dump());
    EXPECT_EQ(success.gateAction, GateAction::open);

    auto failureJson = successfulResult();
    failureJson["status"] = "FAILED";
    failureJson["plateNumber"] = nullptr;
    failureJson["errorCode"] = "PLATE_NOT_FOUND";
    failureJson["errorMessage"] = "plate not found";
    failureJson["gateAction"] = "KEEP_CLOSED";
    const auto failure = ocrservice::tests::embedded::parseDeviceResult(failureJson.dump());
    EXPECT_EQ(failure.gateAction, GateAction::keepClosed);
}

TEST(DeviceResultTest, RejectsUnknownFieldsBadCalendarAndMismatchedAction) {
    auto unknown = successfulResult();
    unknown["captureId"] = "not-allowed";
    EXPECT_THROW(
        ocrservice::tests::embedded::parseDeviceResult(unknown.dump()),
        std::runtime_error);

    auto badDate = successfulResult();
    badDate["completedAt"] = "2026-02-30T10:00:00.200+08:00";
    EXPECT_THROW(
        ocrservice::tests::embedded::parseDeviceResult(badDate.dump()),
        std::runtime_error);

    auto wrongAction = successfulResult();
    wrongAction["gateAction"] = "KEEP_CLOSED";
    EXPECT_THROW(
        ocrservice::tests::embedded::parseDeviceResult(wrongAction.dump()),
        std::runtime_error);
}

TEST(DeviceResultTest, DeduplicatesOnlyByRecognitionIdAndRevision) {
    ResultDeduplicator deduplicator;
    DeviceResult first{
        "a15c7268-b211-4a4c-a765-49b922af1910", 2U, "device-task-022",
        "SUCCEEDED", GateAction::open};
    EXPECT_TRUE(deduplicator.shouldExecute(first));
    EXPECT_FALSE(deduplicator.shouldExecute(first));
    first.revision = 3U;
    EXPECT_TRUE(deduplicator.shouldExecute(first));
    EXPECT_EQ(deduplicator.executionCount(), 2U);
}

TEST(DeviceResultTest, AcceptsTheStableModelInferenceErrorCode) {
    auto failureJson = successfulResult();
    failureJson["status"] = "FAILED";
    failureJson["plateNumber"] = nullptr;
    failureJson["errorCode"] = "MODEL_INFERENCE_ERROR";
    failureJson["errorMessage"] = "model inference error";
    failureJson["gateAction"] = "KEEP_CLOSED";
    EXPECT_EQ(
        ocrservice::tests::embedded::parseDeviceResult(failureJson.dump()).gateAction,
        GateAction::keepClosed);
}

}  // namespace
