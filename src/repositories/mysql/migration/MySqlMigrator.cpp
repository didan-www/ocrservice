#include "MySqlMigrator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cppconn/connection.h>
#include <cppconn/exception.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>
#include <openssl/evp.h>

#include "SchemaValidator.h"

namespace ocrservice::repositories::mysql::migration {
namespace {

struct MigrationFile final {
    std::uint64_t version;
    std::string name;
    std::string checksum;
    std::string sql;
};

struct AppliedMigration final {
    std::string name;
    std::string checksum;
};

std::string sha256(const std::string_view bytes) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
        throw MigrationError();
    }
    std::array<unsigned char, 32> digest{};
    unsigned int digestSize = 0U;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1 ||
        digestSize != static_cast<unsigned int>(digest.size())) {
        throw MigrationError();
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string result(digest.size() * 2U, '0');
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        result[index * 2U] = kHex[digest[index] >> 4U];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return result;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw MigrationError();
    }
    std::string result(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) {
        throw MigrationError();
    }
    return result;
}

std::vector<MigrationFile> discoverMigrations(const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        throw MigrationError();
    }
    static const std::regex kNamePattern(R"(^([0-9]{3})_([A-Za-z0-9_]+)\.sql$)");
    std::vector<MigrationFile> migrations;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().string();
        if (entry.path().extension() != ".sql") {
            continue;
        }
        if (entry.is_symlink(error) || error || !entry.is_regular_file(error) || error ||
            name.size() > 128U) {
            throw MigrationError();
        }
        std::smatch match;
        if (!std::regex_match(name, match, kNamePattern)) {
            throw MigrationError();
        }
        const auto version = static_cast<std::uint64_t>(std::stoul(match[1].str()));
        if (version == 0U) {
            throw MigrationError();
        }
        auto contents = readFile(entry.path());
        migrations.push_back(MigrationFile{version, name, sha256(contents), std::move(contents)});
    }
    if (error || migrations.empty()) {
        throw MigrationError();
    }
    std::sort(migrations.begin(), migrations.end(), [](const auto& left, const auto& right) {
        return left.version < right.version;
    });
    for (std::size_t index = 1U; index < migrations.size(); ++index) {
        if (migrations[index - 1U].version == migrations[index].version) {
            throw MigrationError();
        }
    }
    return migrations;
}

std::vector<std::string> splitStatements(const std::string_view sqlText) {
    enum class State { normal, singleQuote, doubleQuote, backtick, lineComment, blockComment };
    State state = State::normal;
    bool escaped = false;
    std::string current;
    std::vector<std::string> statements;
    const auto appendStatement = [&current, &statements]() {
        const auto first = std::find_if_not(current.begin(), current.end(), [](const char value) {
            return std::isspace(static_cast<unsigned char>(value)) != 0;
        });
        const auto last = std::find_if_not(current.rbegin(), current.rend(), [](const char value) {
                              return std::isspace(static_cast<unsigned char>(value)) != 0;
                          }).base();
        if (first < last) {
            statements.emplace_back(first, last);
        }
        current.clear();
    };
    for (std::size_t index = 0U; index < sqlText.size(); ++index) {
        const char value = sqlText[index];
        const char next = index + 1U < sqlText.size() ? sqlText[index + 1U] : '\0';
        if (state == State::lineComment) {
            if (value == '\n') {
                state = State::normal;
                current.push_back(value);
            }
            continue;
        }
        if (state == State::blockComment) {
            if (value == '*' && next == '/') {
                state = State::normal;
                ++index;
            }
            continue;
        }
        if (state == State::normal) {
            if (value == '-' && next == '-' &&
                (index + 2U == sqlText.size() ||
                 std::isspace(static_cast<unsigned char>(sqlText[index + 2U])) != 0)) {
                state = State::lineComment;
                ++index;
            } else if (value == '#') {
                state = State::lineComment;
            } else if (value == '/' && next == '*') {
                state = State::blockComment;
                ++index;
            } else if (value == '\'') {
                state = State::singleQuote;
                current.push_back(value);
            } else if (value == '"') {
                state = State::doubleQuote;
                current.push_back(value);
            } else if (value == '`') {
                state = State::backtick;
                current.push_back(value);
            } else if (value == ';') {
                appendStatement();
            } else {
                current.push_back(value);
            }
            continue;
        }
        current.push_back(value);
        if (escaped) {
            escaped = false;
        } else if (value == '\\' && state != State::backtick) {
            escaped = true;
        } else if ((state == State::singleQuote && value == '\'') ||
                   (state == State::doubleQuote && value == '"') ||
                   (state == State::backtick && value == '`')) {
            if (next == value) {
                current.push_back(next);
                ++index;
            } else {
                state = State::normal;
            }
        }
    }
    if (state != State::normal && state != State::lineComment) {
        throw MigrationError();
    }
    appendStatement();
    return statements;
}

std::string lockName(const std::string_view database) {
    constexpr std::string_view prefix = "ocrservice:migration:";
    const auto databaseHash = sha256(database);
    return std::string(prefix) + databaseHash.substr(0U, 64U - prefix.size());
}

enum class LockAcquisition { acquired, notAcquired, unknown };

LockAcquisition acquireLock(
    sql::Connection& connection,
    const std::string& name,
    const unsigned int waitSeconds) {
    std::unique_ptr<sql::PreparedStatement> statement(
        connection.prepareStatement("SELECT GET_LOCK(?, ?)"));
    statement->setString(1U, name);
    statement->setUInt(2U, waitSeconds);
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    if (!result->next() || result->isNull(1U)) {
        return LockAcquisition::unknown;
    }
    const auto value = result->getInt(1U);
    if (value == 1) {
        return LockAcquisition::acquired;
    }
    return value == 0 ? LockAcquisition::notAcquired : LockAcquisition::unknown;
}

bool releaseLock(sql::Connection& connection, const std::string& name) {
    std::unique_ptr<sql::PreparedStatement> statement(
        connection.prepareStatement("SELECT RELEASE_LOCK(?)"));
    statement->setString(1U, name);
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    return result->next() && !result->isNull(1U) && result->getInt(1U) == 1;
}

bool releaseLockNoThrow(sql::Connection& connection, const std::string& name) noexcept {
    try {
        return releaseLock(connection, name);
    } catch (...) {
        return false;
    }
}

std::map<std::uint64_t, AppliedMigration> loadApplied(sql::Connection& connection) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery(
        "SELECT version, name, checksum FROM schema_migrations ORDER BY version"));
    std::map<std::uint64_t, AppliedMigration> applied;
    while (result->next()) {
        const auto version = result->getUInt64(1U);
        const auto inserted = applied.emplace(
            version,
            AppliedMigration{result->getString(2U).asStdString(),
                             result->getString(3U).asStdString()});
        if (!inserted.second) {
            throw MigrationError();
        }
    }
    return applied;
}

void verifyHistory(
    const std::vector<MigrationFile>& files,
    const std::map<std::uint64_t, AppliedMigration>& applied) {
    std::map<std::uint64_t, const MigrationFile*> byVersion;
    for (const auto& file : files) {
        byVersion.emplace(file.version, &file);
    }
    for (const auto& [version, record] : applied) {
        const auto found = byVersion.find(version);
        if (found == byVersion.end() || found->second->name != record.name ||
            found->second->checksum != record.checksum) {
            throw MigrationError();
        }
    }
    if (!applied.empty()) {
        const auto maximumApplied = applied.rbegin()->first;
        for (const auto& file : files) {
            if (file.version < maximumApplied && applied.count(file.version) == 0U) {
                throw MigrationError();
            }
        }
    }
}

void executeMigration(sql::Connection& connection, const MigrationFile& file) {
    for (const auto& sql : splitStatements(file.sql)) {
        std::unique_ptr<sql::Statement> statement(connection.createStatement());
        (void)statement->execute(sql);
    }
}

void recordMigration(sql::Connection& connection, const MigrationFile& file) {
    std::unique_ptr<sql::PreparedStatement> statement(connection.prepareStatement(
        "INSERT INTO schema_migrations(version, name, checksum, applied_at) "
        "VALUES (?, ?, ?, UTC_TIMESTAMP(3))"));
    statement->setUInt64(1U, file.version);
    statement->setString(2U, file.name);
    statement->setString(3U, file.checksum);
    if (statement->executeUpdate() != 1) {
        throw MigrationError();
    }
}

}  // namespace

MigrationError::MigrationError()
    : std::runtime_error("MySQL migration failed") {}

MySqlMigrator::MySqlMigrator(
    connection::MySqlConnectionPool& pool,
    std::filesystem::path migrationDirectory,
    MigrationOptions options)
    : pool_(pool),
      migrationDirectory_(std::move(migrationDirectory)),
      options_(options) {
    if (options_.leaseWait.count() <= 0 || options_.lockWaitSeconds == 0U) {
        throw MigrationError();
    }
}

void MySqlMigrator::migrate() {
    try {
        const auto files = discoverMigrations(migrationDirectory_);
        auto lease = pool_.acquire(options_.leaseWait);
        if (!lease) {
            throw MigrationError();
        }
        auto& connection = lease->connection();
        const auto name = lockName(pool_.database());
        bool locked = false;
        const auto releaseHeldLock = [&]() noexcept {
            if (!locked) {
                return true;
            }
            locked = false;
            if (releaseLockNoThrow(connection, name)) {
                return true;
            }
            lease->discard();
            return false;
        };
        try {
            LockAcquisition acquisition = LockAcquisition::unknown;
            try {
                acquisition = acquireLock(connection, name, options_.lockWaitSeconds);
            } catch (...) {
                lease->discard();
                throw MigrationError();
            }
            if (acquisition == LockAcquisition::unknown) {
                lease->discard();
                throw MigrationError();
            }
            if (acquisition == LockAcquisition::notAcquired) {
                throw MigrationError();
            }
            locked = true;
            detail::validateConfiguredSchema(connection, pool_.database());
            detail::bootstrapMigrationTable(connection);
            detail::validateMigrationTable(connection, pool_.database());
            auto applied = loadApplied(connection);
            verifyHistory(files, applied);
            for (const auto& file : files) {
                if (applied.count(file.version) != 0U) {
                    continue;
                }
                executeMigration(connection, file);
                detail::validateCompleteSchema(connection, pool_.database());
                recordMigration(connection, file);
                applied.emplace(file.version, AppliedMigration{file.name, file.checksum});
            }
            detail::validateCompleteSchema(connection, pool_.database());
            if (!releaseHeldLock()) {
                throw MigrationError();
            }
        } catch (...) {
            (void)releaseHeldLock();
            throw;
        }
    } catch (const MigrationError&) {
        throw;
    } catch (...) {
        throw MigrationError();
    }
}

}  // namespace ocrservice::repositories::mysql::migration
