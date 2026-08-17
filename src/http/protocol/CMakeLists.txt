ocr_add_module(
    NAME http_protocol
    SOURCES
        HttpPolicy.cpp
        ProtocolError.cpp
        QueryDecoders.cpp
        RequestDecoders.cpp
    PUBLIC_DEPENDENCIES
        domain
        serialization_time
        text
        nlohmann_json::nlohmann_json
)
