ocr_add_test(
    NAME tst_http_runtime
    TEST_NAME component.http_runtime
    SOURCES
        tst_http_runtime.cpp
    DEPENDENCIES
        http_server
        serialization_json
    LABELS
        task013
        component
)

set_tests_properties(component.http_runtime PROPERTIES TIMEOUT 90)
