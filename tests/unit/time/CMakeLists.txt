ocr_add_test(
    NAME tst_protocol_time
    TEST_NAME unit.time
    SOURCES
        tst_protocol_time.cpp
    DEPENDENCIES
        serialization_time
    LABELS
        task003
        unit
)
