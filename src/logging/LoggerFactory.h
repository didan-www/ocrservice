#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ServerConfig.h"

namespace spdlog {
class logger;
}

namespace ocrservice::logging {

enum class LogLevel {
    debug,
    info,
    warning,
    error,
    critical,
};

struct LogEvent {
    LogEvent(
        std::string moduleValue,
        std::string eventValue,
        std::string codeValue,
        std::string requestIdValue,
        std::optional<std::string> recognitionIdValue = std::nullopt,
        std::optional<std::string> deviceIdValue = std::nullopt,
        std::optional<std::uint64_t> durationMsValue = std::nullopt,
        std::optional<std::string> detailValue = std::nullopt,
        std::optional<app::config::NonSensitiveConfigSummary> configValue = std::nullopt)
        : module(std::move(moduleValue)),
          event(std::move(eventValue)),
          code(std::move(codeValue)),
          requestId(std::move(requestIdValue)),
          recognitionId(std::move(recognitionIdValue)),
          deviceId(std::move(deviceIdValue)),
          durationMs(durationMsValue),
          detail(std::move(detailValue)),
          config(std::move(configValue)) {}

    std::string module;
    std::string event;
    std::string code;
    std::string requestId;
    std::optional<std::string> recognitionId;
    std::optional<std::string> deviceId;
    std::optional<std::uint64_t> durationMs;
    std::optional<std::string> detail;
    std::optional<app::config::NonSensitiveConfigSummary> config;
};

class JsonLinesLogger final {
public:
    explicit JsonLinesLogger(std::shared_ptr<spdlog::logger> logger);

    void log(LogLevel level, const LogEvent& event);
    void flush();

private:
    std::shared_ptr<spdlog::logger> logger_;
};

class LoggerFactory final {
public:
    static std::shared_ptr<JsonLinesLogger> createConsole(std::string loggerName);
    static std::shared_ptr<JsonLinesLogger> createRotatingFile(
        std::string loggerName,
        const std::filesystem::path& logRoot,
        std::size_t maximumFileSize = 10U * 1024U * 1024U,
        std::size_t maximumFiles = 5U);
};

}  // namespace ocrservice::logging
