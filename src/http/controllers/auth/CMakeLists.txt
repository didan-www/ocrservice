ocr_add_module(
    NAME auth_controller
    SOURCES
        AuthController.cpp
        AuthResponses.cpp
    PUBLIC_DEPENDENCIES
        app_config
        auth_service
        http_server
    PRIVATE_DEPENDENCIES
        http_protocol
        serialization_json
)
