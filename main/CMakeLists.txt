idf_component_register(
    SRCS "main.c"
         "config/device_config.c"
         "pair/espnow_pair.c"
         "trigger/rf_trigger.c"
         "trigger/rf_supervisor.c"
         "rf/rf_receiver.c"
         "rf/rf_data.c"
         "nrf24/nrf24_receiver.c"
         "vibrator/vibrator.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event
                  esp_driver_gpio esp_driver_spi esp_timer esp_hw_support
                  mbedtls
)
