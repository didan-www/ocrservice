#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "DeviceConfig.h"
#include "Simulator.h"

namespace {

struct Arguments final {
    std::string configPath;
    std::string imagePath;
    std::size_t count = 1U;
    int intervalMs = 1000;
    int timeoutSeconds = 60;
    int settleSeconds = 0;
    std::optional<ocrservice::tests::embedded::GateAction> expectedAction;
    bool offlineDuringUpload = false;
    bool expectConnectFailure = false;
    bool expectNoResult = false;
    std::string readyFile;
    std::string continueFile;
    std::string forbiddenTopic;
    std::string persistentNoResultId;
};

std::size_t number(const std::string& value, const char* name) {
    std::size_t consumed = 0U;
    unsigned long long parsed = 0U;
    try {
        parsed = std::stoull(value, &consumed, 10);
    } catch (...) {
        throw std::runtime_error(std::string(name) + " must be an integer");
    }
    if (consumed != value.size()) {
        throw std::runtime_error(std::string(name) + " must be an integer");
    }
    return static_cast<std::size_t>(parsed);
}

Arguments parseArguments(const int argc, char** argv) {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const auto value = [&](const char* name) {
            if (index + 1 >= argc) {
                throw std::runtime_error(std::string(name) + " requires a value");
            }
            return std::string(argv[++index]);
        };
        if (option == "--config") {
            result.configPath = value("--config");
        } else if (option == "--image") {
            result.imagePath = value("--image");
        } else if (option == "--count") {
            result.count = number(value("--count"), "--count");
        } else if (option == "--interval-ms") {
            result.intervalMs = static_cast<int>(number(value("--interval-ms"), "--interval-ms"));
        } else if (option == "--timeout-seconds") {
            result.timeoutSeconds = static_cast<int>(number(value("--timeout-seconds"), "--timeout-seconds"));
        } else if (option == "--settle-seconds") {
            result.settleSeconds = static_cast<int>(number(value("--settle-seconds"), "--settle-seconds"));
        } else if (option == "--expect-action") {
            const auto action = value("--expect-action");
            if (action == "OPEN") {
                result.expectedAction = ocrservice::tests::embedded::GateAction::open;
            } else if (action == "KEEP_CLOSED") {
                result.expectedAction = ocrservice::tests::embedded::GateAction::keepClosed;
            } else {
                throw std::runtime_error("--expect-action must be OPEN or KEEP_CLOSED");
            }
        } else if (option == "--offline-during-upload") {
            result.offlineDuringUpload = true;
        } else if (option == "--expect-connect-failure") {
            result.expectConnectFailure = true;
        } else if (option == "--expect-no-result") {
            result.expectNoResult = true;
        } else if (option == "--ready-file") {
            result.readyFile = value("--ready-file");
        } else if (option == "--continue-file") {
            result.continueFile = value("--continue-file");
        } else if (option == "--probe-forbidden-topic") {
            result.forbiddenTopic = value("--probe-forbidden-topic");
        } else if (option == "--persistent-no-result-id") {
            result.persistentNoResultId = value("--persistent-no-result-id");
        } else {
            throw std::runtime_error("unknown simulator option");
        }
    }
    const bool noImageMode = result.expectConnectFailure || !result.forbiddenTopic.empty() ||
                             !result.persistentNoResultId.empty();
    if (result.configPath.empty() || (!noImageMode && result.imagePath.empty())) {
        throw std::runtime_error("--config and --image are required");
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto arguments = parseArguments(argc, argv);
        auto config = ocrservice::tests::embedded::loadDeviceConfig(arguments.configPath);
        ocrservice::tests::embedded::EmbeddedSimulator simulator(std::move(config));
        if (arguments.expectConnectFailure) {
            simulator.verifyConnectRejected(std::chrono::seconds(arguments.timeoutSeconds));
            std::cout << "connectRejected=true\n";
            return EXIT_SUCCESS;
        }
        if (!arguments.forbiddenTopic.empty()) {
            simulator.verifyNoDelivery(
                arguments.forbiddenTopic, std::chrono::seconds(arguments.timeoutSeconds),
                arguments.readyFile);
            std::cout << "forbiddenDelivery=false\n";
            return EXIT_SUCCESS;
        }
        if (!arguments.persistentNoResultId.empty()) {
            simulator.verifyPersistentSessionHasNoResult(
                arguments.persistentNoResultId,
                std::chrono::seconds(arguments.timeoutSeconds));
            std::cout << "replayedResult=false\n";
            return EXIT_SUCCESS;
        }
        ocrservice::tests::embedded::RunOptions options;
        options.imagePath = arguments.imagePath;
        options.count = arguments.count;
        options.interval = std::chrono::milliseconds(arguments.intervalMs);
        options.timeout = std::chrono::seconds(arguments.timeoutSeconds);
        options.settle = std::chrono::seconds(arguments.settleSeconds);
        options.expectedAction = arguments.expectedAction;
        options.offlineDuringUpload = arguments.offlineDuringUpload;
        options.expectNoResult = arguments.expectNoResult;
        options.readyFile = arguments.readyFile;
        options.continueFile = arguments.continueFile;
        const auto summary = simulator.run(options);
        std::cout << "accepted=" << summary.accepted
                  << " uniqueResults=" << summary.uniqueResults
                  << " duplicateResults=" << summary.duplicateResults
                  << " actionsExecuted=" << summary.actionsExecuted << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "embedded-simulator: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
