#pragma once

#include <string_view>

namespace sql {
class Connection;
}

namespace ocrservice::repositories::mysql::migration::detail {

void validateConfiguredSchema(sql::Connection& connection, std::string_view database);
void bootstrapMigrationTable(sql::Connection& connection);
void validateMigrationTable(sql::Connection& connection, std::string_view database);
void validateCompleteSchema(sql::Connection& connection, std::string_view database);

}  // namespace ocrservice::repositories::mysql::migration::detail
