#pragma once

#include "HttpRuntime.h"
#include "RecognitionAcceptanceService.h"

namespace ocrservice::http::controllers::device_recognition {

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    services::recognition::acceptance::AcceptanceFailure failure);
server::HttpResponse successResponse(
    const domain::Uuid& requestId,
    const services::recognition::acceptance::AcceptanceSucceeded& result);

}  // namespace ocrservice::http::controllers::device_recognition
