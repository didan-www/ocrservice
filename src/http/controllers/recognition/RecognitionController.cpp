#include "RecognitionController.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "AuthResponses.h"
#include "ExactJson.h"
#include "ProtocolError.h"
#include "QueryDecoders.h"
#include "RecognitionResponses.h"
#include "RequestDecoders.h"

namespace ocrservice::http::controllers::recognition {
namespace {

std::string_view token(const server::HttpRequest& request) noexcept {
    return request.bearerToken ? std::string_view(*request.bearerToken) : std::string_view{};
}

domain::RecognitionId recognitionId(const server::HttpRequest& request) {
    const auto found = request.pathParameters.find("recognitionId");
    if (found == request.pathParameters.end()) {
        throw std::logic_error("recognition route parameter is missing");
    }
    try {
        return domain::RecognitionId::parse(found->second);
    } catch (const domain::DomainError&) {
        protocol::throwInvalidRequest("recognitionId is invalid");
    }
}

protocol::ResponseBodyKind responseKind(const domain::ImageMime mime) {
    switch (mime) {
        case domain::ImageMime::jpeg:
            return protocol::ResponseBodyKind::jpeg;
        case domain::ImageMime::png:
            return protocol::ResponseBodyKind::png;
    }
    throw std::logic_error("image MIME is invalid");
}

class CsvSink final : public services::csv::ICsvSink {
public:
    explicit CsvSink(server::IStreamWriter& writer) : writer_(writer) {}

    void write(const std::string_view bytes) override {
        writer_.write(bytes);
    }

private:
    server::IStreamWriter& writer_;
};

server::HttpResponse detailSuccess(
    const domain::Uuid& requestId,
    const domain::RecognitionRecord& record) {
    return server::HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(
                requestId, serialization::json::toJsonExact(record))));
}

server::HttpResponse historySuccess(
    const domain::Uuid& requestId,
    const domain::PageResult<domain::RecognitionRecord>& page) {
    return server::HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(
                requestId, serialization::json::toJsonExact(page))));
}

}  // namespace

RecognitionController::RecognitionController(
    services::auth::IAuthService& auth,
    services::history::IHistoryService& history,
    services::csv::ICsvService& csv)
    : auth_(auth), history_(history), csv_(csv) {}

void RecognitionController::registerRoutes(server::IRouteRegistrar& registrar) {
    const server::RoutePolicy policy{middleware::RequestBodyMode::none, true};
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/recognitions",
        policy,
        [this](const server::HttpRequest& request) { return history(request); });
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/recognitions/export",
        policy,
        [this](const server::HttpRequest& request) { return exportCsv(request); });
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/recognitions/{recognitionId}",
        policy,
        [this](const server::HttpRequest& request) { return detail(request); });
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/recognitions/{recognitionId}/image",
        policy,
        [this](const server::HttpRequest& request) { return image(request); });
}

server::HttpResponse RecognitionController::detail(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto result = history_.detail(recognitionId(request));
    if (std::holds_alternative<services::history::HistoryFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::history::HistoryFailure>(result));
    }
    return detailSuccess(
        request.requestId,
        std::get<domain::RecognitionRecord>(result));
}

server::HttpResponse RecognitionController::history(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    const auto query = protocol::decodeHistoryQuery(request.rawQuery);
    auto result = history_.query(query.filter, query.page);
    if (std::holds_alternative<services::history::HistoryFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::history::HistoryFailure>(result));
    }
    return historySuccess(
        request.requestId,
        std::get<domain::PageResult<domain::RecognitionRecord>>(result));
}

server::HttpResponse RecognitionController::image(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto result = history_.openImage(recognitionId(request));
    if (std::holds_alternative<services::history::HistoryFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::history::HistoryFailure>(result));
    }
    auto imageFile = std::make_shared<domain::ImageFile>(
        std::get<domain::ImageFile>(std::move(result)));
    const auto kind = responseKind(imageFile->mime());
    const auto size = static_cast<std::size_t>(imageFile->sizeBytes());
    return server::HttpResponse::image(
        kind,
        size,
        [imageFile](server::IStreamWriter& writer) {
            std::array<std::uint8_t, 64U * 1024U> buffer{};
            while (true) {
                const auto read = imageFile->read(buffer.data(), buffer.size());
                if (read == 0U) {
                    return;
                }
                writer.write(std::string_view(
                    reinterpret_cast<const char*>(buffer.data()), read));
            }
        });
}

server::HttpResponse RecognitionController::exportCsv(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    const auto filter = protocol::decodeHistoryExportQuery(request.rawQuery);
    auto result = csv_.prepare(filter);
    if (std::holds_alternative<services::csv::CsvFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::csv::CsvFailure>(result));
    }
    auto exportStream = std::make_shared<services::csv::CsvExport>(
        std::get<services::csv::CsvExport>(std::move(result)));
    return server::HttpResponse::csv(
        [exportStream](server::IStreamWriter& writer) {
            CsvSink sink(writer);
            exportStream->writeTo(sink);
        });
}

}  // namespace ocrservice::http::controllers::recognition
