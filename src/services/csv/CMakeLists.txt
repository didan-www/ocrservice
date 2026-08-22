ocr_add_module(
    NAME csv_service
    SOURCES
        CsvService.cpp
    PUBLIC_DEPENDENCIES
        domain
    PRIVATE_DEPENDENCIES
        serialization_csv
)
