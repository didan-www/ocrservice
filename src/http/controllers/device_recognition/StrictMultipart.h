#pragma once

#include <string_view>

namespace ocrservice::http::controllers::device_recognition {

struct UploadParts final {
    std::string_view image;
    std::string_view captureId;
    std::string_view capturedAt;
};

UploadParts decodeUploadMultipart(std::string_view body, std::string_view boundary);

}  // namespace ocrservice::http::controllers::device_recognition
