ocr_add_test(
    NAME tst_posix_image_storage
    TEST_NAME component.storage
    SOURCES
        tst_posix_image_storage.cpp
    DEPENDENCIES
        image_storage
    LABELS
        component
        task006
)
