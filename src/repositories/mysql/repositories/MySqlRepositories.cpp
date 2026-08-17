#include "MySqlRepositories.h"

#include <array>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <cppconn/connection.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

namespace ocrservice::repositories::mysql::repositories {
namespace {

using connection::MySqlConnectionPool;
using domain::AccessListConflict;
using domain::AccessListInsertResult;
using domain::AccessListRecord;
using domain::AccessListType;
using domain::AdminUserRecord;
using domain::DeviceRecord;
using domain::HistoryCursorResult;
using domain::ImageMime;
using domain::RecognitionFailureCode;
using domain::RecognitionRecord;
using domain::RecognitionSnapshot;
using domain::RecognitionStatus;
using domain::RepositoryFailure;
using domain::RepositoryResult;
using domain::UtcTimePoint;

constexpr std::int64_t kMillisecondsPerSecond = 1000;
constexpr std::int64_t kSecondsPerDay = 86400;

enum class SqlContext { general, recognitionInsert };

struct SqlFailure final {
    RepositoryFailure failure;
    bool discard;
};

struct VisitorException final {
    std::exception_ptr cause;
};

void validateOptions(const RepositoryOptions& options) {
    if (options.leaseTimeout.count() <= 0) {
        throw std::invalid_argument("repository lease timeout must be positive");
    }
}

int parseDigits(const std::string_view input, const std::size_t offset, const std::size_t count) {
    int result = 0;
    for (std::size_t index = 0U; index < count; ++index) {
        const char value = input[offset + index];
        if (value < '0' || value > '9') {
            throw domain::DomainError("database time contains a non-digit");
        }
        result = result * 10 + (value - '0');
    }
    return result;
}

bool isLeapYear(const int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int daysInMonth(const int year, const int month) noexcept {
    constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && isLeapYear(year) ? 29 : kDays[static_cast<std::size_t>(month - 1)];
}

std::int64_t daysFromCivil(int year, const unsigned month, const unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const auto era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<unsigned>(year - era * 400);
    const auto shiftedMonth = month > 2U ? month - 3U : month + 9U;
    const auto dayOfYear = (153U * shiftedMonth + 2U) / 5U + day - 1U;
    const auto dayOfEra = yearOfEra * 365U + yearOfEra / 4U - yearOfEra / 100U + dayOfYear;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(dayOfEra) -
           719468;
}

void civilFromDays(std::int64_t days, int& year, unsigned& month, unsigned& day) noexcept {
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const auto yearOfEra =
        (dayOfEra - dayOfEra / 1460U + dayOfEra / 36524U - dayOfEra / 146096U) / 365U;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400;
    const auto dayOfYear = dayOfEra - (365U * yearOfEra + yearOfEra / 4U - yearOfEra / 100U);
    const auto monthPrime = (5U * dayOfYear + 2U) / 153U;
    day = dayOfYear - (153U * monthPrime + 2U) / 5U + 1U;
    month = monthPrime < 10U ? monthPrime + 3U : monthPrime - 9U;
    year += month <= 2U ? 1 : 0;
}

std::int64_t floorDivide(const std::int64_t value, const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

std::string formatDatabaseTime(const UtcTimePoint timePoint) {
    constexpr std::int64_t kMillisecondsPerDay = kSecondsPerDay * kMillisecondsPerSecond;
    const auto milliseconds = timePoint.unixMilliseconds();
    const auto maximumUtcMillisecondsExclusive =
        daysFromCivil(10000, 1U, 1U) * kMillisecondsPerDay - 8LL * 60LL * 60LL * 1000LL;
    if (milliseconds >= maximumUtcMillisecondsExclusive) {
        throw domain::DomainError("UTC time is outside the protocol range");
    }
    const auto days = floorDivide(milliseconds, kMillisecondsPerDay);
    const auto millisecondsOfDay = milliseconds - days * kMillisecondsPerDay;
    int year = 0;
    unsigned month = 0U;
    unsigned day = 0U;
    civilFromDays(days, year, month, day);
    if (year < 1000 || year > 9999) {
        throw domain::DomainError("UTC time is outside MySQL DATETIME range");
    }
    const auto secondsOfDay = millisecondsOfDay / kMillisecondsPerSecond;
    const auto millisecond = static_cast<int>(millisecondsOfDay % kMillisecondsPerSecond);
    const auto hour = static_cast<int>(secondsOfDay / 3600);
    const auto minute = static_cast<int>((secondsOfDay % 3600) / 60);
    const auto second = static_cast<int>(secondsOfDay % 60);
    std::array<char, 24> output{};
    const int written = std::snprintf(
        output.data(),
        output.size(),
        "%04d-%02u-%02u %02d:%02d:%02d.%03d",
        year,
        month,
        day,
        hour,
        minute,
        second,
        millisecond);
    if (written != 23) {
        throw domain::DomainError("failed to format database time");
    }
    return std::string(output.data(), static_cast<std::size_t>(written));
}

UtcTimePoint parseDatabaseTime(const std::string_view input) {
    const bool hasSupportedLength =
        input.size() == 19U || input.size() == 23U || input.size() == 26U;
    if (!hasSupportedLength || input[4] != '-' || input[7] != '-' || input[10] != ' ' ||
        input[13] != ':' || input[16] != ':' ||
        (input.size() > 19U && input[19] != '.')) {
        throw domain::DomainError("database time has an invalid format");
    }
    const int year = parseDigits(input, 0U, 4U);
    const int month = parseDigits(input, 5U, 2U);
    const int day = parseDigits(input, 8U, 2U);
    const int hour = parseDigits(input, 11U, 2U);
    const int minute = parseDigits(input, 14U, 2U);
    const int second = parseDigits(input, 17U, 2U);
    const int millisecond = input.size() == 19U ? 0 : parseDigits(input, 20U, 3U);
    if (input.size() == 26U && parseDigits(input, 23U, 3U) != 0) {
        throw domain::DomainError("database time exceeds millisecond precision");
    }
    if (year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 59) {
        throw domain::DomainError("database time has an invalid calendar value");
    }
    const auto seconds =
        daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) *
            kSecondsPerDay +
        hour * 3600 + minute * 60 + second;
    const auto milliseconds = seconds * kMillisecondsPerSecond + millisecond;
    const auto maximumUtcMillisecondsExclusive =
        daysFromCivil(10000, 1U, 1U) * kSecondsPerDay * kMillisecondsPerSecond -
        8LL * 60LL * 60LL * 1000LL;
    if (milliseconds >= maximumUtcMillisecondsExclusive) {
        throw domain::DomainError("database time is outside the protocol range");
    }
    return UtcTimePoint(milliseconds);
}

std::string stringColumn(sql::ResultSet& result, const unsigned int column) {
    return result.getString(column).asStdString();
}

RecognitionStatus parseRecognitionStatus(const std::string_view value) {
    if (value == "PROCESSING") {
        return RecognitionStatus::processing;
    }
    if (value == "SUCCEEDED") {
        return RecognitionStatus::succeeded;
    }
    if (value == "FAILED") {
        return RecognitionStatus::failed;
    }
    throw domain::DomainError("database recognition status is invalid");
}

RecognitionFailureCode parseRecognitionFailureCode(const std::string_view value) {
    if (value == "PLATE_NOT_FOUND") {
        return RecognitionFailureCode::plateNotFound;
    }
    if (value == "PLATE_RECOGNITION_FAILED") {
        return RecognitionFailureCode::plateRecognitionFailed;
    }
    if (value == "MODEL_INFERENCE_ERROR") {
        return RecognitionFailureCode::modelInferenceError;
    }
    if (value == "SERVER_RESTARTED") {
        return RecognitionFailureCode::serverRestarted;
    }
    throw domain::DomainError("database recognition error code is invalid");
}

AccessListType parseAccessListType(const std::string_view value) {
    if (value == "WHITE") {
        return AccessListType::white;
    }
    if (value == "BLACK") {
        return AccessListType::black;
    }
    throw domain::DomainError("database access-list type is invalid");
}

ImageMime parseImageMime(const std::string_view value) {
    if (value == "image/jpeg") {
        return ImageMime::jpeg;
    }
    if (value == "image/png") {
        return ImageMime::png;
    }
    throw domain::DomainError("database image MIME is invalid");
}

RecognitionRecord mapRecognition(sql::ResultSet& result) {
    const auto status = parseRecognitionStatus(stringColumn(result, 6U));
    std::optional<domain::PlateNumber> plateNumber;
    if (!result.isNull(7U)) {
        plateNumber = domain::PlateNumber::parse(stringColumn(result, 7U));
    }
    std::optional<RecognitionFailureCode> errorCode;
    if (!result.isNull(8U)) {
        errorCode = parseRecognitionFailureCode(stringColumn(result, 8U));
    }
    std::optional<std::string> errorMessage;
    if (!result.isNull(9U)) {
        errorMessage = stringColumn(result, 9U);
    }
    std::optional<UtcTimePoint> completedAt;
    if (!result.isNull(15U)) {
        completedAt = parseDatabaseTime(stringColumn(result, 15U));
    }
    std::optional<std::uint64_t> durationMs;
    if (!result.isNull(16U)) {
        durationMs = result.getUInt64(16U);
    }
    RecognitionSnapshot snapshot(
        domain::RecognitionId::parse(stringColumn(result, 1U)),
        result.getUInt64(5U),
        domain::DeviceId::parse(stringColumn(result, 2U)),
        status,
        std::move(plateNumber),
        errorCode,
        std::move(errorMessage),
        parseDatabaseTime(stringColumn(result, 13U)),
        parseDatabaseTime(stringColumn(result, 14U)),
        completedAt,
        durationMs);
    return RecognitionRecord(
        std::move(snapshot),
        domain::CaptureId::parse(stringColumn(result, 3U)),
        domain::Sha256Digest::parseHex(stringColumn(result, 4U)),
        domain::RelativeImagePath::parseGenerated(stringColumn(result, 10U)),
        parseImageMime(stringColumn(result, 11U)),
        result.getUInt64(12U));
}

AccessListRecord mapAccessList(sql::ResultSet& result) {
    return AccessListRecord(
        result.getUInt64(1U),
        parseAccessListType(stringColumn(result, 2U)),
        domain::PlateNumber::parse(stringColumn(result, 3U)),
        stringColumn(result, 4U),
        stringColumn(result, 5U),
        parseDatabaseTime(stringColumn(result, 6U)));
}

constexpr std::string_view kRecognitionColumns =
    "recognition_id, device_id, capture_id, image_sha256, revision, status, plate_number, "
    "error_code, error_message, image_path, image_mime, image_size_bytes, "
    "captured_at, started_at, completed_at, duration_ms";

std::unique_ptr<sql::PreparedStatement> recognitionStatement(
    sql::Connection& connection,
    const std::string_view suffix) {
    return std::unique_ptr<sql::PreparedStatement>(connection.prepareStatement(
        "SELECT " + std::string(kRecognitionColumns) + " FROM recognition_logs " +
        std::string(suffix)));
}

std::optional<RecognitionRecord> queryRecognitionById(
    sql::Connection& connection,
    const domain::RecognitionId& recognitionId,
    const bool forUpdate) {
    auto statement = recognitionStatement(
        connection,
        forUpdate ? "WHERE recognition_id = ? FOR UPDATE" : "WHERE recognition_id = ?");
    statement->setString(1U, recognitionId.toString());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    if (!result->next()) {
        return std::nullopt;
    }
    auto record = mapRecognition(*result);
    if (result->next()) {
        throw domain::DomainError("recognition id query returned duplicate rows");
    }
    return record;
}

std::optional<AccessListRecord> queryAccessByPlate(
    sql::Connection& connection,
    const domain::PlateNumber& plateNumber,
    const bool forUpdate) {
    const std::string sqlText = forUpdate
                                    ? "SELECT id, list_type, plate_number, remark, "
                                      "created_by_display_name, created_at FROM access_lists "
                                      "WHERE plate_number = ? FOR UPDATE"
                                    : "SELECT id, list_type, plate_number, remark, "
                                      "created_by_display_name, created_at FROM access_lists "
                                      "WHERE plate_number = ?";
    std::unique_ptr<sql::PreparedStatement> statement(connection.prepareStatement(sqlText));
    statement->setString(1U, plateNumber.value());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    if (!result->next()) {
        return std::nullopt;
    }
    auto record = mapAccessList(*result);
    if (result->next()) {
        throw domain::DomainError("access-list lookup returned duplicate rows");
    }
    return record;
}

void beginWrite(sql::Connection& connection) {
    connection.setAutoCommit(false);
}

void beginReadSnapshot(sql::Connection& connection) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    (void)statement->execute("SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    connection.setAutoCommit(false);
    (void)statement->execute("START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY");
}

void rollbackNoThrow(MySqlConnectionPool::Lease& lease) noexcept {
    try {
        lease.connection().rollback();
    } catch (...) {
        lease.discard();
    }
}

SqlFailure classifySqlException(const sql::SQLException& error, const SqlContext context) {
    const auto state = error.getSQLState();
    const int code = error.getErrorCode();
    if (state.rfind("08", 0U) == 0U || code == 2006 || code == 2013) {
        return {RepositoryFailure::unavailable, true};
    }
    if (code == 1205 || code == 1213) {
        return {RepositoryFailure::unavailable, false};
    }
    if (code == 1452) {
        return {RepositoryFailure::notFound, false};
    }
    if (code == 1062 && context == SqlContext::recognitionInsert) {
        return {RepositoryFailure::conflict, false};
    }
    return {RepositoryFailure::internal, false};
}

RepositoryFailure handleSqlException(
    MySqlConnectionPool::Lease& lease,
    const sql::SQLException& error,
    const SqlContext context,
    const bool commitInProgress = false) noexcept {
    if (commitInProgress) {
        lease.discard();
        return RepositoryFailure::unavailable;
    }
    const auto mapped = classifySqlException(error, context);
    if (mapped.discard) {
        lease.discard();
    } else {
        rollbackNoThrow(lease);
    }
    return mapped.failure;
}

template <typename T>
RepositoryResult<T> internalFailure(MySqlConnectionPool::Lease& lease) noexcept {
    rollbackNoThrow(lease);
    return RepositoryFailure::internal;
}

RepositoryFailure handleUnknownException(
    MySqlConnectionPool::Lease& lease,
    const bool commitInProgress = false) noexcept {
    if (commitInProgress) {
        lease.discard();
        return RepositoryFailure::unavailable;
    }
    rollbackNoThrow(lease);
    return RepositoryFailure::internal;
}

std::string failureMessage(const domain::ModelFailureCode code) {
    switch (code) {
        case domain::ModelFailureCode::plateNotFound:
            return "未检测到车牌";
        case domain::ModelFailureCode::plateRecognitionFailed:
            return "车牌识别失败";
        case domain::ModelFailureCode::modelInferenceError:
            return "模型推理失败";
    }
    throw domain::DomainError("invalid model failure code");
}

std::string escapeLikeKeyword(const std::string_view keyword) {
    std::string pattern;
    pattern.reserve(keyword.size() * 2U + 2U);
    pattern.push_back('%');
    for (const char value : keyword) {
        if (value == '=' || value == '%' || value == '_' || value == '\\') {
            pattern.push_back('=');
        }
        pattern.push_back(value);
    }
    pattern.push_back('%');
    return pattern;
}

}  // namespace

MySqlAdminUserRepository::MySqlAdminUserRepository(
    MySqlConnectionPool& pool,
    const RepositoryOptions options)
    : pool_(pool), options_(options) {
    validateOptions(options_);
}

RepositoryResult<std::optional<AdminUserRecord>> MySqlAdminUserRepository::findByUsername(
    const std::string_view username) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    try {
        std::unique_ptr<sql::PreparedStatement> statement(lease->connection().prepareStatement(
            "SELECT id, username, display_name, password_hash, enabled "
            "FROM admin_users WHERE username = ?"));
        statement->setString(1U, std::string(username));
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
        if (!result->next()) {
            return std::optional<AdminUserRecord>{};
        }
        AdminUserRecord record(
            result->getUInt64(1U),
            stringColumn(*result, 2U),
            stringColumn(*result, 3U),
            stringColumn(*result, 4U),
            result->getBoolean(5U));
        if (result->next()) {
            return internalFailure<std::optional<AdminUserRecord>>(*lease);
        }
        return std::optional<AdminUserRecord>(std::move(record));
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general);
    } catch (...) {
        return internalFailure<std::optional<AdminUserRecord>>(*lease);
    }
}

MySqlDeviceRepository::MySqlDeviceRepository(
    MySqlConnectionPool& pool,
    const RepositoryOptions options)
    : pool_(pool), options_(options) {
    validateOptions(options_);
}

RepositoryResult<std::optional<DeviceRecord>> MySqlDeviceRepository::findByHttpTokenHash(
    const domain::Sha256Digest& tokenHash) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    try {
        std::unique_ptr<sql::PreparedStatement> statement(lease->connection().prepareStatement(
            "SELECT device_id, device_name, http_token_hash, mqtt_username, enabled "
            "FROM devices WHERE http_token_hash = ?"));
        statement->setString(1U, tokenHash.toHex());
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
        if (!result->next()) {
            return std::optional<DeviceRecord>{};
        }
        DeviceRecord record(
            domain::DeviceId::parse(stringColumn(*result, 1U)),
            stringColumn(*result, 2U),
            domain::Sha256Digest::parseHex(stringColumn(*result, 3U)),
            stringColumn(*result, 4U),
            result->getBoolean(5U));
        if (result->next()) {
            return internalFailure<std::optional<DeviceRecord>>(*lease);
        }
        return std::optional<DeviceRecord>(std::move(record));
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general);
    } catch (...) {
        return internalFailure<std::optional<DeviceRecord>>(*lease);
    }
}

MySqlRecognitionRepository::MySqlRecognitionRepository(
    MySqlConnectionPool& pool,
    const RepositoryOptions options)
    : pool_(pool), options_(options) {
    validateOptions(options_);
}

RepositoryResult<std::optional<RecognitionRecord>> MySqlRecognitionRepository::findByCapture(
    const domain::DeviceId& deviceId,
    const domain::CaptureId& captureId) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    try {
        auto statement = recognitionStatement(
            lease->connection(), "WHERE device_id = ? AND capture_id = ?");
        statement->setString(1U, deviceId.value());
        statement->setString(2U, captureId.toString());
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
        if (!result->next()) {
            return std::optional<RecognitionRecord>{};
        }
        auto record = mapRecognition(*result);
        if (result->next()) {
            return internalFailure<std::optional<RecognitionRecord>>(*lease);
        }
        return std::optional<RecognitionRecord>(std::move(record));
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general);
    } catch (...) {
        return internalFailure<std::optional<RecognitionRecord>>(*lease);
    }
}

RepositoryResult<std::optional<RecognitionRecord>> MySqlRecognitionRepository::findById(
    const domain::RecognitionId& recognitionId) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    try {
        return queryRecognitionById(lease->connection(), recognitionId, false);
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general);
    } catch (...) {
        return internalFailure<std::optional<RecognitionRecord>>(*lease);
    }
}

RepositoryResult<RecognitionRecord> MySqlRecognitionRepository::insertProcessing(
    const domain::NewRecognition& recognition) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginWrite(lease->connection());
        std::unique_ptr<sql::PreparedStatement> statement(lease->connection().prepareStatement(
            "INSERT INTO recognition_logs (recognition_id, device_id, capture_id, image_sha256, "
            "revision, status, plate_number, error_code, error_message, image_path, image_mime, "
            "image_size_bytes, captured_at, started_at, completed_at, duration_ms, created_at, "
            "updated_at) VALUES (?, ?, ?, ?, 1, 'PROCESSING', NULL, NULL, NULL, ?, ?, ?, ?, ?, "
            "NULL, NULL, ?, ?)"));
        statement->setString(1U, recognition.recognitionId().toString());
        statement->setString(2U, recognition.deviceId().value());
        statement->setString(3U, recognition.captureId().toString());
        statement->setString(4U, recognition.imageSha256().toHex());
        statement->setString(5U, recognition.relativeImagePath().value());
        statement->setString(6U, std::string(domain::toString(recognition.imageMime())));
        statement->setUInt64(7U, recognition.imageSizeBytes());
        statement->setString(8U, formatDatabaseTime(recognition.capturedAtUtc()));
        const auto startedAt = formatDatabaseTime(recognition.startedAtUtc());
        statement->setString(9U, startedAt);
        statement->setString(10U, startedAt);
        statement->setString(11U, startedAt);
        if (statement->executeUpdate() != 1) {
            return internalFailure<RecognitionRecord>(*lease);
        }
        auto inserted = queryRecognitionById(lease->connection(), recognition.recognitionId(), false);
        if (!inserted) {
            return internalFailure<RecognitionRecord>(*lease);
        }
        auto record = std::move(*inserted);
        committing = true;
        lease->connection().commit();
        committing = false;
        return record;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::recognitionInsert, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

RepositoryResult<RecognitionRecord> MySqlRecognitionRepository::finalize(
    const domain::FinalizeRecognition& recognition) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginWrite(lease->connection());
        auto current = queryRecognitionById(lease->connection(), recognition.recognitionId, true);
        if (!current) {
            rollbackNoThrow(*lease);
            return RepositoryFailure::notFound;
        }
        if (current->snapshot().status() != RecognitionStatus::processing) {
            rollbackNoThrow(*lease);
            return RepositoryFailure::stateConflict;
        }
        domain::requireNonNegativeJsonSafe(recognition.durationMs, "durationMs");
        if (recognition.completedAtUtc < current->snapshot().startedAt()) {
            return internalFailure<RecognitionRecord>(*lease);
        }
        std::unique_ptr<sql::PreparedStatement> statement;
        if (recognition.outcome.isSuccess()) {
            statement.reset(lease->connection().prepareStatement(
                "UPDATE recognition_logs SET revision = revision + 1, status = 'SUCCEEDED', "
                "plate_number = ?, error_code = NULL, error_message = NULL, completed_at = ?, "
                "duration_ms = ?, updated_at = ? "
                "WHERE recognition_id = ? AND status = 'PROCESSING'"));
            statement->setString(1U, recognition.outcome.plateNumber().value());
            const auto completedAt = formatDatabaseTime(recognition.completedAtUtc);
            statement->setString(2U, completedAt);
            statement->setUInt64(3U, recognition.durationMs);
            statement->setString(4U, completedAt);
            statement->setString(5U, recognition.recognitionId.toString());
        } else {
            statement.reset(lease->connection().prepareStatement(
                "UPDATE recognition_logs SET revision = revision + 1, status = 'FAILED', "
                "plate_number = NULL, error_code = ?, error_message = ?, completed_at = ?, "
                "duration_ms = ?, updated_at = ? "
                "WHERE recognition_id = ? AND status = 'PROCESSING'"));
            statement->setString(
                1U,
                std::string(domain::toString(domain::toRecognitionFailureCode(
                    recognition.outcome.failureCode()))));
            statement->setString(2U, failureMessage(recognition.outcome.failureCode()));
            const auto completedAt = formatDatabaseTime(recognition.completedAtUtc);
            statement->setString(3U, completedAt);
            statement->setUInt64(4U, recognition.durationMs);
            statement->setString(5U, completedAt);
            statement->setString(6U, recognition.recognitionId.toString());
        }
        if (statement->executeUpdate() != 1) {
            return internalFailure<RecognitionRecord>(*lease);
        }
        auto finalized = queryRecognitionById(lease->connection(), recognition.recognitionId, false);
        if (!finalized) {
            return internalFailure<RecognitionRecord>(*lease);
        }
        auto record = std::move(*finalized);
        committing = true;
        lease->connection().commit();
        committing = false;
        return record;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

RepositoryResult<std::vector<RecognitionRecord>>
MySqlRecognitionRepository::failInterruptedOnStartup(const UtcTimePoint completedAtUtc) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginWrite(lease->connection());
        auto statement = recognitionStatement(
            lease->connection(), "WHERE status = 'PROCESSING' ORDER BY recognition_id FOR UPDATE");
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
        std::vector<RecognitionRecord> interrupted;
        while (result->next()) {
            interrupted.push_back(mapRecognition(*result));
        }
        result.reset();
        statement.reset();
        std::vector<RecognitionRecord> recovered;
        recovered.reserve(interrupted.size());
        for (const auto& record : interrupted) {
            const auto startedAt = record.snapshot().startedAt();
            const auto effectiveCompletedAt =
                completedAtUtc < startedAt ? startedAt : completedAtUtc;
            const auto elapsed =
                effectiveCompletedAt.unixMilliseconds() - startedAt.unixMilliseconds();
            const auto durationMs = static_cast<std::uint64_t>(elapsed);
            domain::requireNonNegativeJsonSafe(durationMs, "durationMs");
            std::unique_ptr<sql::PreparedStatement> update(lease->connection().prepareStatement(
                "UPDATE recognition_logs SET revision = revision + 1, status = 'FAILED', "
                "plate_number = NULL, error_code = 'SERVER_RESTARTED', "
                "error_message = '服务重启，识别任务已中断', completed_at = ?, duration_ms = ?, "
                "updated_at = ? WHERE recognition_id = ? AND status = 'PROCESSING'"));
            const auto completedAt = formatDatabaseTime(effectiveCompletedAt);
            update->setString(1U, completedAt);
            update->setUInt64(2U, durationMs);
            update->setString(3U, completedAt);
            update->setString(4U, record.snapshot().recognitionId().toString());
            if (update->executeUpdate() != 1) {
                return internalFailure<std::vector<RecognitionRecord>>(*lease);
            }
            auto finalRecord = queryRecognitionById(
                lease->connection(), record.snapshot().recognitionId(), false);
            if (!finalRecord) {
                return internalFailure<std::vector<RecognitionRecord>>(*lease);
            }
            recovered.push_back(std::move(*finalRecord));
        }
        committing = true;
        lease->connection().commit();
        committing = false;
        return recovered;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

RepositoryResult<domain::PageResult<RecognitionRecord>> MySqlRecognitionRepository::queryHistory(
    const domain::HistoryFilter& filter,
    const domain::PageRequest& page) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginReadSnapshot(lease->connection());
        const bool hasDevice = filter.deviceId().has_value();
        const std::string countSql = hasDevice
                                         ? "SELECT COUNT(*) FROM recognition_logs WHERE "
                                           "captured_at >= ? AND captured_at < ? AND device_id = ?"
                                         : "SELECT COUNT(*) FROM recognition_logs WHERE "
                                           "captured_at >= ? AND captured_at < ?";
        std::unique_ptr<sql::PreparedStatement> count(lease->connection().prepareStatement(countSql));
        count->setString(1U, formatDatabaseTime(filter.startInclusiveUtc()));
        count->setString(2U, formatDatabaseTime(filter.endExclusiveUtc()));
        if (hasDevice) {
            count->setString(3U, filter.deviceId()->value());
        }
        std::unique_ptr<sql::ResultSet> countResult(count->executeQuery());
        if (!countResult->next()) {
            return internalFailure<domain::PageResult<RecognitionRecord>>(*lease);
        }
        const auto total = countResult->getUInt64(1U);
        domain::requireNonNegativeJsonSafe(total, "total");
        countResult.reset();
        count.reset();

        std::vector<RecognitionRecord> items;
        const auto offset = (page.page() - 1U) * domain::PageRequest::pageSize();
        if (offset < total) {
            const std::string suffix = hasDevice
                                           ? "WHERE captured_at >= ? AND captured_at < ? AND "
                                             "device_id = ? ORDER BY captured_at DESC, "
                                             "recognition_id DESC LIMIT ? OFFSET ?"
                                           : "WHERE captured_at >= ? AND captured_at < ? ORDER BY "
                                             "captured_at DESC, recognition_id DESC LIMIT ? OFFSET ?";
            auto statement = recognitionStatement(lease->connection(), suffix);
            statement->setString(1U, formatDatabaseTime(filter.startInclusiveUtc()));
            statement->setString(2U, formatDatabaseTime(filter.endExclusiveUtc()));
            unsigned int parameter = 3U;
            if (hasDevice) {
                statement->setString(parameter++, filter.deviceId()->value());
            }
            statement->setUInt64(parameter++, domain::PageRequest::pageSize());
            statement->setUInt64(parameter, offset);
            std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
            while (result->next()) {
                items.push_back(mapRecognition(*result));
            }
        }
        domain::PageResult<RecognitionRecord> pageResult(std::move(items), page, total);
        committing = true;
        lease->connection().commit();
        committing = false;
        return pageResult;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

RepositoryResult<HistoryCursorResult> MySqlRecognitionRepository::visitHistory(
    const domain::HistoryFilter& filter,
    const domain::HistoryVisitor& visitor) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginReadSnapshot(lease->connection());
        const bool hasDevice = filter.deviceId().has_value();
        const std::string suffix = hasDevice
                                       ? "WHERE captured_at >= ? AND captured_at < ? AND "
                                         "device_id = ? ORDER BY captured_at DESC, recognition_id DESC"
                                       : "WHERE captured_at >= ? AND captured_at < ? ORDER BY "
                                         "captured_at DESC, recognition_id DESC";
        auto statement = recognitionStatement(lease->connection(), suffix);
        statement->setString(1U, formatDatabaseTime(filter.startInclusiveUtc()));
        statement->setString(2U, formatDatabaseTime(filter.endExclusiveUtc()));
        if (hasDevice) {
            statement->setString(3U, filter.deviceId()->value());
        }
        std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
        std::uint64_t visited = 0U;
        while (result->next()) {
            auto record = mapRecognition(*result);
            ++visited;
            bool keepGoing = false;
            try {
                keepGoing = visitor(record);
            } catch (...) {
                throw VisitorException{std::current_exception()};
            }
            if (!keepGoing) {
                result.reset();
                rollbackNoThrow(*lease);
                return HistoryCursorResult(visited, false);
            }
        }
        HistoryCursorResult cursorResult(visited, true);
        committing = true;
        lease->connection().commit();
        committing = false;
        return cursorResult;
    } catch (const VisitorException& error) {
        rollbackNoThrow(*lease);
        std::rethrow_exception(error.cause);
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

MySqlAccessListRepository::MySqlAccessListRepository(
    MySqlConnectionPool& pool,
    const RepositoryOptions options)
    : pool_(pool), options_(options) {
    validateOptions(options_);
}

RepositoryResult<domain::PageResult<AccessListRecord>> MySqlAccessListRepository::query(
    const domain::AccessListFilter& filter,
    const domain::PageRequest& page) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginReadSnapshot(lease->connection());
        const auto pattern = escapeLikeKeyword(filter.keyword().value());
        std::unique_ptr<sql::PreparedStatement> count(lease->connection().prepareStatement(
            "SELECT COUNT(*) FROM access_lists WHERE list_type = ? "
            "AND plate_number LIKE ? ESCAPE '='"));
        count->setString(1U, std::string(domain::toString(filter.listType())));
        count->setString(2U, pattern);
        std::unique_ptr<sql::ResultSet> countResult(count->executeQuery());
        if (!countResult->next()) {
            return internalFailure<domain::PageResult<AccessListRecord>>(*lease);
        }
        const auto total = countResult->getUInt64(1U);
        domain::requireNonNegativeJsonSafe(total, "total");
        countResult.reset();
        count.reset();
        std::vector<AccessListRecord> items;
        const auto offset = (page.page() - 1U) * domain::PageRequest::pageSize();
        if (offset < total) {
            std::unique_ptr<sql::PreparedStatement> statement(lease->connection().prepareStatement(
                "SELECT id, list_type, plate_number, remark, created_by_display_name, created_at "
                "FROM access_lists WHERE list_type = ? AND plate_number LIKE ? ESCAPE '=' "
                "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?"));
            statement->setString(1U, std::string(domain::toString(filter.listType())));
            statement->setString(2U, pattern);
            statement->setUInt64(3U, domain::PageRequest::pageSize());
            statement->setUInt64(4U, offset);
            std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
            while (result->next()) {
                items.push_back(mapAccessList(*result));
            }
        }
        domain::PageResult<AccessListRecord> pageResult(std::move(items), page, total);
        committing = true;
        lease->connection().commit();
        committing = false;
        return pageResult;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

RepositoryResult<std::optional<AccessListRecord>> MySqlAccessListRepository::lookup(
    const domain::PlateNumber& plateNumber) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    try {
        return queryAccessByPlate(lease->connection(), plateNumber, false);
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general);
    } catch (...) {
        return internalFailure<std::optional<AccessListRecord>>(*lease);
    }
}

AccessListInsertResult MySqlAccessListRepository::insert(
    const domain::NewAccessListRecord& record) {
    for (unsigned int attempt = 0U; attempt < 2U; ++attempt) {
        auto lease = pool_.acquire(options_.leaseTimeout);
        if (!lease) {
            return RepositoryFailure::unavailable;
        }
        bool committing = false;
        try {
            beginWrite(lease->connection());
            try {
                std::unique_ptr<sql::PreparedStatement> statement(lease->connection().prepareStatement(
                    "INSERT INTO access_lists (list_type, plate_number, remark, created_by_user_id, "
                    "created_by_display_name, created_at) VALUES (?, ?, ?, ?, ?, ?)"));
                statement->setString(1U, std::string(domain::toString(record.listType())));
                statement->setString(2U, record.plateNumber().value());
                statement->setString(3U, record.remark());
                statement->setUInt64(4U, record.createdByUserId());
                statement->setString(5U, record.createdBy());
                statement->setString(6U, formatDatabaseTime(record.createdAt()));
                if (statement->executeUpdate() != 1) {
                    rollbackNoThrow(*lease);
                    return RepositoryFailure::internal;
                }
            } catch (const sql::SQLException& error) {
                if (error.getErrorCode() != 1062) {
                    return handleSqlException(*lease, error, SqlContext::general);
                }
                auto conflict = queryAccessByPlate(lease->connection(), record.plateNumber(), true);
                if (conflict) {
                    const auto existingType = conflict->listType();
                    rollbackNoThrow(*lease);
                    return AccessListConflict(existingType);
                }
                rollbackNoThrow(*lease);
                if (attempt == 0U) {
                    continue;
                }
                return RepositoryFailure::unavailable;
            }
            std::unique_ptr<sql::Statement> idStatement(lease->connection().createStatement());
            std::unique_ptr<sql::ResultSet> idResult(
                idStatement->executeQuery("SELECT LAST_INSERT_ID()"));
            if (!idResult->next()) {
                rollbackNoThrow(*lease);
                return RepositoryFailure::internal;
            }
            const auto id = idResult->getUInt64(1U);
            std::unique_ptr<sql::PreparedStatement> select(lease->connection().prepareStatement(
                "SELECT id, list_type, plate_number, remark, created_by_display_name, created_at "
                "FROM access_lists WHERE id = ?"));
            select->setUInt64(1U, id);
            std::unique_ptr<sql::ResultSet> selected(select->executeQuery());
            if (!selected->next()) {
                rollbackNoThrow(*lease);
                return RepositoryFailure::internal;
            }
            auto inserted = mapAccessList(*selected);
            if (selected->next()) {
                rollbackNoThrow(*lease);
                return RepositoryFailure::internal;
            }
            committing = true;
            lease->connection().commit();
            committing = false;
            return inserted;
        } catch (const sql::SQLException& error) {
            return handleSqlException(*lease, error, SqlContext::general, committing);
        } catch (...) {
            return handleUnknownException(*lease, committing);
        }
    }
    return RepositoryFailure::unavailable;
}

RepositoryResult<bool> MySqlAccessListRepository::remove(const std::uint64_t id) {
    auto lease = pool_.acquire(options_.leaseTimeout);
    if (!lease) {
        return RepositoryFailure::unavailable;
    }
    bool committing = false;
    try {
        beginWrite(lease->connection());
        std::unique_ptr<sql::PreparedStatement> statement(
            lease->connection().prepareStatement("DELETE FROM access_lists WHERE id = ?"));
        statement->setUInt64(1U, id);
        const int affected = statement->executeUpdate();
        if (affected < 0 || affected > 1) {
            return internalFailure<bool>(*lease);
        }
        committing = true;
        lease->connection().commit();
        committing = false;
        return affected == 1;
    } catch (const sql::SQLException& error) {
        return handleSqlException(*lease, error, SqlContext::general, committing);
    } catch (...) {
        return handleUnknownException(*lease, committing);
    }
}

}  // namespace ocrservice::repositories::mysql::repositories
