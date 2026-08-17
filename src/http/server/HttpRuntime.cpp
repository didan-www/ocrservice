#include "HttpRuntime.h"

#include <stdexcept>
#include <utility>

namespace ocrservice::http::server {

std::string_view toString(const HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::get:
            return "GET";
        case HttpMethod::post:
            return "POST";
        case HttpMethod::put:
            return "PUT";
        case HttpMethod::deleteMethod:
            return "DELETE";
        case HttpMethod::patch:
            return "PATCH";
    }
    return "UNKNOWN";
}

HttpResponse::HttpResponse(
    const int status,
    const protocol::ResponseBodyKind kind,
    std::string body,
    std::string code,
    std::optional<std::size_t> contentLength,
    StreamProducer producer)
    : status_(status),
      kind_(kind),
      body_(std::move(body)),
      code_(std::move(code)),
      contentLength_(contentLength),
      producer_(std::move(producer)) {}

HttpResponse HttpResponse::json(const int status, std::string body, std::string code) {
    return HttpResponse(
        status, protocol::ResponseBodyKind::json, std::move(body), std::move(code),
        std::nullopt, {});
}

HttpResponse HttpResponse::image(
    const protocol::ResponseBodyKind kind,
    const std::size_t contentLength,
    StreamProducer producer) {
    if ((kind != protocol::ResponseBodyKind::jpeg && kind != protocol::ResponseBodyKind::png) ||
        !producer) {
        throw std::invalid_argument("image response requires JPEG/PNG and a producer");
    }
    return HttpResponse(200, kind, {}, "OK", contentLength, std::move(producer));
}

HttpResponse HttpResponse::csv(StreamProducer producer) {
    if (!producer) {
        throw std::invalid_argument("CSV response requires a producer");
    }
    return HttpResponse(
        200, protocol::ResponseBodyKind::csv, {}, "OK", std::nullopt, std::move(producer));
}

int HttpResponse::status() const noexcept {
    return status_;
}

protocol::ResponseBodyKind HttpResponse::kind() const noexcept {
    return kind_;
}

const std::string& HttpResponse::body() const noexcept {
    return body_;
}

const std::string& HttpResponse::code() const noexcept {
    return code_;
}

}  // namespace ocrservice::http::server
