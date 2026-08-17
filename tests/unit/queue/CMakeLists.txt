ocr_add_test(
    NAME tst_recognition_queue
    TEST_NAME unit.queue
    SOURCES
        tst_recognition_queue.cpp
    DEPENDENCIES
        recognition_queue
    LABELS
        task005
        unit
)
