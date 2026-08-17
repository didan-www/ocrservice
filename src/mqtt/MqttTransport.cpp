#include "MqttTransport.h"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace ocrservice::mqtt {
namespace {

std::string buildBrokerUri(const std::string_view host, const std::uint16_t port) {
    if (host.empty() || port == 0U || host.find("://") != std::string_view::npos ||
        host.find_first_of("/\\?#@") != std::string_view::npos) {
        throw std::invalid_argument("MQTT host is invalid");
    }
    for (const char raw : host) {
        const auto value = static_cast<unsigned char>(raw);
        if (value <= 0x20U || value == 0x7FU) {
            throw std::invalid_argument("MQTT host is invalid");
        }
    }

    const bool containsBracket = host.find_first_of("[]") != std::string_view::npos;
    const bool bracketed = host.size() >= 2U && host.front() == '[' && host.back() == ']';
    const bool hasColon = host.find(':') != std::string_view::npos;
    if (containsBracket && !bracketed) {
        throw std::invalid_argument("MQTT host has invalid IPv6 brackets");
    }

    if (bracketed || hasColon) {
        const auto candidate = bracketed ? host.substr(1U, host.size() - 2U) : host;
        if (candidate.find_first_of("[]") != std::string_view::npos) {
            throw std::invalid_argument("MQTT host has invalid IPv6 brackets");
        }
        in6_addr address{};
        const std::string rawAddress(candidate);
        if (::inet_pton(AF_INET6, rawAddress.c_str(), &address) != 1) {
            throw std::invalid_argument("MQTT host is not a valid IPv6 address");
        }
        return "tcp://[" + rawAddress + "]:" + std::to_string(port);
    }
    return "tcp://" + std::string(host) + ':' + std::to_string(port);
}

}  // namespace

MqttClientConfiguration makeMqttClientConfiguration(
    const app::config::MqttConfig& config) {
    if (config.tls || config.serverUsername.empty() || config.serverPassword.empty()) {
        throw std::invalid_argument("MQTT publisher configuration is invalid");
    }
    return {
        buildBrokerUri(config.host, config.port),
        kServerClientId,
        config.serverUsername,
        config.serverPassword,
        kMqtt311ProtocolVersion,
        true,
        std::chrono::seconds(5),
        std::chrono::seconds(60),
        std::chrono::seconds(2),
    };
}

MqttTransportError::MqttTransportError(const TransportFailure failure)
    : std::runtime_error("MQTT transport operation failed"), failure_(failure) {}

TransportFailure MqttTransportError::failure() const noexcept { return failure_; }

}  // namespace ocrservice::mqtt
