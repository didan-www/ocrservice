ocr_add_test(
    NAME tst_qt_mqtt_contract
    TEST_NAME contract.qt.mqtt
    SOURCES
        tst_qt_mqtt_contract.cpp
    DEPENDENCIES
        mqtt_publisher
        serialization_json
        serialization_time
    LABELS
        task021
        contract
        qt
)

target_include_directories(
    tst_qt_mqtt_contract
    PRIVATE
        "${PROJECT_SOURCE_DIR}/tests/contract/qt_http"
)

target_compile_definitions(
    tst_qt_mqtt_contract
    PRIVATE
        OCR_QT_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures/qt"
)

set_tests_properties(contract.qt.mqtt PROPERTIES TIMEOUT 20)
