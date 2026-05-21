/**
 * @file wifi.h
 * @brief ESP-IDF Wi-Fi helpers for station and SoftAP modes.
 */

#ifndef RECEIVER_WIFI_H
#define RECEIVER_WIFI_H

#include <stdbool.h>
#include "esp_err.h"
#include "config/device_config.h"
#include "portmacro.h"

typedef enum {
    RECEIVER_WIFI_MODE_IDLE = 0,
    RECEIVER_WIFI_MODE_STA,
    RECEIVER_WIFI_MODE_SOFTAP,
} receiver_wifi_mode_t;

/**
 * @brief Start the Wi-Fi station and wait for an IP address.
 *
 * @param cfg Provisioned device configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_start_sta(const device_config_t *cfg);

/**
 * @brief Start the provisioning SoftAP.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_start_softap(void);

/**
 * @brief Stop the currently running Wi-Fi mode.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t wifi_stop(void);

/**
 * @brief Get the currently active Wi-Fi mode.
 *
 * @return Current Wi-Fi mode
 */
receiver_wifi_mode_t wifi_get_mode(void);

/**
 * @brief Get the generated provisioning SoftAP SSID.
 *
 * @return Null-terminated SoftAP SSID string
 */
const char *wifi_get_softap_ssid(void);

/**
 * @brief Report whether the station is currently connected.
 *
 * @return True when the station has obtained an IP address
 */
bool wifi_is_sta_connected(void);

/**
 * @brief One-shot STA connection test with raw credentials.
 *
 * @param ssid     Wi-Fi SSID (must not be NULL)
 * @param password Wi-Fi password (may be NULL for open networks)
 * @param timeout  Maximum time to wait for connection
 * @return ESP_OK on successful connection, ESP_FAIL on timeout, or an error code
 */
esp_err_t wifi_start_sta_test(const char *ssid, const char *password, TickType_t timeout);

#endif /* RECEIVER_WIFI_H */
