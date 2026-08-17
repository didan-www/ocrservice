ocr_add_module(
    NAME mqtt_publisher
    SOURCES
        MqttPayload.cpp
        MqttTransport.cpp
        PahoMqttPublisher.cpp
        PahoMqttTransport.cpp
        ReconnectDelay.cpp
    PUBLIC_DEPENDENCIES
        app_config
        domain
        logging
    PRIVATE_DEPENDENCIES
        serialization_json
        paho-mqttpp3-static
)
