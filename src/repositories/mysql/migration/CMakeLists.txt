ocr_add_module(
    NAME mysql_migration
    SOURCES
        MySqlMigrator.cpp
        SchemaValidator.cpp
    PUBLIC_DEPENDENCIES
        mysql_connection
    PRIVATE_DEPENDENCIES
        MySQL::ConnectorCpp
        OpenSSL::Crypto
)
