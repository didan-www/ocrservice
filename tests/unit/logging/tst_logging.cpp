#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "LogSanitizer.h"
#include "LoggerFactory.h"

namespace {

using ocrservice::logging::LogEvent;
using ocrservice::logging::LogLevel;
using ocrservice::logging::LogSanitizer;
using ocrservice::logging::LoggerFactory;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<unsigned long> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ocrservice-logging-" + std::to_string(sequence.fetch_add(1)));
        if (!std::filesystem::create_directories(path_)) {
            throw std::runtime_error("failed to create temporary log directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string readSingleLine(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string line;
    std::getline(input, line);
    EXPECT_FALSE(line.empty());
    std::string unexpectedSecondLine;
    EXPECT_FALSE(static_cast<bool>(std::getline(input, unexpectedSecondLine)));
    return line;
}

TEST(LogSanitizerTest, RedactsSecretsPathsAndControlCharacters) {
    const std::string input =
        "Authorization: Bearer bearer-secret mysqlPassword=mysql-secret "
        "token=token-secret mqtt_password='mqtt-secret' file=/app/data/images/secret.jpg\nnext";
    const auto sanitized = LogSanitizer::sanitize(input);

    EXPECT_EQ(sanitized.find("bearer-secret"), std::string::npos);
    EXPECT_EQ(sanitized.find("mysql-secret"), std::string::npos);
    EXPECT_EQ(sanitized.find("token-secret"), std::string::npos);
    EXPECT_EQ(sanitized.find("mqtt-secret"), std::string::npos);
    EXPECT_EQ(sanitized.find("/app/data/images/secret.jpg"), std::string::npos);
    EXPECT_EQ(sanitized.find('\n'), std::string::npos);
    EXPECT_NE(sanitized.find("\\n"), std::string::npos);
    EXPECT_NE(sanitized.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(sanitized.find("[REDACTED_PATH]"), std::string::npos);
}

TEST(LogSanitizerTest, RejectsCompleteJsonBodiesAndPayloads) {
    EXPECT_EQ(
        LogSanitizer::sanitize(R"({"password":"do-not-log","request":"complete"})"),
        "[REDACTED_STRUCTURED_DATA]");
    EXPECT_EQ(LogSanitizer::sanitize(" [1,2,3] \n"), "[REDACTED_STRUCTURED_DATA]");
}

TEST(LoggerFactoryTest, WritesOneUtf8JsonObjectWithExactWhitelistedFields) {
    TemporaryDirectory directory;
    auto logger = LoggerFactory::createRotatingFile("unit-json-lines", directory.path());

    ocrservice::app::config::ServerConfig config;
    config.mysql.password = "mysql-secret";
    config.mqtt.publicHost = "192.0.2.55";
    config.mqtt.serverPassword = "server-secret";
    config.mqtt.managementPassword = "management-secret";
    LogEvent event{
        "recognition",
        "识别完成",
        "OK",
        "request-uuid",
        "recognition-uuid",
        "device-001",
        125U,
        "Authorization: Bearer bearer-secret path=/app/data/images/private.jpg\nnext",
        ocrservice::app::config::ServerConfigLoader::summarize(config),
    };
    logger->log(LogLevel::info, event);
    logger->flush();

    const auto line = readSingleLine(directory.path() / "ocrservice.jsonl");
    const auto document = nlohmann::json::parse(line);
    const std::set<std::string> expectedKeys = {
        "timestamp",
        "level",
        "module",
        "event",
        "code",
        "requestId",
        "recognitionId",
        "deviceId",
        "durationMs",
        "detail",
        "config",
    };
    std::set<std::string> actualKeys;
    for (const auto& [key, unused] : document.items()) {
        (void)unused;
        actualKeys.insert(key);
    }

    EXPECT_EQ(actualKeys, expectedKeys);
    EXPECT_EQ(document.at("level"), "INFO");
    EXPECT_EQ(document.at("event"), "识别完成");
    EXPECT_EQ(document.at("durationMs"), 125);
    EXPECT_TRUE(document.at("timestamp").get<std::string>().back() == 'Z');
    EXPECT_EQ(line.find("bearer-secret"), std::string::npos);
    EXPECT_EQ(line.find("mysql-secret"), std::string::npos);
    EXPECT_EQ(line.find("server-secret"), std::string::npos);
    EXPECT_EQ(line.find("management-secret"), std::string::npos);
    EXPECT_EQ(line.find("/app/data/images/private.jpg"), std::string::npos);
    EXPECT_EQ(line.find("password"), std::string::npos);
    EXPECT_EQ(document.at("config").size(), 19U);
}

TEST(LoggerFactoryTest, OmitsOptionalFieldsInsteadOfAddingExtensions) {
    TemporaryDirectory directory;
    auto logger = LoggerFactory::createRotatingFile("unit-minimal-json-lines", directory.path());
    logger->log(LogLevel::warning, {"mqtt", "publish_failed", "MQTT_UNAVAILABLE", "request-id"});
    logger->flush();

    const auto document = nlohmann::json::parse(
        readSingleLine(directory.path() / "ocrservice.jsonl"));
    EXPECT_EQ(document.size(), 6U);
    EXPECT_TRUE(document.contains("timestamp"));
    EXPECT_TRUE(document.contains("level"));
    EXPECT_TRUE(document.contains("module"));
    EXPECT_TRUE(document.contains("event"));
    EXPECT_TRUE(document.contains("code"));
    EXPECT_TRUE(document.contains("requestId"));
}

TEST(LoggerFactoryTest, RejectsMissingRequiredFields) {
    TemporaryDirectory directory;
    auto logger = LoggerFactory::createRotatingFile("unit-required-fields", directory.path());

    EXPECT_THROW(
        logger->log(LogLevel::error, {"", "event", "CODE", "request-id"}),
        std::invalid_argument);
    EXPECT_THROW(
        logger->log(LogLevel::error, {"module", "", "CODE", "request-id"}),
        std::invalid_argument);
    EXPECT_THROW(
        logger->log(LogLevel::error, {"module", "event", "", "request-id"}),
        std::invalid_argument);
    EXPECT_THROW(
        logger->log(LogLevel::error, {"module", "event", "CODE", ""}),
        std::invalid_argument);
}

TEST(LoggerFactoryTest, ReplacesInvalidUtf8WithoutBreakingJsonLines) {
    TemporaryDirectory directory;
    auto logger = LoggerFactory::createRotatingFile("unit-invalid-utf8", directory.path());
    std::string invalidUtf8 = "invalid-";
    invalidUtf8.push_back(static_cast<char>(0xFF));
    logger->log(LogLevel::debug, {"module", "event", "CODE", "request-id", {}, {}, {}, invalidUtf8});
    logger->flush();

    EXPECT_NO_THROW({
        const auto document = nlohmann::json::parse(
            readSingleLine(directory.path() / "ocrservice.jsonl"));
        EXPECT_TRUE(document.is_object());
    });
}

}  // namespace
