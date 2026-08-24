#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DeviceConfig.h"

namespace ocrservice::tests::embedded {

struct AcceptedUpload final {
    std::string recognitionId;
    std::string captureId;
};

AcceptedUpload uploadImage(
    const DeviceConfig& config,
    const std::vector<std::uint8_t>& image,
    const std::string& captureId,
    const std::string& capturedAt,
    int timeoutSeconds);

}  // namespace ocrservice::tests::embedded
