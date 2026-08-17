#include "SchemaValidator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <cppconn/statement.h>

#include "MySqlMigrator.h"

namespace ocrservice::repositories::mysql::migration::detail {
namespace {

using Rows = std::vector<std::string>;

constexpr char kSeparator = '\x1f';
constexpr std::string_view kNull = "<NULL>";

void append(std::string& row, const std::string_view value) {
    if (!row.empty()) {
        row.push_back(kSeparator);
    }
    row.append(value);
}

std::string value(sql::ResultSet& result, const unsigned int column) {
    return result.isNull(column) ? std::string(kNull)
                                 : result.getString(column).asStdString();
}

Rows querySchema(
    sql::Connection& connection,
    const std::string_view database,
    const std::string_view query,
    const unsigned int columnCount) {
    std::unique_ptr<sql::PreparedStatement> statement(
        connection.prepareStatement(std::string(query)));
    statement->setString(1U, std::string(database));
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery());
    Rows rows;
    while (result->next()) {
        std::string row;
        for (unsigned int column = 1U; column <= columnCount; ++column) {
            append(row, value(*result, column));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void requireEqual(const Rows& actual, const Rows& expected) {
    if (actual != expected) {
        throw MigrationError();
    }
}

std::string column(
    const std::string_view table,
    const std::string_view name,
    const unsigned int position,
    const std::string_view type,
    const bool nullable,
    const std::string_view defaultValue,
    const std::string_view characterSet,
    const std::string_view collation,
    const std::string_view extra = "") {
    std::string row;
    append(row, table);
    append(row, name);
    append(row, std::to_string(position));
    append(row, type);
    append(row, nullable ? "YES" : "NO");
    append(row, defaultValue);
    append(row, characterSet);
    append(row, collation);
    append(row, extra);
    append(row, "");
    return row;
}

std::string index(
    const std::string_view table,
    const std::string_view name,
    const bool unique,
    const unsigned int position,
    const std::string_view columnName,
    const std::string_view direction = "A") {
    std::string row;
    append(row, table);
    append(row, name);
    append(row, unique ? "0" : "1");
    append(row, std::to_string(position));
    append(row, columnName);
    append(row, direction);
    append(row, kNull);
    append(row, "BTREE");
    append(row, "YES");
    append(row, kNull);
    return row;
}

std::string foreignKey(
    const std::string_view table,
    const std::string_view name,
    const std::string_view columnName,
    const std::string_view referencedTable,
    const std::string_view referencedColumn) {
    std::string row;
    append(row, table);
    append(row, name);
    append(row, columnName);
    append(row, referencedTable);
    append(row, referencedColumn);
    append(row, "1");
    append(row, "NO ACTION");
    append(row, "NO ACTION");
    return row;
}

enum class CheckTokenKind { word, stringLiteral, symbol, leftParenthesis, rightParenthesis };

struct CheckToken final {
    CheckTokenKind kind;
    std::string text;
};

std::string asciiLower(std::string text) {
    for (auto& character : text) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

std::vector<CheckToken> tokenizeCheck(const std::string_view source) {
    std::string text;
    text.reserve(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
        if (source[index] == '\\' && index + 1U < source.size() &&
            source[index + 1U] == '\'') {
            continue;
        }
        text.push_back(source[index]);
    }

    std::vector<CheckToken> tokens;
    for (std::size_t index = 0U; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (std::isspace(byte) != 0) {
            ++index;
            continue;
        }
        if (text[index] == '(') {
            tokens.push_back(CheckToken{CheckTokenKind::leftParenthesis, "("});
            ++index;
            continue;
        }
        if (text[index] == ')') {
            tokens.push_back(CheckToken{CheckTokenKind::rightParenthesis, ")"});
            ++index;
            continue;
        }
        if (text[index] == '`') {
            std::string identifier;
            ++index;
            while (index < text.size() && text[index] != '`') {
                identifier.push_back(text[index]);
                ++index;
            }
            if (index >= text.size()) {
                throw MigrationError();
            }
            ++index;
            tokens.push_back(CheckToken{CheckTokenKind::word, asciiLower(std::move(identifier))});
            continue;
        }
        if (text[index] == '\'') {
            std::string literal;
            ++index;
            bool closed = false;
            while (index < text.size()) {
                if (text[index] == '\'' && index + 1U < text.size() &&
                    text[index + 1U] == '\'') {
                    literal.push_back('\'');
                    index += 2U;
                } else if (text[index] == '\'') {
                    ++index;
                    closed = true;
                    break;
                } else {
                    literal.push_back(text[index]);
                    ++index;
                }
            }
            if (!closed) {
                throw MigrationError();
            }
            tokens.push_back(CheckToken{CheckTokenKind::stringLiteral, std::move(literal)});
            continue;
        }
        if (std::isalnum(byte) != 0 || text[index] == '_') {
            std::string word;
            while (index < text.size()) {
                const auto current = static_cast<unsigned char>(text[index]);
                if (std::isalnum(current) == 0 && text[index] != '_' && text[index] != '.') {
                    break;
                }
                word.push_back(text[index]);
                ++index;
            }
            tokens.push_back(CheckToken{CheckTokenKind::word, asciiLower(std::move(word))});
            continue;
        }
        std::string symbol(1U, text[index]);
        if (index + 1U < text.size() &&
            ((text[index] == '<' || text[index] == '>' || text[index] == '!') &&
             text[index + 1U] == '=')) {
            symbol.push_back(text[index + 1U]);
            index += 2U;
        } else {
            ++index;
        }
        tokens.push_back(CheckToken{CheckTokenKind::symbol, std::move(symbol)});
    }

    std::vector<CheckToken> normalized;
    normalized.reserve(tokens.size());
    for (std::size_t index = 0U; index < tokens.size(); ++index) {
        const bool introducer =
            tokens[index].kind == CheckTokenKind::word &&
            (tokens[index].text == "_latin1" || tokens[index].text == "_utf8mb4") &&
            index + 1U < tokens.size() &&
            tokens[index + 1U].kind == CheckTokenKind::stringLiteral;
        if (!introducer) {
            normalized.push_back(std::move(tokens[index]));
        }
    }
    return normalized;
}

struct CheckExpression final {
    enum class Kind { predicate, conjunction, disjunction };

    Kind kind;
    std::vector<CheckToken> predicate;
    std::vector<CheckExpression> children;
};

class CheckParser final {
public:
    explicit CheckParser(std::vector<CheckToken> tokens)
        : tokens_(std::move(tokens)) {}

    CheckExpression parse() {
        auto expression = parseDisjunction();
        if (position_ != tokens_.size()) {
            throw MigrationError();
        }
        return expression;
    }

private:
    static CheckExpression combine(
        const CheckExpression::Kind kind,
        CheckExpression left,
        CheckExpression right) {
        CheckExpression result{kind, {}, {}};
        const auto appendChild = [&result, kind](CheckExpression child) {
            if (child.kind == kind) {
                for (auto& grandchild : child.children) {
                    result.children.push_back(std::move(grandchild));
                }
            } else {
                result.children.push_back(std::move(child));
            }
        };
        appendChild(std::move(left));
        appendChild(std::move(right));
        return result;
    }

    bool matchesWord(const std::string_view word) const {
        return position_ < tokens_.size() &&
               tokens_[position_].kind == CheckTokenKind::word &&
               tokens_[position_].text == word;
    }

    CheckExpression parseDisjunction() {
        auto expression = parseConjunction();
        while (matchesWord("or")) {
            ++position_;
            expression = combine(
                CheckExpression::Kind::disjunction,
                std::move(expression),
                parseConjunction());
        }
        return expression;
    }

    CheckExpression parseConjunction() {
        auto expression = parseFactor();
        while (matchesWord("and")) {
            ++position_;
            expression = combine(
                CheckExpression::Kind::conjunction,
                std::move(expression),
                parseFactor());
        }
        return expression;
    }

    CheckExpression parseFactor() {
        if (position_ < tokens_.size() &&
            tokens_[position_].kind == CheckTokenKind::leftParenthesis) {
            ++position_;
            auto expression = parseDisjunction();
            if (position_ >= tokens_.size() ||
                tokens_[position_].kind != CheckTokenKind::rightParenthesis) {
                throw MigrationError();
            }
            ++position_;
            return expression;
        }
        return parsePredicate();
    }

    CheckExpression parsePredicate() {
        std::vector<CheckToken> predicate;
        std::size_t parenthesisDepth = 0U;
        bool awaitingBetweenAnd = false;
        while (position_ < tokens_.size()) {
            const auto& token = tokens_[position_];
            if (token.kind == CheckTokenKind::leftParenthesis) {
                ++parenthesisDepth;
            } else if (token.kind == CheckTokenKind::rightParenthesis) {
                if (parenthesisDepth == 0U) {
                    break;
                }
                --parenthesisDepth;
            } else if (parenthesisDepth == 0U && token.kind == CheckTokenKind::word) {
                if (token.text == "between") {
                    awaitingBetweenAnd = true;
                } else if (token.text == "and") {
                    if (awaitingBetweenAnd) {
                        awaitingBetweenAnd = false;
                    } else {
                        break;
                    }
                } else if (token.text == "or") {
                    break;
                }
            }
            predicate.push_back(token);
            ++position_;
        }
        if (predicate.empty() || parenthesisDepth != 0U || awaitingBetweenAnd) {
            throw MigrationError();
        }
        return CheckExpression{CheckExpression::Kind::predicate, std::move(predicate), {}};
    }

    std::vector<CheckToken> tokens_;
    std::size_t position_ = 0U;
};

void serializeCheck(const CheckExpression& expression, std::string& result) {
    if (expression.kind == CheckExpression::Kind::predicate) {
        result.append("p[");
        for (const auto& token : expression.predicate) {
            if (token.kind == CheckTokenKind::stringLiteral) {
                result.append("s");
                result.append(std::to_string(token.text.size()));
                result.push_back(':');
            } else {
                result.push_back('t');
            }
            result.append(token.text);
            result.push_back(kSeparator);
        }
        result.push_back(']');
        return;
    }
    result.append(
        expression.kind == CheckExpression::Kind::conjunction ? "and[" : "or[");
    for (const auto& child : expression.children) {
        serializeCheck(child, result);
        result.push_back(kSeparator);
    }
    result.push_back(']');
}

std::string normalizeCheck(const std::string& text) {
    const auto expression = CheckParser(tokenizeCheck(text)).parse();
    std::string result;
    serializeCheck(expression, result);
    return result;
}

void validateVersion(sql::Connection& connection) {
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    std::unique_ptr<sql::ResultSet> result(statement->executeQuery("SELECT VERSION()"));
    if (!result->next() || result->isNull(1U)) {
        throw MigrationError();
    }
    const auto version = result->getString(1U).asStdString();
    std::array<unsigned int, 3> components{};
    std::size_t offset = 0U;
    for (auto& component : components) {
        if (offset >= version.size() || std::isdigit(static_cast<unsigned char>(version[offset])) == 0) {
            throw MigrationError();
        }
        while (offset < version.size() &&
               std::isdigit(static_cast<unsigned char>(version[offset])) != 0) {
            component = component * 10U + static_cast<unsigned int>(version[offset] - '0');
            ++offset;
        }
        if (&component != &components.back()) {
            if (offset >= version.size() || version[offset] != '.') {
                throw MigrationError();
            }
            ++offset;
        }
    }
    const bool supported = components[0] > 8U ||
                           (components[0] == 8U && components[1] > 0U) ||
                           (components[0] == 8U && components[1] == 0U &&
                            components[2] >= 16U);
    if (!supported || version.find("MariaDB") != std::string::npos) {
        throw MigrationError();
    }
}

void validateTables(sql::Connection& connection, const std::string_view database) {
    const auto actual = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, ENGINE, TABLE_COLLATION FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = ? AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME",
        3U);
    const Rows expected{
        "access_lists\x1fInnoDB\x1futf8mb4_0900_as_cs",
        "admin_users\x1fInnoDB\x1futf8mb4_0900_as_cs",
        "devices\x1fInnoDB\x1futf8mb4_0900_as_cs",
        "recognition_logs\x1fInnoDB\x1futf8mb4_0900_as_cs",
        "schema_migrations\x1fInnoDB\x1futf8mb4_0900_as_cs",
    };
    requireEqual(actual, expected);
}

void validateColumns(sql::Connection& connection, const std::string_view database) {
    const auto actual = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
        "COLUMN_DEFAULT, CHARACTER_SET_NAME, COLLATION_NAME, EXTRA, GENERATION_EXPRESSION "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ? "
        "ORDER BY TABLE_NAME, ORDINAL_POSITION",
        10U);
    const Rows expected{
        column("access_lists", "id", 1U, "bigint unsigned", false, kNull, kNull, kNull, "auto_increment"),
        column("access_lists", "list_type", 2U, "enum('WHITE','BLACK')", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("access_lists", "plate_number", 3U, "varchar(16)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("access_lists", "remark", 4U, "varchar(200)", false, "", "utf8mb4", "utf8mb4_0900_as_cs"),
        column("access_lists", "created_by_user_id", 5U, "bigint unsigned", false, kNull, kNull, kNull),
        column("access_lists", "created_by_display_name", 6U, "varchar(64)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("access_lists", "created_at", 7U, "datetime(3)", false, kNull, kNull, kNull),
        column("admin_users", "id", 1U, "bigint unsigned", false, kNull, kNull, kNull, "auto_increment"),
        column("admin_users", "username", 2U, "varchar(64)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("admin_users", "display_name", 3U, "varchar(64)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("admin_users", "password_hash", 4U, "varchar(100)", false, kNull, "ascii", "ascii_bin"),
        column("admin_users", "enabled", 5U, "tinyint(1)", false, "1", kNull, kNull),
        column("admin_users", "created_at", 6U, "datetime(3)", false, kNull, kNull, kNull),
        column("admin_users", "updated_at", 7U, "datetime(3)", false, kNull, kNull, kNull),
        column("devices", "device_id", 1U, "varchar(64)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("devices", "device_name", 2U, "varchar(100)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("devices", "http_token_hash", 3U, "char(64)", false, kNull, "ascii", "ascii_bin"),
        column("devices", "mqtt_username", 4U, "varchar(64)", false, kNull, "ascii", "ascii_bin"),
        column("devices", "enabled", 5U, "tinyint(1)", false, "1", kNull, kNull),
        column("devices", "created_at", 6U, "datetime(3)", false, kNull, kNull, kNull),
        column("devices", "updated_at", 7U, "datetime(3)", false, kNull, kNull, kNull),
        column("recognition_logs", "recognition_id", 1U, "char(36)", false, kNull, "ascii", "ascii_bin"),
        column("recognition_logs", "device_id", 2U, "varchar(64)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "capture_id", 3U, "char(36)", false, kNull, "ascii", "ascii_bin"),
        column("recognition_logs", "image_sha256", 4U, "char(64)", false, kNull, "ascii", "ascii_bin"),
        column("recognition_logs", "revision", 5U, "bigint unsigned", false, kNull, kNull, kNull),
        column("recognition_logs", "status", 6U, "enum('PROCESSING','SUCCEEDED','FAILED')", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "plate_number", 7U, "varchar(16)", true, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "error_code", 8U, "varchar(64)", true, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "error_message", 9U, "varchar(512)", true, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "image_path", 10U, "varchar(512)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "image_mime", 11U, "enum('image/jpeg','image/png')", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("recognition_logs", "image_size_bytes", 12U, "bigint unsigned", false, kNull, kNull, kNull),
        column("recognition_logs", "captured_at", 13U, "datetime(3)", false, kNull, kNull, kNull),
        column("recognition_logs", "started_at", 14U, "datetime(3)", false, kNull, kNull, kNull),
        column("recognition_logs", "completed_at", 15U, "datetime(3)", true, kNull, kNull, kNull),
        column("recognition_logs", "duration_ms", 16U, "bigint unsigned", true, kNull, kNull, kNull),
        column("recognition_logs", "created_at", 17U, "datetime(3)", false, kNull, kNull, kNull),
        column("recognition_logs", "updated_at", 18U, "datetime(3)", false, kNull, kNull, kNull),
        column("schema_migrations", "version", 1U, "bigint unsigned", false, kNull, kNull, kNull),
        column("schema_migrations", "name", 2U, "varchar(128)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
        column("schema_migrations", "checksum", 3U, "char(64)", false, kNull, "ascii", "ascii_bin"),
        column("schema_migrations", "applied_at", 4U, "datetime(3)", false, kNull, kNull, kNull),
    };
    requireEqual(actual, expected);
}

void validateIndexes(sql::Connection& connection, const std::string_view database) {
    const auto actual = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, SEQ_IN_INDEX, COLUMN_NAME, COLLATION, "
        "SUB_PART, INDEX_TYPE, IS_VISIBLE, EXPRESSION FROM information_schema.STATISTICS "
        "WHERE TABLE_SCHEMA = ? ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX",
        10U);
    const Rows expected{
        index("access_lists", "fk_access_lists_user", false, 1U, "created_by_user_id"),
        index("access_lists", "idx_access_lists_type_created", false, 1U, "list_type"),
        index("access_lists", "idx_access_lists_type_created", false, 2U, "created_at", "D"),
        index("access_lists", "idx_access_lists_type_created", false, 3U, "id", "D"),
        index("access_lists", "PRIMARY", true, 1U, "id"),
        index("access_lists", "uk_access_lists_plate", true, 1U, "plate_number"),
        index("admin_users", "PRIMARY", true, 1U, "id"),
        index("admin_users", "uk_admin_users_username", true, 1U, "username"),
        index("devices", "PRIMARY", true, 1U, "device_id"),
        index("devices", "uk_devices_http_token_hash", true, 1U, "http_token_hash"),
        index("devices", "uk_devices_mqtt_username", true, 1U, "mqtt_username"),
        index("recognition_logs", "idx_recognition_captured", false, 1U, "captured_at", "D"),
        index("recognition_logs", "idx_recognition_captured", false, 2U, "recognition_id", "D"),
        index("recognition_logs", "idx_recognition_device_captured", false, 1U, "device_id"),
        index("recognition_logs", "idx_recognition_device_captured", false, 2U, "captured_at", "D"),
        index("recognition_logs", "idx_recognition_device_captured", false, 3U, "recognition_id", "D"),
        index("recognition_logs", "PRIMARY", true, 1U, "recognition_id"),
        index("recognition_logs", "uk_recognition_device_capture", true, 1U, "device_id"),
        index("recognition_logs", "uk_recognition_device_capture", true, 2U, "capture_id"),
        index("schema_migrations", "PRIMARY", true, 1U, "version"),
    };
    requireEqual(actual, expected);
}

void validateForeignKeys(sql::Connection& connection, const std::string_view database) {
    const auto actual = querySchema(
        connection,
        database,
        "SELECT k.TABLE_NAME, k.CONSTRAINT_NAME, k.COLUMN_NAME, k.REFERENCED_TABLE_NAME, "
        "k.REFERENCED_COLUMN_NAME, k.ORDINAL_POSITION, r.UPDATE_RULE, r.DELETE_RULE "
        "FROM information_schema.KEY_COLUMN_USAGE k JOIN information_schema.REFERENTIAL_CONSTRAINTS r "
        "ON r.CONSTRAINT_SCHEMA = k.CONSTRAINT_SCHEMA AND r.CONSTRAINT_NAME = k.CONSTRAINT_NAME "
        "WHERE k.TABLE_SCHEMA = ? AND k.REFERENCED_TABLE_NAME IS NOT NULL "
        "ORDER BY k.TABLE_NAME, k.CONSTRAINT_NAME, k.ORDINAL_POSITION",
        8U);
    const Rows expected{
        foreignKey("access_lists", "fk_access_lists_user", "created_by_user_id", "admin_users", "id"),
        foreignKey("recognition_logs", "fk_recognition_device", "device_id", "devices", "device_id"),
    };
    requireEqual(actual, expected);
}

void validateChecks(sql::Connection& connection, const std::string_view database) {
    auto actual = querySchema(
        connection,
        database,
        "SELECT t.TABLE_NAME, t.CONSTRAINT_NAME, c.CHECK_CLAUSE, t.ENFORCED "
        "FROM information_schema.TABLE_CONSTRAINTS t JOIN information_schema.CHECK_CONSTRAINTS c "
        "ON c.CONSTRAINT_SCHEMA = t.CONSTRAINT_SCHEMA AND c.CONSTRAINT_NAME = t.CONSTRAINT_NAME "
        "WHERE t.CONSTRAINT_SCHEMA = ? AND t.CONSTRAINT_TYPE = 'CHECK' "
        "ORDER BY t.TABLE_NAME, t.CONSTRAINT_NAME",
        4U);
    for (auto& row : actual) {
        const auto first = row.find(kSeparator);
        const auto second = row.find(kSeparator, first + 1U);
        const auto third = row.rfind(kSeparator);
        if (first == std::string::npos || second == std::string::npos || third == second) {
            throw MigrationError();
        }
        row = row.substr(0U, second + 1U) +
              normalizeCheck(row.substr(second + 1U, third - second - 1U)) +
              row.substr(third);
    }
    const auto check = [](const std::string_view table,
                          const std::string_view name,
                          const std::string_view clause) {
        std::string row;
        append(row, table);
        append(row, name);
        append(row, normalizeCheck(std::string(clause)));
        append(row, "YES");
        return row;
    };
    const Rows expected{
        check("access_lists", "chk_access_lists_plate", "CHAR_LENGTH(plate_number) BETWEEN 1 AND 16"),
        check("access_lists", "chk_access_lists_remark", "CHAR_LENGTH(remark) <= 200"),
        check("admin_users", "chk_admin_users_display_name", "CHAR_LENGTH(display_name) BETWEEN 1 AND 64"),
        check("recognition_logs", "chk_recognition_image_size", "image_size_bytes <= 10485760"),
        check("recognition_logs", "chk_recognition_revision", "revision >= 1 AND revision <= 9007199254740991"),
        check("recognition_logs", "chk_recognition_state", "(status = 'PROCESSING' AND plate_number IS NULL AND error_code IS NULL AND error_message IS NULL AND completed_at IS NULL AND duration_ms IS NULL) OR (status = 'SUCCEEDED' AND plate_number IS NOT NULL AND error_code IS NULL AND error_message IS NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL) OR (status = 'FAILED' AND plate_number IS NULL AND error_code IS NOT NULL AND error_message IS NOT NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL)"),
    };
    requireEqual(actual, expected);
}

}  // namespace

void validateConfiguredSchema(
    sql::Connection& connection,
    const std::string_view database) {
    const auto schema = querySchema(
        connection,
        database,
        "SELECT DEFAULT_CHARACTER_SET_NAME, DEFAULT_COLLATION_NAME "
        "FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?",
        2U);
    requireEqual(schema, Rows{"utf8mb4\x1futf8mb4_0900_as_cs"});
}

void bootstrapMigrationTable(sql::Connection& connection) {
    validateVersion(connection);
    std::unique_ptr<sql::Statement> statement(connection.createStatement());
    (void)statement->execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "version BIGINT UNSIGNED NOT NULL,"
        "name VARCHAR(128) NOT NULL,"
        "checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,"
        "applied_at DATETIME(3) NOT NULL,"
        "PRIMARY KEY (version)"
        ") ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs");
}

void validateMigrationTable(sql::Connection& connection, const std::string_view database) {
    const auto tables = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, ENGINE, TABLE_COLLATION FROM information_schema.TABLES "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 'schema_migrations' AND TABLE_TYPE = 'BASE TABLE'",
        3U);
    requireEqual(tables, Rows{"schema_migrations\x1fInnoDB\x1futf8mb4_0900_as_cs"});

    const auto columns = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, COLUMN_NAME, ORDINAL_POSITION, COLUMN_TYPE, IS_NULLABLE, "
        "COLUMN_DEFAULT, CHARACTER_SET_NAME, COLLATION_NAME, EXTRA, GENERATION_EXPRESSION "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = ? "
        "AND TABLE_NAME = 'schema_migrations' ORDER BY ORDINAL_POSITION",
        10U);
    requireEqual(
        columns,
        Rows{
            column("schema_migrations", "version", 1U, "bigint unsigned", false, kNull, kNull, kNull),
            column("schema_migrations", "name", 2U, "varchar(128)", false, kNull, "utf8mb4", "utf8mb4_0900_as_cs"),
            column("schema_migrations", "checksum", 3U, "char(64)", false, kNull, "ascii", "ascii_bin"),
            column("schema_migrations", "applied_at", 4U, "datetime(3)", false, kNull, kNull, kNull),
        });

    const auto indexes = querySchema(
        connection,
        database,
        "SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, SEQ_IN_INDEX, COLUMN_NAME, COLLATION, "
        "SUB_PART, INDEX_TYPE, IS_VISIBLE, EXPRESSION FROM information_schema.STATISTICS "
        "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = 'schema_migrations' "
        "ORDER BY INDEX_NAME, SEQ_IN_INDEX",
        10U);
    requireEqual(indexes, Rows{index("schema_migrations", "PRIMARY", true, 1U, "version")});
}

void validateCompleteSchema(sql::Connection& connection, const std::string_view database) {
    validateConfiguredSchema(connection, database);
    validateTables(connection, database);
    validateColumns(connection, database);
    validateIndexes(connection, database);
    validateForeignKeys(connection, database);
    validateChecks(connection, database);
}

}  // namespace ocrservice::repositories::mysql::migration::detail
