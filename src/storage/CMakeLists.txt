ocr_add_module(
    NAME image_storage
    SOURCES
        ImageValidator.cpp
        PosixFileOps.cpp
        PosixImageStorage.cpp
    PUBLIC_DEPENDENCIES
        domain
    PRIVATE_DEPENDENCIES
        OpenSSL::Crypto
        opencv_core
        opencv_imgcodecs
        spdlog::spdlog
)
