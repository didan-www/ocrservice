ocr_add_test(
    NAME tst_text
    TEST_NAME unit.text
    SOURCES
        tst_utf8.cpp
    DEPENDENCIES
        text
    LABELS
        task003
        unit
)
