/**
 * @file vibrator.h
 * @brief Vibrator Module - Interface and Configuration
 * 
 * This header defines the vibrator module interface and configuration
 * for the ESP8266 receiver.
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

#ifndef ESP_RETURN_ON_FALSE
#define ESP_RETURN_ON_FALSE(condition, err_code, tag, format, ...) \
    do { \
        if (!(condition)) { \
            ESP_LOGE(tag, format, ##__VA_ARGS__); \
            return err_code; \
        } \
    } while (0)
#endif

/**
 * @brief Vibrator handler structure
 * 
 * This structure contains the vibrator handler.
 */
typedef struct {
    gpio_num_t vibrator_gpio;          // Vibrator GPIO pin
    bool vibrator_active;              // Vibrator output is currently high
    bool vibrator_pulsing;             // Continuous pulsing flag
    bool vibrator_initialized;         // Vibrator initialized flag
    TaskHandle_t vibrator_task_handle; // Vibrator task handle
    SemaphoreHandle_t state_lock;      // Serializes task and public API state changes
} VibratorHandler;

/**
 * @brief Initialize the vibrator module
 * 
 * This function initializes the vibrator module.
 * 
 * @param vibrator_gpio Vibrator GPIO pin
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_init(gpio_num_t vibrator_gpio, VibratorHandler* vibrator_handler);

/**
 * @brief Deinitialize the vibrator module
 * 
 * This function deinitializes the vibrator module.
 * 
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_deinit(VibratorHandler* vibrator_handler);

/**
 * @brief Start the vibrator module
 * 
 * This function starts the vibrator module.
 * 
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_start(VibratorHandler* vibrator_handler);

/**
 * @brief Stop the vibrator module
 * 
 * This function stops the vibrator module.
 * 
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_stop(VibratorHandler* vibrator_handler);

/**
 * @brief Pulse the vibrator module
 * 
 * This function pulses the vibrator module.
 * 
 * @param vibrator_handler Vibrator handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t vibrator_pulse(VibratorHandler* vibrator_handler);
esp_err_t vibrator_set_pulsing(VibratorHandler* vibrator_handler, bool enabled);
esp_err_t vibrator_toggle_pulsing(VibratorHandler* vibrator_handler);

#endif
