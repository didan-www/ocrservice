ocr_add_module(
    NAME logging
    SOURCES
        LoggerFactory.cpp
        LogSanitizer.cpp
    PUBLIC_DEPENDENCIES
        app_config
    PRIVATE_DEPENDENCIES
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)
