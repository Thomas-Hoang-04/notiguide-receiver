/**
 * @file rf_supervisor.h
 * @brief Radio-variant dispatcher for receiver lifecycle operations.
 */

#ifndef RF_SUPERVISOR_H
#define RF_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config/device_config.h"
#include "esp_err.h"

/**
 * @brief Start the compiled RF receiver variant.
 *
 * On RECEIVER_2_4G the supervisor sources RX_ADDR_P1 from
 * `cfg->rf_code`; on RECEIVER_433M `cfg` is unused.
 *
 * @param cfg Persisted device configuration (may be NULL on 433M)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_start(const device_config_t *cfg);

/**
 * @brief Apply a new RX_ADDR_P1 address to the active RF receiver.
 *
 * No-op on RECEIVER_433M.
 *
 * @param addr 5-byte address, LSByte first on the wire
 * @param addr_len Must be 5
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_apply_rx_address(const uint8_t *addr, size_t addr_len);

/**
 * @brief Suspend the active RF receiver variant.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_suspend(void);

/**
 * @brief Resume a previously suspended RF receiver variant.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_resume(void);

/**
 * @brief Delete the active RF receiver variant.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_delete(void);

/**
 * @brief Report whether the RF supervisor currently owns a receiver instance.
 *
 * @return True when the receiver has been started
 */
bool rf_sup_is_started(void);

/**
 * @brief Report whether the receiver is currently suspended.
 *
 * @return True when started and suspended
 */
bool rf_sup_is_suspended(void);

#endif /* RF_SUPERVISOR_H */
