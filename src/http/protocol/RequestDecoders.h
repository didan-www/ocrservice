#pragma once

#include <string>
#include <string_view>

#include "Identifiers.h"
#include "Recognition.h"

namespace ocrservice::http::protocol {

struct LoginRequest final {
    std::string username;
    std::string password;
    domain::Uuid clientId;
};

struct HeartbeatRequest final {
    domain::Uuid clientId;
    std::string appVersion;
};

struct AccessListCreateRequest final {
    domain::AccessListType listType;
    domain::PlateNumber plateNumber;
    std::string remark;
};

LoginRequest decodeLoginRequest(std::string_view body);
HeartbeatRequest decodeHeartbeatRequest(std::string_view body);
AccessListCreateRequest decodeAccessListCreateRequest(std::string_view body);
void requireEmptyRequestBody(std::string_view body);

}  // namespace ocrservice::http::protocol
