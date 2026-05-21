idf_component_register(
    SRCS "main.c"
         "config/device_config.c"
         "network/wifi.c"
         "network/mqtt.c"
         "provision/http_server.c"
         "security/device_identity.c"
         "trigger/rf_trigger.c"
         "trigger/rf_supervisor.c"
         "rf/rf_receiver.c"
         "rf/rf_data.c"
         "nrf24/nrf24_receiver.c"
         "vibrator/vibrator.c"
         "serial/serial_protocol.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "network/certs/mqtt_ca.pem"
    EMBED_FILES "provision/index.html.gz"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_http_server
                  esp_driver_gpio esp_driver_spi esp_timer esp_hw_support
                  mbedtls espressif__mqtt espressif__cjson
                  esp_driver_usb_serial_jtag
)
