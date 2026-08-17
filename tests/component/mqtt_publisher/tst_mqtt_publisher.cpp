#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "LoggerFactory.h"
#include "MqttPayload.h"
#include "MqttTransport.h"
#include "PahoMqttPublisher.h"
#include "ProtocolTime.h"
#include "Recognition.h"
#include "ReconnectDelay.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::domain::DeviceId;
using ocrservice::domain::GateAction;
using ocrservice::domain::PlateNumber;
using ocrservice::domain::PublishFailure;
using ocrservice::domain::RecognitionFailureCode;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionSnapshot;
using ocrservice::domain::RecognitionStatus;
using ocrservice::mqtt::IMqttTransport;
using ocrservice::mqtt::IMqttTransportObserver;
using ocrservice::mqtt::IReconnectDelay;
using ocrservice::mqtt::MqttClientConfiguration;
using ocrservice::mqtt::MqttTransportError;
using ocrservice::mqtt::PahoMqttPublisher;
using ocrservice::mqtt::TransportFailure;

struct PublishedMessage final {
    std::string topic;
    std::string payload;
    int qos;
    bool retained;
};

class FakeTransport final : public IMqttTransport {
public:
    void setObserver(IMqttTransportObserver* observer) noexcept override {
        std::lock_guard<std::mutex> lock(observerMutex_);
        observer_ = observer;
    }

    void connect(const MqttClientConfiguration& configuration) override {
        std::optional<TransportFailure> failure;
        std::chrono::milliseconds delay;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            configurations_.push_back(configuration);
            if (!connectFailures_.empty()) {
                failure = connectFailures_.front();
                connectFailures_.pop_front();
            }
            delay = connectDelay_;
        }
        connectCalls_.fetch_add(1U, std::memory_order_release);
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
        if (failure.has_value()) {
            throw MqttTransportError(*failure);
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            connected_ = true;
        }
    }

    bool isConnected() const noexcept override {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return connected_;
    }

    void publish(
        const std::string_view topic,
        const std::string_view payload,
        const int qos,
        const bool retained) override {
        std::optional<TransportFailure> failure;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (publishFailure_.has_value()) {
                failure = publishFailure_;
                publishFailure_.reset();
            } else {
                published_.push_back(
                    {std::string(topic), std::string(payload), qos, retained});
            }
        }
        if (failure.has_value()) {
            throw MqttTransportError(*failure);
        }
    }

    void disconnect(std::chrono::milliseconds timeout) noexcept override {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connected_ = false;
        disconnectTimeout_ = timeout;
    }

    void queueConnectFailure(const TransportFailure failure) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connectFailures_.push_back(failure);
    }

    void setPublishFailure(const TransportFailure failure) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        publishFailure_ = failure;
    }

    void setConnectDelay(const std::chrono::milliseconds delay) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        connectDelay_ = delay;
    }

    void loseConnection() {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            connected_ = false;
        }
        std::lock_guard<std::mutex> observerLock(observerMutex_);
        if (observer_ != nullptr) {
            observer_->onConnectionLost();
        }
    }

    void failDelivery(const TransportFailure failure) {
        std::lock_guard<std::mutex> lock(observerMutex_);
        if (observer_ != nullptr) {
            observer_->onDeliveryFailure(failure);
        }
    }

    bool waitForConnectCalls(const std::size_t count, const std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (connectCalls() >= count) {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return connectCalls() >= count;
    }

    std::size_t connectCalls() const {
        return connectCalls_.load(std::memory_order_acquire);
    }

    std::vector<MqttClientConfiguration> configurations() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return configurations_;
    }

    std::vector<PublishedMessage> published() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return published_;
    }

    std::optional<std::chrono::milliseconds> disconnectTimeout() const {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return disconnectTimeout_;
    }

    bool hasObserver() const {
        std::lock_guard<std::mutex> lock(observerMutex_);
        return observer_ != nullptr;
    }

private:
    mutable std::mutex observerMutex_;
    IMqttTransportObserver* observer_ = nullptr;
    mutable std::mutex stateMutex_;
    bool connected_ = false;
    std::atomic<std::size_t> connectCalls_{0U};
    std::deque<TransportFailure> connectFailures_;
    std::optional<TransportFailure> publishFailure_;
    std::chrono::milliseconds connectDelay_{0};
    std::vector<MqttClientConfiguration> configurations_;
    std::vector<PublishedMessage> published_;
    std::optional<std::chrono::milliseconds> disconnectTimeout_;
};

class RecordingDelay final : public IReconnectDelay {
public:
    std::chrono::milliseconds jittered(
        const std::chrono::milliseconds baseDelay) override {
        std::lock_guard<std::mutex> lock(mutex_);
        baseDelays_.push_back(baseDelay);
        return baseDelay;
    }

    bool waitFor(const std::chrono::milliseconds delay) override {
        std::lock_guard<std::mutex> lock(mutex_);
        waitedDelays_.push_back(delay);
        return !interrupted_;
    }

    void interrupt() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupted_ = true;
    }

    std::vector<std::chrono::milliseconds> baseDelays() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return baseDelays_;
    }

private:
    mutable std::mutex mutex_;
    bool interrupted_ = false;
    std::vector<std::chrono::milliseconds> baseDelays_;
    std::vector<std::chrono::milliseconds> waitedDelays_;
};

MqttClientConfiguration configuration() {
    return {
        "tcp://127.0.0.1:1883",
        ocrservice::mqtt::kServerClientId,
        "plate-server",
        "test-only-password",
        ocrservice::mqtt::kMqtt311ProtocolVersion,
        true,
        5s,
        60s,
        2s,
    };
}

std::shared_ptr<ocrservice::logging::JsonLinesLogger> logger() {
    static std::atomic<unsigned int> sequence{0U};
    return ocrservice::logging::LoggerFactory::createConsole(
        "mqtt-publisher-test-" + std::to_string(sequence.fetch_add(1U)));
}

RecognitionId recognitionId() {
    return RecognitionId::parse("a15c7268-b211-4a4c-a765-49b922af1910");
}

auto at(const std::string& value) {
    return ocrservice::serialization::time::parseProtocolTime(value);
}

RecognitionSnapshot processing() {
    return RecognitionSnapshot(
        recognitionId(), 1U, DeviceId::parse("device-001"), RecognitionStatus::processing,
        std::nullopt, std::nullopt, std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"), std::nullopt, std::nullopt);
}

RecognitionSnapshot succeeded() {
    return RecognitionSnapshot(
        recognitionId(), 2U, DeviceId::parse("device-001"), RecognitionStatus::succeeded,
        PlateNumber::parse(u8"京A12345"), std::nullopt, std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"), 120U);
}

RecognitionSnapshot failed() {
    return RecognitionSnapshot(
        recognitionId(), 2U, DeviceId::parse("device-001"), RecognitionStatus::failed,
        std::nullopt, RecognitionFailureCode::modelInferenceError,
        std::string(u8"模型推理失败"), at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"), 120U);
}

bool waitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(MqttPublisherTest, UsesFixedConnectionSettingsAndCappedBackoff) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    for (std::size_t index = 0; index < 6U; ++index) {
        fakePtr->queueConnectFailure(TransportFailure::disconnected);
    }
    auto delay = std::make_unique<RecordingDelay>();
    auto* delayPtr = delay.get();
    std::atomic<unsigned int> observerCalls{0U};
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay),
        [&observerCalls](ocrservice::domain::IMqttPublisher&) { ++observerCalls; });

    publisher.start();
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 2s));
    ASSERT_TRUE(waitUntil([&observerCalls] { return observerCalls.load() == 1U; }, 2s));
    EXPECT_EQ(observerCalls.load(), 1U);
    EXPECT_EQ(
        delayPtr->baseDelays(),
        (std::vector<std::chrono::milliseconds>{1s, 2s, 4s, 8s, 16s, 30s}));

    const auto settings = fakePtr->configurations();
    ASSERT_GE(settings.size(), 7U);
    EXPECT_EQ(settings.back().clientId, "plate-server");
    EXPECT_EQ(settings.back().protocolVersion, 4);
    EXPECT_TRUE(settings.back().cleanSession);
    EXPECT_EQ(settings.back().connectTimeout, 5s);
    EXPECT_EQ(settings.back().keepAlive, 60s);

    fakePtr->loseConnection();
    ASSERT_TRUE(fakePtr->waitForConnectCalls(8U, 2s));
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 2s));
    EXPECT_EQ(observerCalls.load(), 1U);

    publisher.stop();
    publisher.stop();
    ASSERT_TRUE(fakePtr->disconnectTimeout().has_value());
    EXPECT_EQ(*fakePtr->disconnectTimeout(), 2s);
    EXPECT_THROW(publisher.start(), std::logic_error);
}

TEST(MqttPublisherTest, FirstConnectedObserverCanPublishThroughTheProvidedPort) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    std::atomic<unsigned int> observerCalls{0U};
    std::atomic<bool> publishAccepted{false};
    std::atomic<bool> observerCompleted{false};
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay),
        [&observerCalls, &publishAccepted, &observerCompleted](
            ocrservice::domain::IMqttPublisher& port) {
            ++observerCalls;
            publishAccepted.store(
                port.publishManagement(processing()).wasAccepted(),
                std::memory_order_release);
            observerCompleted.store(true, std::memory_order_release);
        });

    publisher.start();
    ASSERT_TRUE(waitUntil(
        [&observerCompleted] { return observerCompleted.load(std::memory_order_acquire); },
        1s));
    EXPECT_EQ(observerCalls.load(), 1U);
    EXPECT_TRUE(publishAccepted.load(std::memory_order_acquire));
    EXPECT_EQ(fakePtr->published().front().topic, ocrservice::mqtt::kManagementTopic);
    publisher.stop();
    EXPECT_FALSE(fakePtr->hasObserver());
}

TEST(MqttPublisherTest, StopWaitsForInFlightFirstConnectedObserverAndDetachesIt) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    std::atomic<bool> observerEntered{false};
    std::atomic<bool> allowObserverReturn{false};
    std::atomic<bool> observerReturned{false};
    std::atomic<bool> stopStarted{false};
    std::atomic<bool> stopReturned{false};
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay),
        [&observerEntered, &allowObserverReturn, &observerReturned](
            ocrservice::domain::IMqttPublisher&) {
            observerEntered.store(true, std::memory_order_release);
            while (!allowObserverReturn.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            observerReturned.store(true, std::memory_order_release);
        });

    publisher.start();
    ASSERT_TRUE(waitUntil(
        [&observerEntered] { return observerEntered.load(std::memory_order_acquire); },
        1s));

    std::thread stopper([&publisher, &stopStarted, &stopReturned] {
        stopStarted.store(true, std::memory_order_release);
        publisher.stop();
        stopReturned.store(true, std::memory_order_release);
    });
    const bool stopperDidStart = waitUntil(
        [&stopStarted] { return stopStarted.load(std::memory_order_acquire); }, 1s);
    EXPECT_TRUE(stopperDidStart);
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));

    allowObserverReturn.store(true, std::memory_order_release);
    stopper.join();
    EXPECT_TRUE(observerReturned.load(std::memory_order_acquire));
    EXPECT_TRUE(stopReturned.load(std::memory_order_acquire));
    EXPECT_FALSE(fakePtr->hasObserver());
}

TEST(MqttPublisherTest, ObserverExceptionDoesNotStopControlThreadOrRepeatAfterReconnect) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    std::atomic<unsigned int> observerCalls{0U};
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay),
        [&observerCalls](ocrservice::domain::IMqttPublisher&) {
            ++observerCalls;
            throw std::runtime_error("test observer failure");
        });

    publisher.start();
    ASSERT_TRUE(waitUntil([&observerCalls] { return observerCalls.load() == 1U; }, 1s));
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 1s));

    fakePtr->loseConnection();
    ASSERT_TRUE(fakePtr->waitForConnectCalls(2U, 1s));
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 1s));
    EXPECT_EQ(observerCalls.load(), 1U);
    EXPECT_TRUE(publisher.publishManagement(processing()).wasAccepted());
    publisher.stop();
    EXPECT_FALSE(fakePtr->hasObserver());
}

TEST(MqttPublisherTest, StartIsNonBlockingAndStopRejectsNewPublishes) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    fakePtr->setConnectDelay(200ms);
    auto delay = std::make_unique<RecordingDelay>();
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay));

    const auto startedAt = std::chrono::steady_clock::now();
    publisher.start();
    EXPECT_LT(std::chrono::steady_clock::now() - startedAt, 50ms);
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 1s));
    publisher.stop();
    EXPECT_FALSE(publisher.isConnected());
    const auto attempt = publisher.publishManagement(processing());
    EXPECT_FALSE(attempt.wasAccepted());
    ASSERT_TRUE(attempt.failure().has_value());
    EXPECT_EQ(*attempt.failure(), PublishFailure::stopping);
}

TEST(MqttPublisherTest, PublishesEveryExplicitCallWithFixedQosAndRetain) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay));
    publisher.start();
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 1s));

    EXPECT_TRUE(publisher.publishManagement(processing()).wasAccepted());
    EXPECT_TRUE(publisher.publishDeviceFinal(succeeded(), GateAction::open).wasAccepted());
    EXPECT_TRUE(publisher.publishDeviceFinal(succeeded(), GateAction::open).wasAccepted());

    const auto messages = fakePtr->published();
    ASSERT_EQ(messages.size(), 3U);
    EXPECT_EQ(messages[0].topic, ocrservice::mqtt::kManagementTopic);
    EXPECT_EQ(messages[1].topic, "plate/devices/device-001/recognition-results");
    EXPECT_EQ(messages[1].payload, messages[2].payload);
    for (const auto& message : messages) {
        EXPECT_EQ(message.qos, 1);
        EXPECT_FALSE(message.retained);
    }
    publisher.stop();
}

TEST(MqttPublisherTest, MapsTransportFailuresWithoutThrowing) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay));
    publisher.start();
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 1s));

    for (const auto& [transportFailure, expected] : {
             std::pair{TransportFailure::rejected, PublishFailure::brokerRejected},
             std::pair{TransportFailure::other, PublishFailure::transportError},
         }) {
        fakePtr->setPublishFailure(transportFailure);
        const auto attempt = publisher.publishManagement(processing());
        EXPECT_FALSE(attempt.wasAccepted());
        ASSERT_TRUE(attempt.failure().has_value());
        EXPECT_EQ(*attempt.failure(), expected);
    }

    fakePtr->setPublishFailure(TransportFailure::disconnected);
    const auto disconnected = publisher.publishManagement(processing());
    ASSERT_TRUE(disconnected.failure().has_value());
    EXPECT_EQ(*disconnected.failure(), PublishFailure::notConnected);
    fakePtr->failDelivery(TransportFailure::other);
    publisher.stop();
}

TEST(MqttPublisherTest, RejectsIllegalDeviceMessagesBeforeTransport) {
    auto fake = std::make_unique<FakeTransport>();
    auto* fakePtr = fake.get();
    auto delay = std::make_unique<RecordingDelay>();
    PahoMqttPublisher publisher(
        configuration(), logger(), std::move(fake), std::move(delay));

    EXPECT_THROW(
        publisher.publishDeviceFinal(processing(), GateAction::keepClosed),
        std::invalid_argument);
    EXPECT_THROW(
        publisher.publishDeviceFinal(succeeded(), GateAction::keepClosed),
        std::invalid_argument);
    EXPECT_THROW(
        publisher.publishDeviceFinal(failed(), GateAction::open),
        std::invalid_argument);
    EXPECT_TRUE(fakePtr->published().empty());
}

TEST(MqttConfigurationTest, BuildsOnlyTheFixedPlaintextBrokerUri) {
    ocrservice::app::config::MqttConfig config;
    config.host = "broker.local";
    config.port = 1883U;
    config.serverUsername = "plate-server";
    config.serverPassword = "test-only-password";
    auto value = ocrservice::mqtt::makeMqttClientConfiguration(config);
    EXPECT_EQ(value.brokerUri, "tcp://broker.local:1883");
    EXPECT_EQ(value.clientId, "plate-server");

    config.host = "2001:db8::1";
    value = ocrservice::mqtt::makeMqttClientConfiguration(config);
    EXPECT_EQ(value.brokerUri, "tcp://[2001:db8::1]:1883");

    config.host = "[2001:db8::1]";
    value = ocrservice::mqtt::makeMqttClientConfiguration(config);
    EXPECT_EQ(value.brokerUri, "tcp://[2001:db8::1]:1883");

    for (const auto* invalid : {
             "tcp://broker",
             "broker/path",
             "bad host",
             "bad:host",
             "broker.local:1883",
             "127.0.0.1:1883",
             "[broken",
             "broken]",
             "bad[host]name",
             "[bad:host]",
             "[]",
             "[2001:db8::1]:1883",
             "[2001:db8::1]]",
         }) {
        config.host = invalid;
        EXPECT_THROW(
            ocrservice::mqtt::makeMqttClientConfiguration(config),
            std::invalid_argument);
    }
}

TEST(ReconnectDelayTest, ProductionJitterStaysWithinTwentyPercent) {
    ocrservice::mqtt::SystemReconnectDelay delay;
    for (std::size_t index = 0; index < 200U; ++index) {
        const auto value = delay.jittered(1000ms);
        EXPECT_GE(value, 800ms);
        EXPECT_LE(value, 1200ms);
    }
    delay.interrupt();
    EXPECT_FALSE(delay.waitFor(1s));
}

}  // namespace
