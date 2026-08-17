ocr_add_test(
    NAME tst_image_validator
    TEST_NAME unit.storage.validator
    SOURCES
        tst_image_validator.cpp
    DEPENDENCIES
        image_storage
    LABELS
        task006
        unit
)
