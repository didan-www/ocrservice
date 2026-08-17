ocr_add_test(
    NAME tst_http_protocol
    TEST_NAME unit.http_protocol
    SOURCES
        tst_http_protocol.cpp
    DEPENDENCIES
        http_protocol
        serialization_json
    LABELS
        task004
        unit
)
