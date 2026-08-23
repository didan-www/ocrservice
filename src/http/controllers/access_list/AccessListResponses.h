#pragma once

#include "AccessListService.h"
#include "HttpRuntime.h"

namespace ocrservice::http::controllers::access_list {

server::HttpResponse failureResponse(
    const domain::Uuid& requestId,
    services::access_list::AccessListFailure failure);
server::HttpResponse recordSuccessResponse(
    const domain::Uuid& requestId,
    const domain::AccessListRecord& record);
server::HttpResponse pageSuccessResponse(
    const domain::Uuid& requestId,
    const domain::PageResult<domain::AccessListRecord>& page);
server::HttpResponse emptySuccessResponse(const domain::Uuid& requestId);

}  // namespace ocrservice::http::controllers::access_list
