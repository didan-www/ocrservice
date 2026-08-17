#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mqtt/async_client.h>
#include <mqtt/connect_options.h>

#include "LoggerFactory.h"
#include "MqttPayload.h"
#include "PahoMqttPublisher.h"
#include "ProtocolTime.h"
#include "Recognition.h"
#include "ServerConfig.h"

namespace {

using namespace std::chrono_literals;

std::optional<std::string> environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::string requireEnvironment(const char* name) {
    const auto value = environment(name);
    if (!value.has_value()) {
        throw std::runtime_error(std::string("missing test environment: ") + name);
    }
    return *value;
}

class BrokerProcess final {
public:
    BrokerProcess(std::string executable, std::string configuration)
        : executable_(std::move(executable)), configuration_(std::move(configuration)) {}

    ~BrokerProcess() { stop(); }

    void start() {
        if (pid_ > 0) {
            throw std::logic_error("test Broker is already running");
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            throw std::runtime_error("could not fork test Broker");
        }
        if (pid_ == 0) {
            ::execl(
                executable_.c_str(),
                executable_.c_str(),
                "-c",
                configuration_.c_str(),
                static_cast<char*>(nullptr));
            _exit(127);
        }
    }

    void stop() noexcept {
        if (pid_ <= 0) {
            return;
        }
        (void)::kill(pid_, SIGTERM);
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

private:
    std::string executable_;
    std::string configuration_;
    pid_t pid_ = -1;
};

class Subscriber final {
public:
    Subscriber(
        const std::string& brokerUri,
        const std::string& clientId,
        const std::string& username,
        const std::string& password,
        const std::string& topicFilter)
        : client_(brokerUri, clientId) {
        mqtt::connect_options options(MQTTVERSION_3_1_1);
        options.set_clean_session(true);
        options.set_connect_timeout(5);
        options.set_user_name(username);
        options.set_password(password);
        client_.start_consuming();
        client_.connect(std::move(options))->wait();
        client_.subscribe(topicFilter, 1)->wait();
    }

    ~Subscriber() {
        try {
            if (client_.is_connected()) {
                client_.disconnect()->wait_for(1s);
            }
        } catch (...) {
        }
        try {
            client_.stop_consuming();
        } catch (...) {
        }
    }

    mqtt::const_message_ptr next(const std::chrono::milliseconds timeout) {
        return client_.try_consume_message_for(timeout);
    }

private:
    mqtt::async_client client_;
};

bool waitUntil(const std::function<bool()>& predicate, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

auto at(const std::string& value) {
    return ocrservice::serialization::time::parseProtocolTime(value);
}

ocrservice::domain::RecognitionSnapshot processingSnapshot() {
    return ocrservice::domain::RecognitionSnapshot(
        ocrservice::domain::RecognitionId::parse(
            "a15c7268-b211-4a4c-a765-49b922af1910"),
        1U,
        ocrservice::domain::DeviceId::parse("device-real-001"),
        ocrservice::domain::RecognitionStatus::processing,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        std::nullopt,
        std::nullopt);
}

ocrservice::domain::RecognitionSnapshot succeededSnapshot() {
    return ocrservice::domain::RecognitionSnapshot(
        ocrservice::domain::RecognitionId::parse(
            "a15c7268-b211-4a4c-a765-49b922af1910"),
        2U,
        ocrservice::domain::DeviceId::parse("device-real-001"),
        ocrservice::domain::RecognitionStatus::succeeded,
        ocrservice::domain::PlateNumber::parse(u8"京A12345"),
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:30:44.000+08:00"),
        at("2026-08-15T12:30:45.000+08:00"),
        at("2026-08-15T12:30:45.120+08:00"),
        120U);
}

ocrservice::domain::RecognitionSnapshot otherDeviceSucceededSnapshot() {
    return ocrservice::domain::RecognitionSnapshot(
        ocrservice::domain::RecognitionId::parse(
            "45bca8ea-df5b-49ed-9744-441d3d93ab70"),
        2U,
        ocrservice::domain::DeviceId::parse("device-other"),
        ocrservice::domain::RecognitionStatus::succeeded,
        ocrservice::domain::PlateNumber::parse("TEST123"),
        std::nullopt,
        std::nullopt,
        at("2026-08-15T12:31:44.000+08:00"),
        at("2026-08-15T12:31:45.000+08:00"),
        at("2026-08-15T12:31:45.120+08:00"),
        120U);
}

ocrservice::app::config::MqttConfig publisherConfig(
    const std::string& port,
    std::string password) {
    ocrservice::app::config::MqttConfig config;
    config.host = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(std::stoul(port));
    config.serverUsername = "plate-server";
    config.serverPassword = std::move(password);
    return config;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

TEST(MqttPublisherRealTest, CoversAclQosRetainReconnectDuplicatesAndSecrets) {
    if (!environment("OCRSERVICE_MQTT_REAL_TEST").has_value()) {
        GTEST_SKIP() << "real Mosquitto test is orchestrated by its CTest wrapper";
    }

    const auto brokerExecutable = requireEnvironment("OCRSERVICE_MOSQUITTO_BIN");
    const auto brokerConfig = requireEnvironment("OCRSERVICE_MQTT_BROKER_CONFIG");
    const auto port = requireEnvironment("OCRSERVICE_MQTT_TEST_PORT");
    const auto serverPassword = requireEnvironment("OCRSERVICE_MQTT_SERVER_PASSWORD");
    const auto managementPassword = requireEnvironment("OCRSERVICE_MQTT_MANAGEMENT_PASSWORD");
    const auto devicePassword = requireEnvironment("OCRSERVICE_MQTT_DEVICE_PASSWORD");
    const auto logRoot = std::filesystem::path(requireEnvironment("OCRSERVICE_MQTT_LOG_ROOT"));
    const auto uri = "tcp://127.0.0.1:" + port;

    BrokerProcess broker(brokerExecutable, brokerConfig);
    broker.start();

    auto wrongLogger = ocrservice::logging::LoggerFactory::createRotatingFile(
        "mqtt-real-wrong", logRoot / "wrong");
    ocrservice::mqtt::PahoMqttPublisher wrongPublisher(
        publisherConfig(port, "wrong-test-password"), wrongLogger);
    const auto wrongStarted = std::chrono::steady_clock::now();
    wrongPublisher.start();
    EXPECT_LT(std::chrono::steady_clock::now() - wrongStarted, 100ms);
    std::this_thread::sleep_for(750ms);
    EXPECT_FALSE(wrongPublisher.isConnected());
    wrongPublisher.stop();
    wrongLogger->flush();

    std::atomic<unsigned int> observerCalls{0U};
    auto publisherLogger = ocrservice::logging::LoggerFactory::createRotatingFile(
        "mqtt-real-publisher", logRoot / "publisher");
    ocrservice::mqtt::PahoMqttPublisher publisher(
        publisherConfig(port, serverPassword),
        publisherLogger,
        [&observerCalls](ocrservice::domain::IMqttPublisher&) { ++observerCalls; });
    publisher.start();
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 10s));
    ASSERT_TRUE(waitUntil([&observerCalls] { return observerCalls.load() == 1U; }, 10s));
    EXPECT_EQ(observerCalls.load(), 1U);

    {
        Subscriber management(
            uri, "management-real-test", "management-client", managementPassword, "plate/#");
        Subscriber device(
            uri, "device-real-test", "device-real-001", devicePassword, "plate/#");
        Subscriber serverReadProbe(
            uri, "server-read-probe", "plate-server", serverPassword, "plate/#");

        ASSERT_TRUE(publisher.publishManagement(processingSnapshot()).wasAccepted());
        ASSERT_TRUE(
            publisher
                .publishDeviceFinal(
                    succeededSnapshot(), ocrservice::domain::GateAction::open)
                .wasAccepted());

        const auto managementMessage = management.next(5s);
        const auto deviceMessage = device.next(5s);
        ASSERT_TRUE(managementMessage);
        ASSERT_TRUE(deviceMessage);
        EXPECT_EQ(managementMessage->get_topic(), ocrservice::mqtt::kManagementTopic);
        EXPECT_EQ(
            managementMessage->get_payload_str(),
            ocrservice::mqtt::makeManagementMessage(processingSnapshot()).payload);
        EXPECT_EQ(managementMessage->get_qos(), 1);
        EXPECT_FALSE(managementMessage->is_retained());
        EXPECT_EQ(
            deviceMessage->get_topic(),
            "plate/devices/device-real-001/recognition-results");
        EXPECT_EQ(
            deviceMessage->get_payload_str(),
            ocrservice::mqtt::makeDeviceFinalMessage(
                succeededSnapshot(), ocrservice::domain::GateAction::open)
                .payload);
        EXPECT_EQ(deviceMessage->get_qos(), 1);
        EXPECT_FALSE(deviceMessage->is_retained());
        EXPECT_FALSE(management.next(400ms));
        EXPECT_FALSE(device.next(400ms));
        EXPECT_FALSE(serverReadProbe.next(400ms));

        ASSERT_TRUE(
            publisher
                .publishDeviceFinal(
                    succeededSnapshot(), ocrservice::domain::GateAction::open)
                .wasAccepted());
        ASSERT_TRUE(
            publisher
                .publishDeviceFinal(
                    succeededSnapshot(), ocrservice::domain::GateAction::open)
                .wasAccepted());
        const auto duplicateOne = device.next(5s);
        const auto duplicateTwo = device.next(5s);
        ASSERT_TRUE(duplicateOne);
        ASSERT_TRUE(duplicateTwo);
        EXPECT_EQ(duplicateOne->get_payload_str(), duplicateTwo->get_payload_str());

        ASSERT_TRUE(publisher.publishManagement(succeededSnapshot()).wasAccepted());
        ASSERT_TRUE(management.next(5s));

        ASSERT_TRUE(
            publisher
                .publishDeviceFinal(
                    otherDeviceSucceededSnapshot(),
                    ocrservice::domain::GateAction::open)
                .wasAccepted());
        EXPECT_FALSE(device.next(750ms));
        EXPECT_FALSE(management.next(750ms));
    }

    {
        Subscriber freshManagement(
            uri,
            "management-retain-probe",
            "management-client",
            managementPassword,
            ocrservice::mqtt::kManagementTopic);
        EXPECT_FALSE(freshManagement.next(750ms));
    }

    broker.stop();
    ASSERT_TRUE(waitUntil([&publisher] { return !publisher.isConnected(); }, 10s));
    const auto offline = publisher.publishManagement(processingSnapshot());
    EXPECT_FALSE(offline.wasAccepted());
    ASSERT_TRUE(offline.failure().has_value());
    EXPECT_EQ(*offline.failure(), ocrservice::domain::PublishFailure::notConnected);

    broker.start();
    ASSERT_TRUE(waitUntil([&publisher] { return publisher.isConnected(); }, 15s));
    EXPECT_EQ(observerCalls.load(), 1U);
    {
        Subscriber afterReconnect(
            uri,
            "management-after-reconnect",
            "management-client",
            managementPassword,
            ocrservice::mqtt::kManagementTopic);
        ASSERT_TRUE(publisher.publishManagement(succeededSnapshot()).wasAccepted());
        ASSERT_TRUE(afterReconnect.next(5s));
    }

    publisher.stop();
    publisherLogger->flush();
    broker.stop();

    const auto publisherLog = readFile(logRoot / "publisher" / "ocrservice.jsonl");
    const auto wrongLog = readFile(logRoot / "wrong" / "ocrservice.jsonl");
    EXPECT_EQ(publisherLog.find(serverPassword), std::string::npos);
    EXPECT_EQ(publisherLog.find(managementPassword), std::string::npos);
    EXPECT_EQ(publisherLog.find(devicePassword), std::string::npos);
    EXPECT_EQ(wrongLog.find("wrong-test-password"), std::string::npos);
    EXPECT_EQ(
        publisherLog.find(
            ocrservice::mqtt::makeManagementMessage(processingSnapshot()).payload),
        std::string::npos);
    EXPECT_NE(publisherLog.find("\"requestId\":\"system\""), std::string::npos);
}

}  // namespace
