/**
 * @file wifi.h
 * @brief Wi-Fi Module - Runtime Networking Interface
 *
 * Declares the STA and SoftAP helpers used by the receiver lifecycle to
 * initialize Wi-Fi, connect to infrastructure mode, and expose provisioning.
 */

#ifndef WIFI_H
#define WIFI_H

#include <stddef.h>

#include "esp_err.h"

#define WIFI_TAG "WIFI"

esp_err_t wifi_init(void);
esp_err_t wifi_start_sta(const char *ssid, const char *password);
esp_err_t wifi_start_softap(char *ssid_buf, size_t ssid_buf_len);
esp_err_t wifi_stop(void);
esp_err_t wifi_get_mac_label(char *buf, size_t buf_len);

#endif
