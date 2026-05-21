/**
 * @file serial_protocol.h
 * @brief Serial Provisioning Protocol - Interface
 *
 * Declares the synchronous blocking serial protocol used for USB provisioning
 * of the ESP-01 receiver module.  The protocol exchanges newline-delimited
 * JSON messages over UART0 at 115200 baud.
 */

#ifndef RECEIVER_SERIAL_PROTOCOL_H
#define RECEIVER_SERIAL_PROTOCOL_H

#include "config/device_config.h"
#include "esp_err.h"

typedef enum {
    SERIAL_PROV_RESULT_PROVISIONED,
    SERIAL_PROV_RESULT_RETRY,
    SERIAL_PROV_RESULT_RESET,
    SERIAL_PROV_RESULT_ERROR,
} serial_prov_result_t;

/**
 * @brief Initialize UART0 for the serial protocol.
 *
 * Configures 115200 8N1 and installs the UART driver with a 2048-byte RX
 * buffer.  Must be called once before serial_protocol_run_blocking().
 *
 * @return ESP_OK on success.
 */
esp_err_t serial_protocol_init(void);

/**
 * @brief Run the serial provisioning loop (blocking).
 *
 * Blocks the calling task, reading newline-delimited JSON commands from UART0.
 * Returns when a provisioning, factory-reset, or retry command completes
 * successfully, indicating the action the caller should take.
 *
 * @param cfg             Current device configuration (read-only, for identify).
 * @param recovery_reason Human-readable reason string shown in identify, or NULL.
 * @return The provisioning outcome that should drive the next main-loop action.
 */
serial_prov_result_t serial_protocol_run_blocking(const device_config_t *cfg,
                                                  const char *recovery_reason);

#endif
