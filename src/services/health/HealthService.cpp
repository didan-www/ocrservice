#include "HealthService.h"

namespace ocrservice::services::health {

HealthService::HealthService(IHealthStateSource& source) noexcept : source_(source) {}

HealthResult HealthService::check() noexcept {
    try {
        if (!source_.modelAvailable() || !source_.mysqlAvailable()) {
            return HealthFailure::serviceUnavailable;
        }
        const bool mqttConnected = source_.mqttConnected();
        return serialization::json::HealthData{
            mqttConnected ? serialization::json::HealthStatus::up :
                            serialization::json::HealthStatus::degraded,
            serialization::json::ComponentStatus::up,
            serialization::json::ComponentStatus::up,
            mqttConnected ? serialization::json::ComponentStatus::up :
                            serialization::json::ComponentStatus::down,
            source_.queueDepth(),
            source_.queueCapacity()};
    } catch (...) {
        return HealthFailure::serviceUnavailable;
    }
}

}  // namespace ocrservice::services::health
