ocr_add_module(
    NAME mysql_repositories
    SOURCES
        MySqlRepositories.cpp
    PUBLIC_DEPENDENCIES
        domain
        mysql_connection
    PRIVATE_DEPENDENCIES
        MySQL::ConnectorCpp
)
