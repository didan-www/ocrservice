#pragma once

#include <cstdint>
#include <string>

namespace ocrservice::tests::embedded {

struct HttpConfig final {
    std::string baseUrl;
    std::string bearerToken;
    std::string uploadPath;
};

struct MqttConfig final {
    std::string host;
    std::uint16_t port = 0;
    bool tls = false;
    std::string username;
    std::string password;
    std::string clientId;
    bool cleanSession = true;
    int qos = 0;
    std::string resultTopic;
};

struct DeviceConfig final {
    std::string deviceId;
    HttpConfig http;
    MqttConfig mqtt;
};

DeviceConfig loadDeviceConfig(const std::string& path);

}  // namespace ocrservice::tests::embedded
