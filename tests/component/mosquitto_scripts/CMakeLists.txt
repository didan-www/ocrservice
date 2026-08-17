find_program(OCRSERVICE_BASH_EXECUTABLE bash)

if(OCRSERVICE_BASH_EXECUTABLE)
    add_test(
        NAME component.mosquitto_scripts
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "OCRSERVICE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "${OCRSERVICE_BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tst_mosquitto_scripts.sh"
    )
    set_tests_properties(
        component.mosquitto_scripts
        PROPERTIES
            LABELS "component;task011"
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77
            TIMEOUT 300
    )
endif()
