include_guard(GLOBAL)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)
find_package(OpenCV 4.5 REQUIRED COMPONENTS core imgcodecs imgproc)

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Runtime dependency baselines. System packages are supplied by the Linux
# image; source-only libraries and ONNX Runtime are pinned below.
set(OCRSERVICE_GCC_BASELINE "11.4.0" CACHE INTERNAL "GCC baseline")
set(OCRSERVICE_CMAKE_BASELINE "3.22.1" CACHE INTERNAL "CMake baseline")
set(OCRSERVICE_OPENCV_VERSION "4.5.4" CACHE INTERNAL "OpenCV apt baseline")
set(OCRSERVICE_OPENSSL_VERSION "3.0.2" CACHE INTERNAL "OpenSSL apt baseline")
set(OCRSERVICE_LIBXCRYPT_VERSION "4.4.27" CACHE INTERNAL "libxcrypt apt baseline")
set(OCRSERVICE_MYSQL_CONCPP_VERSION "8.4.0" CACHE INTERNAL "MySQL Connector/C++ baseline")
set(OCRSERVICE_ONNXRUNTIME_VERSION "1.20.1" CACHE INTERNAL "ONNX Runtime version")

set(OCRSERVICE_MYSQL_CONCPP_ROOT "" CACHE PATH "MySQL Connector/C++ 8.4.0 installation root")
find_path(
    OCRSERVICE_MYSQL_CONCPP_INCLUDE_DIR
    NAMES cppconn/driver.h
    HINTS "${OCRSERVICE_MYSQL_CONCPP_ROOT}"
    PATH_SUFFIXES include/jdbc include mysql-cppconn-8/jdbc
)
find_library(
    OCRSERVICE_MYSQL_CONCPP_LIBRARY
    NAMES mysqlcppconn
    HINTS "${OCRSERVICE_MYSQL_CONCPP_ROOT}"
    PATH_SUFFIXES lib64 lib lib/x86_64-linux-gnu
)

if(NOT OCRSERVICE_MYSQL_CONCPP_INCLUDE_DIR OR NOT OCRSERVICE_MYSQL_CONCPP_LIBRARY)
    message(FATAL_ERROR
        "MySQL Connector/C++ ${OCRSERVICE_MYSQL_CONCPP_VERSION} Classic API was not found. "
        "Install the fixed Linux package and set OCRSERVICE_MYSQL_CONCPP_ROOT if it is outside the system prefix."
    )
endif()

set(_mysql_version_header "${OCRSERVICE_MYSQL_CONCPP_INCLUDE_DIR}/cppconn/version_info.h")
if(NOT EXISTS "${_mysql_version_header}")
    message(FATAL_ERROR "MySQL Connector/C++ version_info.h was not found")
endif()
file(STRINGS "${_mysql_version_header}" _mysql_version_line
    REGEX "^#define[ \t]+MYSQL_CONCPP_VERSION_NUMBER[ \t]+[0-9]+")
if(NOT _mysql_version_line MATCHES "([0-9]+)$")
    message(FATAL_ERROR "Unable to determine MySQL Connector/C++ version")
endif()
set(_mysql_version_number "${CMAKE_MATCH_1}")
if(NOT _mysql_version_number EQUAL 8040000)
    message(FATAL_ERROR
        "MySQL Connector/C++ ${OCRSERVICE_MYSQL_CONCPP_VERSION} is required; "
        "found version number ${_mysql_version_number}"
    )
endif()

add_library(mysql_concpp UNKNOWN IMPORTED GLOBAL)
set_target_properties(
    mysql_concpp
    PROPERTIES
        IMPORTED_LOCATION "${OCRSERVICE_MYSQL_CONCPP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OCRSERVICE_MYSQL_CONCPP_INCLUDE_DIR}"
)
add_library(MySQL::ConnectorCpp ALIAS mysql_concpp)

find_path(OCRSERVICE_XCRYPT_INCLUDE_DIR NAMES crypt.h REQUIRED)
find_library(OCRSERVICE_XCRYPT_LIBRARY NAMES crypt REQUIRED)
add_library(xcrypt UNKNOWN IMPORTED GLOBAL)
set_target_properties(
    xcrypt
    PROPERTIES
        IMPORTED_LOCATION "${OCRSERVICE_XCRYPT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OCRSERVICE_XCRYPT_INCLUDE_DIR}"
)
add_library(xcrypt::xcrypt ALIAS xcrypt)

FetchContent_Declare(
    asio
    URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/asio-1-30-2
    URL_HASH SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8
)
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
endif()
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/asio/include" CACHE PATH "Pinned Asio include directory" FORCE)

set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(CROW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    crow
    URL https://codeload.github.com/CrowCpp/Crow/tar.gz/refs/tags/v1.2.1
    URL_HASH SHA256=552f2e447adf70ed4c667d6f82db53dfc70710b50431004ab1405f5b53f04c30
)
FetchContent_MakeAvailable(crow)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.11.3
    URL_HASH SHA256=0d8ef5af7f9794e3263480193c491549b2ba6cc74bb018906202ada498a79406
)
FetchContent_MakeAvailable(nlohmann_json)

set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    utf8proc
    URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/refs/tags/v2.9.0
    URL_HASH SHA256=18c1626e9fc5a2e192311e36b3010bfc698078f692888940f1fa150547abb0c1
)
FetchContent_MakeAvailable(utf8proc)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    URL https://codeload.github.com/gabime/spdlog/tar.gz/refs/tags/v1.14.1
    URL_HASH SHA256=1586508029a7d0670dfcb2d97575dcdc242d3868a259742b69f100801ab4e16b
)
FetchContent_MakeAvailable(spdlog)

set(PAHO_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(PAHO_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(PAHO_BUILD_DEB_PACKAGE OFF CACHE BOOL "" FORCE)
set(PAHO_WITH_SSL ON CACHE BOOL "" FORCE)
set(PAHO_WITH_MQTT_C ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    paho_mqtt_c
    URL https://codeload.github.com/eclipse-paho/paho.mqtt.c/tar.gz/refs/tags/v1.3.13
    URL_HASH SHA256=47c77e95609812da82feee30db435c3b7c720d4fd3147d466ead126e657b6d9c
)
FetchContent_Declare(
    paho_mqtt_cpp
    URL https://codeload.github.com/eclipse-paho/paho.mqtt.cpp/tar.gz/refs/tags/v1.4.1
    URL_HASH SHA256=48e7ba6e1032aa73e4d985a7387e02a77cc5807a8420d16790b84b941d86374e
)
FetchContent_GetProperties(paho_mqtt_c)
if(NOT paho_mqtt_c_POPULATED)
    FetchContent_Populate(paho_mqtt_c)
endif()
FetchContent_GetProperties(paho_mqtt_cpp)
if(NOT paho_mqtt_cpp_POPULATED)
    FetchContent_Populate(paho_mqtt_cpp)
endif()
file(
    COPY "${paho_mqtt_c_SOURCE_DIR}/"
    DESTINATION "${paho_mqtt_cpp_SOURCE_DIR}/externals/paho-mqtt-c"
)
add_subdirectory("${paho_mqtt_cpp_SOURCE_DIR}" "${paho_mqtt_cpp_BINARY_DIR}")

FetchContent_Declare(
    onnxruntime
    URL https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz
    URL_HASH SHA256=67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105
)
FetchContent_GetProperties(onnxruntime)
if(NOT onnxruntime_POPULATED)
    FetchContent_Populate(onnxruntime)
endif()
add_library(onnxruntime UNKNOWN IMPORTED GLOBAL)
set_target_properties(
    onnxruntime
    PROPERTIES
        IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
)
add_library(onnxruntime::onnxruntime ALIAS onnxruntime)

if(OCRSERVICE_BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.15.2
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    )
    FetchContent_MakeAvailable(googletest)
endif()

function(ocr_mark_dependency_headers_system target_name)
    get_target_property(_include_dirs "${target_name}" INTERFACE_INCLUDE_DIRECTORIES)
    if(_include_dirs AND NOT _include_dirs MATCHES "-NOTFOUND$")
        set_property(
            TARGET "${target_name}"
            APPEND
            PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_include_dirs}"
        )
    endif()
endfunction()

foreach(_dependency_target IN ITEMS Crow nlohmann_json utf8proc spdlog paho-mqttpp3-static)
    ocr_mark_dependency_headers_system("${_dependency_target}")
endforeach()

add_library(ocrservice_dependencies INTERFACE)
add_library(ocrservice::dependencies ALIAS ocrservice_dependencies)
target_link_libraries(
    ocrservice_dependencies
    INTERFACE
        Crow::Crow
        nlohmann_json::nlohmann_json
        utf8proc
        opencv_core
        opencv_imgcodecs
        opencv_imgproc
        onnxruntime::onnxruntime
        MySQL::ConnectorCpp
        paho-mqttpp3-static
        spdlog::spdlog
        xcrypt::xcrypt
        OpenSSL::Crypto
        OpenSSL::SSL
        Threads::Threads
)

message(STATUS "Dependency baseline: GCC ${OCRSERVICE_GCC_BASELINE}, CMake ${OCRSERVICE_CMAKE_BASELINE}")
message(STATUS "Dependency baseline: OpenCV ${OCRSERVICE_OPENCV_VERSION}, OpenSSL ${OCRSERVICE_OPENSSL_VERSION}")
message(STATUS "Dependency baseline: MySQL Connector/C++ ${OCRSERVICE_MYSQL_CONCPP_VERSION}, ONNX Runtime ${OCRSERVICE_ONNXRUNTIME_VERSION}")
message(STATUS "Pinned source dependencies: Crow 1.2.1, nlohmann/json 3.11.3, utf8proc 2.9.0, spdlog 1.14.1")
message(STATUS "Pinned source dependencies: Asio 1.30.2, Paho C 1.3.13/C++ 1.4.1, GoogleTest 1.15.2")
