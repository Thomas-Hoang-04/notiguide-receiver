/**
 * @file device_config.h
 * @brief Minimal NVS-backed pairing configuration for locally-paired receivers.
 */

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define DEVICE_CONFIG_MAX_RF_CODE_LEN 16U
#define DEVICE_CONFIG_PAIR_NAMESPACE  "pair"

typedef struct {
    uint8_t slot_id;
    uint8_t hub_mac[6];
    uint8_t rf_code[DEVICE_CONFIG_MAX_RF_CODE_LEN];
    size_t rf_code_len;
    uint8_t rf_code_bits;
    uint32_t rf_code_ver;
    uint8_t rf_band;
    bool paired;
} device_config_t;

/**
 * @brief Initialize NVS or recover from corruption.
 */
esp_err_t nvs_init_or_recover(void);

/**
 * @brief Load pairing config from NVS into RAM.
 */
esp_err_t device_config_load(device_config_t *cfg);

/**
 * @brief Persist a completed pairing to NVS.
 */
esp_err_t device_config_save_pairing(device_config_t *cfg,
                                     uint8_t slot_id,
                                     const uint8_t hub_mac[6],
                                     const uint8_t *rf_code,
                                     size_t rf_code_len,
                                     uint8_t rf_bits,
                                     uint8_t rf_band,
                                     uint32_t rf_code_ver);

/**
 * @brief Erase the pairing namespace (factory reset).
 */
esp_err_t device_config_factory_reset(void);

/**
 * @brief Check if rf_code fields are populated and valid.
 */
bool device_config_has_rf_code(const device_config_t *cfg);

#endif /* DEVICE_CONFIG_H */
