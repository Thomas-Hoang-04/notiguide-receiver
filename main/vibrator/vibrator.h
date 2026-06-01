/**
 * @file vibrator.h
 * @brief Vibrator output control interface.
 */

#ifndef VIBRATOR_H
#define VIBRATOR_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define VIBRATOR_TAG "VIBRATOR"

#define VIBRATOR_ON 1
#define VIBRATOR_OFF 0
#define VIBRATOR_PULSE_MS 250
#define VIBRATOR_GAP_MS 250

/**
 * @brief Vibrator runtime state container.
 */
typedef struct {
    gpio_num_t vibrator_gpio;         // Vibrator GPIO pin
    bool vibrator_active;             // Vibrator output is currently high
    bool vibrator_pulsing;            // Continuous pulsing flag
    bool vibrator_initialized;        // Vibrator initialized flag
    TaskHandle_t vibrator_task_handle; // Vibrator task handle
    SemaphoreHandle_t state_lock;     // Serializes task and public API state changes
} VibratorHandler;

typedef enum {
    VIBRATOR_DEINIT_HOLD_OFF = 0,     // Keep GPIO configured output-low
    VIBRATOR_DEINIT_RESET_PIN,        // Force low, then release GPIO to reset state
} VibratorDeinitMode;

/**
 * @brief Initialize the vibrator GPIO and worker task.
 *
 * @param vibrator_gpio Vibrator GPIO pin
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_init(gpio_num_t vibrator_gpio, VibratorHandler* vibrator_handler);

/**
 * @brief Deinitialize the vibrator GPIO and worker task.
 *
 * @param vibrator_handler Vibrator handler structure
 * @param mode Whether to keep GPIO output-low or reset/release the pin
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_deinit(VibratorHandler* vibrator_handler, VibratorDeinitMode mode);

/**
 * @brief Drive the vibrator output high immediately.
 *
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_start(VibratorHandler* vibrator_handler);

/**
 * @brief Drive the vibrator output low and stop any pulsing.
 *
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_stop(VibratorHandler* vibrator_handler);

/**
 * @brief Emit one fixed-length pulse when continuous pulsing is inactive.
 *
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_pulse(VibratorHandler* vibrator_handler);

/**
 * @brief Enable or disable continuous pulsing.
 *
 * @param vibrator_handler Vibrator handler structure
 * @param enabled True to enable pulsing, false to disable pulsing
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_set_pulsing(VibratorHandler* vibrator_handler, bool enabled);

#endif
