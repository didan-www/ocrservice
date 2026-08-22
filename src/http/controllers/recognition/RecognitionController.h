#pragma once

#include "AuthService.h"
#include "CsvService.h"
#include "HistoryService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::recognition {

class RecognitionController final {
public:
    RecognitionController(
        services::auth::IAuthService& auth,
        services::history::IHistoryService& history,
        services::csv::ICsvService& csv);

    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse detail(const server::HttpRequest& request);
    server::HttpResponse history(const server::HttpRequest& request);
    server::HttpResponse image(const server::HttpRequest& request);
    server::HttpResponse exportCsv(const server::HttpRequest& request);

    services::auth::IAuthService& auth_;
    services::history::IHistoryService& history_;
    services::csv::ICsvService& csv_;
};

}  // namespace ocrservice::http::controllers::recognition
