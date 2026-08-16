ocr_add_test(
    NAME tst_logging
    TEST_NAME unit.logging
    SOURCES
        tst_logging.cpp
    DEPENDENCIES
        logging
        nlohmann_json::nlohmann_json
    LABELS
        task002
        unit
)
