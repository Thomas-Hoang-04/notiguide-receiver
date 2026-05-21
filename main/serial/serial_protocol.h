/**
 * @file serial_protocol.h
 * @brief USB Serial/JTAG JSON-line protocol for provisioning and diagnostics.
 */

#ifndef RECEIVER_SERIAL_PROTOCOL_H
#define RECEIVER_SERIAL_PROTOCOL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the USB serial protocol driver and start the read-loop task.
 *
 * Installs the USB Serial/JTAG driver with TX/RX ring buffers, binds VFS
 * so log output routes through the driver, and spawns the protocol task.
 *
 * @return ESP_OK on success.
 */
esp_err_t serial_protocol_init(void);

/**
 * @brief Stop the serial protocol task.
 */
void serial_protocol_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RECEIVER_SERIAL_PROTOCOL_H */
