/**
 *
 * @file rf_common.h
 * @brief RF Communication Module - Common Definitions and Interface
 *
 * This header defines the common interface for RF transmission and reception
 * functionality. It supports multiple RF protocols and provides both
 * transmitter and receiver capabilities using ESP32 GPIO and timer peripherals.
 */

#ifndef RF_TEST_H
#define RF_TEST_H

#include <stdbool.h>
#include <inttypes.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gptimer.h"

#define RF_TAG "RF"
#define RF_NVS_NAMESPACE "rf_storage"
#define RF_NVS_TRANSMIT_CODE_KEY "transmit_code"
#define RF_NVS_CHN_COUNT_KEY "chn_count"
#define RF_NVS_PROTO_ID_KEY "proto_id"
#define RF_NVS_RPT_KEY "repeat_count"

// Timer configuration constants
#define DEFAULT_RESOLUTION 1000000  // 1MHz timer resolution (1μs precision)
#define MAX_EDGES 67                // Maximum number of signal edges to capture
#define RECV_TOLERANCE 60           // Reception tolerance percentage (±60%)
#define SEPARATION_LIMIT 4300       // Minimum gap between transmissions (μs)
#define PROTO_COUNT 15             // Number of supported RF protocols

// Original code & channel count for the remote control
#define ORIGINAL_CODE 0x5555
#define CHN_COUNT 4               // Default channel count
#define CHN_COUNT_MAX 4           // Maximum channel count
#define PROTO_ID 1               // Default protocol index
#define TX_REPEAT_COUNT 10       // Default transmission repeat count per signal

// Tristate code for the remote control
extern char* tristate_code;

// Channel count for the remote control
extern uint8_t chn_count;

// Protocol index for the remote control
extern uint8_t proto_idx;

// Repeat count for the remote control
extern uint8_t repeat_count;

// 2-channel code mapping for the remote control
extern const char* chn_2[2];

// 3-channel code mapping for the remote control
extern const char* chn_3[3];

// 4-channel code mapping for the remote control
extern const char* chn_4[4];

// RF event queue
extern QueueHandle_t rf_trans_event_queue, rf_recv_data_queue;

// RF transmitter component changed flag
extern volatile uint8_t rf_cmp_changed;

typedef enum {
    TRANS_CHN_UP = 0xB0,
    TRANS_CHN_DOWN = 0xB3,
    TRANS_CHN_LOCK = 0xB1,
    TRANS_CHN_STOP = 0xB2,
    TRANS_RECOMPUTE_CHNS = 0xBA,
} rf_trans_event_t;

// Standard pulse lengths for different RF protocols (in microseconds)
enum {
    RC_SWITCH_1_PULSE_LEN = 350,    // RC Switch protocol pulse length
    COM_PULSE_LEN = 320,            // Common protocol pulse length
    FAST_PULSE_LEN = 240,           // Fast protocol pulse length
    FLASH_PULSE_LEN = 150,          // Flash protocol pulse length
};

// Repeat counts for different transmission modes
enum {
    RC_SWITCH_REPEAT_COUNT = 10,    // Standard RC switch repeat count
    OPT_REPEAT_COUNT = 5,           // Optimized repeat count
    FAST_REPEAT_COUNT = 4,          // Fast transmission repeat count
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
 * @brief RF Pulse Data Structure
 *
 * Represents a single RF pulse with its logic level and duration.
 */
typedef struct {
    uint8_t level;              // GPIO logic level (0 or 1)
    uint32_t pulse_length;      // Pulse duration in microseconds
} RFPulse;

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
 * Central structure containing all RF module state and configuration.
 * Manages both transmission and reception functionality.
 */
typedef struct {
    // Transmission pulse data
    RFPulse* pulses[4];           // Array of pulses to transmit
    size_t chn_count;           // Number of channels
    size_t pulse_count_per_chn; // Total number of pulses per channel
    uint8_t current_chn;        // Current channel being transmitted
    uint8_t current_pulse;      // Current pulse being transmitted

    // Transmission control
    uint8_t current_rep;       // Current repetition number
    uint8_t repeat_count;      // Total repetitions to send

    // Reception configuration and data
    uint32_t recv_value;       // Received data value
    uint8_t recv_bit_length;  // Number of received bits
    uint32_t recv_delay;       // Detected pulse delay
    uint32_t separation_limit; // Minimum gap between transmissions
    uint8_t recv_proto;        // Detected protocol index
    uint8_t recv_tolerance;    // Reception tolerance percentage
    uint32_t recv_timings[MAX_EDGES]; // Captured timing data

    // GPIO configuration
    bool tx_active;            // Transmission active flag
    bool rx_active, rx_suspended;   // Reception active flag
    gpio_num_t tx_gpio;        // Transmission GPIO pin
    gpio_num_t rx_gpio;        // Reception GPIO pin

    // Protocol and hardware handles
    Protocol* proto;           // Current transmission protocol
    TaskHandle_t rf_trans_handle;  // Transmission task handle
    TaskHandle_t rf_recv_handle;   // Reception task handle
    gptimer_handle_t timer;    // Hardware timer handle
} RFHandler;

// === RF Transmitter Functions ===

/**
 * @brief Initialize RF Transmitter
 *
 * Configures GPIO pin for RF transmission, sets up protocol parameters,
 * and initializes the hardware timer for precise pulse timing.
 *
 * @param tx_gpio GPIO pin number for RF transmission
 * @param repeat_count Number of times to repeat each transmission
 * @param tx_proto Pointer to RF protocol configuration
 * @param rf_rmt RF handler structure to initialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trans_init(gpio_num_t tx_gpio, int8_t repeat_count, Protocol* tx_proto, RFHandler* rf_rmt);

/**
 * @brief Deinitialize RF Transmitter
 *
 * Cleans up RF transmitter resources including pulse data, timer,
 * GPIO configuration, and receiver if active.
 *
 * @param rf_rmt RF handler structure to deinitialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trans_deinit(RFHandler* rf_rmt);

/**
 * @brief Suspend RF Transmitter
 *
 * Suspends the RF transmitter task and removes the ISR handler.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trans_suspend(RFHandler* rf_rmt);

/**
 * @brief Resume RF Transmitter
 *
 * Resumes the RF transmitter task and adds the ISR handler back.
 *
 * @param rf_rmt RF handler structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trans_resume(RFHandler* rf_rmt);

/**
 * @brief Start RF Signal Transmission
 *
 * Begins transmission of the prepared pulse sequence. Temporarily disables
 * the receiver if active, then starts the hardware timer to generate
 * precise pulse timing on the configured GPIO pin.
 *
 * @param rf_rmt RF handler with prepared pulse data
 * @param chn_idx Channel index to transmit
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_send(RFHandler* rf_rmt, uint8_t chn_idx);


/**
 * @brief Select RF Transmit Protocol
 *
 * Selects the RF protocol to use for transmission.
 *
 * @param rf_rmt RF handler structure
 * @param tx_proto Pointer to RF protocol configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_trans_proto_select(RFHandler* rf_rmt, Protocol* tx_proto);

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

// === RF Timer Functions ===

/**
 * @brief Initialize RF Timer
 *
 * Creates and configures a general purpose timer for RF pulse timing.
 * Sets up interrupt callback and enables the timer for operation.
 *
 * Timer configuration:
 * - 1MHz resolution (1μs precision)
 * - Count-up direction
 * - Alarm-based interrupts
 *
 * @param rf_rmt RF handler structure to store timer handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_timer_init(RFHandler* rf_rmt);

/**
 * @brief Deinitialize RF Timer
 *
 * Stops, disables, and deletes the RF timer, freeing associated resources.
 * Should be called during RF module cleanup.
 *
 * @param timer Timer handle to deinitialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_timer_deinit(gptimer_handle_t timer);

/**
 * @brief Reset RF Timer State
 *
 * Stops the timer, resets counter and transmission state variables.
 * Used to abort ongoing transmissions or prepare for new transmission.
 *
 * @param rf_rmt RF handler containing timer and state information
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rf_timer_reset(RFHandler* rf_rmt);

// === RF Data Handling Functions ===

/**
 * @brief Translate Tri-state Data to RF Pulse Sequence
 *
 * Converts a tri-state string (containing '0', '1', 'F' characters) into binary logic, which
 * is then converted into a sequence of RF pulses according to the specified protocol.
 *
 * The tri-state string is encoded as 4 pulses (2 pulse pairs) plus sync pulses.
 *
 * Tri-state encoding:
 * - '0': Double zero pulse pair
 * - '1': Double one pulse pair
 * - 'F': Zero pulse pair + One pulse pair (floating/unknown)
 *
 * @param data Tri-state string to encode
 * @param pulses Pointer to array of RFPulse structures to store the result
 * @param pulse_count_per_chn Pointer to variable to store the number of pulses per channel
 * @param proto Pointer to RF protocol configuration
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on invalid input
 *
 * @note The pulse array will be cleaned and reallocated within this function.
 * @note The pulse_count_per_chn will be set to the number of pulses per channel.
 */
esp_err_t tristate_to_pulses(const char* data, RFPulse** pulses, size_t* pulse_count_per_chn, Protocol* proto);

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

/**
 * @brief Translate Tri-state String to Raw Code (Integer)
 *
 * Converts a tri-state string into a raw code (integer) representation.
 * The raw code is constructed by mapping each tri-state character
 * to a binary pair ('00', '01' or '11').
 *
 * @param tristate_code Tri-state string to translate
 * @param raw_code Raw code to store the result
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid data is detected,
 * @return ESP_ERR_NO_MEM if memory allocation fails, error code otherwise
 *
 */
esp_err_t tristate_to_uint32(const char* tristate_code, uint32_t* raw_code);

/**
 * @brief Load RF Transmit Channel Code
 *
 * Loads the RF codes for each channel from data within NVS storage.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t load_rf_chn_pulses(RFHandler* rf_rmt);

#endif // RF_TEST_H
