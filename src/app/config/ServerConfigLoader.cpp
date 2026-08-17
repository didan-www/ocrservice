#include "ServerConfig.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

namespace ocrservice::app::config {
namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, 16> kJsonFields = {
    "httpPort",
    "mysqlHost",
    "mysqlPort",
    "mysqlDatabase",
    "mysqlUser",
    "mqttHost",
    "mqttPublicHost",
    "mqttPort",
    "mqttServerUsername",
    "mqttManagementUsername",
    "imageRoot",
    "logRoot",
    "modelRoot",
    "recognitionWorkers",
    "recognitionQueueCapacity",
    "tokenTtlSeconds",
};

constexpr std::array<std::string_view, 2> kJsonModelFields = {
    "yoloConfidence",
    "yoloNmsIou",
};

constexpr std::array<std::string_view, 19> kEnvironmentFields = {
    "HTTP_PORT",
    "MYSQL_HOST",
    "MYSQL_PORT",
    "MYSQL_DATABASE",
    "MYSQL_USER",
    "MYSQL_PASSWORD",
    "MQTT_HOST",
    "MQTT_PUBLIC_HOST",
    "MQTT_PORT",
    "MQTT_SERVER_USERNAME",
    "MQTT_SERVER_PASSWORD",
    "MQTT_MANAGEMENT_USERNAME",
    "MQTT_MANAGEMENT_PASSWORD",
    "IMAGE_ROOT",
    "LOG_ROOT",
    "MODEL_ROOT",
    "RECOGNITION_WORKERS",
    "RECOGNITION_QUEUE_CAPACITY",
    "TOKEN_TTL_SECONDS",
};

constexpr std::array<std::string_view, 2> kEnvironmentModelFields = {
    "YOLO_CONFIDENCE",
    "YOLO_NMS_IOU",
};

bool isAllowedJsonField(const std::string_view field) {
    for (const auto allowed : kJsonFields) {
        if (field == allowed) {
            return true;
        }
    }
    for (const auto allowed : kJsonModelFields) {
        if (field == allowed) {
            return true;
        }
    }
    return false;
}

std::string requireJsonString(const Json& value, const std::string_view field) {
    if (!value.is_string()) {
        throw ConfigError("configuration field " + std::string(field) + " must be a string");
    }
    const auto result = value.get<std::string>();
    if (result.empty()) {
        throw ConfigError("configuration field " + std::string(field) + " must not be empty");
    }
    return result;
}

std::uint64_t requireJsonUnsigned(
    const Json& value,
    const std::string_view field,
    const std::uint64_t minimum,
    const std::uint64_t maximum) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        if (signedValue < 0) {
            throw ConfigError("configuration field " + std::string(field) + " is out of range");
        }
        result = static_cast<std::uint64_t>(signedValue);
    } else {
        throw ConfigError("configuration field " + std::string(field) + " must be an integer");
    }
    if (result < minimum || result > maximum) {
        throw ConfigError("configuration field " + std::string(field) + " is out of range");
    }
    return result;
}

double requireJsonUnitInterval(const Json& value, const std::string_view field) {
    if (!value.is_number()) {
        throw ConfigError("configuration field " + std::string(field) + " must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result) || result < 0.0 || result > 1.0) {
        throw ConfigError("configuration field " + std::string(field) + " is out of range");
    }
    return result;
}

std::string requireEnvironmentString(
    const ServerConfigLoader::Environment& environment,
    const std::string_view name) {
    const auto found = environment.find(name);
    if (found == environment.end() || found->second.empty()) {
        throw ConfigError(
            "required environment variable " + std::string(name) + " is missing or empty");
    }
    return found->second;
}

std::optional<std::string_view> environmentValue(
    const ServerConfigLoader::Environment& environment,
    const std::string_view name) {
    const auto found = environment.find(name);
    if (found == environment.end()) {
        return std::nullopt;
    }
    if (found->second.empty()) {
        throw ConfigError("environment variable " + std::string(name) + " must not be empty");
    }
    return found->second;
}

bool isValidMqttUsername(const std::string_view value) {
    if (value.empty() || value.size() > 64U) {
        return false;
    }
    const auto isAlphaNumeric = [](const char character) {
        return (character >= 'A' && character <= 'Z') ||
               (character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9');
    };
    if (!isAlphaNumeric(value.front())) {
        return false;
    }
    for (const char character : value) {
        if (!isAlphaNumeric(character) && character != '.' && character != '_' &&
            character != '-') {
            return false;
        }
    }
    return true;
}

std::uint64_t parseEnvironmentUnsigned(
    const std::string_view value,
    const std::string_view name,
    const std::uint64_t minimum,
    const std::uint64_t maximum) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result < minimum || result > maximum) {
        throw ConfigError("environment variable " + std::string(name) + " is invalid");
    }
    return result;
}

double parseEnvironmentUnitInterval(
    const std::string_view value,
    const std::string_view name) {
    std::istringstream input{std::string(value)};
    input.imbue(std::locale::classic());
    double result = 0.0;
    input >> std::noskipws >> result;
    if (!input.eof() || input.fail() || !std::isfinite(result) || result < 0.0 ||
        result > 1.0) {
        throw ConfigError("environment variable " + std::string(name) + " is invalid");
    }
    return result;
}

void applyJson(ServerConfig& config, const Json& document) {
    if (!document.is_object()) {
        throw ConfigError("server configuration JSON root must be an object");
    }
    for (const auto& [key, value] : document.items()) {
        if (!isAllowedJsonField(key)) {
            throw ConfigError("server configuration JSON contains an unknown field");
        }
        if (key == "httpPort") {
            config.http.port =
                static_cast<std::uint16_t>(requireJsonUnsigned(value, key, 1, 65535));
        } else if (key == "mysqlHost") {
            config.mysql.host = requireJsonString(value, key);
        } else if (key == "mysqlPort") {
            config.mysql.port =
                static_cast<std::uint16_t>(requireJsonUnsigned(value, key, 1, 65535));
        } else if (key == "mysqlDatabase") {
            config.mysql.database = requireJsonString(value, key);
        } else if (key == "mysqlUser") {
            config.mysql.user = requireJsonString(value, key);
        } else if (key == "mqttHost") {
            config.mqtt.host = requireJsonString(value, key);
        } else if (key == "mqttPublicHost") {
            config.mqtt.publicHost = requireJsonString(value, key);
        } else if (key == "mqttPort") {
            config.mqtt.port =
                static_cast<std::uint16_t>(requireJsonUnsigned(value, key, 1, 65535));
        } else if (key == "mqttServerUsername") {
            config.mqtt.serverUsername = requireJsonString(value, key);
        } else if (key == "mqttManagementUsername") {
            config.mqtt.managementUsername = requireJsonString(value, key);
        } else if (key == "imageRoot") {
            config.storage.imageRoot = requireJsonString(value, key);
        } else if (key == "logRoot") {
            config.storage.logRoot = requireJsonString(value, key);
        } else if (key == "modelRoot") {
            config.storage.modelRoot = requireJsonString(value, key);
        } else if (key == "recognitionWorkers") {
            config.recognition.workers = static_cast<std::uint32_t>(requireJsonUnsigned(
                value, key, 1, std::numeric_limits<std::uint32_t>::max()));
        } else if (key == "recognitionQueueCapacity") {
            config.recognition.queueCapacity = static_cast<std::uint32_t>(requireJsonUnsigned(
                value, key, 1, std::numeric_limits<std::uint32_t>::max()));
        } else if (key == "tokenTtlSeconds") {
            config.session.tokenTtlSeconds = static_cast<std::uint32_t>(requireJsonUnsigned(
                value, key, 1, std::numeric_limits<std::uint32_t>::max()));
        } else if (key == "yoloConfidence") {
            config.model.yoloConfidence = requireJsonUnitInterval(value, key);
        } else if (key == "yoloNmsIou") {
            config.model.yoloNmsIou = requireJsonUnitInterval(value, key);
        }
    }
}

void applyEnvironment(ServerConfig& config, const ServerConfigLoader::Environment& environment) {
    if (const auto value = environmentValue(environment, "HTTP_PORT")) {
        config.http.port = static_cast<std::uint16_t>(
            parseEnvironmentUnsigned(*value, "HTTP_PORT", 1, 65535));
    }
    if (const auto value = environmentValue(environment, "MYSQL_HOST")) {
        config.mysql.host = *value;
    }
    if (const auto value = environmentValue(environment, "MYSQL_PORT")) {
        config.mysql.port = static_cast<std::uint16_t>(
            parseEnvironmentUnsigned(*value, "MYSQL_PORT", 1, 65535));
    }
    if (const auto value = environmentValue(environment, "MYSQL_DATABASE")) {
        config.mysql.database = *value;
    }
    if (const auto value = environmentValue(environment, "MYSQL_USER")) {
        config.mysql.user = *value;
    }
    if (const auto value = environmentValue(environment, "MQTT_HOST")) {
        config.mqtt.host = *value;
    }
    if (const auto value = environmentValue(environment, "MQTT_PUBLIC_HOST")) {
        config.mqtt.publicHost = *value;
    }
    if (const auto value = environmentValue(environment, "MQTT_PORT")) {
        config.mqtt.port = static_cast<std::uint16_t>(
            parseEnvironmentUnsigned(*value, "MQTT_PORT", 1, 65535));
    }
    if (const auto value = environmentValue(environment, "MQTT_SERVER_USERNAME")) {
        config.mqtt.serverUsername = *value;
    }
    if (const auto value = environmentValue(environment, "MQTT_MANAGEMENT_USERNAME")) {
        config.mqtt.managementUsername = *value;
    }
    if (const auto value = environmentValue(environment, "IMAGE_ROOT")) {
        config.storage.imageRoot = *value;
    }
    if (const auto value = environmentValue(environment, "LOG_ROOT")) {
        config.storage.logRoot = *value;
    }
    if (const auto value = environmentValue(environment, "MODEL_ROOT")) {
        config.storage.modelRoot = *value;
    }
    if (const auto value = environmentValue(environment, "RECOGNITION_WORKERS")) {
        config.recognition.workers = static_cast<std::uint32_t>(parseEnvironmentUnsigned(
            *value, "RECOGNITION_WORKERS", 1, std::numeric_limits<std::uint32_t>::max()));
    }
    if (const auto value = environmentValue(environment, "RECOGNITION_QUEUE_CAPACITY")) {
        config.recognition.queueCapacity = static_cast<std::uint32_t>(parseEnvironmentUnsigned(
            *value, "RECOGNITION_QUEUE_CAPACITY", 1, std::numeric_limits<std::uint32_t>::max()));
    }
    if (const auto value = environmentValue(environment, "TOKEN_TTL_SECONDS")) {
        config.session.tokenTtlSeconds = static_cast<std::uint32_t>(parseEnvironmentUnsigned(
            *value, "TOKEN_TTL_SECONDS", 1, std::numeric_limits<std::uint32_t>::max()));
    }
    if (const auto value = environmentValue(environment, "YOLO_CONFIDENCE")) {
        config.model.yoloConfidence = parseEnvironmentUnitInterval(*value, "YOLO_CONFIDENCE");
    }
    if (const auto value = environmentValue(environment, "YOLO_NMS_IOU")) {
        config.model.yoloNmsIou = parseEnvironmentUnitInterval(*value, "YOLO_NMS_IOU");
    }

    config.mysql.password = requireEnvironmentString(environment, "MYSQL_PASSWORD");
    config.mqtt.serverPassword = requireEnvironmentString(environment, "MQTT_SERVER_PASSWORD");
    config.mqtt.managementPassword =
        requireEnvironmentString(environment, "MQTT_MANAGEMENT_PASSWORD");
}

void validateFinal(const ServerConfig& config) {
    if (config.mqtt.publicHost.empty()) {
        throw ConfigError("mqttPublicHost must be provided by JSON or MQTT_PUBLIC_HOST");
    }
    if (!isValidMqttUsername(config.mqtt.serverUsername)) {
        throw ConfigError("mqttServerUsername is invalid");
    }
    if (!isValidMqttUsername(config.mqtt.managementUsername)) {
        throw ConfigError("mqttManagementUsername is invalid");
    }
    if (config.mqtt.serverUsername == config.mqtt.managementUsername) {
        throw ConfigError("MQTT server and management usernames must be distinct");
    }
}

Json readJson(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ConfigError("server configuration file could not be read");
    }
    try {
        std::set<std::string> rootKeys;
        bool hasDuplicateRootKey = false;
        const auto callback = [&rootKeys, &hasDuplicateRootKey](
                                  const int,
                                  const Json::parse_event_t event,
                                  Json& parsed) {
            if (event == Json::parse_event_t::key) {
                hasDuplicateRootKey = !rootKeys.insert(parsed.get<std::string>()).second ||
                                      hasDuplicateRootKey;
            }
            return true;
        };
        auto document = Json::parse(input, callback, true, false);
        if (hasDuplicateRootKey) {
            throw ConfigError("server configuration JSON contains a duplicate field");
        }
        return document;
    } catch (const ConfigError&) {
        throw;
    } catch (const Json::exception&) {
        throw ConfigError("server configuration file contains invalid JSON");
    }
}

}  // namespace

ServerConfig ServerConfigLoader::load(
    const std::filesystem::path& jsonPath,
    const Environment& environment) {
    ServerConfig config;
    std::error_code error;
    const bool exists = std::filesystem::exists(jsonPath, error);
    if (error) {
        throw ConfigError("server configuration file could not be inspected");
    }
    if (exists) {
        applyJson(config, readJson(jsonPath));
    }
    applyEnvironment(config, environment);
    validateFinal(config);
    return config;
}

ServerConfig ServerConfigLoader::loadFromProcess(const std::filesystem::path& jsonPath) {
    Environment environment;
    const auto collect = [&environment](const std::string_view name) {
        const std::string ownedName(name);
        if (const char* value = std::getenv(ownedName.c_str()); value != nullptr) {
            environment.emplace(ownedName, value);
        }
    };
    for (const auto name : kEnvironmentFields) {
        collect(name);
    }
    for (const auto name : kEnvironmentModelFields) {
        collect(name);
    }
    return load(jsonPath, environment);
}

NonSensitiveConfigSummary ServerConfigLoader::summarize(const ServerConfig& config) {
    return {
        config.http.port,
        config.mysql.host,
        config.mysql.port,
        config.mysql.database,
        config.mysql.user,
        config.mqtt.host,
        config.mqtt.publicHost,
        config.mqtt.port,
        config.mqtt.tls,
        config.mqtt.serverUsername,
        config.mqtt.managementUsername,
        !config.storage.imageRoot.empty(),
        !config.storage.logRoot.empty(),
        !config.storage.modelRoot.empty(),
        config.recognition.workers,
        config.recognition.queueCapacity,
        config.session.tokenTtlSeconds,
        config.model.yoloConfidence,
        config.model.yoloNmsIou,
    };
}

nlohmann::json toJson(const NonSensitiveConfigSummary& summary) {
    return {
        {"httpPort", summary.httpPort},
        {"mysqlHost", summary.mysqlHost},
        {"mysqlPort", summary.mysqlPort},
        {"mysqlDatabase", summary.mysqlDatabase},
        {"mysqlUser", summary.mysqlUser},
        {"mqttHost", summary.mqttHost},
        {"mqttPublicHost", summary.mqttPublicHost},
        {"mqttPort", summary.mqttPort},
        {"mqttTls", summary.mqttTls},
        {"mqttServerUsername", summary.mqttServerUsername},
        {"mqttManagementUsername", summary.mqttManagementUsername},
        {"imageRootConfigured", summary.imageRootConfigured},
        {"logRootConfigured", summary.logRootConfigured},
        {"modelRootConfigured", summary.modelRootConfigured},
        {"recognitionWorkers", summary.recognitionWorkers},
        {"recognitionQueueCapacity", summary.recognitionQueueCapacity},
        {"tokenTtlSeconds", summary.tokenTtlSeconds},
        {"yoloConfidence", summary.yoloConfidence},
        {"yoloNmsIou", summary.yoloNmsIou},
    };
}

}  // namespace ocrservice::app::config
