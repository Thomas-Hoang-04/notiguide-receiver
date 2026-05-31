idf_component_register(
    SRCS "main.c"
         "config/device_config.c"
         "pair/espnow_pair.c"
         "trigger/rf_trigger.c"
         "trigger/rf_supervisor.c"
         "rf/rf_receiver.c"
         "rf/rf_data.c"
         "vibrator/vibrator.c"
    INCLUDE_DIRS "." "config" "pair" "rf" "trigger" "vibrator"
    REQUIRES esp8266 esp_common esp_event log mbedtls nvs_flash
)
