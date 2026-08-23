ocr_add_test(
    NAME tst_device_upload_api
    TEST_NAME integration.device_upload_api
    SOURCES
        tst_device_upload_api.cpp
    DEPENDENCIES
        device_recognition_controller
        http_server
        image_storage
        recognition_acceptance
        serialization_json
        serialization_time
        OpenSSL::Crypto
        opencv_core
        opencv_imgcodecs
)

set_tests_properties(
    integration.device_upload_api
    PROPERTIES
        LABELS "task017;integration"
        TIMEOUT 30
)
