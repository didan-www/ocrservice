ocr_add_module(
    NAME mysql_connection
    SOURCES
        MySqlConnectionPool.cpp
    PUBLIC_DEPENDENCIES
        app_config
        nlohmann_json::nlohmann_json
    PRIVATE_DEPENDENCIES
        MySQL::ConnectorCpp
)
