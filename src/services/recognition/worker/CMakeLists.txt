ocr_add_module(
    NAME recognition_worker
    SOURCES
        RecognitionWorker.cpp
    PUBLIC_DEPENDENCIES
        domain
        recognition_queue
    PRIVATE_DEPENDENCIES
        logging
        nlohmann_json::nlohmann_json
        opencv_core
        opencv_imgcodecs
)
