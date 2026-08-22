#pragma once

#include "AuthService.h"
#include "HttpRuntime.h"
#include "ServerConfig.h"

namespace ocrservice::http::controllers::auth {

class AuthController final {
public:
    AuthController(
        services::auth::IAuthService& service,
        const app::config::MqttConfig& mqttConfig);

    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse login(const server::HttpRequest& request);
    server::HttpResponse logout(const server::HttpRequest& request);

    services::auth::IAuthService& service_;
    const app::config::MqttConfig& mqttConfig_;
};

}  // namespace ocrservice::http::controllers::auth
