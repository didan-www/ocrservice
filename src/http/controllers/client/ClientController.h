#pragma once

#include "AuthService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::client {

class ClientController final {
public:
    explicit ClientController(services::auth::IAuthService& service);

    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse heartbeat(const server::HttpRequest& request);

    services::auth::IAuthService& service_;
};

}  // namespace ocrservice::http::controllers::client
