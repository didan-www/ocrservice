#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ServerConfig.h"

namespace ocrservice::mqtt {

inline constexpr const char* kServerClientId = "plate-server";
inline constexpr int kMqtt311ProtocolVersion = 4;
inline constexpr int kPublishQos = 1;
inline constexpr bool kPublishRetained = false;

struct MqttClientConfiguration final {
    std::string brokerUri;
    std::string clientId;
    std::string username;
    std::string password;
    int protocolVersion;
    bool cleanSession;
    std::chrono::seconds connectTimeout;
    std::chrono::seconds keepAlive;
    std::chrono::milliseconds disconnectTimeout;
};

MqttClientConfiguration makeMqttClientConfiguration(
    const app::config::MqttConfig& config);

enum class TransportFailure { disconnected, rejected, other };

class MqttTransportError final : public std::runtime_error {
public:
    explicit MqttTransportError(TransportFailure failure);
    TransportFailure failure() const noexcept;

private:
    TransportFailure failure_;
};

class IMqttTransportObserver {
public:
    virtual ~IMqttTransportObserver() = default;
    virtual void onConnectionLost() noexcept = 0;
    virtual void onDeliveryFailure(TransportFailure failure) noexcept = 0;
};

class IMqttTransport {
public:
    virtual ~IMqttTransport() = default;
    virtual void setObserver(IMqttTransportObserver* observer) noexcept = 0;
    virtual void connect(const MqttClientConfiguration& configuration) = 0;
    virtual bool isConnected() const noexcept = 0;
    virtual void publish(
        std::string_view topic,
        std::string_view payload,
        int qos,
        bool retained) = 0;
    virtual void disconnect(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace ocrservice::mqtt
