/**
 * @file espnow_pair.h
 * @brief ESP-NOW pairing state machine for locally-paired receivers.
 */

#ifndef ESPNOW_PAIR_H
#define ESPNOW_PAIR_H

#include "config/device_config.h"
#include "esp_err.h"

/**
 * @brief Block until paired with a transmitter hub.
 *
 * Initializes WiFi (STA, no connection) and ESP-NOW. Scans channels
 * 1-13, broadcasts PAIR_REQUEST, completes the 6-message handshake,
 * and persists the binding to NVS. Deinitializes WiFi before returning.
 *
 * @param cfg Device config to populate on successful pairing
 * @return ESP_OK on success
 */
esp_err_t espnow_pair_wait(device_config_t *cfg);

#endif /* ESPNOW_PAIR_H */
