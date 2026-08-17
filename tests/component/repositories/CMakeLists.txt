ocr_add_test(
    NAME tst_mysql_repositories
    TEST_NAME component.repositories
    SOURCES
        tst_mysql_repositories.cpp
    DEPENDENCIES
        mysql_repositories
        mysql_migration
        MySQL::ConnectorCpp
    LABELS
        component
        task009
)

target_compile_definitions(
    tst_mysql_repositories
    PRIVATE
        OCRSERVICE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_tests_properties(
    component.repositories
    PROPERTIES
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
)
