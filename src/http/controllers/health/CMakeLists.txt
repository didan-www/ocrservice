ocr_add_module(
    NAME health_controller
    SOURCES
        HealthController.cpp
    PUBLIC_DEPENDENCIES
        health_service
        http_server
    PRIVATE_DEPENDENCIES
        http_protocol
        serialization_json
)
