/**
 * @file espnow_pair.h
 * @brief ESP-NOW pairing state machine for ESP8266 receivers.
 */

#ifndef ESPNOW_PAIR_H
#define ESPNOW_PAIR_H

#include "config/device_config.h"
#include "esp_err.h"

esp_err_t espnow_pair_wait(device_config_t *cfg);

#endif /* ESPNOW_PAIR_H */
