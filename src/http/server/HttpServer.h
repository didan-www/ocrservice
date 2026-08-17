#pragma once

#include <cstdint>
#include <memory>

#include "AccessLog.h"
#include "HttpRuntime.h"
#include "RequestId.h"

namespace ocrservice::http::server {

class HttpServerImpl;

class HttpServer final : public IRouteRegistrar {
public:
    explicit HttpServer(
        std::uint16_t port,
        std::shared_ptr<middleware::IRequestIdGenerator> requestIds =
            middleware::makeRandomRequestIdGenerator(),
        std::shared_ptr<middleware::IAccessLogSink> accessLog = nullptr);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void registerRoute(
        HttpMethod method,
        std::string routeTemplate,
        RoutePolicy policy,
        RouteHandler handler) override;

    void run();
    void stop() noexcept;
    void waitUntilStarted();
    std::uint16_t port() const noexcept;

private:
    std::unique_ptr<HttpServerImpl> impl_;
};

}  // namespace ocrservice::http::server
