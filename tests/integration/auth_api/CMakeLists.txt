ocr_add_test(
    NAME tst_auth_api
    TEST_NAME integration.auth_api
    SOURCES
        tst_auth_api.cpp
    DEPENDENCIES
        auth_controller
        client_controller
        http_server
        serialization_json
        serialization_time
    LABELS
        task014
        integration
)

set_tests_properties(integration.auth_api PROPERTIES TIMEOUT 30)
