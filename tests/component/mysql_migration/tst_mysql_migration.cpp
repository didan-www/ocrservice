#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <mysql_driver.h>

#include "MySqlConnectionPool.h"
#include "MySqlMigrator.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::app::config::MySqlConfig;
using ocrservice::repositories::mysql::connection::ConnectionPoolError;
using ocrservice::repositories::mysql::connection::ConnectionPoolOptions;
using ocrservice::repositories::mysql::connection::MySqlConnectionPool;
using ocrservice::repositories::mysql::migration::MigrationError;
using ocrservice::repositories::mysql::migration::MigrationOptions;
using ocrservice::repositories::mysql::migration::MySqlMigrator;

constexpr std::string_view kAdminHash =
    "$2b$12$abcdefghijklmnopqrstuumj.RgFt55etWFiErc2FEn.hnLKiyjYi";
constexpr std::string_view kDeviceHash =
    "3ce1a47030c0f5fdcd04315f246c4ef4c5f70bc70e8320efdac6c092c8fe1186";

const char* environment(const char* const name) {
    const auto* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

std::uint16_t parsePort(const char* const value) {
    const auto parsed = std::stoul(value);
    if (parsed == 0U || parsed > 65535U) {
        throw std::runtime_error("invalid test port");
    }
    return static_cast<std::uint16_t>(parsed);
}

MySqlConfig loadConfig() {
    const auto* const host = environment("OCRSERVICE_TEST_MYSQL_HOST");
    const auto* const port = environment("OCRSERVICE_TEST_MYSQL_PORT");
    const auto* const database = environment("OCRSERVICE_TEST_MYSQL_DATABASE");
    const auto* const user = environment("OCRSERVICE_TEST_MYSQL_USER");
    const auto* const password = environment("OCRSERVICE_TEST_MYSQL_PASSWORD");
    if (host == nullptr || port == nullptr || database == nullptr || user == nullptr ||
        password == nullptr) {
        throw std::runtime_error("MySQL test environment is incomplete");
    }
    return MySqlConfig{host, parsePort(port), database, user, password};
}

std::unique_ptr<sql::Connection> connect(const MySqlConfig& config) {
    sql::ConnectOptionsMap properties;
    properties["hostName"] = config.host;
    properties["port"] = static_cast<int>(config.port);
    properties["userName"] = config.user;
    properties["password"] = config.password;
    properties["schema"] = config.database;
    auto* const driver = sql::mysql::get_mysql_driver_instance();
    return std::unique_ptr<sql::Connection>(driver->connect(properties));
}

void execute(sql::Connection& connection, const std::string& text) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    (void)statement->execute(text);
}

int namedLock(
    sql::Connection& connection,
    const std::string& function,
    const std::string& name,
    const unsigned int waitSeconds = 0U) {
    std::unique_ptr<sql::PreparedStatement> statement(connection.prepareStatement(
        "SELECT " + function + (function == "GET_LOCK" ? "(?, ?)" : "(?)")));
    statement->setString(1U, name);
    if (function == "GET_LOCK") {
        statement->setUInt(2U, waitSeconds);
    }
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    if (!result->next() || result->isNull(1U)) {
        return -1;
    }
    return result->getInt(1U);
}

std::string scalar(sql::Connection& connection, const std::string& text) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery(text));
    if (!result->next() || result->isNull(1U)) {
        throw std::runtime_error("query returned no scalar");
    }
    return result->getString(1U).asStdString();
}

std::vector<std::string> rows(
    sql::Connection& connection,
    const std::string& text,
    const unsigned int columns) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery(text));
    std::vector<std::string> values;
    while (result->next()) {
        std::string row;
        for (unsigned int column = 1U; column <= columns; ++column) {
            if (column > 1U) {
                row.push_back('|');
            }
            row += result->isNull(column) ? "<NULL>"
                                          : result->getString(column).asStdString();
        }
        values.push_back(std::move(row));
    }
    return values;
}

void cleanSchema(const MySqlConfig& config) {
    auto connection = connect(config);
    execute(*connection, "SET FOREIGN_KEY_CHECKS = 0");
    execute(*connection, "DROP TABLE IF EXISTS access_lists");
    execute(*connection, "DROP TABLE IF EXISTS recognition_logs");
    execute(*connection, "DROP TABLE IF EXISTS devices");
    execute(*connection, "DROP TABLE IF EXISTS admin_users");
    execute(*connection, "DROP TABLE IF EXISTS schema_migrations");
    execute(*connection, "SET FOREIGN_KEY_CHECKS = 1");
}

void restoreSchemaDefaults(const MySqlConfig& config) {
    auto connection = connect(config);
    execute(
        *connection,
        "ALTER DATABASE CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs");
}

ConnectionPoolOptions oneConnection() {
    ConnectionPoolOptions options;
    options.size = 1U;
    options.connectRetryBudget = 3s;
    options.retryDelay = 50ms;
    return options;
}

std::filesystem::path migrations() {
    return std::filesystem::path(OCRSERVICE_SOURCE_DIR) / "migrations";
}

class TemporaryMigrations final {
public:
    TemporaryMigrations() {
        static std::atomic<unsigned long long> sequence{0U};
        path_ = std::filesystem::temp_directory_path() /
                ("ocrservice-migrations-" +
                 std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
        std::filesystem::create_directory(path_);
    }

    ~TemporaryMigrations() { std::filesystem::remove_all(path_); }
    TemporaryMigrations(const TemporaryMigrations&) = delete;
    TemporaryMigrations& operator=(const TemporaryMigrations&) = delete;

    void copyInitial(const std::string& name = "001_initial.sql") const {
        std::filesystem::copy_file(
            migrations() / "001_initial.sql",
            path_ / name,
            std::filesystem::copy_options::overwrite_existing);
    }

    void append(const std::string& name, const std::string& text) const {
        std::ofstream output(path_ / name, std::ios::binary | std::ios::app);
        output << text;
        if (!output) {
            throw std::runtime_error("failed to write temporary migration");
        }
    }

    void rename(const std::string& from, const std::string& to) const {
        std::filesystem::rename(path_ / from, path_ / to);
    }

    void remove(const std::string& name) const { std::filesystem::remove(path_ / name); }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class MySqlComponentTest : public testing::Test {
protected:
    void SetUp() override {
        if (environment("OCRSERVICE_TEST_MYSQL_HOST") == nullptr) {
            GTEST_SKIP() << "set OCRSERVICE_TEST_MYSQL_* to run MySQL component tests";
        }
        config_ = loadConfig();
        restoreSchemaDefaults(config_);
        cleanSchema(config_);
        configured_ = true;
    }

    void TearDown() override {
        if (configured_) {
            try {
                restoreSchemaDefaults(config_);
                cleanSchema(config_);
            } catch (...) {
            }
        }
    }

    void migrate(const std::filesystem::path& directory = migrations()) {
        MySqlConnectionPool pool(config_, oneConnection());
        MySqlMigrator migrator(pool, directory, MigrationOptions{3s, 3U});
        migrator.migrate();
    }

    MySqlConfig config_;
    bool configured_ = false;
};

TEST_F(MySqlComponentTest, EmptySchemaCreatesExactTablesHistoryAndSeed) {
    migrate();
    auto connection = connect(config_);
    EXPECT_EQ(
        rows(
            *connection,
            "SELECT TABLE_NAME, ENGINE, TABLE_COLLATION FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME",
            3U),
        (std::vector<std::string>{
            "access_lists|InnoDB|utf8mb4_0900_as_cs",
            "admin_users|InnoDB|utf8mb4_0900_as_cs",
            "devices|InnoDB|utf8mb4_0900_as_cs",
            "recognition_logs|InnoDB|utf8mb4_0900_as_cs",
            "schema_migrations|InnoDB|utf8mb4_0900_as_cs",
        }));
    EXPECT_EQ(
        rows(
            *connection,
            "SELECT username, display_name, password_hash, enabled, created_at = updated_at "
            "FROM admin_users ORDER BY id",
            5U),
        (std::vector<std::string>{
            "admin|演示管理员|" + std::string(kAdminHash) + "|1|1"}));
    EXPECT_EQ(
        rows(
            *connection,
            "SELECT device_id, device_name, http_token_hash, mqtt_username, enabled, "
            "created_at = updated_at FROM devices ORDER BY device_id",
            6U),
        (std::vector<std::string>{
            "device-001|入口设备|" + std::string(kDeviceHash) + "|device-001|1|1"}));
    const auto history = rows(
        *connection,
        "SELECT version, name, checksum REGEXP '^[0-9a-f]{64}$', "
        "ABS(TIMESTAMPDIFF(SECOND, applied_at, UTC_TIMESTAMP(3))) < 60 "
        "FROM schema_migrations",
        4U);
    EXPECT_EQ(history, (std::vector<std::string>{"1|001_initial.sql|1|1"}));
}

TEST_F(MySqlComponentTest, RepeatedStartupPreservesExistingSeedValues) {
    migrate();
    auto connection = connect(config_);
    execute(
        *connection,
        "UPDATE admin_users SET display_name = 'changed', password_hash = 'changed' WHERE id = 1");
    execute(
        *connection,
        "UPDATE devices SET device_name = 'changed', enabled = 0 WHERE device_id = 'device-001'");
    migrate();
    EXPECT_EQ(
        scalar(*connection, "SELECT CONCAT(display_name, '|', password_hash) FROM admin_users WHERE id = 1"),
        "changed|changed");
    EXPECT_EQ(
        scalar(*connection, "SELECT CONCAT(device_name, '|', enabled) FROM devices WHERE device_id = 'device-001'"),
        "changed|0");
}

TEST_F(MySqlComponentTest, RejectsChecksumNameAndMissingAppliedMigrationDrift) {
    TemporaryMigrations files;
    files.copyInitial();
    migrate(files.path());

    files.append("001_initial.sql", "\n-- drift\n");
    EXPECT_THROW(migrate(files.path()), MigrationError);
    files.copyInitial();

    files.rename("001_initial.sql", "001_renamed.sql");
    EXPECT_THROW(migrate(files.path()), MigrationError);
    files.rename("001_renamed.sql", "001_initial.sql");

    files.remove("001_initial.sql");
    EXPECT_THROW(migrate(files.path()), MigrationError);
}

TEST_F(MySqlComponentTest, RejectsDuplicateVersionAndLowerVersionBackfill) {
    TemporaryMigrations duplicates;
    duplicates.copyInitial();
    duplicates.copyInitial("001_duplicate.sql");
    EXPECT_THROW(migrate(duplicates.path()), MigrationError);

    cleanSchema(config_);
    TemporaryMigrations backfill;
    backfill.copyInitial("002_initial.sql");
    migrate(backfill.path());
    backfill.copyInitial("001_backfill.sql");
    EXPECT_THROW(migrate(backfill.path()), MigrationError);
}

TEST_F(MySqlComponentTest, RejectsUnexpectedSchemaChangesAndEverySeedIdentityConflict) {
    migrate();
    auto connection = connect(config_);
    execute(*connection, "ALTER TABLE devices ADD COLUMN unexpected INT NULL");
    EXPECT_THROW(migrate(), MigrationError);

    cleanSchema(config_);
    migrate();
    connection = connect(config_);
    execute(*connection, "DELETE FROM schema_migrations");
    execute(*connection, "DELETE FROM admin_users WHERE id = 1");
    execute(
        *connection,
        "INSERT INTO admin_users(id, username, display_name, password_hash, enabled, "
        "created_at, updated_at) VALUES (2, 'admin', 'other', 'other', 1, "
        "UTC_TIMESTAMP(3), UTC_TIMESTAMP(3))");
    EXPECT_THROW(migrate(), MigrationError);
    EXPECT_EQ(scalar(*connection, "SELECT COUNT(*) FROM schema_migrations"), "0");

    cleanSchema(config_);
    migrate();
    connection = connect(config_);
    execute(*connection, "DELETE FROM schema_migrations");
    execute(*connection, "DELETE FROM devices WHERE device_id = 'device-001'");
    execute(
        *connection,
        "INSERT INTO devices(device_id, device_name, http_token_hash, mqtt_username, enabled, "
        "created_at, updated_at) VALUES ('other', 'other', '" + std::string(kDeviceHash) +
            "', 'other', 1, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3))");
    EXPECT_THROW(migrate(), MigrationError);
    EXPECT_EQ(scalar(*connection, "SELECT COUNT(*) FROM schema_migrations"), "0");

    cleanSchema(config_);
    migrate();
    connection = connect(config_);
    execute(*connection, "DELETE FROM schema_migrations");
    execute(*connection, "DELETE FROM devices WHERE device_id = 'device-001'");
    execute(
        *connection,
        "INSERT INTO devices(device_id, device_name, http_token_hash, mqtt_username, enabled, "
        "created_at, updated_at) VALUES ('other', 'other', REPEAT('a', 64), "
        "'device-001', 1, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3))");
    EXPECT_THROW(migrate(), MigrationError);
    EXPECT_EQ(scalar(*connection, "SELECT COUNT(*) FROM schema_migrations"), "0");
}

TEST_F(MySqlComponentTest, RejectsSchemaDefaultsAndSemanticCheckDrift) {
    migrate();
    auto connection = connect(config_);
    execute(
        *connection,
        "ALTER DATABASE CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci");
    EXPECT_THROW(migrate(), MigrationError);
    restoreSchemaDefaults(config_);

    execute(
        *connection,
        "ALTER TABLE recognition_logs DROP CHECK chk_recognition_state, "
        "ADD CONSTRAINT chk_recognition_state CHECK ("
        "(status = 'processing' AND plate_number IS NULL AND error_code IS NULL "
        "AND error_message IS NULL AND completed_at IS NULL AND duration_ms IS NULL) OR "
        "(status = 'SUCCEEDED' AND plate_number IS NOT NULL AND error_code IS NULL "
        "AND error_message IS NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL) OR "
        "(status = 'FAILED' AND plate_number IS NULL AND error_code IS NOT NULL "
        "AND error_message IS NOT NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL))");
    EXPECT_THROW(migrate(), MigrationError);

    cleanSchema(config_);
    migrate();
    connection = connect(config_);
    execute(
        *connection,
        "ALTER TABLE recognition_logs DROP CHECK chk_recognition_state, "
        "ADD CONSTRAINT chk_recognition_state CHECK ("
        "(status = 'PROCESSING' OR status = 'SUCCEEDED') AND plate_number IS NOT NULL "
        "OR status = 'FAILED' AND error_code IS NOT NULL)");
    EXPECT_THROW(migrate(), MigrationError);
}

TEST_F(MySqlComponentTest, AdvisoryLockWaitsThenTimesOutWithoutPollutingPool) {
    auto holder = connect(config_);
    const auto lockName = scalar(
        *holder,
        "SELECT CONCAT('ocrservice:migration:', LEFT(SHA2(DATABASE(), 256), 43))");
    ASSERT_EQ(namedLock(*holder, "GET_LOCK", lockName), 1);

    MySqlConnectionPool waitingPool(config_, oneConnection());
    MySqlMigrator waitingMigrator(
        waitingPool, migrations(), MigrationOptions{3s, 3U});
    auto waiting = std::async(
        std::launch::async, [&waitingMigrator] { waitingMigrator.migrate(); });
    EXPECT_EQ(waiting.wait_for(200ms), std::future_status::timeout);
    ASSERT_EQ(namedLock(*holder, "RELEASE_LOCK", lockName), 1);
    EXPECT_NO_THROW(waiting.get());

    cleanSchema(config_);
    ASSERT_EQ(namedLock(*holder, "GET_LOCK", lockName), 1);
    MySqlConnectionPool timeoutPool(config_, oneConnection());
    MySqlMigrator timeoutMigrator(
        timeoutPool, migrations(), MigrationOptions{3s, 1U});
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(timeoutMigrator.migrate(), MigrationError);
    EXPECT_GE(std::chrono::steady_clock::now() - started, 800ms);
    EXPECT_EQ(timeoutPool.idleCount(), 1U);
    ASSERT_EQ(namedLock(*holder, "RELEASE_LOCK", lockName), 1);
    EXPECT_NO_THROW(timeoutMigrator.migrate());
}

TEST_F(MySqlComponentTest, AdvisoryLockAcquisitionExceptionDiscardsUncertainConnection) {
    auto holder = connect(config_);
    auto killer = connect(config_);
    const auto lockName = scalar(
        *holder,
        "SELECT CONCAT('ocrservice:migration:', LEFT(SHA2(DATABASE(), 256), 43))");
    ASSERT_EQ(namedLock(*holder, "GET_LOCK", lockName), 1);

    MySqlConnectionPool pool(config_, oneConnection());
    std::string killedConnectionId;
    {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        killedConnectionId = scalar(lease->connection(), "SELECT CONNECTION_ID()");
    }
    MySqlMigrator migrator(pool, migrations(), MigrationOptions{3s, 3U});
    auto blocked = std::async(std::launch::async, [&migrator] { migrator.migrate(); });
    ASSERT_EQ(blocked.wait_for(200ms), std::future_status::timeout);
    execute(*killer, "KILL CONNECTION " + killedConnectionId);
    EXPECT_THROW(blocked.get(), MigrationError);
    EXPECT_EQ(pool.idleCount(), 0U);

    ASSERT_EQ(namedLock(*holder, "RELEASE_LOCK", lockName), 1);
    auto replacement = pool.acquire(3s);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(
        scalar(replacement->connection(), "SELECT CONNECTION_ID()"),
        killedConnectionId);
    EXPECT_EQ(namedLock(replacement->connection(), "GET_LOCK", lockName), 1);
    EXPECT_EQ(namedLock(replacement->connection(), "RELEASE_LOCK", lockName), 1);
}

TEST_F(MySqlComponentTest, FailedLockReleaseAndBrokenLockConnectionAreDiscarded) {
    TemporaryMigrations released;
    released.copyInitial();
    released.append("001_initial.sql", "\nDO RELEASE_ALL_LOCKS();\n");
    MySqlConnectionPool releasedPool(config_, oneConnection());
    MySqlMigrator releasedMigrator(
        releasedPool, released.path(), MigrationOptions{3s, 3U});
    EXPECT_THROW(releasedMigrator.migrate(), MigrationError);
    EXPECT_EQ(releasedPool.idleCount(), 0U);
    {
        auto replacement = releasedPool.acquire(3s);
        ASSERT_TRUE(replacement.has_value());
        EXPECT_EQ(scalar(replacement->connection(), "SELECT 1"), "1");
    }
    EXPECT_NO_THROW(releasedMigrator.migrate());

    cleanSchema(config_);
    TemporaryMigrations killed;
    killed.copyInitial();
    killed.append(
        "001_initial.sql",
        "\nSET @ocrservice_kill_sql = CONCAT('KILL CONNECTION ', CONNECTION_ID());\n"
        "PREPARE ocrservice_kill FROM @ocrservice_kill_sql;\n"
        "EXECUTE ocrservice_kill;\n");
    MySqlConnectionPool killedPool(config_, oneConnection());
    MySqlMigrator killedMigrator(
        killedPool, killed.path(), MigrationOptions{3s, 3U});
    EXPECT_THROW(killedMigrator.migrate(), MigrationError);
    EXPECT_EQ(killedPool.idleCount(), 0U);
    {
        auto replacement = killedPool.acquire(3s);
        ASSERT_TRUE(replacement.has_value());
        EXPECT_EQ(scalar(replacement->connection(), "SELECT 1"), "1");
    }
    MySqlMigrator recovery(killedPool, migrations(), MigrationOptions{3s, 3U});
    EXPECT_NO_THROW(recovery.migrate());
}

TEST_F(MySqlComponentTest, DefaultPoolProvidesSixUtcConnections) {
    MySqlConnectionPool pool(config_);
    std::vector<MySqlConnectionPool::Lease> leases;
    for (std::size_t index = 0U; index < 6U; ++index) {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        EXPECT_EQ(scalar(lease->connection(), "SELECT @@session.time_zone"), "+00:00");
        leases.push_back(std::move(*lease));
    }
    EXPECT_FALSE(pool.acquire(20ms).has_value());
}

TEST_F(MySqlComponentTest, HealthProbeUsesIndependentConnectionWhenBusinessPoolIsExhausted) {
    MySqlConnectionPool pool(config_, oneConnection());
    EXPECT_TRUE(pool.ping());

    auto held = pool.acquire(1s);
    ASSERT_TRUE(held.has_value());
    EXPECT_EQ(pool.idleCount(), 0U);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(pool.ping());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, 1s);
    EXPECT_EQ(pool.idleCount(), 0U);
}

TEST_F(MySqlComponentTest, HealthProbeNeverInspectsOrReplacesBusinessPoolConnections) {
    MySqlConnectionPool pool(config_, oneConnection());
    std::string connectionId;
    {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        connectionId = scalar(lease->connection(), "SELECT CONNECTION_ID()");
    }
    auto killer = connect(config_);
    execute(*killer, "KILL CONNECTION " + connectionId);

    EXPECT_TRUE(pool.ping());
    EXPECT_EQ(pool.idleCount(), 1U);

    pool.close();
    EXPECT_FALSE(pool.ping());
}

TEST_F(MySqlComponentTest, ReturningLeaseRollsBackAndResetsSession) {
    migrate();
    MySqlConnectionPool pool(config_, oneConnection());
    {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        lease->connection().setAutoCommit(false);
        execute(lease->connection(), "UPDATE devices SET device_name = 'not-committed' WHERE device_id = 'device-001'");
        execute(lease->connection(), "SET time_zone = '+08:00'");
        lease->connection().setSchema("information_schema");
    }
    {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        EXPECT_TRUE(lease->connection().getAutoCommit());
        EXPECT_EQ(lease->connection().getSchema(), config_.database);
        EXPECT_EQ(scalar(lease->connection(), "SELECT @@session.time_zone"), "+00:00");
        EXPECT_EQ(
            scalar(lease->connection(), "SELECT device_name FROM devices WHERE device_id = 'device-001'"),
            "入口设备");
    }
    {
        auto lease = pool.acquire(1s);
        ASSERT_TRUE(lease.has_value());
        execute(lease->connection(), "START TRANSACTION");
        EXPECT_TRUE(lease->connection().getAutoCommit());
        EXPECT_EQ(scalar(lease->connection(), "SELECT @@autocommit"), "1");
        execute(
            lease->connection(),
            "UPDATE devices SET device_name = 'explicit-transaction' "
            "WHERE device_id = 'device-001'");
    }
    auto lease = pool.acquire(1s);
    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(
        scalar(lease->connection(), "SELECT device_name FROM devices WHERE device_id = 'device-001'"),
        "入口设备");
}

TEST_F(MySqlComponentTest, LeaseStateOutlivesDestroyedPoolWithoutDanglingOwner) {
    std::optional<MySqlConnectionPool::Lease> survivingLease;
    {
        auto pool = std::make_unique<MySqlConnectionPool>(config_, oneConnection());
        survivingLease = pool->acquire(1s);
        ASSERT_TRUE(survivingLease.has_value());
        pool.reset();
        EXPECT_EQ(scalar(survivingLease->connection(), "SELECT 1"), "1");
    }
    EXPECT_NO_THROW(survivingLease.reset());
}

TEST_F(MySqlComponentTest, CloseWakesWaiterAndKilledConnectionIsReplaced) {
    {
        MySqlConnectionPool pool(config_, oneConnection());
        auto held = pool.acquire(1s);
        ASSERT_TRUE(held.has_value());
        auto waiter = std::async(std::launch::async, [&pool] { return pool.acquire(5s); });
        std::this_thread::sleep_for(50ms);
        pool.close();
        EXPECT_EQ(waiter.wait_for(500ms), std::future_status::ready);
        EXPECT_FALSE(waiter.get().has_value());
    }

    MySqlConnectionPool victimPool(config_, oneConnection());
    MySqlConnectionPool killerPool(config_, oneConnection());
    std::string oldId;
    {
        auto victim = victimPool.acquire(1s);
        auto killer = killerPool.acquire(1s);
        ASSERT_TRUE(victim.has_value());
        ASSERT_TRUE(killer.has_value());
        oldId = scalar(victim->connection(), "SELECT CONNECTION_ID()");
        execute(killer->connection(), "KILL CONNECTION " + oldId);
    }
    EXPECT_EQ(victimPool.idleCount(), 0U);
    auto replacement = victimPool.acquire(3s);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(scalar(replacement->connection(), "SELECT CONNECTION_ID()"), oldId);
    EXPECT_EQ(scalar(replacement->connection(), "SELECT @@session.time_zone"), "+00:00");
}

TEST(MySqlConnectionFailureTest, RetriesWithinBudgetThenFails) {
    if (environment("OCRSERVICE_TEST_MYSQL_HOST") == nullptr) {
        GTEST_SKIP() << "set OCRSERVICE_TEST_MYSQL_* to run MySQL component tests";
    }
    auto config = loadConfig();
    config.port = 1U;
    auto options = oneConnection();
    options.connectRetryBudget = 200ms;
    options.retryDelay = 25ms;
    EXPECT_THROW(MySqlConnectionPool(config, options), ConnectionPoolError);
}

}  // namespace
