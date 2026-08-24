#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <mqtt/async_client.h>

#include "DeviceConfig.h"
#include "DeviceResult.h"

namespace ocrservice::tests::embedded {

struct RunOptions final {
    std::string imagePath;
    std::size_t count = 1;
    std::chrono::milliseconds interval{1000};
    std::chrono::seconds timeout{60};
    std::chrono::seconds settle{0};
    std::optional<GateAction> expectedAction;
    bool offlineDuringUpload = false;
    bool expectNoResult = false;
    std::string readyFile;
    std::string continueFile;
};

struct RunSummary final {
    std::size_t accepted = 0;
    std::size_t uniqueResults = 0;
    std::size_t duplicateResults = 0;
    std::size_t actionsExecuted = 0;
};

class EmbeddedSimulator final : private virtual mqtt::callback {
public:
    explicit EmbeddedSimulator(DeviceConfig config);
    ~EmbeddedSimulator() override;

    EmbeddedSimulator(const EmbeddedSimulator&) = delete;
    EmbeddedSimulator& operator=(const EmbeddedSimulator&) = delete;

    RunSummary run(const RunOptions& options);
    void verifyConnectRejected(std::chrono::seconds timeout);
    void verifyNoDelivery(
        const std::string& topic,
        std::chrono::seconds timeout,
        const std::string& readyFile);
    void verifyPersistentSessionHasNoResult(
        const std::string& recognitionId,
        std::chrono::seconds timeout);

private:
    void connect(std::chrono::seconds timeout, bool subscribe);
    void acceptRecognition(const std::string& recognitionId);
    void processResult(DeviceResult result);
    void disconnect() noexcept;
    void connection_lost(const std::string& cause) override;
    void message_arrived(mqtt::const_message_ptr message) override;

    DeviceConfig config_;
    mqtt::async_client client_;
    std::mutex mutex_;
    std::condition_variable condition_;
    ResultDeduplicator deduplicator_;
    std::set<std::string> acceptedIds_;
    std::vector<DeviceResult> pendingResults_;
    std::vector<DeviceResult> uniqueResults_;
    std::size_t receivedMessages_ = 0;
    std::optional<std::string> callbackError_;
};

std::string makeUuid();
std::string currentProtocolTime();

}  // namespace ocrservice::tests::embedded
