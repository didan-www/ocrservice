ocr_add_module(
    NAME recognition_controller
    SOURCES
        RecognitionController.cpp
        RecognitionResponses.cpp
    PUBLIC_DEPENDENCIES
        auth_service
        csv_service
        history_service
        http_server
    PRIVATE_DEPENDENCIES
        auth_controller
        http_protocol
        serialization_json
)
