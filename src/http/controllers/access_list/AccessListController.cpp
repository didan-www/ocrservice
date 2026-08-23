#include "AccessListController.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "AccessListResponses.h"
#include "AuthResponses.h"
#include "ProtocolError.h"
#include "QueryDecoders.h"
#include "RequestDecoders.h"

namespace ocrservice::http::controllers::access_list {
namespace {

std::string_view token(const server::HttpRequest& request) noexcept {
    return request.bearerToken ? std::string_view(*request.bearerToken) : std::string_view{};
}

std::uint64_t accessListId(const server::HttpRequest& request) {
    const auto found = request.pathParameters.find("id");
    if (found == request.pathParameters.end()) {
        throw std::logic_error("access-list route parameter is missing");
    }
    return protocol::parsePositiveSafeInteger(found->second, "id");
}

}  // namespace

AccessListController::AccessListController(
    services::auth::IAuthService& auth,
    services::access_list::IAccessListService& accessLists)
    : auth_(auth), accessLists_(accessLists) {}

void AccessListController::registerRoutes(server::IRouteRegistrar& registrar) {
    const server::RoutePolicy readPolicy{middleware::RequestBodyMode::none, true};
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/access-lists",
        readPolicy,
        [this](const server::HttpRequest& request) { return query(request); });
    registrar.registerRoute(
        server::HttpMethod::get,
        "/api/v1/access-lists/lookup",
        readPolicy,
        [this](const server::HttpRequest& request) { return lookup(request); });
    registrar.registerRoute(
        server::HttpMethod::post,
        "/api/v1/access-lists",
        {middleware::RequestBodyMode::json, true},
        [this](const server::HttpRequest& request) { return create(request); });
    registrar.registerRoute(
        server::HttpMethod::deleteMethod,
        "/api/v1/access-lists/{id}",
        readPolicy,
        [this](const server::HttpRequest& request) { return remove(request); });
}

server::HttpResponse AccessListController::query(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    const auto decoded = protocol::decodeAccessListPageQuery(request.rawQuery);
    auto result = accessLists_.query(decoded.filter, decoded.page);
    if (std::holds_alternative<services::access_list::AccessListFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::access_list::AccessListFailure>(result));
    }
    return pageSuccessResponse(
        request.requestId,
        std::get<domain::PageResult<domain::AccessListRecord>>(result));
}

server::HttpResponse AccessListController::lookup(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    auto result = accessLists_.lookup(protocol::decodeAccessListLookupQuery(request.rawQuery));
    if (std::holds_alternative<services::access_list::AccessListFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::access_list::AccessListFailure>(result));
    }
    return recordSuccessResponse(request.requestId, std::get<domain::AccessListRecord>(result));
}

server::HttpResponse AccessListController::create(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto decoded = protocol::decodeAccessListCreateRequest(request.body);
    const auto& session = std::get<services::auth::Authenticated>(authentication).session;
    auto result = accessLists_.create(
        decoded.listType,
        std::move(decoded.plateNumber),
        std::move(decoded.remark),
        {session.userId, session.displayName});
    if (std::holds_alternative<services::access_list::AccessListFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::access_list::AccessListFailure>(result));
    }
    return recordSuccessResponse(request.requestId, std::get<domain::AccessListRecord>(result));
}

server::HttpResponse AccessListController::remove(const server::HttpRequest& request) {
    auto authentication = auth_.authenticate(token(request));
    if (std::holds_alternative<services::auth::AuthFailure>(authentication)) {
        return auth::failureResponse(
            request.requestId,
            std::get<services::auth::AuthFailure>(authentication));
    }
    protocol::requireEmptyRawQuery(request.rawQuery);
    auto result = accessLists_.remove(accessListId(request));
    if (std::holds_alternative<services::access_list::AccessListFailure>(result)) {
        return failureResponse(
            request.requestId,
            std::get<services::access_list::AccessListFailure>(result));
    }
    return emptySuccessResponse(request.requestId);
}

}  // namespace ocrservice::http::controllers::access_list
