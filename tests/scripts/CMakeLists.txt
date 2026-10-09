find_program(OCRSERVICE_BASH_EXECUTABLE bash)

if(OCRSERVICE_BASH_EXECUTABLE)
    add_test(
        NAME component.classroom_device_demo_script
        COMMAND
            "${OCRSERVICE_BASH_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/test_run_classroom_device_demo.sh"
    )
    set_tests_properties(
        component.classroom_device_demo_script
        PROPERTIES
            LABELS "component;classroom"
            RUN_SERIAL TRUE
            TIMEOUT 120
    )
endif()
