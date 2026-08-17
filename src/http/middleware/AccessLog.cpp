#include "AccessLog.h"

#include <utility>

#include "LoggerFactory.h"

namespace ocrservice::http::middleware {

StructuredAccessLogSink::StructuredAccessLogSink(
    std::shared_ptr<logging::JsonLinesLogger> logger)
    : logger_(std::move(logger)) {}

void StructuredAccessLogSink::write(const AccessLogEntry& entry) noexcept {
    if (!logger_) {
        return;
    }
    try {
        const std::string detail = "method=" + entry.method + " route=" + entry.routeTemplate +
                                   " status=" + std::to_string(entry.httpStatus);
        logger_->log(
            logging::LogLevel::info,
            logging::LogEvent(
                "http", "request_completed", entry.code, entry.requestId, std::nullopt,
                std::nullopt, entry.durationMs, detail));
    } catch (...) {
    }
}

std::uint64_t elapsedMilliseconds(const std::chrono::steady_clock::time_point start) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return elapsed.count() < 0 ? 0U : static_cast<std::uint64_t>(elapsed.count());
}

}  // namespace ocrservice::http::middleware
