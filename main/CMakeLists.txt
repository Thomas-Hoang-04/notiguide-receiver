idf_component_register(
    SRCS "main.c"
         "config/device_config.c"
         "network/wifi.c"
         "network/mqtt.c"
         "serial/serial_protocol.c"
         "rf/rf_data.c"
         "rf/rf_receiver.c"
         "security/device_identity.c"
         "trigger/rf_supervisor.c"
         "trigger/rf_trigger.c"
         "vibrator/vibrator.c"
    INCLUDE_DIRS "." "config" "network" "rf" "security" "serial" "trigger" "vibrator"
    REQUIRES app_update esp8266 esp_common esp_event json log mbedtls mqtt nvs_flash tcpip_adapter
    EMBED_TXTFILES "network/certs/mqtt_ca.pem"
)
