ocr_add_module(
    NAME http_middleware
    SOURCES
        AccessLog.cpp
        RequestGuards.cpp
        RequestId.cpp
    PUBLIC_DEPENDENCIES
        domain
        http_protocol
        logging
    PRIVATE_DEPENDENCIES
        Crow::Crow
        OpenSSL::Crypto
)
