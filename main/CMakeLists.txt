idf_component_register(
    SRCS "main.cpp" "../${CASTER_TYPE}_caster/encoders.cpp" "protocol.cpp" "transport.cpp" "recorder.cpp"
    INCLUDE_DIRS "." "${CASTER_CONFIG_DIRECTORY}"
    REQUIRES esp_driver_gpio esp_driver_pcnt esp_driver_spi esp_timer
             esp_wifi esp_netif esp_event nvs_flash json esp_system esp_http_server esp_partition)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CASTER_NODE_ID=${CASTER_NODE_ID}
    CASTER_WHEEL_INDEX=${CASTER_WHEEL_INDEX}
    CASTER_CONFIG_HEADER="${CASTER_CONFIG_HEADER}")
