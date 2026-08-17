#include "PahoMqttPublisher.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "MqttPayload.h"
#include "PahoMqttTransport.h"

namespace ocrservice::mqtt {
namespace {

constexpr const char* kSystemRequestId = "system";

const char* failureCode(const domain::PublishFailure failure) noexcept {
    switch (failure) {
        case domain::PublishFailure::notConnected:
            return "MQTT_NOT_CONNECTED";
        case domain::PublishFailure::brokerRejected:
            return "MQTT_BROKER_REJECTED";
        case domain::PublishFailure::transportError:
            return "MQTT_TRANSPORT_ERROR";
        case domain::PublishFailure::stopping:
            return "MQTT_STOPPING";
    }
    return "MQTT_TRANSPORT_ERROR";
}

}  // namespace

PahoMqttPublisher::PahoMqttPublisher(
    const app::config::MqttConfig& config,
    std::shared_ptr<logging::JsonLinesLogger> logger,
    FirstConnectedObserver firstConnectedObserver)
    : PahoMqttPublisher(
          makeMqttClientConfiguration(config),
          std::move(logger),
          std::make_unique<PahoMqttTransport>(makeMqttClientConfiguration(config)),
          std::make_unique<SystemReconnectDelay>(),
          std::move(firstConnectedObserver)) {}

PahoMqttPublisher::PahoMqttPublisher(
    MqttClientConfiguration configuration,
    std::shared_ptr<logging::JsonLinesLogger> logger,
    std::unique_ptr<IMqttTransport> transport,
    std::unique_ptr<IReconnectDelay> reconnectDelay,
    FirstConnectedObserver firstConnectedObserver)
    : configuration_(std::move(configuration)),
      logger_(std::move(logger)),
      transport_(std::move(transport)),
      reconnectDelay_(std::move(reconnectDelay)),
      firstConnectedObserver_(std::move(firstConnectedObserver)) {
    if (!logger_ || !transport_ || !reconnectDelay_ ||
        configuration_.clientId != kServerClientId ||
        configuration_.protocolVersion != kMqtt311ProtocolVersion ||
        !configuration_.cleanSession || configuration_.connectTimeout != std::chrono::seconds(5) ||
        configuration_.keepAlive != std::chrono::seconds(60) ||
        configuration_.disconnectTimeout != std::chrono::seconds(2)) {
        throw std::invalid_argument("MQTT publisher dependencies are invalid");
    }
    transport_->setObserver(this);
}

PahoMqttPublisher::~PahoMqttPublisher() { stop(); }

void PahoMqttPublisher::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (lifecycle_ != Lifecycle::created) {
        throw std::logic_error("MQTT publisher can only be started once");
    }
    stopRequested_.store(false, std::memory_order_release);
    controlThread_ = std::thread(&PahoMqttPublisher::run, this);
    lifecycle_ = Lifecycle::running;
}

void PahoMqttPublisher::stop() noexcept {
    std::thread threadToJoin;
    {
        std::unique_lock<std::mutex> lock(lifecycleMutex_);
        if (lifecycle_ == Lifecycle::created) {
            lifecycle_ = Lifecycle::stopped;
            stopRequested_.store(true, std::memory_order_release);
            reconnectDelay_->interrupt();
            transport_->setObserver(nullptr);
            lifecycleCondition_.notify_all();
            return;
        }
        if (lifecycle_ == Lifecycle::stopping) {
            if (controlThread_.joinable() &&
                controlThread_.get_id() == std::this_thread::get_id()) {
                return;
            }
            lifecycleCondition_.wait(lock, [this] { return lifecycle_ == Lifecycle::stopped; });
        } else if (lifecycle_ == Lifecycle::running) {
            lifecycle_ = Lifecycle::stopping;
            {
                std::lock_guard<std::mutex> connectionLock(connectionMutex_);
                stopRequested_.store(true, std::memory_order_release);
            }
            reconnectDelay_->interrupt();
            connectionCondition_.notify_all();
            if (controlThread_.joinable() &&
                controlThread_.get_id() == std::this_thread::get_id()) {
                return;
            }
        }
        if (controlThread_.joinable()) {
            threadToJoin = std::move(controlThread_);
        }
    }
    if (threadToJoin.joinable()) {
        try {
            threadToJoin.join();
        } catch (...) {
        }
    }
}

bool PahoMqttPublisher::isConnected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

domain::PublishAttempt PahoMqttPublisher::publishManagement(
    const domain::RecognitionSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(publishMutex_);
    auto message = makeManagementMessage(snapshot);
    return publish(snapshot, std::move(message.topic), std::move(message.payload));
}

domain::PublishAttempt PahoMqttPublisher::publishDeviceFinal(
    const domain::RecognitionSnapshot& snapshot,
    const domain::GateAction gateAction) {
    std::lock_guard<std::mutex> lock(publishMutex_);
    auto message = makeDeviceFinalMessage(snapshot, gateAction);
    return publish(snapshot, std::move(message.topic), std::move(message.payload));
}

void PahoMqttPublisher::run() noexcept {
    std::size_t failureIndex = 0U;
    while (!stopRequested_.load(std::memory_order_acquire)) {
        bool connected = false;
        try {
            transport_->connect(configuration_);
            {
                std::lock_guard<std::mutex> lock(connectionMutex_);
                connected = !stopRequested_.load(std::memory_order_acquire) &&
                            transport_->isConnected();
                connected_.store(connected, std::memory_order_release);
            }
        } catch (const MqttTransportError& error) {
            const auto failure = mapFailure(error.failure());
            logConnectionEvent(logging::LogLevel::warning, "connect_failed", failureCode(failure));
        } catch (...) {
            logConnectionEvent(
                logging::LogLevel::error, "connect_failed", "MQTT_TRANSPORT_ERROR");
        }

        if (connected) {
            failureIndex = 0U;
            logConnectionEvent(logging::LogLevel::info, "connected", "OK");
            invokeFirstConnectedObserver();
            std::unique_lock<std::mutex> lock(connectionMutex_);
            connectionCondition_.wait(lock, [this] {
                return stopRequested_.load(std::memory_order_acquire) ||
                       !connected_.load(std::memory_order_acquire);
            });
            continue;
        }

        setConnected(false);
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        const auto delay = reconnectDelay_->jittered(baseDelay(failureIndex));
        failureIndex = std::min<std::size_t>(failureIndex + 1U, 5U);
        if (!reconnectDelay_->waitFor(delay)) {
            break;
        }
    }

    setConnected(false);
    transport_->disconnect(configuration_.disconnectTimeout);
    transport_->setObserver(nullptr);
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        lifecycle_ = Lifecycle::stopped;
    }
    lifecycleCondition_.notify_all();
}

domain::PublishAttempt PahoMqttPublisher::publish(
    const domain::RecognitionSnapshot& snapshot,
    std::string topic,
    std::string payload) {
    if (stopRequested_.load(std::memory_order_acquire)) {
        const auto attempt = domain::PublishAttempt::rejected(domain::PublishFailure::stopping);
        logPublishFailure(snapshot, domain::PublishFailure::stopping);
        return attempt;
    }
    if (!connected_.load(std::memory_order_acquire)) {
        const auto attempt = domain::PublishAttempt::rejected(domain::PublishFailure::notConnected);
        logPublishFailure(snapshot, domain::PublishFailure::notConnected);
        return attempt;
    }

    try {
        transport_->publish(topic, payload, kPublishQos, kPublishRetained);
        return domain::PublishAttempt::accepted();
    } catch (const MqttTransportError& error) {
        const auto failure = mapFailure(error.failure());
        if (failure == domain::PublishFailure::notConnected) {
            onConnectionLost();
        }
        logPublishFailure(snapshot, failure);
        return domain::PublishAttempt::rejected(failure);
    } catch (...) {
        logPublishFailure(snapshot, domain::PublishFailure::transportError);
        return domain::PublishAttempt::rejected(domain::PublishFailure::transportError);
    }
}

void PahoMqttPublisher::setConnected(const bool connected) noexcept {
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        connected_.store(connected, std::memory_order_release);
    }
    connectionCondition_.notify_all();
}

void PahoMqttPublisher::invokeFirstConnectedObserver() noexcept {
    if (firstConnectedObserverInvoked_) {
        return;
    }
    firstConnectedObserverInvoked_ = true;
    if (!firstConnectedObserver_) {
        return;
    }
    try {
        firstConnectedObserver_(*this);
    } catch (...) {
        logConnectionEvent(
            logging::LogLevel::error, "first_connected_observer_failed", "MQTT_OBSERVER_ERROR");
    }
}

void PahoMqttPublisher::logConnectionEvent(
    const logging::LogLevel level,
    const char* event,
    const char* code) noexcept {
    try {
        logger_->log(level, logging::LogEvent("mqtt", event, code, kSystemRequestId));
    } catch (...) {
    }
}

void PahoMqttPublisher::logPublishFailure(
    const domain::RecognitionSnapshot& snapshot,
    const domain::PublishFailure failure) noexcept {
    try {
        logger_->log(
            logging::LogLevel::warning,
            logging::LogEvent(
                "mqtt",
                "publish_failed",
                failureCode(failure),
                kSystemRequestId,
                snapshot.recognitionId().toString(),
                snapshot.deviceId().value()));
    } catch (...) {
    }
}

domain::PublishFailure PahoMqttPublisher::mapFailure(const TransportFailure failure) noexcept {
    switch (failure) {
        case TransportFailure::disconnected:
            return domain::PublishFailure::notConnected;
        case TransportFailure::rejected:
            return domain::PublishFailure::brokerRejected;
        case TransportFailure::other:
            return domain::PublishFailure::transportError;
    }
    return domain::PublishFailure::transportError;
}

std::chrono::milliseconds PahoMqttPublisher::baseDelay(
    const std::size_t failureIndex) noexcept {
    constexpr std::array<std::chrono::milliseconds, 6> delays = {
        std::chrono::seconds(1),
        std::chrono::seconds(2),
        std::chrono::seconds(4),
        std::chrono::seconds(8),
        std::chrono::seconds(16),
        std::chrono::seconds(30),
    };
    return delays[std::min(failureIndex, delays.size() - 1U)];
}

void PahoMqttPublisher::onConnectionLost() noexcept {
    setConnected(false);
    logConnectionEvent(logging::LogLevel::warning, "connection_lost", "MQTT_NOT_CONNECTED");
}

void PahoMqttPublisher::onDeliveryFailure(const TransportFailure failure) noexcept {
    const auto mapped = mapFailure(failure);
    logConnectionEvent(logging::LogLevel::warning, "delivery_failed", failureCode(mapped));
    if (mapped == domain::PublishFailure::notConnected) {
        setConnected(false);
    }
}

}  // namespace ocrservice::mqtt
