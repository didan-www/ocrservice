ocr_add_test(
    NAME tst_qt_http_contract
    TEST_NAME contract.qt.http
    SOURCES
        tst_qt_http_contract.cpp
    DEPENDENCIES
        access_list_controller
        auth_controller
        csv_service
        http_protocol
        http_server
        recognition_controller
        serialization_json
        serialization_time
    LABELS
        task021
        contract
        qt
)

target_compile_definitions(
    tst_qt_http_contract
    PRIVATE
        OCR_QT_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures/qt"
)

set_tests_properties(contract.qt.http PROPERTIES TIMEOUT 20)
