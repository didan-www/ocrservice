ocr_add_module(
    NAME serialization_json
    SOURCES
        ExactJson.cpp
    PUBLIC_DEPENDENCIES
        domain
        http_protocol
        serialization_time
        text
        nlohmann_json::nlohmann_json
)
