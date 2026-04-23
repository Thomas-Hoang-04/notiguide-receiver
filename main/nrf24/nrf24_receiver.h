/**
 * @file nrf24_receiver.h
 * @brief Native ESP-IDF nRF24L01/L01+ receiver lifecycle API.
 */

#ifndef NRF24_RECEIVER_H
#define NRF24_RECEIVER_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    NRF_CHIP_UNKNOWN = 0,
    NRF_CHIP_LEGACY,
    NRF_CHIP_PLUS,
} nrf24_variant_t;

typedef struct {
    bool rx_active;
    bool rx_suspended;
    TaskHandle_t task;
    void *spi;
    nrf24_variant_t chip;
} nrf24_handle_t;

/**
 * @brief Initialize the nRF24 SPI/IRQ path and start the RX task.
 *
 * @param handle Receiver state structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nrf24_recv_start_task(nrf24_handle_t *handle);

/**
 * @brief Suspend the nRF24 receiver and mask IRQ delivery.
 *
 * @param handle Receiver state structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nrf24_recv_suspend(nrf24_handle_t *handle);

/**
 * @brief Resume a previously suspended nRF24 receiver.
 *
 * @param handle Receiver state structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nrf24_recv_resume(nrf24_handle_t *handle);

/**
 * @brief Stop and deinitialize the nRF24 receiver.
 *
 * @param handle Receiver state structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nrf24_recv_deinit(nrf24_handle_t *handle);

#endif /* NRF24_RECEIVER_H */
