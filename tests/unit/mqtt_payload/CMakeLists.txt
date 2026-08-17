ocr_add_test(
    NAME tst_mqtt_payload
    TEST_NAME unit.mqtt_payload
    SOURCES
        tst_mqtt_payload.cpp
    DEPENDENCIES
        mqtt_publisher
        serialization_json
    LABELS
        task012
        unit
)
