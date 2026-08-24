add_library(
    embedded_simulator_core
    STATIC
        DeviceConfig.cpp
        DeviceResult.cpp
        HttpUploader.cpp
        Simulator.cpp
)
target_compile_features(embedded_simulator_core PUBLIC cxx_std_17)
target_include_directories(embedded_simulator_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(
    embedded_simulator_core
    PUBLIC
        ocrservice::core
        nlohmann_json::nlohmann_json
        paho-mqttpp3-static
    PRIVATE
        ocrservice_project_warnings
)

add_executable(embedded_device_simulator main.cpp)
target_compile_features(embedded_device_simulator PRIVATE cxx_std_17)
target_link_libraries(
    embedded_device_simulator
    PRIVATE
        embedded_simulator_core
        ocrservice_project_warnings
)

ocr_add_test(
    NAME tst_embedded_simulator
    TEST_NAME unit.embedded_simulator
    SOURCES tst_embedded_simulator.cpp
    DEPENDENCIES embedded_simulator_core
    LABELS "unit;embedded"
)
