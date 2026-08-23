cmake_minimum_required(VERSION 3.22)

project(
    ocrservice
    VERSION 0.1.0
    DESCRIPTION "Teaching vehicle plate recognition service"
    LANGUAGES C CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ocrservice supports Linux only")
endif()

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "ocrservice supports linux/amd64 only")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "ocrservice requires GCC")
endif()

if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11.4)
    message(FATAL_ERROR "ocrservice requires GCC 11.4 or newer")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OCRSERVICE_BUILD_TESTING "Build ocrservice tests" ON)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Dependencies)
include(AddOcrModule)

add_library(ocrservice_project_options INTERFACE)
target_compile_features(ocrservice_project_options INTERFACE cxx_std_17)

add_library(ocrservice_project_warnings INTERFACE)
target_compile_options(
    ocrservice_project_warnings
    INTERFACE
        $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)

set(_ocrservice_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_ocrservice_generated_core "${_ocrservice_generated_dir}/ocrservice_core.cpp")
file(
    GENERATE
    OUTPUT "${_ocrservice_generated_core}"
    CONTENT "namespace ocrservice { void core_link_anchor() {} }\n"
)

add_library(ocrservice_core STATIC "${_ocrservice_generated_core}")
add_library(ocrservice::core ALIAS ocrservice_core)
target_link_libraries(
    ocrservice_core
    PUBLIC
        ocrservice_project_options
        ocrservice_project_warnings
        ocrservice_dependencies
)

ocr_discover_subdirectories("${CMAKE_CURRENT_SOURCE_DIR}/src" "modules")

add_executable(ocrservice src/main.cpp)
target_link_libraries(ocrservice PRIVATE ocrservice::core)

if(OCRSERVICE_BUILD_TESTING)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
