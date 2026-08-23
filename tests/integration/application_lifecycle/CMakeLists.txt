ocr_add_test(
    NAME tst_application_lifecycle
    TEST_NAME integration.application_lifecycle
    SOURCES
        tst_application_lifecycle.cpp
    DEPENDENCIES
        app_runtime
        health_controller
        health_service
        http_server
        serialization_json
    LABELS
        task019
        integration
)

set_tests_properties(integration.application_lifecycle PROPERTIES TIMEOUT 30)
