#pragma once

#include "AccessListService.h"
#include "AuthService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::access_list {

class AccessListController final {
public:
    AccessListController(
        services::auth::IAuthService& auth,
        services::access_list::IAccessListService& accessLists);

    void registerRoutes(server::IRouteRegistrar& registrar);

private:
    server::HttpResponse query(const server::HttpRequest& request);
    server::HttpResponse lookup(const server::HttpRequest& request);
    server::HttpResponse create(const server::HttpRequest& request);
    server::HttpResponse remove(const server::HttpRequest& request);

    services::auth::IAuthService& auth_;
    services::access_list::IAccessListService& accessLists_;
};

}  // namespace ocrservice::http::controllers::access_list
