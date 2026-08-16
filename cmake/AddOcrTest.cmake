include_guard(GLOBAL)

include(CMakeParseArguments)

function(ocr_add_test)
    set(options)
    set(one_value_args NAME TEST_NAME)
    set(multi_value_args SOURCES DEPENDENCIES LABELS)
    cmake_parse_arguments(OCR "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT OCR_NAME OR NOT OCR_TEST_NAME)
        message(FATAL_ERROR "ocr_add_test requires NAME and TEST_NAME")
    endif()
    if(NOT OCR_SOURCES)
        message(FATAL_ERROR "ocr_add_test(${OCR_NAME}) requires SOURCES")
    endif()

    add_executable("${OCR_NAME}" ${OCR_SOURCES})
    target_compile_features("${OCR_NAME}" PRIVATE cxx_std_17)
    target_link_libraries(
        "${OCR_NAME}"
        PRIVATE
            ocrservice::core
            GTest::gtest_main
            ${OCR_DEPENDENCIES}
    )

    add_test(NAME "${OCR_TEST_NAME}" COMMAND "${OCR_NAME}")
    if(OCR_LABELS)
        set_tests_properties("${OCR_TEST_NAME}" PROPERTIES LABELS "${OCR_LABELS}")
    endif()
endfunction()
