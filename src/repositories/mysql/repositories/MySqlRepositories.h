#pragma once

#include <chrono>
#include <string_view>

#include "MySqlConnectionPool.h"
#include "Ports.h"

namespace ocrservice::repositories::mysql::repositories {

inline constexpr std::chrono::seconds kDefaultRepositoryLeaseTimeout{5};

struct RepositoryOptions final {
    std::chrono::milliseconds leaseTimeout = kDefaultRepositoryLeaseTimeout;
};

class MySqlAdminUserRepository final : public domain::IAdminUserRepository {
public:
    explicit MySqlAdminUserRepository(
        connection::MySqlConnectionPool& pool,
        RepositoryOptions options = {});

    domain::RepositoryResult<std::optional<domain::AdminUserRecord>> findByUsername(
        std::string_view username) override;

private:
    connection::MySqlConnectionPool& pool_;
    RepositoryOptions options_;
};

class MySqlDeviceRepository final : public domain::IDeviceRepository {
public:
    explicit MySqlDeviceRepository(
        connection::MySqlConnectionPool& pool,
        RepositoryOptions options = {});

    domain::RepositoryResult<std::optional<domain::DeviceRecord>> findByHttpTokenHash(
        const domain::Sha256Digest& tokenHash) override;

private:
    connection::MySqlConnectionPool& pool_;
    RepositoryOptions options_;
};

class MySqlRecognitionRepository final : public domain::IRecognitionRepository {
public:
    explicit MySqlRecognitionRepository(
        connection::MySqlConnectionPool& pool,
        RepositoryOptions options = {});

    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findByCapture(
        const domain::DeviceId& deviceId,
        const domain::CaptureId& captureId) override;
    domain::RepositoryResult<std::optional<domain::RecognitionRecord>> findById(
        const domain::RecognitionId& recognitionId) override;
    domain::RepositoryResult<domain::RecognitionRecord> insertProcessing(
        const domain::NewRecognition& recognition) override;
    domain::RepositoryResult<domain::RecognitionRecord> finalize(
        const domain::FinalizeRecognition& recognition) override;
    domain::RepositoryResult<std::vector<domain::RecognitionRecord>> failInterruptedOnStartup(
        domain::UtcTimePoint completedAtUtc) override;
    domain::RepositoryResult<domain::PageResult<domain::RecognitionRecord>> queryHistory(
        const domain::HistoryFilter& filter,
        const domain::PageRequest& page) override;
    domain::RepositoryResult<std::unique_ptr<domain::IHistoryCursor>> openHistoryCursor(
        const domain::HistoryFilter& filter) override;
    domain::RepositoryResult<domain::HistoryCursorResult> visitHistory(
        const domain::HistoryFilter& filter,
        const domain::HistoryVisitor& visitor) override;

private:
    connection::MySqlConnectionPool& pool_;
    RepositoryOptions options_;
};

class MySqlAccessListRepository final : public domain::IAccessListRepository {
public:
    explicit MySqlAccessListRepository(
        connection::MySqlConnectionPool& pool,
        RepositoryOptions options = {});

    domain::RepositoryResult<domain::PageResult<domain::AccessListRecord>> query(
        const domain::AccessListFilter& filter,
        const domain::PageRequest& page) override;
    domain::RepositoryResult<std::optional<domain::AccessListRecord>> lookup(
        const domain::PlateNumber& plateNumber) override;
    domain::AccessListInsertResult insert(const domain::NewAccessListRecord& record) override;
    domain::RepositoryResult<bool> remove(std::uint64_t id) override;

private:
    connection::MySqlConnectionPool& pool_;
    RepositoryOptions options_;
};

}  // namespace ocrservice::repositories::mysql::repositories
