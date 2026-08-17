#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "LoggerFactory.h"
#include "MqttTransport.h"
#include "Ports.h"
#include "ReconnectDelay.h"
#include "ServerConfig.h"

namespace ocrservice::mqtt {

class PahoMqttPublisher final : public domain::IMqttPublisher,
                                private IMqttTransportObserver {
public:
    using FirstConnectedObserver = std::function<void(domain::IMqttPublisher&)>;

    PahoMqttPublisher(
        const app::config::MqttConfig& config,
        std::shared_ptr<logging::JsonLinesLogger> logger,
        FirstConnectedObserver firstConnectedObserver = {});

    PahoMqttPublisher(
        MqttClientConfiguration configuration,
        std::shared_ptr<logging::JsonLinesLogger> logger,
        std::unique_ptr<IMqttTransport> transport,
        std::unique_ptr<IReconnectDelay> reconnectDelay,
        FirstConnectedObserver firstConnectedObserver = {});

    ~PahoMqttPublisher() override;
    PahoMqttPublisher(const PahoMqttPublisher&) = delete;
    PahoMqttPublisher& operator=(const PahoMqttPublisher&) = delete;

    void start();
    void stop() noexcept;
    bool isConnected() const noexcept;

    domain::PublishAttempt publishManagement(
        const domain::RecognitionSnapshot& snapshot) override;
    domain::PublishAttempt publishDeviceFinal(
        const domain::RecognitionSnapshot& snapshot,
        domain::GateAction gateAction) override;

private:
    enum class Lifecycle { created, running, stopping, stopped };

    void run() noexcept;
    domain::PublishAttempt publish(
        const domain::RecognitionSnapshot& snapshot,
        std::string topic,
        std::string payload);
    void setConnected(bool connected) noexcept;
    void invokeFirstConnectedObserver() noexcept;
    void logConnectionEvent(
        logging::LogLevel level,
        const char* event,
        const char* code) noexcept;
    void logPublishFailure(
        const domain::RecognitionSnapshot& snapshot,
        domain::PublishFailure failure) noexcept;
    static domain::PublishFailure mapFailure(TransportFailure failure) noexcept;
    static std::chrono::milliseconds baseDelay(std::size_t failureIndex) noexcept;

    void onConnectionLost() noexcept override;
    void onDeliveryFailure(TransportFailure failure) noexcept override;

    MqttClientConfiguration configuration_;
    std::shared_ptr<logging::JsonLinesLogger> logger_;
    std::unique_ptr<IMqttTransport> transport_;
    std::unique_ptr<IReconnectDelay> reconnectDelay_;
    FirstConnectedObserver firstConnectedObserver_;

    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    Lifecycle lifecycle_ = Lifecycle::created;
    std::thread controlThread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> connected_{false};
    std::mutex connectionMutex_;
    std::condition_variable connectionCondition_;
    std::mutex publishMutex_;
    bool firstConnectedObserverInvoked_ = false;
};

}  // namespace ocrservice::mqtt
