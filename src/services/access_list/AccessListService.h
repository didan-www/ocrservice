#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "Ports.h"

namespace ocrservice::services::access_list {

enum class AccessListFailure {
    notFound,
    conflictWhite,
    conflictBlack,
    databaseUnavailable,
    internal
};

struct AccessListCreator final {
    std::uint64_t userId;
    std::string displayName;
};

struct OperationSucceeded final {};

using PageResult =
    std::variant<domain::PageResult<domain::AccessListRecord>, AccessListFailure>;
using RecordResult = std::variant<domain::AccessListRecord, AccessListFailure>;
using OperationResult = std::variant<OperationSucceeded, AccessListFailure>;

class IAccessListClock {
public:
    virtual ~IAccessListClock() = default;
    virtual domain::UtcTimePoint nowUtc() = 0;
};

class SystemAccessListClock final : public IAccessListClock {
public:
    domain::UtcTimePoint nowUtc() override;
};

class IAccessListService {
public:
    virtual ~IAccessListService() = default;

    virtual PageResult query(
        const domain::AccessListFilter& filter,
        const domain::PageRequest& page) = 0;
    virtual RecordResult lookup(const domain::PlateNumber& plateNumber) = 0;
    virtual RecordResult create(
        domain::AccessListType listType,
        domain::PlateNumber plateNumber,
        std::string remark,
        AccessListCreator creator) = 0;
    virtual OperationResult remove(std::uint64_t id) = 0;
};

class AccessListService final : public IAccessListService {
public:
    AccessListService(
        domain::IAccessListRepository& repository,
        IAccessListClock& clock);

    PageResult query(
        const domain::AccessListFilter& filter,
        const domain::PageRequest& page) override;
    RecordResult lookup(const domain::PlateNumber& plateNumber) override;
    RecordResult create(
        domain::AccessListType listType,
        domain::PlateNumber plateNumber,
        std::string remark,
        AccessListCreator creator) override;
    OperationResult remove(std::uint64_t id) override;

private:
    domain::IAccessListRepository& repository_;
    IAccessListClock& clock_;
};

}  // namespace ocrservice::services::access_list
