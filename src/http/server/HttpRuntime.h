#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "HttpPolicy.h"
#include "RequestGuards.h"
#include "Identifiers.h"

namespace ocrservice::http::server {

enum class HttpMethod { get, post, put, deleteMethod, patch };

std::string_view toString(HttpMethod method) noexcept;

struct RoutePolicy final {
    middleware::RequestBodyMode bodyMode = middleware::RequestBodyMode::none;
    bool parseBearer = false;
};

struct HttpRequest final {
    domain::Uuid requestId;
    HttpMethod method;
    std::string_view body;
    std::string_view rawQuery;
    std::map<std::string, std::string, std::less<>> pathParameters;
    std::optional<std::string> bearerToken;
    std::optional<std::string> multipartBoundary;
};

class IStreamWriter {
public:
    virtual ~IStreamWriter() = default;
    virtual void write(std::string_view bytes) = 0;
};

class HttpResponse final {
public:
    using StreamProducer = std::function<void(IStreamWriter&)>;

    static HttpResponse json(int status, std::string body, std::string code = "OK");
    static HttpResponse image(
        protocol::ResponseBodyKind kind,
        std::size_t contentLength,
        StreamProducer producer);
    static HttpResponse csv(StreamProducer producer);

    int status() const noexcept;
    protocol::ResponseBodyKind kind() const noexcept;
    const std::string& body() const noexcept;
    const std::string& code() const noexcept;

private:
    friend class HttpServer;
    friend class HttpServerImpl;

    HttpResponse(
        int status,
        protocol::ResponseBodyKind kind,
        std::string body,
        std::string code,
        std::optional<std::size_t> contentLength,
        StreamProducer producer);

    int status_;
    protocol::ResponseBodyKind kind_;
    std::string body_;
    std::string code_;
    std::optional<std::size_t> contentLength_;
    StreamProducer producer_;
};

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

class IRouteRegistrar {
public:
    virtual ~IRouteRegistrar() = default;
    virtual void registerRoute(
        HttpMethod method,
        std::string routeTemplate,
        RoutePolicy policy,
        RouteHandler handler) = 0;
};

}  // namespace ocrservice::http::server
