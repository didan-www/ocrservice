ocr_add_test(
    NAME tst_domain
    TEST_NAME unit.domain
    SOURCES
        tst_domain.cpp
    DEPENDENCIES
        domain
    LABELS
        task003
        unit
)
