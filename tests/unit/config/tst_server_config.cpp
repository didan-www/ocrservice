#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "ServerConfig.h"

namespace {

using ocrservice::app::config::ConfigError;
using ocrservice::app::config::ServerConfigLoader;

class TemporaryJson final {
public:
    explicit TemporaryJson(const std::string& content) {
        static std::atomic<unsigned long> sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("ocrservice-config-" + std::to_string(sequence.fetch_add(1)) + ".json");
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output << content;
        output.close();
        if (!output) {
            throw std::runtime_error("failed to create temporary configuration");
        }
    }

    ~TemporaryJson() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

ServerConfigLoader::Environment requiredEnvironment() {
    return {
        {"MYSQL_PASSWORD", "mysql-test-password"},
        {"MQTT_SERVER_PASSWORD", "server-test-password"},
        {"MQTT_MANAGEMENT_PASSWORD", "management-test-password"},
        {"MQTT_PUBLIC_HOST", "192.0.2.10"},
    };
}

TEST(ServerConfigLoaderTest, UsesCompiledDefaultsAndRequiredEnvironment) {
    const auto missingFile = std::filesystem::temp_directory_path() /
                             "ocrservice-config-file-that-does-not-exist.json";
    const auto config = ServerConfigLoader::load(missingFile, requiredEnvironment());

    EXPECT_EQ(config.http.port, 8080);
    EXPECT_EQ(config.mysql.host, "mysql");
    EXPECT_EQ(config.mysql.port, 3306);
    EXPECT_EQ(config.mysql.database, "ocrservice");
    EXPECT_EQ(config.mysql.user, "ocrservice");
    EXPECT_EQ(config.mysql.password, "mysql-test-password");
    EXPECT_EQ(config.mqtt.host, "mqtt");
    EXPECT_EQ(config.mqtt.publicHost, "192.0.2.10");
    EXPECT_EQ(config.mqtt.port, 1883);
    EXPECT_FALSE(config.mqtt.tls);
    EXPECT_EQ(config.mqtt.serverUsername, "plate-server");
    EXPECT_EQ(config.mqtt.managementUsername, "management-client");
    EXPECT_EQ(config.storage.imageRoot, "/app/data/images");
    EXPECT_EQ(config.storage.logRoot, "/app/data/logs");
    EXPECT_EQ(config.storage.modelRoot, "/app/models");
    EXPECT_EQ(config.recognition.workers, 1U);
    EXPECT_EQ(config.recognition.queueCapacity, 20U);
    EXPECT_EQ(config.session.tokenTtlSeconds, 28800U);
    EXPECT_DOUBLE_EQ(config.model.yoloConfidence, 0.25);
    EXPECT_DOUBLE_EQ(config.model.yoloNmsIou, 0.45);
}

TEST(ServerConfigLoaderTest, EnvironmentOverridesFlatJsonWhichOverridesDefaults) {
    TemporaryJson file(R"({
        "httpPort": 8000,
        "mysqlHost": "json-mysql",
        "mysqlPort": 3307,
        "mysqlDatabase": "json-db",
        "mysqlUser": "json-user",
        "mqttHost": "json-mqtt",
        "mqttPublicHost": "198.51.100.1",
        "mqttPort": 1884,
        "mqttServerUsername": "json-server-user",
        "mqttManagementUsername": "json-management-user",
        "imageRoot": "/json/images",
        "logRoot": "/json/logs",
        "modelRoot": "/json/models",
        "recognitionWorkers": 2,
        "recognitionQueueCapacity": 30,
        "tokenTtlSeconds": 3600,
        "yoloConfidence": 0.30,
        "yoloNmsIou": 0.50
    })");
    auto environment = requiredEnvironment();
    environment["HTTP_PORT"] = "9000";
    environment["MYSQL_HOST"] = "environment-mysql";
    environment["MQTT_PUBLIC_HOST"] = "203.0.113.2";
    environment["RECOGNITION_QUEUE_CAPACITY"] = "40";
    environment["YOLO_CONFIDENCE"] = "0.75";

    const auto config = ServerConfigLoader::load(file.path(), environment);

    EXPECT_EQ(config.http.port, 9000);
    EXPECT_EQ(config.mysql.host, "environment-mysql");
    EXPECT_EQ(config.mysql.port, 3307);
    EXPECT_EQ(config.mysql.database, "json-db");
    EXPECT_EQ(config.mysql.user, "json-user");
    EXPECT_EQ(config.mqtt.host, "json-mqtt");
    EXPECT_EQ(config.mqtt.publicHost, "203.0.113.2");
    EXPECT_EQ(config.mqtt.serverUsername, "json-server-user");
    EXPECT_EQ(config.storage.imageRoot, "/json/images");
    EXPECT_EQ(config.recognition.workers, 2U);
    EXPECT_EQ(config.recognition.queueCapacity, 40U);
    EXPECT_EQ(config.session.tokenTtlSeconds, 3600U);
    EXPECT_DOUBLE_EQ(config.model.yoloConfidence, 0.75);
    EXPECT_DOUBLE_EQ(config.model.yoloNmsIou, 0.50);
}

TEST(ServerConfigLoaderTest, RejectsUnknownAndSensitiveJsonFields) {
    for (const std::string field : {
             "unknownField",
             "mysqlPassword",
             "mqttServerPassword",
             "mqttManagementPassword",
         }) {
        TemporaryJson file("{\"mqttPublicHost\":\"192.0.2.1\",\"" + field +
                           "\":\"must-not-be-read\"}");
        EXPECT_THROW(ServerConfigLoader::load(file.path(), requiredEnvironment()), ConfigError)
            << field;
    }
}

TEST(ServerConfigLoaderTest, RejectsNonObjectMalformedAndWrongTypes) {
    for (const std::string content : {
             "[]",
             "{not-json",
             "{\"httpPort\": 8080 // comments are not JSON\n}",
             "{\"httpPort\": 8080, /* comments are not JSON */ \"mqttPort\": 1883}",
             "{\"httpPort\": 8080, \"httpPort\": 8081}",
             R"({"httpPort":"8080"})",
             R"({"mysqlHost":42})",
             R"({"recognitionWorkers":1.0})",
             R"({"yoloConfidence":"0.25"})",
         }) {
        TemporaryJson file(content);
        EXPECT_THROW(ServerConfigLoader::load(file.path(), requiredEnvironment()), ConfigError)
            << content;
    }
}

TEST(ServerConfigLoaderTest, EnforcesNumericRanges) {
    for (const std::string content : {
             R"({"httpPort":0})",
             R"({"httpPort":65536})",
             R"({"mysqlPort":-1})",
             R"({"mqttPort":0})",
             R"({"recognitionWorkers":0})",
             R"({"recognitionQueueCapacity":4294967296})",
             R"({"tokenTtlSeconds":0})",
             R"({"yoloConfidence":-0.01})",
             R"({"yoloNmsIou":1.01})",
         }) {
        TemporaryJson file(content);
        EXPECT_THROW(ServerConfigLoader::load(file.path(), requiredEnvironment()), ConfigError)
            << content;
    }

    TemporaryJson boundaryFile(R"({
        "httpPort": 65535,
        "recognitionWorkers": 4294967295,
        "recognitionQueueCapacity": 4294967295,
        "tokenTtlSeconds": 4294967295,
        "yoloConfidence": 0,
        "yoloNmsIou": 1
    })");
    const auto config = ServerConfigLoader::load(boundaryFile.path(), requiredEnvironment());
    EXPECT_EQ(config.http.port, 65535);
    EXPECT_EQ(config.recognition.workers, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(config.recognition.queueCapacity, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(config.session.tokenTtlSeconds, std::numeric_limits<std::uint32_t>::max());
    EXPECT_DOUBLE_EQ(config.model.yoloConfidence, 0.0);
    EXPECT_DOUBLE_EQ(config.model.yoloNmsIou, 1.0);
}

TEST(ServerConfigLoaderTest, RejectsInvalidEnvironmentValues) {
    const auto missingFile = std::filesystem::temp_directory_path() /
                             "ocrservice-config-file-that-does-not-exist.json";
    for (const auto& [name, value] : {
             std::pair{"HTTP_PORT", " 8080"},
             std::pair{"MYSQL_PORT", "3306x"},
             std::pair{"MQTT_PORT", "0"},
             std::pair{"RECOGNITION_WORKERS", "-1"},
             std::pair{"RECOGNITION_QUEUE_CAPACITY", "1.0"},
             std::pair{"TOKEN_TTL_SECONDS", "4294967296"},
             std::pair{"YOLO_CONFIDENCE", "nan"},
             std::pair{"YOLO_NMS_IOU", "1.1"},
         }) {
        auto environment = requiredEnvironment();
        environment[name] = value;
        EXPECT_THROW(ServerConfigLoader::load(missingFile, environment), ConfigError) << name;
    }
}

TEST(ServerConfigLoaderTest, RequiresAllSecretsAndPublicMqttHost) {
    const auto missingFile = std::filesystem::temp_directory_path() /
                             "ocrservice-config-file-that-does-not-exist.json";
    for (const std::string name : {
             "MYSQL_PASSWORD",
             "MQTT_SERVER_PASSWORD",
             "MQTT_MANAGEMENT_PASSWORD",
         }) {
        auto environment = requiredEnvironment();
        environment.erase(name);
        EXPECT_THROW(ServerConfigLoader::load(missingFile, environment), ConfigError) << name;

        environment = requiredEnvironment();
        environment[name] = "";
        EXPECT_THROW(ServerConfigLoader::load(missingFile, environment), ConfigError) << name;
    }

    auto environment = requiredEnvironment();
    environment.erase("MQTT_PUBLIC_HOST");
    EXPECT_THROW(ServerConfigLoader::load(missingFile, environment), ConfigError);
}

TEST(ServerConfigLoaderTest, SummaryHasExactNonSensitiveFields) {
    const auto missingFile = std::filesystem::temp_directory_path() /
                             "ocrservice-config-file-that-does-not-exist.json";
    const auto config = ServerConfigLoader::load(missingFile, requiredEnvironment());
    const auto summary = ocrservice::app::config::toJson(ServerConfigLoader::summarize(config));
    const auto serialized = summary.dump();

    EXPECT_EQ(summary.size(), 19U);
    EXPECT_TRUE(summary.at("imageRootConfigured").get<bool>());
    EXPECT_TRUE(summary.at("logRootConfigured").get<bool>());
    EXPECT_TRUE(summary.at("modelRootConfigured").get<bool>());
    EXPECT_EQ(serialized.find("mysql-test-password"), std::string::npos);
    EXPECT_EQ(serialized.find("server-test-password"), std::string::npos);
    EXPECT_EQ(serialized.find("management-test-password"), std::string::npos);
    EXPECT_EQ(serialized.find("/app/data/images"), std::string::npos);
    EXPECT_EQ(serialized.find("password"), std::string::npos);
}

}  // namespace
