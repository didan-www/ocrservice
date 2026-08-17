#pragma once

#include <memory>
#include <mutex>

#include <mqtt/async_client.h>
#include <mqtt/callback.h>
#include <mqtt/iaction_listener.h>

#include "MqttTransport.h"

namespace ocrservice::mqtt {

class PahoMqttTransport final : public IMqttTransport {
public:
    explicit PahoMqttTransport(const MqttClientConfiguration& configuration);
    ~PahoMqttTransport() override;

    void setObserver(IMqttTransportObserver* observer) noexcept override;
    void connect(const MqttClientConfiguration& configuration) override;
    bool isConnected() const noexcept override;
    void publish(
        std::string_view topic,
        std::string_view payload,
        int qos,
        bool retained) override;
    void disconnect(std::chrono::milliseconds timeout) noexcept override;

private:
    class Callbacks final : public ::mqtt::callback, public ::mqtt::iaction_listener {
    public:
        explicit Callbacks(PahoMqttTransport& owner) noexcept;
        void connection_lost(const ::mqtt::string& cause) override;
        void on_failure(const ::mqtt::token& token) override;
        void on_success(const ::mqtt::token& token) override;

    private:
        PahoMqttTransport& owner_;
    };

    static TransportFailure classify(const ::mqtt::exception& error) noexcept;
    void notifyConnectionLost() noexcept;
    void notifyDeliveryFailure(TransportFailure failure) noexcept;

    mutable std::mutex observerMutex_;
    IMqttTransportObserver* observer_ = nullptr;
    Callbacks callbacks_;
    std::unique_ptr<::mqtt::async_client> client_;
};

}  // namespace ocrservice::mqtt
