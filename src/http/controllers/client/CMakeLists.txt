ocr_add_module(
    NAME client_controller
    SOURCES
        ClientController.cpp
    PUBLIC_DEPENDENCIES
        auth_controller
        auth_service
        http_server
    PRIVATE_DEPENDENCIES
        http_protocol
)
