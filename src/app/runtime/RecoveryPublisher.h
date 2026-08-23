#pragma once

#include <memory>
#include <vector>

#include "Ports.h"

namespace ocrservice::logging {
class JsonLinesLogger;
}

namespace ocrservice::app::runtime {

void publishRecoveredOnce(
    std::vector<domain::RecognitionRecord>& recovered,
    domain::IMqttPublisher& publisher,
    const std::shared_ptr<logging::JsonLinesLogger>& logger = nullptr) noexcept;

}  // namespace ocrservice::app::runtime
