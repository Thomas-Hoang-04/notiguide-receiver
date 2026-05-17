/**
 *
 * @file rf_common.h
 * @brief RF Receiver Module - Common Definitions and Interface
 *
 * This header defines the receive-side interface and shared decoding helpers
 * for the ESP8266 RF receiver.
 */

#ifndef RF_TEST_H
#define RF_TEST_H

#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RF_TAG "RF"

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
 * @brief RF GPIO unassigned value
 * 
 * The GPIO number for the unassigned RF GPIO.
 */
#define RF_GPIO_UNASSIGNED GPIO_NUM_MAX

#define MAX_EDGES 67                // Maximum number of signal edges to capture
#define RECV_TOLERANCE 60           // Reception tolerance percentage (±60%)
#define SEPARATION_LIMIT 4300       // Minimum gap between transmissions (μs)
#define PROTO_COUNT 15              // Number of supported RF protocols

// Standard pulse lengths for different RF protocols (in microseconds)
enum {
    RC_SWITCH_1_PULSE_LEN = 350,    // RC Switch protocol pulse length
    COM_PULSE_LEN = 320,            // Common protocol pulse length
    FAST_PULSE_LEN = 240,           // Fast protocol pulse length
    FLASH_PULSE_LEN = 150,          // Flash protocol pulse length
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
 * Array of commonly used RF protocols with their timing parameters.
 * Stored in DRAM for fast ISR access.
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
 * Central structure containing receiver state and configuration.
 */
typedef struct {
    // Reception configuration and data
    uint32_t recv_value;       // Received data value
    uint8_t recv_bit_length;  // Number of received bits
    uint32_t recv_delay;       // Detected pulse delay
    uint32_t separation_limit; // Minimum gap between transmissions
    uint8_t recv_proto;        // Detected protocol index
    uint8_t recv_tolerance;    // Reception tolerance percentage
    uint32_t recv_timings[MAX_EDGES]; // Captured timing data

    // GPIO configuration
    bool rx_active, rx_suspended;   // Reception active flags
    bool recv_pending;              // Frame decoded and awaiting processing
    gpio_num_t rx_gpio;             // Reception GPIO pin

    // Receiver task handle
    TaskHandle_t rf_recv_handle;    // Reception task handle
} RFHandler;

typedef void (*rf_frame_callback_t)(uint32_t decoded, uint8_t decoded_bits, void *ctx);

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
esp_err_t rf_recv_start_task(gpio_num_t rx_gpio, RFHandler* rf_rmt);
void rf_recv_set_frame_callback(rf_frame_callback_t callback, void *ctx);

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
 * @brief Check if RF Data is Available
 *
 * Tests whether valid RF data has been received and decoded.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK if data available, ESP_ERR_NOT_FOUND if no data, error code on failure
 */
esp_err_t recv_available(RFHandler* rf_rmt);

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
