#include "LoggerFactory.h"

#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include "LogSanitizer.h"

namespace ocrservice::logging {
namespace {

std::string formatTimestamp(const std::chrono::system_clock::time_point now) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fraction = milliseconds - seconds;
    const std::time_t rawTime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::time_point(seconds));
    std::tm utcTime{};
    if (gmtime_r(&rawTime, &utcTime) == nullptr) {
        throw std::runtime_error("failed to format log timestamp");
    }
    std::ostringstream output;
    output << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << fraction.count() << 'Z';
    return output.str();
}

std::string_view levelName(const LogLevel level) {
    switch (level) {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO";
        case LogLevel::warning:
            return "WARN";
        case LogLevel::error:
            return "ERROR";
        case LogLevel::critical:
            return "CRITICAL";
    }
    throw std::invalid_argument("invalid log level");
}

spdlog::level::level_enum spdlogLevel(const LogLevel level) {
    switch (level) {
        case LogLevel::debug:
            return spdlog::level::debug;
        case LogLevel::info:
            return spdlog::level::info;
        case LogLevel::warning:
            return spdlog::level::warn;
        case LogLevel::error:
            return spdlog::level::err;
        case LogLevel::critical:
            return spdlog::level::critical;
    }
    throw std::invalid_argument("invalid log level");
}

std::string requireField(const std::string& value, const char* fieldName) {
    if (value.empty()) {
        throw std::invalid_argument(std::string("log field must not be empty: ") + fieldName);
    }
    return LogSanitizer::sanitize(value);
}

nlohmann::json serialize(const LogLevel level, const LogEvent& event) {
    nlohmann::json document = {
        {"timestamp", formatTimestamp(std::chrono::system_clock::now())},
        {"level", levelName(level)},
        {"module", requireField(event.module, "module")},
        {"event", requireField(event.event, "event")},
        {"code", requireField(event.code, "code")},
        {"requestId", requireField(event.requestId, "requestId")},
    };
    if (event.recognitionId) {
        document["recognitionId"] = requireField(*event.recognitionId, "recognitionId");
    }
    if (event.deviceId) {
        document["deviceId"] = requireField(*event.deviceId, "deviceId");
    }
    if (event.durationMs) {
        document["durationMs"] = *event.durationMs;
    }
    if (event.detail) {
        document["detail"] = LogSanitizer::sanitize(*event.detail);
    }
    if (event.config) {
        document["config"] = app::config::toJson(*event.config);
    }
    return document;
}

std::shared_ptr<JsonLinesLogger> makeLogger(
    std::string loggerName,
    const std::shared_ptr<spdlog::sinks::sink>& sink) {
    if (loggerName.empty()) {
        throw std::invalid_argument("logger name must not be empty");
    }
    auto logger = std::make_shared<spdlog::logger>(std::move(loggerName), sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");
    return std::make_shared<JsonLinesLogger>(std::move(logger));
}

}  // namespace

JsonLinesLogger::JsonLinesLogger(std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)) {
    if (!logger_) {
        throw std::invalid_argument("logger must not be null");
    }
}

void JsonLinesLogger::log(const LogLevel level, const LogEvent& event) {
    const auto document = serialize(level, event);
    logger_->log(
        spdlogLevel(level),
        document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void JsonLinesLogger::flush() {
    logger_->flush();
}

std::shared_ptr<JsonLinesLogger> LoggerFactory::createConsole(std::string loggerName) {
    return makeLogger(
        std::move(loggerName),
        std::make_shared<spdlog::sinks::stdout_sink_mt>());
}

std::shared_ptr<JsonLinesLogger> LoggerFactory::createRotatingFile(
    std::string loggerName,
    const std::filesystem::path& logRoot,
    const std::size_t maximumFileSize,
    const std::size_t maximumFiles) {
    if (logRoot.empty() || maximumFileSize == 0U || maximumFiles == 0U) {
        throw std::invalid_argument("invalid rotating log configuration");
    }
    const auto logFile = logRoot / "ocrservice.jsonl";
    return makeLogger(
        std::move(loggerName),
        std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFile.string(), maximumFileSize, maximumFiles));
}

}  // namespace ocrservice::logging
