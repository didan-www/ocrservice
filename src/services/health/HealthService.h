#pragma once

#include <cstdint>
#include <variant>

#include "ExactJson.h"

namespace ocrservice::services::health {

class IHealthStateSource {
public:
    virtual ~IHealthStateSource() = default;

    virtual bool modelAvailable() const noexcept = 0;
    virtual bool mysqlAvailable() noexcept = 0;
    virtual bool mqttConnected() const noexcept = 0;
    virtual std::uint64_t queueDepth() const noexcept = 0;
    virtual std::uint64_t queueCapacity() const noexcept = 0;
};

enum class HealthFailure { serviceUnavailable };
using HealthResult = std::variant<serialization::json::HealthData, HealthFailure>;

class IHealthService {
public:
    virtual ~IHealthService() = default;
    virtual HealthResult check() noexcept = 0;
};

class HealthService final : public IHealthService {
public:
    explicit HealthService(IHealthStateSource& source) noexcept;
    HealthResult check() noexcept override;

private:
    IHealthStateSource& source_;
};

}  // namespace ocrservice::services::health
