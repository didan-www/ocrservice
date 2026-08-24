#include "DeviceConfig.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ocrservice::tests::embedded {
namespace {

using Json = nlohmann::json;

void requireExactKeys(
    const Json& value,
    const std::set<std::string>& expected,
    const char* name) {
    if (!value.is_object()) {
        throw std::runtime_error(std::string(name) + " must be an object");
    }
    std::set<std::string> actual;
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        actual.insert(key);
    }
    if (actual != expected) {
        throw std::runtime_error(std::string(name) + " has an invalid field set");
    }
}

std::string requiredString(const Json& value, const char* name) {
    if (!value.is_string()) {
        throw std::runtime_error(std::string(name) + " must be a string");
    }
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 1024U || result.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string(name) + " is invalid");
    }
    return result;
}

bool validIdentifier(const std::string& value) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$");
    return std::regex_match(value, pattern);
}

bool visibleAscii(const std::string& value, const std::size_t maximum, const bool allowComma) {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU || (!allowComma && byte == 0x2cU)) {
            return false;
        }
    }
    return true;
}

Json parseStrict(std::istream& input) {
    bool duplicate = false;
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&duplicate, &objectKeys](
                              int depth,
                              Json::parse_event_t event,
                              Json& parsed) {
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            objectKeys[index].clear();
        } else if (event == Json::parse_event_t::key) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            duplicate = !objectKeys[index].insert(parsed.get<std::string>()).second || duplicate;
        } else if (event == Json::parse_event_t::object_end && objectKeys.size() > index) {
            objectKeys[index].clear();
        }
        return true;
    };
    auto result = Json::parse(input, callback, true, false);
    if (duplicate) {
        throw std::runtime_error("device summary contains duplicate fields");
    }
    return result;
}

}  // namespace

DeviceConfig loadDeviceConfig(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        throw std::runtime_error("device summary must be a regular file");
    }
    const auto permissions = status.permissions();
    constexpr auto expected = fs::perms::owner_read | fs::perms::owner_write;
    if ((permissions & fs::perms::all) != expected) {
        throw std::runtime_error("device summary permissions must be exactly 0600");
    }
    const auto size = fs::file_size(path, error);
    if (error || size == 0U || size > 64U * 1024U) {
        throw std::runtime_error("device summary size is invalid");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("device summary cannot be opened");
    }
    const auto root = parseStrict(input);
    requireExactKeys(root, {"deviceId", "http", "mqtt"}, "device summary");
    requireExactKeys(root.at("http"), {"baseUrl", "bearerToken", "uploadPath"}, "http");
    requireExactKeys(
        root.at("mqtt"),
        {"host", "port", "tls", "username", "password", "clientId",
         "cleanSession", "qos", "resultTopic"},
        "mqtt");

    DeviceConfig result;
    result.deviceId = requiredString(root.at("deviceId"), "deviceId");
    if (!validIdentifier(result.deviceId)) {
        throw std::runtime_error("deviceId is invalid");
    }

    const auto& http = root.at("http");
    result.http.baseUrl = requiredString(http.at("baseUrl"), "http.baseUrl");
    result.http.bearerToken = requiredString(http.at("bearerToken"), "http.bearerToken");
    result.http.uploadPath = requiredString(http.at("uploadPath"), "http.uploadPath");
    static const std::regex httpUrl("^http://[A-Za-z0-9._:\\[\\]-]+$");
    if (!std::regex_match(result.http.baseUrl, httpUrl) ||
        result.http.uploadPath !=
            "/api/v1/devices/" + result.deviceId + "/recognitions" ||
        !visibleAscii(result.http.bearerToken, 256U, false)) {
        throw std::runtime_error("HTTP device configuration is invalid");
    }

    const auto& mqtt = root.at("mqtt");
    result.mqtt.host = requiredString(mqtt.at("host"), "mqtt.host");
    result.mqtt.username = requiredString(mqtt.at("username"), "mqtt.username");
    result.mqtt.password = requiredString(mqtt.at("password"), "mqtt.password");
    result.mqtt.clientId = requiredString(mqtt.at("clientId"), "mqtt.clientId");
    result.mqtt.resultTopic = requiredString(mqtt.at("resultTopic"), "mqtt.resultTopic");
    if (!mqtt.at("port").is_number_unsigned() ||
        mqtt.at("port").get<std::uint64_t>() == 0U ||
        mqtt.at("port").get<std::uint64_t>() > std::numeric_limits<std::uint16_t>::max() ||
        !mqtt.at("tls").is_boolean() || mqtt.at("tls").get<bool>() ||
        !mqtt.at("cleanSession").is_boolean() || mqtt.at("cleanSession").get<bool>() ||
        !mqtt.at("qos").is_number_integer() || mqtt.at("qos").get<int>() != 1 ||
        result.mqtt.clientId != result.deviceId ||
        result.mqtt.resultTopic !=
            "plate/devices/" + result.deviceId + "/recognition-results" ||
        !validIdentifier(result.mqtt.username) ||
        !visibleAscii(result.mqtt.password, 256U, true) ||
        !std::regex_match(result.mqtt.host, std::regex("^[A-Za-z0-9._:\\[\\]-]+$"))) {
        throw std::runtime_error("MQTT device configuration is invalid");
    }
    result.mqtt.port = static_cast<std::uint16_t>(mqtt.at("port").get<std::uint64_t>());
    result.mqtt.tls = false;
    result.mqtt.cleanSession = false;
    result.mqtt.qos = 1;
    return result;
}

}  // namespace ocrservice::tests::embedded
