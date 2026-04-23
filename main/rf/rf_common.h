/**
 * @file rf_common.h
 * @brief RF receiver definitions and decode helpers.
 */

#ifndef RF_TEST_H
#define RF_TEST_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h" 
#include "freertos/task.h"

#define RF_TAG "RF"
#define MAX_EDGES 67
#define RECV_TOLERANCE 60
#define SEPARATION_LIMIT 4300
#define PROTO_COUNT 15

// Standard pulse lengths for supported RF protocols (in microseconds)
enum {
    RC_SWITCH_1_PULSE_LEN = 350,
    COM_PULSE_LEN = 320,
    FAST_PULSE_LEN = 240,
    FLASH_PULSE_LEN = 150,
};

/**
 * @brief RF Protocol Timing Structure
 *
 * Defines the high and low timing ratios for RF signal encoding.
 * These ratios are multiplied by the protocol's pulse_length to get actual timing.
 */
typedef struct {
    uint8_t high;   // High signal duration ratio
    uint8_t low;    // Low signal duration ratio
} RFTicks;

/**
 * @brief RF Protocol Definition
 *
 * Complete definition of an RF protocol including timing parameters
 * and signal inversion settings.
 */
typedef struct {
    uint16_t pulse_length;      // Base pulse length in microseconds
    RFTicks sync_factor;        // Synchronization pulse timing ratios
    RFTicks zero;              // Logic '0' bit timing ratios
    RFTicks one;               // Logic '1' bit timing ratios
    bool inverted;             // Signal inversion flag
} Protocol;

/**
 * @brief Predefined RF Protocols
 *
 * Array of supported RF protocols with their timing parameters.
 */
extern const Protocol proto[];

/**
 * @brief RF Reception Data Structure
 *
 * Contains decoded RF reception data in multiple formats.
 */
typedef struct {
    uint32_t original_value;    // Raw received value
    char* tri_state;           // Tri-state representation (0, 1, F)
    char* binary;              // Binary representation (0, 1)
} RFRecvData;

/**
 * @brief RF Handler Main Structure
 *
 * Central structure containing RF receiver state.
 */
typedef struct {
    uint32_t recv_value;       // Received data value
    uint8_t recv_bit_length;   // Number of received bits
    uint32_t recv_delay;       // Detected pulse delay
    uint32_t separation_limit; // Minimum gap between transmissions
    uint8_t recv_proto;        // Detected protocol index
    uint8_t recv_tolerance;    // Reception tolerance percentage
    uint32_t recv_timings[MAX_EDGES]; // Captured timing data

    bool rx_active;
    bool rx_suspended;
    bool recv_pending;
    gpio_num_t rx_gpio;
    TaskHandle_t rf_recv_handle;
} RFHandler;

// === RF Receiver Functions ===

/**
 * @brief Initialize RF Receiver
 *
 * Configures GPIO pin for RF reception with interrupt-driven edge detection.
 * Sets up ISR service and initializes reception parameters.
 *
 * @param rx_gpio GPIO pin number for RF reception
 * @param rf_rmt RF handler structure to initialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_recv_init(gpio_num_t rx_gpio, RFHandler* rf_rmt);

/**
 * @brief Deinitialize RF Receiver
 *
 * Removes ISR handler, resets GPIO configuration, and cleans up receiver state.
 *
 * @param rf_rmt RF handler structure to deinitialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_recv_deinit(RFHandler* rf_rmt);

/**
 * @brief Suspend RF Receiver
 *
 * Suspends the RF receiver task and removes the ISR handler.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_recv_suspend(RFHandler* rf_rmt);

/**
 * @brief Resume RF Receiver
 *
 * Resumes the RF receiver task and adds the ISR handler back.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_recv_resume(RFHandler* rf_rmt);

/**
 * @brief Start the 433 MHz receiver task.
 *
 * Initializes the GPIO/ISR path if needed and spawns the receiver task that
 * waits on notifications before forwarding decoded frames into the shared
 * trigger matcher.
 *
 * @param rx_gpio GPIO pin number for RF reception
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_recv_start_task(gpio_num_t rx_gpio, RFHandler* rf_rmt);

/**
 * @brief Reset Reception Buffer
 *
 * Clears received data to prepare for next reception.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t reset_recv(RFHandler* rf_rmt);

/**
 * @brief Output Received RF Data
 *
 * Decodes and logs received RF data in multiple formats for debugging
 * and analysis purposes.
 *
 * @param rf_rmt RF handler containing received data
 * @param recv_data RF reception data structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t output_recv(RFHandler* rf_rmt, RFRecvData* recv_data);

// === RF Data Handling Functions ===

/**
 * @brief Translate Raw Code (Integer) to Binary String
 *
 * Converts a raw code (integer) into a binary string representation.
 * The binary string is stored in the provided buffer and is null-terminated.
 *
 * The binary string is constructed by converting the raw code to binary,
 * reversing the order of the bits, and padding with zeros to the specified length.
 *
 * @param raw_code Raw code to translate
 * @param binary_code Binary string to store the result
 * @param bit_length Number of bits to translate
 * @return ESP_OK on success, ESP_ERR_NO_MEM if memory allocation fails, error code otherwise
 *
 * @note The string pointer for the binary code will be cleaned and reallocated within this function
 */
esp_err_t uint32_to_binary(uint32_t raw_code, char** binary_code, uint8_t bit_length);

/**
 * @brief Translate Raw Code (Integer) to Tri-state String
 *
 * Converts a raw code (integer) into a tri-state string representation.
 * The tri-state string is constructed by mapping each pair of bits to a tri-state character ('0', '1', or 'F').
 *
 * @param raw_code Raw code to translate
 * @param tristate_code Tri-state string to store the result
 * @param bit_length Number of bits to translate
 * @return ESP_OK on success, ESP_ERR_NO_MEM if memory allocation fails, error code otherwise
 *
 * @note The string pointer for the tri-state code will be cleaned and reallocated within this function
 */
esp_err_t uint32_to_tristate(uint32_t raw_code, char** tristate_code, uint8_t bit_length);

#endif // RF_TEST_H
