#pragma once

#include "AuthService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::auth {

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    services::auth::AuthFailure failure);
server::HttpResponse emptySuccessResponse(const domain::Uuid& requestId);

}  // namespace ocrservice::http::controllers::auth
