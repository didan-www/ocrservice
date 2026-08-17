ocr_add_test(
    NAME tst_mysql_migration
    TEST_NAME component.mysql_migration
    SOURCES
        tst_mysql_migration.cpp
    DEPENDENCIES
        mysql_migration
        MySQL::ConnectorCpp
    LABELS
        component
        task008
)

target_compile_definitions(
    tst_mysql_migration
    PRIVATE
        OCRSERVICE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

set_tests_properties(
    component.mysql_migration
    PROPERTIES
        SKIP_RETURN_CODE 77
)
