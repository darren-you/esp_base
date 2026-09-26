if(IDF_TARGET STREQUAL "esp32c3")
    set(serial_driver esp_driver_usb_serial_jtag)
elseif(IDF_TARGET STREQUAL "esp32")
    set(serial_driver esp_driver_uart)
else()
    message(FATAL_ERROR "ESP Base serial control supports only esp32c3 and esp32")
endif()
idf_component_register(
    SRCS "esp_base_protocol.c" "command_guard.c" "command_decoder.c" "control_state.c" "network_auth.c" "mqtt_command.c" "mqtt_owner.c" "frp_owner.c" "frp_status_listener.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer esp_system freertos device_identity ${serial_driver} remote_config wifi_runtime time_runtime ota_operation container_binding esp_ota mbedtls mqtt esp_frp lwip
)
