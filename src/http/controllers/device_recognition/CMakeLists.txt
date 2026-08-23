ocr_add_module(
    NAME device_recognition_controller
    SOURCES
        DeviceRecognitionController.cpp
        DeviceRecognitionResponses.cpp
        StrictMultipart.cpp
    PUBLIC_DEPENDENCIES
        http_server
        recognition_acceptance
    PRIVATE_DEPENDENCIES
        http_protocol
        serialization_json
        serialization_time
)
