ocr_add_module(
    NAME app_config
    SOURCES
        ServerConfigLoader.cpp
    PRIVATE_DEPENDENCIES
        nlohmann_json::nlohmann_json
)
