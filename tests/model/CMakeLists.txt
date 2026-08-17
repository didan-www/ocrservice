ocr_add_test(
    NAME tst_model_algorithms
    TEST_NAME model.algorithms
    SOURCES
        tst_model_algorithms.cpp
    DEPENDENCIES
        plate_model
    LABELS
        task007
        unit
        model
)

ocr_add_test(
    NAME tst_onnx_plate_recognizer
    TEST_NAME model.runtime
    SOURCES
        tst_onnx_plate_recognizer.cpp
    DEPENDENCIES
        plate_model
        onnxruntime::onnxruntime
        opencv_core
        opencv_imgcodecs
    LABELS
        task007
        component
        model
)

target_compile_definitions(
    tst_onnx_plate_recognizer
    PRIVATE
        OCRSERVICE_TEST_MODEL_DIR="${PROJECT_SOURCE_DIR}/models"
        OCRSERVICE_TEST_IMAGE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/images"
)
