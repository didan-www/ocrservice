ocr_add_test(
    NAME tst_server_config
    TEST_NAME unit.config
    SOURCES
        tst_server_config.cpp
    DEPENDENCIES
        app_config
        nlohmann_json::nlohmann_json
    LABELS
        task002
        unit
)
