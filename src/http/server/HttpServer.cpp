#include "HttpServer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <crow.h>

#include "ExactJson.h"
#include "ProtocolError.h"

namespace ocrservice::http::server {
namespace {

using protocol::ErrorCode;

struct TemplateSegment final {
    bool parameter;
    std::string value;
};

struct RouteRecord final {
    HttpMethod method;
    std::string routeTemplate;
    std::vector<TemplateSegment> segments;
    RoutePolicy policy;
    RouteHandler handler;
};

std::vector<std::string_view> splitPath(const std::string_view path) {
    std::vector<std::string_view> result;
    std::size_t start = 1U;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        result.push_back(path.substr(start, end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1U;
    }
    return result;
}

bool validParameterName(const std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '_';
           });
}

std::vector<TemplateSegment> parseTemplate(const std::string_view routeTemplate) {
    if (routeTemplate.empty() || routeTemplate.front() != '/' ||
        routeTemplate.find('?') != std::string_view::npos ||
        (routeTemplate.size() > 1U && routeTemplate.back() == '/')) {
        throw std::invalid_argument("route template must be an absolute canonical path");
    }
    std::vector<TemplateSegment> result;
    std::set<std::string, std::less<>> parameterNames;
    if (routeTemplate == "/") {
        return result;
    }
    for (const auto segment : splitPath(routeTemplate)) {
        if (segment.empty()) {
            throw std::invalid_argument("route template contains an empty segment");
        }
        if (segment.front() == '{' && segment.back() == '}') {
            const auto name = segment.substr(1U, segment.size() - 2U);
            if (!validParameterName(name)) {
                throw std::invalid_argument("route parameter name is invalid");
            }
            if (!parameterNames.emplace(name).second) {
                throw std::invalid_argument("route parameter name is repeated");
            }
            result.push_back({true, std::string(name)});
        } else {
            if (segment.find('{') != std::string_view::npos ||
                segment.find('}') != std::string_view::npos) {
                throw std::invalid_argument("route parameter must occupy a complete segment");
            }
            result.push_back({false, std::string(segment)});
        }
    }
    return result;
}

bool routesOverlap(
    const std::vector<TemplateSegment>& left,
    const std::vector<TemplateSegment>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (!left[index].parameter && !right[index].parameter &&
            left[index].value != right[index].value) {
            return false;
        }
    }
    return true;
}

std::optional<std::map<std::string, std::string, std::less<>>> matchRoute(
    const RouteRecord& route,
    const std::string_view path) {
    const auto pathSegments = path == "/" ? std::vector<std::string_view>{} : splitPath(path);
    if (pathSegments.size() != route.segments.size()) {
        return std::nullopt;
    }
    std::map<std::string, std::string, std::less<>> parameters;
    for (std::size_t index = 0U; index < route.segments.size(); ++index) {
        const auto& expected = route.segments[index];
        const auto actual = pathSegments[index];
        if (expected.parameter) {
            if (actual.empty()) {
                return std::nullopt;
            }
            parameters.emplace(expected.value, actual);
        } else if (actual != expected.value) {
            return std::nullopt;
        }
    }
    return parameters;
}

std::optional<HttpMethod> fromCrowMethod(const crow::HTTPMethod method) noexcept {
    switch (method) {
        case crow::HTTPMethod::GET:
            return HttpMethod::get;
        case crow::HTTPMethod::POST:
            return HttpMethod::post;
        case crow::HTTPMethod::PUT:
            return HttpMethod::put;
        case crow::HTTPMethod::DELETE:
            return HttpMethod::deleteMethod;
        case crow::HTTPMethod::PATCH:
            return HttpMethod::patch;
        default:
            return std::nullopt;
    }
}

std::string safeCrowMethodName(const crow::HTTPMethod method) {
    const auto value = static_cast<int>(method);
    if (value < 0 || value >= static_cast<int>(crow::HTTPMethod::InternalMethodCount)) {
        return "UNKNOWN";
    }
    return crow::method_name(method);
}

std::size_t specificity(const std::vector<TemplateSegment>& segments) noexcept {
    return static_cast<std::size_t>(std::count_if(
        segments.begin(), segments.end(),
        [](const TemplateSegment& segment) { return !segment.parameter; }));
}

std::size_t specificity(const RouteRecord& route) noexcept {
    return specificity(route.segments);
}

std::string_view rawQuery(const crow::request& request) noexcept {
    const auto position = request.raw_url.find('?');
    return position == std::string::npos ? std::string_view{} :
                                          std::string_view(request.raw_url).substr(position + 1U);
}

std::string displayMessage(const ErrorCode code) {
    switch (code) {
        case ErrorCode::routeNotFound:
            return "请求路径不存在";
        case ErrorCode::methodNotAllowed:
            return "请求方法不允许";
        case ErrorCode::requestTooLarge:
            return "请求超过大小限制";
        case ErrorCode::invalidRequest:
            return "请求格式无效";
        default:
            return "服务器内部错误";
    }
}

HttpResponse failure(const domain::Uuid& requestId, const ErrorCode code) {
    return HttpResponse::json(
        protocol::httpStatusFor(code),
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::failure(
                requestId, code, displayMessage(code))),
        std::string(protocol::toString(code)));
}

domain::Uuid fallbackRequestId() noexcept {
    static std::atomic<std::uint64_t> sequence{1U};
    std::array<std::uint8_t, 16> bytes{};
    const auto value = sequence.fetch_add(1U, std::memory_order_relaxed);
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[bytes.size() - 1U - index] =
            static_cast<std::uint8_t>(value >> (index * 8U));
    }
    return domain::Uuid::v4(bytes);
}

class CrowStreamWriter final : public IStreamWriter {
public:
    CrowStreamWriter(
        crow::response& response,
        const std::optional<std::size_t> expectedLength)
        : response_(response), expectedLength_(expectedLength) {}

    void write(const std::string_view bytes) override {
        if (bytes.empty()) {
            return;
        }
        if (expectedLength_ &&
            (written_ > *expectedLength_ || bytes.size() > *expectedLength_ - written_)) {
            response_.abort_stream();
            throw std::runtime_error("HTTP stream exceeds Content-Length");
        }
        if (!response_.write_stream(bytes)) {
            throw std::runtime_error("HTTP stream write failed");
        }
        written_ += bytes.size();
    }

    bool complete() const noexcept {
        return !expectedLength_ || written_ == *expectedLength_;
    }

private:
    crow::response& response_;
    std::optional<std::size_t> expectedLength_;
    std::size_t written_ = 0U;
};

}  // namespace

class HttpServerImpl final {
public:
    HttpServerImpl(
        const std::uint16_t port,
        std::shared_ptr<middleware::IRequestIdGenerator> requestIds,
        std::shared_ptr<middleware::IAccessLogSink> accessLog)
        : requestIds_(std::move(requestIds)), accessLog_(std::move(accessLog)) {
        if (!requestIds_) {
            throw std::invalid_argument("request ID generator must not be null");
        }
        app_.bindaddr("0.0.0.0")
            .port(port)
            .concurrency(4U)
            .timeout(5U)
            .signal_clear()
            .loglevel(crow::LogLevel::Critical)
            .max_request_url_size(protocol::kMaximumRawUrlBytes)
            .request_body_limit([](const crow::request& request) {
                return middleware::receiveBodyLimit(request);
            })
            .parser_error_handler([this](
                                      const crow::request& request,
                                      crow::response& response,
                                      const crow::http_parse_error_kind kind) {
                handleParserError(request, response, kind);
            });
        registerDispatcher("/");
        registerDispatcher("/<path>");
        app_.exception_handler([this](crow::response& response) {
            const auto requestId = nextRequestId();
            writeBuffered(response, failure(requestId, ErrorCode::internalError));
        });
    }

    void registerRoute(
        const HttpMethod method,
        std::string routeTemplate,
        const RoutePolicy policy,
        RouteHandler handler) {
        std::lock_guard<std::mutex> lock(routesMutex_);
        if (sealed_) {
            throw std::logic_error("routes cannot be registered after server start");
        }
        if (!handler) {
            throw std::invalid_argument("route handler must not be empty");
        }
        auto segments = parseTemplate(routeTemplate);
        for (const auto& route : routes_) {
            if (route.method == method && routesOverlap(route.segments, segments) &&
                specificity(route) == specificity(segments)) {
                throw std::invalid_argument("duplicate or ambiguous route registration");
            }
        }
        routes_.push_back(
            {method, std::move(routeTemplate), std::move(segments), policy, std::move(handler)});
    }

    void run() {
        {
            std::lock_guard<std::mutex> lock(routesMutex_);
            sealed_ = true;
        }
        try {
            app_.run();
        } catch (...) {
            app_.notify_server_start_exception(std::current_exception());
            throw;
        }
    }

    void stop() noexcept {
        try {
            app_.stop();
        } catch (...) {
        }
    }

    void waitUntilStarted() {
        app_.wait_for_server_start();
    }

    std::uint16_t port() const noexcept {
        return const_cast<crow::SimpleApp&>(app_).port();
    }

private:
    void registerDispatcher(const std::string& pattern) {
        auto& rule = app_.route_dynamic(pattern);
        rule.methods(
            crow::HTTPMethod::DELETE, crow::HTTPMethod::GET, crow::HTTPMethod::HEAD,
            crow::HTTPMethod::POST, crow::HTTPMethod::PUT, crow::HTTPMethod::CONNECT,
            crow::HTTPMethod::OPTIONS, crow::HTTPMethod::TRACE, crow::HTTPMethod::PATCH,
            crow::HTTPMethod::PURGE, crow::HTTPMethod::COPY, crow::HTTPMethod::LOCK,
            crow::HTTPMethod::MKCOL, crow::HTTPMethod::MOVE, crow::HTTPMethod::PROPFIND,
            crow::HTTPMethod::PROPPATCH, crow::HTTPMethod::SEARCH, crow::HTTPMethod::UNLOCK,
            crow::HTTPMethod::BIND, crow::HTTPMethod::REBIND, crow::HTTPMethod::UNBIND,
            crow::HTTPMethod::ACL, crow::HTTPMethod::REPORT, crow::HTTPMethod::MKACTIVITY,
            crow::HTTPMethod::CHECKOUT, crow::HTTPMethod::MERGE, crow::HTTPMethod::MSEARCH,
            crow::HTTPMethod::NOTIFY, crow::HTTPMethod::SUBSCRIBE,
            crow::HTTPMethod::UNSUBSCRIBE, crow::HTTPMethod::MKCALENDAR,
            crow::HTTPMethod::LINK, crow::HTTPMethod::UNLINK, crow::HTTPMethod::SOURCE);
        if (pattern == "/") {
            rule([this](const crow::request& request, crow::response& response) {
                dispatch(request, response);
            });
        } else {
            rule([this](
                     const crow::request& request,
                     crow::response& response,
                     std::string) { dispatch(request, response); });
        }
    }

    domain::Uuid nextRequestId() noexcept {
        try {
            return requestIds_->next();
        } catch (...) {
            return fallbackRequestId();
        }
    }

    void handleParserError(
        const crow::request& request,
        crow::response& response,
        const crow::http_parse_error_kind kind) {
        const auto start = std::chrono::steady_clock::now();
        const auto requestId = nextRequestId();
        const auto code = kind == crow::http_parse_error_kind::malformed ?
                              ErrorCode::invalidRequest : ErrorCode::requestTooLarge;
        const auto result = failure(requestId, code);
        writeBuffered(response, result);
        log(request, "<parse-error>", result, requestId, start);
    }

    void dispatch(const crow::request& request, crow::response& response) {
        const auto start = std::chrono::steady_clock::now();
        const auto requestId = nextRequestId();
        const auto method = fromCrowMethod(request.method);
        RouteRecord* selected = nullptr;
        std::map<std::string, std::string, std::less<>> parameters;
        bool pathExists = false;
        for (auto& route : routes_) {
            auto match = matchRoute(route, request.url);
            if (!match) {
                continue;
            }
            pathExists = true;
            if (method && route.method == *method &&
                (selected == nullptr || specificity(route) > specificity(*selected) ||
                 (specificity(route) == specificity(*selected) &&
                  route.routeTemplate < selected->routeTemplate))) {
                selected = &route;
                parameters = std::move(*match);
            }
        }
        if (!selected) {
            const auto result = failure(
                requestId, pathExists ? ErrorCode::methodNotAllowed : ErrorCode::routeNotFound);
            writeBuffered(response, result);
            log(
                request, pathExists ? "<method-not-allowed>" : "<unmatched>", result,
                requestId, start);
            return;
        }

        try {
            auto metadata = middleware::validateRequest(
                request, selected->policy.bodyMode, selected->policy.parseBearer);
            HttpRequest runtimeRequest{
                requestId,
                *method,
                request.body,
                rawQuery(request),
                std::move(parameters),
                std::move(metadata.bearerToken),
                std::move(metadata.multipartBoundary)};
            auto result = selected->handler(runtimeRequest);
            protectResponse(result, requestId);
            if (result.kind_ == protocol::ResponseBodyKind::json) {
                writeBuffered(response, result);
                log(request, selected->routeTemplate, result, requestId, start);
                return;
            }
            writeStream(response, result);
            log(request, selected->routeTemplate, result, requestId, start);
        } catch (const protocol::ProtocolError& error) {
            const auto result = failure(requestId, error.code());
            writeBuffered(response, result);
            log(request, selected->routeTemplate, result, requestId, start);
        } catch (...) {
            if (response.is_completed()) {
                response.abort_stream();
                auto interrupted = failure(requestId, ErrorCode::internalError);
                interrupted.status_ = response.code;
                log(request, selected->routeTemplate, interrupted, requestId, start);
            } else {
                const auto result = failure(requestId, ErrorCode::internalError);
                writeBuffered(response, result);
                log(request, selected->routeTemplate, result, requestId, start);
            }
        }
    }

    static void protectResponse(HttpResponse& response, const domain::Uuid& requestId) {
        if (protocol::isRedirectStatus(response.status_) ||
            (response.kind_ == protocol::ResponseBodyKind::json &&
             !protocol::isJsonSizeAllowed(response.body_.size())) ||
            ((response.kind_ == protocol::ResponseBodyKind::jpeg ||
              response.kind_ == protocol::ResponseBodyKind::png) &&
             (!response.contentLength_ ||
              *response.contentLength_ > protocol::kMaximumImageResponseBytes))) {
            response = failure(requestId, ErrorCode::internalError);
        }
    }

    static void writeBuffered(crow::response& target, const HttpResponse& source) {
        target.clear();
        target.code = source.status_;
        target.set_header("Content-Type", std::string(protocol::contentTypeFor(source.kind_)));
        target.body = source.body_;
        target.end();
    }

    static void writeStream(crow::response& target, HttpResponse& source) {
        target.body.clear();
        target.headers.clear();
        target.code = source.status_;
        target.set_header("Content-Type", std::string(protocol::contentTypeFor(source.kind_)));
        if (source.contentLength_) {
            target.set_header("Content-Length", std::to_string(*source.contentLength_));
        }
        if (!target.start_stream()) {
            throw std::runtime_error("failed to start HTTP stream");
        }
        try {
            CrowStreamWriter writer(target, source.contentLength_);
            source.producer_(writer);
            if (!writer.complete()) {
                throw std::runtime_error("HTTP stream is shorter than Content-Length");
            }
            if (!target.finish_stream()) {
                throw std::runtime_error("failed to finish HTTP stream");
            }
        } catch (...) {
            target.abort_stream();
            throw;
        }
    }

    void log(
        const crow::request& request,
        const std::string& routeTemplate,
        const HttpResponse& response,
        const domain::Uuid& requestId,
        const std::chrono::steady_clock::time_point start) noexcept {
        if (!accessLog_) {
            return;
        }
        accessLog_->write({
            safeCrowMethodName(request.method),
            routeTemplate,
            response.status_,
            response.code_,
            requestId.toString(),
            middleware::elapsedMilliseconds(start)});
    }

    crow::SimpleApp app_;
    std::shared_ptr<middleware::IRequestIdGenerator> requestIds_;
    std::shared_ptr<middleware::IAccessLogSink> accessLog_;
    std::vector<RouteRecord> routes_;
    std::mutex routesMutex_;
    bool sealed_ = false;
};

HttpServer::HttpServer(
    const std::uint16_t port,
    std::shared_ptr<middleware::IRequestIdGenerator> requestIds,
    std::shared_ptr<middleware::IAccessLogSink> accessLog)
    : impl_(std::make_unique<HttpServerImpl>(
          port, std::move(requestIds), std::move(accessLog))) {}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::registerRoute(
    const HttpMethod method,
    std::string routeTemplate,
    const RoutePolicy policy,
    RouteHandler handler) {
    impl_->registerRoute(method, std::move(routeTemplate), policy, std::move(handler));
}

void HttpServer::run() {
    impl_->run();
}

void HttpServer::stop() noexcept {
    impl_->stop();
}

void HttpServer::waitUntilStarted() {
    impl_->waitUntilStarted();
}

std::uint16_t HttpServer::port() const noexcept {
    return impl_->port();
}

}  // namespace ocrservice::http::server
