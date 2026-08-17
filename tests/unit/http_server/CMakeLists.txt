ocr_add_test(
    NAME tst_http_server
    TEST_NAME unit.http_server
    SOURCES
        tst_http_server.cpp
    DEPENDENCIES
        http_middleware
        http_server
        Crow::Crow
    LABELS
        task013
        unit
)
