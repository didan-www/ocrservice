#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace ocrservice::app::config {

struct HttpConfig {
    std::uint16_t port = 8080;
};

struct MySqlConfig {
    std::string host = "mysql";
    std::uint16_t port = 3306;
    std::string database = "ocrservice";
    std::string user = "ocrservice";
    std::string password;
};

struct MqttConfig {
    std::string host = "mqtt";
    std::string publicHost;
    std::uint16_t port = 1883;
    bool tls = false;
    std::string serverUsername = "plate-server";
    std::string serverPassword;
    std::string managementUsername = "management-client";
    std::string managementPassword;
};

struct StorageConfig {
    std::filesystem::path imageRoot = "/app/data/images";
    std::filesystem::path logRoot = "/app/data/logs";
    std::filesystem::path modelRoot = "/app/models";
};

struct RecognitionConfig {
    std::uint32_t workers = 1;
    std::uint32_t queueCapacity = 20;
};

struct SessionConfig {
    std::uint32_t tokenTtlSeconds = 28800;
};

struct ModelConfig {
    double yoloConfidence = 0.25;
    double yoloNmsIou = 0.45;
};

struct ServerConfig {
    HttpConfig http;
    MySqlConfig mysql;
    MqttConfig mqtt;
    StorageConfig storage;
    RecognitionConfig recognition;
    SessionConfig session;
    ModelConfig model;
};

struct NonSensitiveConfigSummary {
    std::uint16_t httpPort;
    std::string mysqlHost;
    std::uint16_t mysqlPort;
    std::string mysqlDatabase;
    std::string mysqlUser;
    std::string mqttHost;
    std::string mqttPublicHost;
    std::uint16_t mqttPort;
    bool mqttTls;
    std::string mqttServerUsername;
    std::string mqttManagementUsername;
    bool imageRootConfigured;
    bool logRootConfigured;
    bool modelRootConfigured;
    std::uint32_t recognitionWorkers;
    std::uint32_t recognitionQueueCapacity;
    std::uint32_t tokenTtlSeconds;
    double yoloConfidence;
    double yoloNmsIou;
};

class ConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ServerConfigLoader final {
public:
    using Environment = std::map<std::string, std::string, std::less<>>;

    static ServerConfig load(
        const std::filesystem::path& jsonPath,
        const Environment& environment);
    static ServerConfig loadFromProcess(
        const std::filesystem::path& jsonPath = "/app/config/server.json");
    static NonSensitiveConfigSummary summarize(const ServerConfig& config);
};

nlohmann::json toJson(const NonSensitiveConfigSummary& summary);

}  // namespace ocrservice::app::config
