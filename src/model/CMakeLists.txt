ocr_add_module(
    NAME plate_model
    SOURCES
        ModelAlgorithms.cpp
        OnnxPlateRecognizer.cpp
    PUBLIC_DEPENDENCIES
        domain
    PRIVATE_DEPENDENCIES
        onnxruntime::onnxruntime
        OpenSSL::Crypto
        opencv_core
        opencv_imgproc
)
