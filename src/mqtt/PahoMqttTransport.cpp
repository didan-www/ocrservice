#include "PahoMqttTransport.h"

#include <chrono>
#include <cstddef>
#include <stdexcept>

#include <MQTTAsync.h>
#include <mqtt/connect_options.h>
#include <mqtt/exception.h>

namespace ocrservice::mqtt {

PahoMqttTransport::Callbacks::Callbacks(PahoMqttTransport& owner) noexcept : owner_(owner) {}

void PahoMqttTransport::Callbacks::connection_lost(const ::mqtt::string&) {
    owner_.notifyConnectionLost();
}

void PahoMqttTransport::Callbacks::on_failure(const ::mqtt::token& token) {
    const auto returnCode = token.get_return_code();
    const auto failure = returnCode == MQTTASYNC_DISCONNECTED
                             ? TransportFailure::disconnected
                             : TransportFailure::other;
    owner_.notifyDeliveryFailure(failure);
}

void PahoMqttTransport::Callbacks::on_success(const ::mqtt::token&) {}

PahoMqttTransport::PahoMqttTransport(const MqttClientConfiguration& configuration)
    : callbacks_(*this),
      client_(std::make_unique<::mqtt::async_client>(
          configuration.brokerUri,
          configuration.clientId,
          static_cast<::mqtt::iclient_persistence*>(nullptr))) {
    client_->set_callback(callbacks_);
}

PahoMqttTransport::~PahoMqttTransport() {
    setObserver(nullptr);
    disconnect(std::chrono::milliseconds(0));
}

void PahoMqttTransport::setObserver(IMqttTransportObserver* observer) noexcept {
    std::lock_guard<std::mutex> lock(observerMutex_);
    observer_ = observer;
}

void PahoMqttTransport::connect(const MqttClientConfiguration& configuration) {
    if (configuration.clientId != client_->get_client_id() ||
        configuration.protocolVersion != kMqtt311ProtocolVersion ||
        !configuration.cleanSession || configuration.connectTimeout.count() != 5 ||
        configuration.keepAlive.count() != 60) {
        throw MqttTransportError(TransportFailure::other);
    }

    ::mqtt::connect_options options(configuration.protocolVersion);
    options.set_clean_session(configuration.cleanSession);
    options.set_automatic_reconnect(false);
    options.set_connect_timeout(configuration.connectTimeout);
    options.set_keep_alive_interval(configuration.keepAlive);
    options.set_user_name(configuration.username);
    options.set_password(configuration.password);

    try {
        const auto token = client_->connect(std::move(options));
        if (!token) {
            throw MqttTransportError(TransportFailure::other);
        }
        token->wait();
    } catch (const ::mqtt::security_exception&) {
        throw MqttTransportError(TransportFailure::rejected);
    } catch (const ::mqtt::exception& error) {
        throw MqttTransportError(classify(error));
    }
}

bool PahoMqttTransport::isConnected() const noexcept {
    try {
        return client_->is_connected();
    } catch (...) {
        return false;
    }
}

void PahoMqttTransport::publish(
    const std::string_view topic,
    const std::string_view payload,
    const int qos,
    const bool retained) {
    if (qos != kPublishQos || retained != kPublishRetained) {
        throw MqttTransportError(TransportFailure::other);
    }
    try {
        const auto token = client_->publish(
            std::string(topic),
            payload.data(),
            payload.size(),
            qos,
            retained,
            nullptr,
            callbacks_);
        if (!token) {
            throw MqttTransportError(TransportFailure::other);
        }
    } catch (const ::mqtt::security_exception&) {
        throw MqttTransportError(TransportFailure::rejected);
    } catch (const ::mqtt::exception& error) {
        throw MqttTransportError(classify(error));
    }
}

void PahoMqttTransport::disconnect(const std::chrono::milliseconds timeout) noexcept {
    try {
        if (client_->is_connected()) {
            const auto token = client_->disconnect(static_cast<int>(timeout.count()));
            if (token && timeout.count() > 0) {
                (void)token->wait_for(timeout);
            }
        }
    } catch (...) {
    }
    try {
        client_->disable_callbacks();
    } catch (...) {
    }
}

TransportFailure PahoMqttTransport::classify(const ::mqtt::exception& error) noexcept {
    return error.get_return_code() == MQTTASYNC_DISCONNECTED
               ? TransportFailure::disconnected
               : TransportFailure::other;
}

void PahoMqttTransport::notifyConnectionLost() noexcept {
    std::lock_guard<std::mutex> lock(observerMutex_);
    if (observer_ != nullptr) {
        observer_->onConnectionLost();
    }
}

void PahoMqttTransport::notifyDeliveryFailure(const TransportFailure failure) noexcept {
    std::lock_guard<std::mutex> lock(observerMutex_);
    if (observer_ != nullptr) {
        observer_->onDeliveryFailure(failure);
    }
}

}  // namespace ocrservice::mqtt
