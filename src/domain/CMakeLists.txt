ocr_add_module(
    NAME domain
    SOURCES
        Identifiers.cpp
        Recognition.cpp
        Ports.cpp
    PUBLIC_DEPENDENCIES
        serialization_time
        text
)
