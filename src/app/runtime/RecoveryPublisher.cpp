#include "RecoveryPublisher.h"

#include <optional>
#include <string>

#include "LoggerFactory.h"

namespace ocrservice::app::runtime {
namespace {

void logFailure(
    const std::shared_ptr<logging::JsonLinesLogger>& logger,
    const char* event,
    const domain::RecognitionSnapshot& snapshot) noexcept {
    if (!logger) {
        return;
    }
    try {
        logger->log(
            logging::LogLevel::warning,
            logging::LogEvent(
                "application_runtime",
                event,
                "MQTT_PUBLISH_FAILED",
                "system",
                snapshot.recognitionId().toString(),
                snapshot.deviceId().value()));
    } catch (...) {
    }
}

}  // namespace

void publishRecoveredOnce(
    std::vector<domain::RecognitionRecord>& recovered,
    domain::IMqttPublisher& publisher,
    const std::shared_ptr<logging::JsonLinesLogger>& logger) noexcept {
    for (const auto& record : recovered) {
        const auto& snapshot = record.snapshot();
        try {
            if (!publisher.publishManagement(snapshot).wasAccepted()) {
                logFailure(logger, "recovery_management_publish_failed", snapshot);
            }
        } catch (...) {
            logFailure(logger, "recovery_management_publish_exception", snapshot);
        }
        try {
            if (!publisher.publishDeviceFinal(snapshot, domain::GateAction::keepClosed)
                     .wasAccepted()) {
                logFailure(logger, "recovery_device_publish_failed", snapshot);
            }
        } catch (...) {
            logFailure(logger, "recovery_device_publish_exception", snapshot);
        }
    }
    recovered.clear();
}

}  // namespace ocrservice::app::runtime
