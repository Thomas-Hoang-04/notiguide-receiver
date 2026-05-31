/**
 * @file device_config.h
 * @brief Minimal NVS-backed pairing configuration for ESP8266 receivers.
 */

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define DEVICE_CONFIG_TAG             "DEVICE_CFG"
#define DEVICE_CONFIG_PAIR_NAMESPACE  "pair"

typedef struct {
    uint8_t  slot_id;
    uint8_t  hub_mac[6];
    uint32_t rf_code;       // 32-bit 433 MHz trigger code
    uint8_t  rf_code_bits;  // always 32
    bool     paired;
} device_config_t;

esp_err_t nvs_init_or_recover(void);
esp_err_t device_config_load(device_config_t *cfg);
esp_err_t device_config_save_pairing(device_config_t *cfg,
                                     uint8_t slot_id,
                                     const uint8_t hub_mac[6],
                                     uint32_t rf_code,
                                     uint8_t rf_bits);
esp_err_t device_config_factory_reset(void);

#endif /* DEVICE_CONFIG_H */
