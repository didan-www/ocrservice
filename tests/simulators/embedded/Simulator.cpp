#include "Simulator.h"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "HttpUploader.h"

namespace ocrservice::tests::embedded {
namespace {

std::vector<std::uint8_t> readImage(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("image cannot be opened");
    }
    const auto end = input.tellg();
    if (end <= 0 || end > static_cast<std::streamoff>(10U * 1024U * 1024U)) {
        throw std::runtime_error("image size is invalid");
    }
    input.seekg(0);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(end));
    if (!input.read(reinterpret_cast<char*>(result.data()), end)) {
        throw std::runtime_error("image cannot be read");
    }
    return result;
}

void coordinateAfterSubscription(const RunOptions& options) {
    if (options.readyFile.empty() && options.continueFile.empty()) {
        return;
    }
    if (options.readyFile.empty() || options.continueFile.empty()) {
        throw std::runtime_error("both subscription coordination files are required");
    }
    {
        std::ofstream ready(options.readyFile, std::ios::binary | std::ios::trunc);
        if (!ready) {
            throw std::runtime_error("cannot create SUBACK ready marker");
        }
        ready << "ready\n";
    }
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    while (!std::filesystem::exists(options.continueFile)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("timed out waiting after SUBACK coordination");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

mqtt::connect_options connectOptions(const DeviceConfig& config) {
    mqtt::connect_options options;
    options.set_mqtt_version(MQTTVERSION_3_1_1);
    options.set_clean_session(false);
    options.set_user_name(config.mqtt.username);
    options.set_password(config.mqtt.password);
    options.set_keep_alive_interval(std::chrono::seconds(30));
    options.set_connect_timeout(std::chrono::seconds(5));
    options.set_automatic_reconnect(false);
    return options;
}

std::string mqttServerUri(const DeviceConfig& config) {
    auto host = config.mqtt.host;
    if (host.find(':') != std::string::npos && host.front() != '[') {
        host = '[' + host + ']';
    }
    return "tcp://" + host + ':' + std::to_string(config.mqtt.port);
}

}  // namespace

EmbeddedSimulator::EmbeddedSimulator(DeviceConfig config)
    : config_(std::move(config)),
      client_(mqttServerUri(config_), config_.mqtt.clientId) {
    client_.set_callback(*this);
}

EmbeddedSimulator::~EmbeddedSimulator() { disconnect(); }

void EmbeddedSimulator::connect(const std::chrono::seconds timeout, const bool subscribe) {
    auto connected = client_.connect(connectOptions(config_));
    if (!connected->wait_for(timeout)) {
        throw std::runtime_error("MQTT CONNECT timed out");
    }
    if (!client_.is_connected()) {
        throw std::runtime_error("MQTT CONNECT was rejected");
    }
    if (!subscribe) {
        return;
    }
    auto subscribed = client_.subscribe(config_.mqtt.resultTopic, config_.mqtt.qos);
    if (!subscribed->wait_for(timeout)) {
        throw std::runtime_error("MQTT SUBACK timed out");
    }
    const auto subscribeResponse = subscribed->get_subscribe_response();
    const auto& reasonCodes = subscribeResponse.get_reason_codes();
    if (reasonCodes.size() != 1U || static_cast<int>(reasonCodes.front()) != config_.mqtt.qos) {
        const auto code = reasonCodes.empty() ? -1 : static_cast<int>(reasonCodes.front());
        throw std::runtime_error(
            "MQTT SUBACK rejected the exact result topic: reasonCount=" +
            std::to_string(reasonCodes.size()) + " reasonCode=" + std::to_string(code));
    }
}

void EmbeddedSimulator::disconnect() noexcept {
    if (!client_.is_connected()) {
        return;
    }
    try {
        auto token = client_.disconnect(std::chrono::milliseconds(2000));
        (void)token->wait_for(std::chrono::seconds(3));
    } catch (...) {
    }
}

void EmbeddedSimulator::acceptRecognition(const std::string& recognitionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    acceptedIds_.insert(recognitionId);
    for (auto pending = pendingResults_.begin(); pending != pendingResults_.end();) {
        if (pending->recognitionId == recognitionId) {
            processResult(std::move(*pending));
            pending = pendingResults_.erase(pending);
        } else {
            ++pending;
        }
    }
    condition_.notify_all();
}

void EmbeddedSimulator::processResult(DeviceResult result) {
    if (deduplicator_.shouldExecute(result)) {
        uniqueResults_.push_back(std::move(result));
    }
}

RunSummary EmbeddedSimulator::run(const RunOptions& options) {
    if (options.count == 0U || options.count > 1000U || options.interval.count() < 0 ||
        options.timeout < std::chrono::seconds(1)) {
        throw std::runtime_error("simulator options are invalid");
    }
    const auto image = readImage(options.imagePath);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        acceptedIds_.clear();
        pendingResults_.clear();
        uniqueResults_.clear();
        receivedMessages_ = 0U;
        callbackError_.reset();
        deduplicator_ = ResultDeduplicator{};
    }

    connect(options.timeout, true);
    coordinateAfterSubscription(options);
    if (options.offlineDuringUpload) {
        disconnect();
    }

    const auto firstUpload = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < options.count; ++index) {
        if (index > 0U) {
            std::this_thread::sleep_until(firstUpload + options.interval * index);
        }
        const auto captureId = makeUuid();
        const auto accepted = uploadImage(
            config_, image, captureId, currentProtocolTime(),
            static_cast<int>(options.timeout.count()));
        acceptRecognition(accepted.recognitionId);
    }

    if (options.offlineDuringUpload) {
        connect(options.timeout, false);
    }

    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    const auto complete = condition_.wait_until(lock, deadline, [&] {
        return callbackError_.has_value() || uniqueResults_.size() >= options.count;
    });
    if (options.expectNoResult) {
        if (callbackError_) {
            throw std::runtime_error(*callbackError_);
        }
        if (complete || !uniqueResults_.empty()) {
            throw std::runtime_error("unexpected final MQTT result was delivered");
        }
        RunSummary summary;
        summary.accepted = acceptedIds_.size();
        summary.uniqueResults = 0U;
        summary.duplicateResults = 0U;
        summary.actionsExecuted = 0U;
        lock.unlock();
        disconnect();
        return summary;
    }
    if (!complete) {
        throw std::runtime_error("timed out waiting for final MQTT results");
    }
    if (callbackError_) {
        throw std::runtime_error(*callbackError_);
    }
    if (uniqueResults_.size() != options.count) {
        throw std::runtime_error("received an unexpected number of unique results");
    }
    for (const auto& result : uniqueResults_) {
        if (options.expectedAction && result.gateAction != *options.expectedAction) {
            throw std::runtime_error(
                "final MQTT gate action did not match expectation: expected=" +
                std::string(toString(*options.expectedAction)) +
                " actual=" + toString(result.gateAction));
        }
    }
    if (options.settle > std::chrono::seconds::zero()) {
        const auto settleDeadline = std::chrono::steady_clock::now() + options.settle;
        condition_.wait_until(lock, settleDeadline, [&] { return callbackError_.has_value(); });
        if (callbackError_) {
            throw std::runtime_error(*callbackError_);
        }
    }
    RunSummary summary;
    summary.accepted = acceptedIds_.size();
    summary.uniqueResults = uniqueResults_.size();
    summary.duplicateResults = receivedMessages_ - uniqueResults_.size();
    summary.actionsExecuted = deduplicator_.executionCount();
    lock.unlock();
    disconnect();
    return summary;
}

void EmbeddedSimulator::verifyConnectRejected(const std::chrono::seconds timeout) {
    try {
        auto token = client_.connect(connectOptions(config_));
        if (!token->wait_for(timeout)) {
            throw std::runtime_error("MQTT invalid-credential check timed out");
        }
        if (client_.is_connected()) {
            disconnect();
            throw std::runtime_error("MQTT invalid credentials unexpectedly connected");
        }
    } catch (const mqtt::exception&) {
        return;
    }
}

void EmbeddedSimulator::verifyNoDelivery(
    const std::string& topic,
    const std::chrono::seconds timeout,
    const std::string& readyFile) {
    if (topic.empty() || topic == config_.mqtt.resultTopic) {
        throw std::runtime_error("ACL probe topic must differ from the own result topic");
    }
    connect(timeout, false);
    try {
        auto token = client_.subscribe(topic, config_.mqtt.qos);
        if (!token->wait_for(timeout)) {
            throw std::runtime_error("ACL probe SUBACK timed out");
        }
        const auto subscribeResponse = token->get_subscribe_response();
        const auto& reasonCodes = subscribeResponse.get_reason_codes();
        if (reasonCodes.size() != 1U) {
            throw std::runtime_error("ACL probe returned an invalid SUBACK");
        }
        if (static_cast<int>(reasonCodes.front()) == 0x80) {
            if (!readyFile.empty()) {
                std::ofstream(readyFile, std::ios::binary | std::ios::trunc) << "rejected\n";
            }
            disconnect();
            return;
        }
    } catch (const mqtt::exception&) {
        disconnect();
        return;
    }
    if (!readyFile.empty()) {
        std::ofstream ready(readyFile, std::ios::binary | std::ios::trunc);
        if (!ready) {
            disconnect();
            throw std::runtime_error("cannot create ACL SUBACK ready marker");
        }
        ready << "ready\n";
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const auto delivered = condition_.wait_for(lock, timeout, [&] {
        return callbackError_.has_value() || receivedMessages_ != 0U;
    });
    lock.unlock();
    disconnect();
    if (delivered) {
        throw std::runtime_error("device received a message through a forbidden subscription");
    }
}

void EmbeddedSimulator::verifyPersistentSessionHasNoResult(
    const std::string& recognitionId,
    const std::chrono::seconds timeout) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        acceptedIds_.insert(recognitionId);
        receivedMessages_ = 0U;
        callbackError_.reset();
    }
    connect(timeout, false);
    std::unique_lock<std::mutex> lock(mutex_);
    const auto delivered = condition_.wait_for(lock, timeout, [&] {
        return callbackError_.has_value() || receivedMessages_ != 0U;
    });
    lock.unlock();
    disconnect();
    if (delivered) {
        throw std::runtime_error("lost server publish was unexpectedly replayed after reconnect");
    }
}

void EmbeddedSimulator::connection_lost(const std::string&) {
    condition_.notify_all();
}

void EmbeddedSimulator::message_arrived(mqtt::const_message_ptr message) {
    try {
        if (!message || message->get_topic() != config_.mqtt.resultTopic ||
            message->get_qos() != config_.mqtt.qos || message->is_retained()) {
            throw std::runtime_error("MQTT result transport properties are invalid");
        }
        auto result = parseDeviceResult(message->to_string());
        if (result.deviceId != config_.deviceId) {
            throw std::runtime_error("MQTT result deviceId does not match the provisioned device");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++receivedMessages_;
        if (acceptedIds_.count(result.recognitionId) == 0U) {
            pendingResults_.push_back(std::move(result));
        } else {
            processResult(std::move(result));
        }
        condition_.notify_all();
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        callbackError_ = error.what();
        condition_.notify_all();
    }
}

std::string makeUuid() {
    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random());
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            output << '-';
        }
        output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return output.str();
}

std::string currentProtocolTime() {
    const auto now = std::chrono::system_clock::now() + std::chrono::hours(8);
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
    const auto raw = std::chrono::system_clock::to_time_t(seconds);
    std::tm value{};
#ifdef _WIN32
    gmtime_s(&value, &raw);
#else
    gmtime_r(&raw, &value);
#endif
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << milliseconds << "+08:00";
    return output.str();
}

}  // namespace ocrservice::tests::embedded
