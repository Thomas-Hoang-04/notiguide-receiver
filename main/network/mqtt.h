/**
 * @file mqtt.h
 * @brief MQTT bootstrap and operational command handling.
 */

#ifndef RECEIVER_MQTT_H
#define RECEIVER_MQTT_H

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "config/device_config.h"
#include "security/device_identity.h"

/**
 * @brief Start the MQTT client for the current activation phase.
 *
 * @param cfg Provisioned device configuration
 * @param identity Device identity context
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_start(device_config_t *cfg, device_identity_t *identity);

/**
 * @brief Stop the current MQTT client instance.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_stop(void);

/**
 * @brief Execute the bootstrap activation exchange.
 *
 * @param cfg Provisioned device configuration
 * @param identity Device identity context
 * @param registration_nonce Session-scoped bootstrap nonce
 * @param timeout_ticks Maximum time to wait for completion
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t mqtt_bootstrap_activate(device_config_t *cfg,
                                  device_identity_t *identity,
                                  const char *registration_nonce,
                                  TickType_t timeout_ticks);

/**
 * @brief Report whether the MQTT session is currently connected.
 *
 * @return True when the broker session is up
 */
bool mqtt_is_connected(void);

#endif /* RECEIVER_MQTT_H */
