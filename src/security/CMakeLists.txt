ocr_add_module(
    NAME security
    SOURCES
        BcryptVerifier.cpp
        OpaqueTokenGenerator.cpp
    PRIVATE_DEPENDENCIES
        xcrypt::xcrypt
        OpenSSL::Crypto
)
