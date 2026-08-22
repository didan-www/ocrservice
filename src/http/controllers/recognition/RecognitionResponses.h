#pragma once

#include "CsvService.h"
#include "HistoryService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::recognition {

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    services::history::HistoryFailure failure);
server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    services::csv::CsvFailure failure);

}  // namespace ocrservice::http::controllers::recognition
