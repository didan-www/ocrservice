ocr_add_module(
    NAME http_server
    SOURCES
        HttpRuntime.cpp
        HttpServer.cpp
    PUBLIC_DEPENDENCIES
        http_middleware
        http_protocol
    PRIVATE_DEPENDENCIES
        serialization_json
        Crow::Crow
)
