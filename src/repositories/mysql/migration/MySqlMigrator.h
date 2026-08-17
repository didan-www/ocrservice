#pragma once

#include <chrono>
#include <filesystem>
#include <stdexcept>

#include "MySqlConnectionPool.h"

namespace ocrservice::repositories::mysql::migration {

inline constexpr std::chrono::seconds kDefaultMigrationLockWait{60};

struct MigrationOptions final {
    std::chrono::milliseconds leaseWait{kDefaultMigrationLockWait};
    unsigned int lockWaitSeconds = 60U;
};

class MigrationError final : public std::runtime_error {
public:
    MigrationError();
};

class MySqlMigrator final {
public:
    MySqlMigrator(
        connection::MySqlConnectionPool& pool,
        std::filesystem::path migrationDirectory,
        MigrationOptions options = {});

    void migrate();

private:
    connection::MySqlConnectionPool& pool_;
    std::filesystem::path migrationDirectory_;
    MigrationOptions options_;
};

}  // namespace ocrservice::repositories::mysql::migration
