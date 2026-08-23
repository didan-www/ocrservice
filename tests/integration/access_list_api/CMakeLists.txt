ocr_add_test(
    NAME tst_access_list_api
    TEST_NAME integration.access_list_api
    SOURCES
        tst_access_list_api.cpp
    DEPENDENCIES
        access_list_controller
        access_list_service
        http_server
        serialization_json
        serialization_time
    LABELS
        task016
        integration
)

set_tests_properties(integration.access_list_api PROPERTIES TIMEOUT 30)
