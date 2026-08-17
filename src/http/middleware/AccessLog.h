#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ocrservice::logging {
class JsonLinesLogger;
}

namespace ocrservice::http::middleware {

struct AccessLogEntry final {
    std::string method;
    std::string routeTemplate;
    int httpStatus;
    std::string code;
    std::string requestId;
    std::uint64_t durationMs;
};

class IAccessLogSink {
public:
    virtual ~IAccessLogSink() = default;
    virtual void write(const AccessLogEntry& entry) noexcept = 0;
};

class StructuredAccessLogSink final : public IAccessLogSink {
public:
    explicit StructuredAccessLogSink(std::shared_ptr<logging::JsonLinesLogger> logger);
    void write(const AccessLogEntry& entry) noexcept override;

private:
    std::shared_ptr<logging::JsonLinesLogger> logger_;
};

std::uint64_t elapsedMilliseconds(std::chrono::steady_clock::time_point start) noexcept;

}  // namespace ocrservice::http::middleware
