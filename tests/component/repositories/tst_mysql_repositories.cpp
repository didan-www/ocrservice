#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <mysql_driver.h>

#include "MySqlConnectionPool.h"
#include "MySqlMigrator.h"
#include "MySqlRepositories.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::app::config::MySqlConfig;
using ocrservice::domain::AccessListConflict;
using ocrservice::domain::AccessListFilter;
using ocrservice::domain::AccessListRecord;
using ocrservice::domain::AccessListType;
using ocrservice::domain::CaptureId;
using ocrservice::domain::DeviceId;
using ocrservice::domain::FinalizeRecognition;
using ocrservice::domain::HistoryCursorResult;
using ocrservice::domain::HistoryFilter;
using ocrservice::domain::ImageMime;
using ocrservice::domain::ModelFailureCode;
using ocrservice::domain::NewAccessListRecord;
using ocrservice::domain::NewRecognition;
using ocrservice::domain::PageRequest;
using ocrservice::domain::PlateKeyword;
using ocrservice::domain::PlateNumber;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionOutcome;
using ocrservice::domain::RecognitionRecord;
using ocrservice::domain::RecognitionStatus;
using ocrservice::domain::RelativeImagePath;
using ocrservice::domain::RepositoryFailure;
using ocrservice::domain::Sha256Digest;
using ocrservice::domain::UtcTimePoint;
using ocrservice::repositories::mysql::connection::ConnectionPoolOptions;
using ocrservice::repositories::mysql::connection::MySqlConnectionPool;
using ocrservice::repositories::mysql::migration::MigrationOptions;
using ocrservice::repositories::mysql::migration::MySqlMigrator;
using ocrservice::repositories::mysql::repositories::MySqlAccessListRepository;
using ocrservice::repositories::mysql::repositories::MySqlAdminUserRepository;
using ocrservice::repositories::mysql::repositories::MySqlDeviceRepository;
using ocrservice::repositories::mysql::repositories::MySqlRecognitionRepository;
using ocrservice::repositories::mysql::repositories::RepositoryOptions;

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
    auto connection = std::unique_ptr<sql::Connection>(driver->connect(properties));
    std::unique_ptr<sql::Statement> statement(connection->createStatement());
    (void)statement->execute("SET time_zone = '+00:00'");
    return connection;
}

void execute(sql::Connection& connection, const std::string& text) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    (void)statement->execute(text);
}

std::uint64_t scalar(sql::Connection& connection, const std::string& text) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery(text));
    if (!result->next()) {
        throw std::runtime_error("query returned no scalar");
    }
    return result->getUInt64(1U);
}

ConnectionPoolOptions poolOptions(const std::size_t size = 3U) {
    ConnectionPoolOptions options;
    options.size = size;
    options.connectRetryBudget = 3s;
    options.retryDelay = 20ms;
    return options;
}

std::string uuidText(const std::uint64_t value) {
    char output[37]{};
    const int written = std::snprintf(
        output,
        sizeof(output),
        "00000000-0000-4000-8000-%012llx",
        static_cast<unsigned long long>(value));
    if (written != 36) {
        throw std::runtime_error("failed to create test UUID");
    }
    return std::string(output);
}

RecognitionId recognitionId(const std::uint64_t value) {
    return RecognitionId::parse(uuidText(value));
}

CaptureId captureId(const std::uint64_t value) {
    return CaptureId::parse(uuidText(value + 0x100000U));
}

Sha256Digest digest(const char value = 'a') {
    return Sha256Digest::parseHex(std::string(64U, value));
}

NewRecognition newRecognition(
    const std::uint64_t value,
    const std::int64_t capturedAt = 1000000,
    const DeviceId& deviceId = DeviceId::parse("device-001"),
    const CaptureId* suppliedCapture = nullptr) {
    const auto capture = suppliedCapture == nullptr ? captureId(value) : *suppliedCapture;
    return NewRecognition(
        recognitionId(value),
        deviceId,
        capture,
        digest(),
        RelativeImagePath::parseGenerated("2026/08/17/" + uuidText(value) + ".jpg"),
        ImageMime::jpeg,
        128U,
        UtcTimePoint(capturedAt),
        UtcTimePoint(capturedAt + 100));
}

NewAccessListRecord newAccess(
    const AccessListType type,
    const std::string& plate,
    const std::string& remark = "") {
    return NewAccessListRecord(
        type,
        PlateNumber::parse(plate),
        remark,
        1U,
        "演示管理员",
        UtcTimePoint(2000000));
}

template <typename T>
bool isFailure(const T& result, const RepositoryFailure expected) {
    return std::holds_alternative<RepositoryFailure>(result) &&
           std::get<RepositoryFailure>(result) == expected;
}

void cleanBusinessData(const MySqlConfig& config) {
    auto connection = connect(config);
    execute(*connection, "DELETE FROM access_lists");
    execute(*connection, "DELETE FROM recognition_logs");
    execute(*connection, "DELETE FROM devices WHERE device_id <> 'device-001'");
    execute(*connection, "DELETE FROM admin_users WHERE id <> 1");
}

class MySqlRepositoryTest : public testing::Test {
protected:
    void SetUp() override {
        if (environment("OCRSERVICE_TEST_MYSQL_HOST") == nullptr) {
            GTEST_SKIP() << "set OCRSERVICE_TEST_MYSQL_* to run repository component tests";
        }
        config_ = loadConfig();
        {
            MySqlConnectionPool pool(config_, poolOptions());
            MySqlMigrator migrator(
                pool,
                std::filesystem::path(OCRSERVICE_SOURCE_DIR) / "migrations",
                MigrationOptions{3s, 3U});
            migrator.migrate();
        }
        cleanBusinessData(config_);
        configured_ = true;
    }

    void TearDown() override {
        if (configured_) {
            try {
                cleanBusinessData(config_);
            } catch (...) {
            }
        }
    }

    MySqlConfig config_;
    bool configured_ = false;
};

TEST_F(MySqlRepositoryTest, AdminAndDeviceQueriesAreBoundAndRejectUnsafeRows) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlAdminUserRepository admins(pool);
    MySqlDeviceRepository devices(pool);

    auto admin = admins.findByUsername("admin");
    ASSERT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::AdminUserRecord>>(admin));
    ASSERT_TRUE(std::get<std::optional<ocrservice::domain::AdminUserRecord>>(admin));
    EXPECT_EQ(std::get<std::optional<ocrservice::domain::AdminUserRecord>>(admin)->id(), 1U);
    auto injection = admins.findByUsername("admin' OR '1'='1");
    ASSERT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::AdminUserRecord>>(injection));
    EXPECT_FALSE(std::get<std::optional<ocrservice::domain::AdminUserRecord>>(injection));

    auto device = devices.findByHttpTokenHash(
        Sha256Digest::parseHex(
            "3ce1a47030c0f5fdcd04315f246c4ef4c5f70bc70e8320efdac6c092c8fe1186"));
    ASSERT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::DeviceRecord>>(device));
    ASSERT_TRUE(std::get<std::optional<ocrservice::domain::DeviceRecord>>(device));
    EXPECT_EQ(
        std::get<std::optional<ocrservice::domain::DeviceRecord>>(device)->deviceId().value(),
        "device-001");
    auto missing = devices.findByHttpTokenHash(digest('b'));
    ASSERT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::DeviceRecord>>(missing));
    EXPECT_FALSE(std::get<std::optional<ocrservice::domain::DeviceRecord>>(missing));

    auto raw = connect(config_);
    execute(
        *raw,
        "INSERT INTO admin_users (id, username, display_name, password_hash, enabled, "
        "created_at, updated_at) VALUES (9007199254740992, 'unsafe', 'unsafe', 'hash', 1, "
        "'2026-01-01 00:00:00.000', '2026-01-01 00:00:00.000')");
    EXPECT_TRUE(isFailure(admins.findByUsername("unsafe"), RepositoryFailure::internal));
}

TEST_F(MySqlRepositoryTest, ProcessingInsertFindAndForeignKeyRollbackAreStable) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);

    const auto missingDevice = DeviceId::parse("missing-device");
    auto rejected = repository.insertProcessing(newRecognition(1U, 1000000, missingDevice));
    EXPECT_TRUE(isFailure(rejected, RepositoryFailure::notFound));
    auto raw = connect(config_);
    EXPECT_EQ(scalar(*raw, "SELECT COUNT(*) FROM recognition_logs"), 0U);

    const auto command = newRecognition(2U);
    auto inserted = repository.insertProcessing(command);
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(inserted));
    EXPECT_EQ(std::get<RecognitionRecord>(inserted).snapshot().revision(), 1U);
    EXPECT_EQ(std::get<RecognitionRecord>(inserted).snapshot().status(), RecognitionStatus::processing);

    auto byId = repository.findById(command.recognitionId());
    ASSERT_TRUE(std::holds_alternative<std::optional<RecognitionRecord>>(byId));
    ASSERT_TRUE(std::get<std::optional<RecognitionRecord>>(byId));
    EXPECT_EQ(
        std::get<std::optional<RecognitionRecord>>(byId)->captureId(), command.captureId());
    auto byCapture = repository.findByCapture(command.deviceId(), command.captureId());
    ASSERT_TRUE(std::holds_alternative<std::optional<RecognitionRecord>>(byCapture));
    ASSERT_TRUE(std::get<std::optional<RecognitionRecord>>(byCapture));
    EXPECT_EQ(
        std::get<std::optional<RecognitionRecord>>(byCapture)->snapshot().recognitionId(),
        command.recognitionId());
}

TEST_F(MySqlRepositoryTest, ConcurrentCaptureInsertCreatesOneRecordAndOneConflict) {
    MySqlConnectionPool pool(config_, poolOptions(2U));
    MySqlRecognitionRepository repository(pool);
    const auto sharedCapture = captureId(500U);
    const auto first = newRecognition(10U, 1000000, DeviceId::parse("device-001"), &sharedCapture);
    const auto second = newRecognition(11U, 1000000, DeviceId::parse("device-001"), &sharedCapture);
    std::promise<void> start;
    auto ready = start.get_future().share();
    auto firstFuture = std::async(std::launch::async, [&] {
        ready.wait();
        return repository.insertProcessing(first);
    });
    auto secondFuture = std::async(std::launch::async, [&] {
        ready.wait();
        return repository.insertProcessing(second);
    });
    start.set_value();
    auto firstResult = firstFuture.get();
    auto secondResult = secondFuture.get();
    const int successes = static_cast<int>(std::holds_alternative<RecognitionRecord>(firstResult)) +
                          static_cast<int>(std::holds_alternative<RecognitionRecord>(secondResult));
    const int conflicts = static_cast<int>(isFailure(firstResult, RepositoryFailure::conflict)) +
                          static_cast<int>(isFailure(secondResult, RepositoryFailure::conflict));
    EXPECT_EQ(successes, 1);
    EXPECT_EQ(conflicts, 1);
    auto raw = connect(config_);
    EXPECT_EQ(scalar(*raw, "SELECT COUNT(*) FROM recognition_logs"), 1U);
}

TEST_F(MySqlRepositoryTest, FinalizeLocksClassifiesZeroRowsAndUsesFixedMessages) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);

    const auto successCommand = newRecognition(20U);
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(successCommand)));
    auto success = repository.finalize(FinalizeRecognition{
        successCommand.recognitionId(),
        RecognitionOutcome::succeeded(PlateNumber::parse("京A12345")),
        UtcTimePoint(1000500),
        400U});
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(success));
    const auto& successSnapshot = std::get<RecognitionRecord>(success).snapshot();
    EXPECT_EQ(successSnapshot.status(), RecognitionStatus::succeeded);
    EXPECT_EQ(successSnapshot.revision(), 2U);
    EXPECT_EQ(successSnapshot.plateNumber()->value(), "京A12345");
    EXPECT_TRUE(isFailure(
        repository.finalize(FinalizeRecognition{
            successCommand.recognitionId(),
            RecognitionOutcome::failed(ModelFailureCode::modelInferenceError),
            UtcTimePoint(1000600),
            500U}),
        RepositoryFailure::stateConflict));
    EXPECT_TRUE(isFailure(
        repository.finalize(FinalizeRecognition{
            recognitionId(999U),
            RecognitionOutcome::failed(ModelFailureCode::modelInferenceError),
            UtcTimePoint(1000600),
            500U}),
        RepositoryFailure::notFound));

    const std::vector<std::pair<ModelFailureCode, std::string>> failures = {
        {ModelFailureCode::plateNotFound, "未检测到车牌"},
        {ModelFailureCode::plateRecognitionFailed, "车牌识别失败"},
        {ModelFailureCode::modelInferenceError, "模型推理失败"},
    };
    std::uint64_t sequence = 30U;
    for (const auto& [code, message] : failures) {
        const auto command = newRecognition(sequence++);
        ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(repository.insertProcessing(command)));
        auto result = repository.finalize(FinalizeRecognition{
            command.recognitionId(),
            RecognitionOutcome::failed(code),
            UtcTimePoint(1000500),
            400U});
        ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(result));
        const auto& snapshot = std::get<RecognitionRecord>(result).snapshot();
        EXPECT_EQ(snapshot.status(), RecognitionStatus::failed);
        ASSERT_TRUE(snapshot.errorMessage());
        EXPECT_EQ(*snapshot.errorMessage(), message);
    }
}

TEST_F(MySqlRepositoryTest, StartupRecoveryUsesEffectiveCompletionAndReturnsFinalRows) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(40U, 1000))));
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(41U, 5000))));

    auto result = repository.failInterruptedOnStartup(UtcTimePoint(3000));
    ASSERT_TRUE(std::holds_alternative<std::vector<RecognitionRecord>>(result));
    const auto& records = std::get<std::vector<RecognitionRecord>>(result);
    ASSERT_EQ(records.size(), 2U);
    for (const auto& record : records) {
        const auto& snapshot = record.snapshot();
        EXPECT_EQ(snapshot.status(), RecognitionStatus::failed);
        EXPECT_EQ(snapshot.revision(), 2U);
        EXPECT_EQ(*snapshot.errorMessage(), "服务重启，识别任务已中断");
        EXPECT_GE(*snapshot.completedAt(), snapshot.startedAt());
        EXPECT_EQ(
            *snapshot.durationMs(),
            static_cast<std::uint64_t>(
                snapshot.completedAt()->unixMilliseconds() -
                snapshot.startedAt().unixMilliseconds()));
    }
    auto empty = repository.failInterruptedOnStartup(UtcTimePoint(7000));
    ASSERT_TRUE(std::holds_alternative<std::vector<RecognitionRecord>>(empty));
    EXPECT_TRUE(std::get<std::vector<RecognitionRecord>>(empty).empty());
}

TEST_F(MySqlRepositoryTest, MapperRejectsDatabaseTimePastProtocolMaximum) {
    auto raw = connect(config_);
    execute(
        *raw,
        "INSERT INTO recognition_logs (recognition_id, device_id, capture_id, image_sha256, "
        "revision, status, plate_number, error_code, error_message, image_path, image_mime, "
        "image_size_bytes, captured_at, started_at, completed_at, duration_ms, created_at, "
        "updated_at) VALUES ('00000000-0000-4000-8000-000000000776', 'device-001', "
        "'00000000-0000-4000-8000-000000100776', "
        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1, "
        "'PROCESSING', NULL, NULL, NULL, "
        "'9999/12/31/00000000-0000-4000-8000-000000000776.jpg', 'image/jpeg', 1, "
        "'9999-12-31 15:59:59.999', '9999-12-31 15:59:59.999', NULL, NULL, "
        "'9999-12-31 15:59:59.999', '9999-12-31 15:59:59.999')");
    execute(
        *raw,
        "INSERT INTO recognition_logs (recognition_id, device_id, capture_id, image_sha256, "
        "revision, status, plate_number, error_code, error_message, image_path, image_mime, "
        "image_size_bytes, captured_at, started_at, completed_at, duration_ms, created_at, "
        "updated_at) VALUES ('00000000-0000-4000-8000-000000000777', 'device-001', "
        "'00000000-0000-4000-8000-000000100777', "
        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa', 1, "
        "'PROCESSING', NULL, NULL, NULL, "
        "'9999/12/31/00000000-0000-4000-8000-000000000777.jpg', 'image/jpeg', 1, "
        "'9999-12-31 16:00:00.000', '9999-12-31 16:00:00.000', NULL, NULL, "
        "'9999-12-31 16:00:00.000', '9999-12-31 16:00:00.000')");
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);
    auto maximum = repository.findById(RecognitionId::parse(
        "00000000-0000-4000-8000-000000000776"));
    ASSERT_TRUE(std::holds_alternative<std::optional<RecognitionRecord>>(maximum));
    ASSERT_TRUE(std::get<std::optional<RecognitionRecord>>(maximum));
    EXPECT_EQ(
        std::get<std::optional<RecognitionRecord>>(maximum)->snapshot().capturedAt(),
        UtcTimePoint(253402271999999LL));
    EXPECT_TRUE(isFailure(repository.findById(
                              RecognitionId::parse(
                                  "00000000-0000-4000-8000-000000000777")),
                          RepositoryFailure::internal));
}

TEST_F(MySqlRepositoryTest, HistoryUsesHalfOpenFilterStableOrderAndSafeOffset) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(50U, 1000))));
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(51U, 1500))));
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(52U, 1500))));
    ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
        repository.insertProcessing(newRecognition(53U, 2000))));

    const HistoryFilter filter(UtcTimePoint(1000), UtcTimePoint(2000));
    auto first = repository.queryHistory(filter, PageRequest(1U));
    ASSERT_TRUE(std::holds_alternative<ocrservice::domain::PageResult<RecognitionRecord>>(first));
    const auto& page = std::get<ocrservice::domain::PageResult<RecognitionRecord>>(first);
    ASSERT_EQ(page.total(), 3U);
    ASSERT_EQ(page.items().size(), 3U);
    EXPECT_EQ(page.items()[0].snapshot().recognitionId(), recognitionId(52U));
    EXPECT_EQ(page.items()[1].snapshot().recognitionId(), recognitionId(51U));
    EXPECT_EQ(page.items()[2].snapshot().recognitionId(), recognitionId(50U));

    auto beyond = repository.queryHistory(
        filter, PageRequest(ocrservice::domain::kJsonSafeIntegerMaximum));
    ASSERT_TRUE(std::holds_alternative<ocrservice::domain::PageResult<RecognitionRecord>>(beyond));
    EXPECT_EQ(std::get<ocrservice::domain::PageResult<RecognitionRecord>>(beyond).total(), 3U);
    EXPECT_TRUE(std::get<ocrservice::domain::PageResult<RecognitionRecord>>(beyond).items().empty());

    auto otherDevice = repository.queryHistory(
        HistoryFilter(
            UtcTimePoint(1000), UtcTimePoint(2000), DeviceId::parse("other-device")),
        PageRequest(1U));
    ASSERT_TRUE(std::holds_alternative<ocrservice::domain::PageResult<RecognitionRecord>>(
        otherDevice));
    EXPECT_EQ(
        std::get<ocrservice::domain::PageResult<RecognitionRecord>>(otherDevice).total(), 0U);
}

TEST_F(MySqlRepositoryTest, HistoryVisitorDefinesStopLastRowAndExceptionSemantics) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlRecognitionRepository repository(pool);
    for (std::uint64_t value = 60U; value < 63U; ++value) {
        ASSERT_TRUE(std::holds_alternative<RecognitionRecord>(
            repository.insertProcessing(newRecognition(value, 1000))));
    }
    const HistoryFilter filter(UtcTimePoint(0), UtcTimePoint(2000));

    auto stopped = repository.visitHistory(filter, [count = 0](const RecognitionRecord&) mutable {
        ++count;
        return count < 2;
    });
    ASSERT_TRUE(std::holds_alternative<HistoryCursorResult>(stopped));
    EXPECT_EQ(std::get<HistoryCursorResult>(stopped).recordsVisited(), 2U);
    EXPECT_FALSE(std::get<HistoryCursorResult>(stopped).fullyConsumed());

    auto stoppedAtLast = repository.visitHistory(
        filter, [count = 0](const RecognitionRecord&) mutable {
            ++count;
            return count < 3;
        });
    ASSERT_TRUE(std::holds_alternative<HistoryCursorResult>(stoppedAtLast));
    EXPECT_EQ(std::get<HistoryCursorResult>(stoppedAtLast).recordsVisited(), 3U);
    EXPECT_FALSE(std::get<HistoryCursorResult>(stoppedAtLast).fullyConsumed());

    EXPECT_THROW(
        repository.visitHistory(filter, [](const RecognitionRecord&) -> bool {
            throw std::runtime_error("visitor stopped");
        }),
        std::runtime_error);
    auto complete = repository.visitHistory(filter, [](const RecognitionRecord&) { return true; });
    ASSERT_TRUE(std::holds_alternative<HistoryCursorResult>(complete));
    EXPECT_EQ(std::get<HistoryCursorResult>(complete).recordsVisited(), 3U);
    EXPECT_TRUE(std::get<HistoryCursorResult>(complete).fullyConsumed());
    EXPECT_EQ(pool.idleCount(), pool.configuredSize());
}

TEST_F(MySqlRepositoryTest, AccessListsUseLiteralLikeGlobalConflictAndBoundValues) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlAccessListRepository repository(pool);
    for (const std::string plate : {"A%1", "A_1", "A\\1", "AX1"}) {
        auto inserted = repository.insert(newAccess(
            AccessListType::white, plate, "引号 ' 与通配符 %_\\"));
        ASSERT_TRUE(std::holds_alternative<AccessListRecord>(inserted));
    }
    const std::vector<std::pair<std::string, std::string>> keywords = {
        {"%", "A%1"}, {"_", "A_1"}, {"\\", "A\\1"}};
    for (const auto& [keyword, expected] : keywords) {
        auto result = repository.query(
            AccessListFilter(AccessListType::white, PlateKeyword::parse(keyword)),
            PageRequest(1U));
        ASSERT_TRUE(std::holds_alternative<ocrservice::domain::PageResult<AccessListRecord>>(result));
        const auto& page = std::get<ocrservice::domain::PageResult<AccessListRecord>>(result);
        ASSERT_EQ(page.total(), 1U);
        EXPECT_EQ(page.items()[0].plateNumber().value(), expected);
    }

    auto conflict = repository.insert(newAccess(AccessListType::black, "A%1"));
    ASSERT_TRUE(std::holds_alternative<AccessListConflict>(conflict));
    EXPECT_EQ(std::get<AccessListConflict>(conflict).existingListType(), AccessListType::white);
    auto lookup = repository.lookup(PlateNumber::parse("A%1"));
    ASSERT_TRUE(std::holds_alternative<std::optional<AccessListRecord>>(lookup));
    ASSERT_TRUE(std::get<std::optional<AccessListRecord>>(lookup));
    const auto id = std::get<std::optional<AccessListRecord>>(lookup)->id();
    auto removed = repository.remove(id);
    ASSERT_TRUE(std::holds_alternative<bool>(removed));
    EXPECT_TRUE(std::get<bool>(removed));
    auto removedAgain = repository.remove(id);
    ASSERT_TRUE(std::holds_alternative<bool>(removedAgain));
    EXPECT_FALSE(std::get<bool>(removedAgain));

    NewAccessListRecord missingUser(
        AccessListType::white,
        PlateNumber::parse("FK1"),
        "",
        999U,
        "missing",
        UtcTimePoint(2000000));
    EXPECT_TRUE(isFailure(repository.insert(missingUser), RepositoryFailure::notFound));
}

TEST_F(MySqlRepositoryTest, AccessInsertWaitsForConcurrentDeleteAndThenSucceeds) {
    MySqlConnectionPool pool(config_, poolOptions());
    MySqlAccessListRepository repository(pool);
    auto existing = repository.insert(newAccess(AccessListType::white, "RACE1"));
    ASSERT_TRUE(std::holds_alternative<AccessListRecord>(existing));
    const auto id = std::get<AccessListRecord>(existing).id();

    auto deleting = connect(config_);
    deleting->setAutoCommit(false);
    std::unique_ptr<sql::PreparedStatement> remove(
        deleting->prepareStatement("DELETE FROM access_lists WHERE id = ?"));
    remove->setUInt64(1U, id);
    ASSERT_EQ(remove->executeUpdate(), 1);
    auto insertion = std::async(std::launch::async, [&] {
        return repository.insert(newAccess(AccessListType::black, "RACE1"));
    });
    EXPECT_EQ(insertion.wait_for(100ms), std::future_status::timeout);
    deleting->commit();
    auto result = insertion.get();
    ASSERT_TRUE(std::holds_alternative<AccessListRecord>(result));
    EXPECT_EQ(std::get<AccessListRecord>(result).listType(), AccessListType::black);
}

TEST_F(MySqlRepositoryTest, LeaseTimeoutAndKilledConnectionMapUnavailableAndRecover) {
    MySqlConnectionPool pool(config_, poolOptions(1U));
    MySqlAdminUserRepository shortWait(pool, RepositoryOptions{50ms});
    auto held = pool.acquire(1s);
    ASSERT_TRUE(held);
    EXPECT_TRUE(isFailure(shortWait.findByUsername("admin"), RepositoryFailure::unavailable));
    held.reset();
    EXPECT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::AdminUserRecord>>(
        shortWait.findByUsername("admin")));

    auto lease = pool.acquire(1s);
    ASSERT_TRUE(lease);
    const auto connectionId = scalar(lease->connection(), "SELECT CONNECTION_ID()");
    lease.reset();
    auto killer = connect(config_);
    execute(*killer, "KILL CONNECTION " + std::to_string(connectionId));
    EXPECT_TRUE(isFailure(shortWait.findByUsername("admin"), RepositoryFailure::unavailable));
    EXPECT_EQ(pool.idleCount(), 0U);
    auto recovered = shortWait.findByUsername("admin");
    ASSERT_TRUE(std::holds_alternative<std::optional<ocrservice::domain::AdminUserRecord>>(
        recovered));
    EXPECT_TRUE(std::get<std::optional<ocrservice::domain::AdminUserRecord>>(recovered));
}

TEST_F(MySqlRepositoryTest, LockTimeoutMapsUnavailableWithoutDiscardingConnection) {
    MySqlConnectionPool pool(config_, poolOptions(1U));
    MySqlAccessListRepository repository(pool, RepositoryOptions{2s});
    auto inserted = repository.insert(newAccess(AccessListType::white, "LOCK1"));
    ASSERT_TRUE(std::holds_alternative<AccessListRecord>(inserted));
    const auto id = std::get<AccessListRecord>(inserted).id();

    auto configure = pool.acquire(1s);
    ASSERT_TRUE(configure);
    execute(configure->connection(), "SET SESSION innodb_lock_wait_timeout = 1");
    configure.reset();
    auto locker = connect(config_);
    locker->setAutoCommit(false);
    std::unique_ptr<sql::PreparedStatement> lock(
        locker->prepareStatement("UPDATE access_lists SET remark = 'held' WHERE id = ?"));
    lock->setUInt64(1U, id);
    ASSERT_EQ(lock->executeUpdate(), 1);

    EXPECT_TRUE(isFailure(repository.remove(id), RepositoryFailure::unavailable));
    EXPECT_EQ(pool.idleCount(), 1U);
    locker->rollback();
    auto removed = repository.remove(id);
    ASSERT_TRUE(std::holds_alternative<bool>(removed));
    EXPECT_TRUE(std::get<bool>(removed));
}

}  // namespace
