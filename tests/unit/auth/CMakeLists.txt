ocr_add_test(
    NAME tst_auth
    TEST_NAME unit.auth
    SOURCES
        tst_auth.cpp
    DEPENDENCIES
        auth_service
        security
    LABELS
        unit
        task010
)
