#pragma once

#include "HealthService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::health {

class HealthController final {
public:
    explicit HealthController(services::health::IHealthService& service) noexcept;
    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse health(const server::HttpRequest& request);

    services::health::IHealthService& service_;
};

}  // namespace ocrservice::http::controllers::health
