ocr_add_test(
    NAME tst_recognition_worker
    TEST_NAME integration.recognition_worker
    SOURCES
        tst_recognition_worker.cpp
    DEPENDENCIES
        recognition_worker
        logging
        spdlog::spdlog
        opencv_core
        opencv_imgcodecs
        opencv_imgproc
    LABELS
        task018
        integration
)

set_tests_properties(
    integration.recognition_worker
    PROPERTIES
        TIMEOUT 30
)
