idf_component_register(
    SRCS "main.c"
         "config/device_config.c"
         "network/wifi.c"
         "network/mqtt.c"
         "provision/http_server.c"
         "rf/rf_data.c"
         "rf/rf_receiver.c"
         "security/device_identity.c"
         "trigger/rf_supervisor.c"
         "trigger/rf_trigger.c"
         "vibrator/vibrator.c"
    INCLUDE_DIRS "." "config" "network" "provision" "rf" "security" "trigger" "vibrator"
    REQUIRES app_update esp8266 esp_common esp_event esp_http_server json log mbedtls mqtt nvs_flash tcpip_adapter
    EMBED_TXTFILES "network/certs/mqtt_ca.pem"
    EMBED_FILES "provision/index.html.gz"
)
