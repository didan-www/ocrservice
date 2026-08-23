#pragma once

#include "HttpRuntime.h"
#include "RecognitionAcceptanceService.h"

namespace ocrservice::http::controllers::device_recognition {

class DeviceRecognitionController final {
public:
    DeviceRecognitionController(
        services::recognition::acceptance::IRecognitionAcceptanceService& acceptance,
        services::recognition::acceptance::IUploadImageValidator& images);

    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse upload(const server::HttpRequest& request);

    services::recognition::acceptance::IRecognitionAcceptanceService& acceptance_;
    services::recognition::acceptance::IUploadImageValidator& images_;
};

}  // namespace ocrservice::http::controllers::device_recognition
