ocr_add_module(
    NAME access_list_controller
    SOURCES
        AccessListController.cpp
        AccessListResponses.cpp
    PUBLIC_DEPENDENCIES
        access_list_service
        auth_service
        http_server
    PRIVATE_DEPENDENCIES
        auth_controller
        http_protocol
        serialization_json
)
