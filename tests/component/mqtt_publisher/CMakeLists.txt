ocr_add_test(
    NAME tst_mqtt_publisher
    TEST_NAME component.mqtt_publisher
    SOURCES
        tst_mqtt_publisher.cpp
        tst_mqtt_publisher_real.cpp
    DEPENDENCIES
        logging
        mqtt_publisher
        paho-mqttpp3-static
    LABELS
        component
        task012
)

find_program(OCRSERVICE_BASH_EXECUTABLE bash)
if(OCRSERVICE_BASH_EXECUTABLE)
    add_test(
        NAME component.mqtt_publisher.mosquitto
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "OCRSERVICE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "OCRSERVICE_MQTT_TEST_BINARY=$<TARGET_FILE:tst_mqtt_publisher>"
            "${OCRSERVICE_BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tst_mqtt_publisher_mosquitto.sh"
    )
    set_tests_properties(
        component.mqtt_publisher.mosquitto
        PROPERTIES
            LABELS "component;task012"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 180
    )
endif()
