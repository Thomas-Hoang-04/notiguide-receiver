/**
 * @file rf_supervisor.h
 * @brief Radio-variant dispatcher for receiver lifecycle operations.
 */

#ifndef RF_SUPERVISOR_H
#define RF_SUPERVISOR_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Start the compiled RF receiver variant.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_sup_start(void);

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
