#pragma once

#include <map>
#include <string>
#include <string_view>

#include "Recognition.h"

namespace ocrservice::http::protocol {

using QueryParameters = std::map<std::string, std::string, std::less<>>;

struct HistoryQuery final {
    domain::HistoryFilter filter;
    domain::PageRequest page;
};

struct AccessListPageQuery final {
    domain::AccessListFilter filter;
    domain::PageRequest page;
};

std::string percentDecodeOnce(std::string_view encoded);
QueryParameters decodeQueryParameters(std::string_view rawQuery);
std::uint64_t parsePositiveSafeInteger(std::string_view value, std::string_view fieldName);

HistoryQuery decodeHistoryQuery(std::string_view rawQuery);
domain::HistoryFilter decodeHistoryExportQuery(std::string_view rawQuery);
AccessListPageQuery decodeAccessListPageQuery(std::string_view rawQuery);
domain::PlateNumber decodeAccessListLookupQuery(std::string_view rawQuery);

}  // namespace ocrservice::http::protocol
