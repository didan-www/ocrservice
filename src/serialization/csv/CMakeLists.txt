ocr_add_module(
    NAME serialization_csv
    SOURCES
        CsvSerialization.cpp
    PUBLIC_DEPENDENCIES
        domain
    PRIVATE_DEPENDENCIES
        serialization_time
)
