/**
 * @file rf_trigger.h
 * @brief Trigger matcher shared by the 433 MHz and nRF24 receivers.
 */

#ifndef RF_TRIGGER_H
#define RF_TRIGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "config/device_config.h"

/* Magic bytes the hub sends in every 2.4G dispatch payload. */
#define RF_TRIGGER_DISPATCH_MAGIC_HI 0xAAU
#define RF_TRIGGER_DISPATCH_MAGIC_LO 0x55U

/**
 * @brief Initialize the shared trigger matcher and vibrator output.
 *
 * @param vibrator_gpio GPIO used to drive the vibrator transistor
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trigger_init(gpio_num_t vibrator_gpio);

/**
 * @brief Deinitialize the shared trigger matcher and vibrator output.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trigger_deinit(void);

/**
 * @brief Install a new RF trigger code in RAM.
 *
 * @param code RF code bytes
 * @param code_len Number of valid bytes in @p code
 * @param bits RF code width in bits
 * @param version Monotonic version for the paired RF code
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trigger_set(const uint8_t *code, size_t code_len, uint8_t bits, uint32_t version);

/**
 * @brief Seed the in-RAM trigger matcher from persisted configuration.
 *
 * @param cfg Persisted device configuration
 */
void rf_trigger_restore_from_cfg(const device_config_t *cfg);

/**
 * @brief Report whether a trigger code is currently installed.
 *
 * @return True when a code is available for matching
 */
bool rf_trigger_has_code(void);

/**
 * @brief Force the vibrator output into the idle state.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trigger_stop_output(void);

/**
 * @brief Match a decoded 433 MHz frame against the active trigger code.
 *
 * @param value Decoded frame value
 * @param value_bits Width of @p value in bits
 */
void rf_trigger_on_frame(uint32_t value, uint8_t value_bits);

/**
 * @brief Match an nRF24 payload against the active trigger code.
 *
 * @param pkt Packet payload bytes
 * @param pkt_len Number of valid bytes in @p pkt
 */
void rf_trigger_on_packet(const uint8_t *pkt, size_t pkt_len);

#endif /* RF_TRIGGER_H */
