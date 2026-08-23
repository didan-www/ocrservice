#include "AccessListService.h"

#include <chrono>
#include <optional>
#include <utility>

namespace ocrservice::services::access_list {
namespace {

AccessListFailure mapRepositoryFailure(const domain::RepositoryFailure failure) noexcept {
    return failure == domain::RepositoryFailure::unavailable ?
               AccessListFailure::databaseUnavailable : AccessListFailure::internal;
}

}  // namespace

domain::UtcTimePoint SystemAccessListClock::nowUtc() {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return domain::UtcTimePoint(milliseconds.count());
}

AccessListService::AccessListService(
    domain::IAccessListRepository& repository,
    IAccessListClock& clock)
    : repository_(repository), clock_(clock) {}

PageResult AccessListService::query(
    const domain::AccessListFilter& filter,
    const domain::PageRequest& page) {
    auto result = repository_.query(filter, page);
    if (std::holds_alternative<domain::RepositoryFailure>(result)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
    }
    return std::get<domain::PageResult<domain::AccessListRecord>>(std::move(result));
}

RecordResult AccessListService::lookup(const domain::PlateNumber& plateNumber) {
    auto result = repository_.lookup(plateNumber);
    if (std::holds_alternative<domain::RepositoryFailure>(result)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
    }
    auto record = std::get<std::optional<domain::AccessListRecord>>(std::move(result));
    if (!record) {
        return AccessListFailure::notFound;
    }
    return std::move(*record);
}

RecordResult AccessListService::create(
    const domain::AccessListType listType,
    domain::PlateNumber plateNumber,
    std::string remark,
    AccessListCreator creator) {
    domain::NewAccessListRecord record(
        listType,
        std::move(plateNumber),
        std::move(remark),
        creator.userId,
        std::move(creator.displayName),
        clock_.nowUtc());
    auto result = repository_.insert(record);
    if (std::holds_alternative<domain::AccessListRecord>(result)) {
        return std::get<domain::AccessListRecord>(std::move(result));
    }
    if (std::holds_alternative<domain::AccessListConflict>(result)) {
        return std::get<domain::AccessListConflict>(result).existingListType() ==
                       domain::AccessListType::white ?
                   AccessListFailure::conflictWhite : AccessListFailure::conflictBlack;
    }
    return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
}

OperationResult AccessListService::remove(const std::uint64_t id) {
    auto result = repository_.remove(id);
    if (std::holds_alternative<domain::RepositoryFailure>(result)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
    }
    if (!std::get<bool>(result)) {
        return AccessListFailure::notFound;
    }
    return OperationSucceeded{};
}

}  // namespace ocrservice::services::access_list
