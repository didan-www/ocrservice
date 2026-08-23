ocr_add_module(
    NAME recognition_acceptance
    SOURCES
        RecognitionAcceptanceService.cpp
    PUBLIC_DEPENDENCIES
        domain
        image_storage
        recognition_queue
    PRIVATE_DEPENDENCIES
        logging
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
)
