include_guard(GLOBAL)

include(CMakeParseArguments)

function(ocr_add_module)
    set(options)
    set(one_value_args NAME)
    set(multi_value_args SOURCES PUBLIC_DEPENDENCIES PRIVATE_DEPENDENCIES)
    cmake_parse_arguments(OCR "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT OCR_NAME)
        message(FATAL_ERROR "ocr_add_module requires NAME")
    endif()
    if(NOT OCR_SOURCES)
        message(FATAL_ERROR "ocr_add_module(${OCR_NAME}) requires SOURCES")
    endif()
    if(TARGET "${OCR_NAME}")
        message(FATAL_ERROR "Target ${OCR_NAME} already exists")
    endif()

    add_library("${OCR_NAME}" STATIC ${OCR_SOURCES})
    add_library("ocrservice::${OCR_NAME}" ALIAS "${OCR_NAME}")
    target_compile_features("${OCR_NAME}" PUBLIC cxx_std_17)
    target_include_directories(
        "${OCR_NAME}"
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    )
    target_link_libraries(
        "${OCR_NAME}"
        PUBLIC
            ocrservice_project_options
            ${OCR_PUBLIC_DEPENDENCIES}
        PRIVATE
            ocrservice_project_warnings
            ${OCR_PRIVATE_DEPENDENCIES}
    )
    target_link_libraries(ocrservice_core INTERFACE "${OCR_NAME}")
endfunction()
function(ocr_discover_subdirectories root_path binary_group)
    if(NOT IS_DIRECTORY "${root_path}")
        return()
    endif()

    file(
        GLOB_RECURSE _ocr_cmake_files
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        "${root_path}/*/CMakeLists.txt"
    )
    list(SORT _ocr_cmake_files)

    foreach(_ocr_cmake_file IN LISTS _ocr_cmake_files)
        get_filename_component(_ocr_source_dir "${_ocr_cmake_file}" DIRECTORY)
        file(RELATIVE_PATH _ocr_relative_dir "${root_path}" "${_ocr_source_dir}")
        string(MAKE_C_IDENTIFIER "${_ocr_relative_dir}" _ocr_binary_id)
        add_subdirectory(
            "${_ocr_source_dir}"
            "${CMAKE_CURRENT_BINARY_DIR}/${binary_group}/${_ocr_binary_id}"
        )
    endforeach()
endfunction()
