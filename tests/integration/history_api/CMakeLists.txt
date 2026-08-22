ocr_add_test(
    NAME tst_history_api
    TEST_NAME integration.history_api
    SOURCES
        tst_history_api.cpp
    DEPENDENCIES
        csv_service
        history_service
        http_server
        recognition_controller
        serialization_csv
        serialization_json
        serialization_time
    LABELS
        task015
        integration
)

set_tests_properties(integration.history_api PROPERTIES TIMEOUT 150)
