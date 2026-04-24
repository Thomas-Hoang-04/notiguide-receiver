/**
 * @file rf_data.c
 * @brief RF protocol table and receiver-side conversion helpers.
 */

#include "rf_common.h"
#include "sdkconfig.h"
#include <stdint.h>

#if CONFIG_RECEIVER_RADIO_433M

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"

const DRAM_ATTR Protocol proto[] = {
    // Protocol 1: rc-switch Protocol 1 (350μs base)
    { RC_SWITCH_1_PULSE_LEN, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },
    // Protocol 2: Moderate timing protocol (320μs base)
    { COM_PULSE_LEN, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },
    // Protocol 3: Fast timing protocol (240μs base)
    { FAST_PULSE_LEN, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },
    // Protocol 4: Flash timing protocol (150μs base)
    { FLASH_PULSE_LEN, {  1, 31 }, {  1,  3 }, {  3,  1 }, false },
    // Protocol 5: Standard 650μs protocol
    { 650, {  1, 10 }, {  1,  2 }, {  2,  1 }, false },
    // Protocol 6: Short pulse protocol (100μs base)
    { 100, { 30, 71 }, {  4, 11 }, {  9,  6 }, false },
    // Protocol 7: Medium pulse protocol (380μs base)
    { 380, {  1,  6 }, {  1,  3 }, {  3,  1 }, false },
    // Protocol 8: Long pulse protocol (500μs base)
    { 500, {  6, 14 }, {  1,  2 }, {  2,  1 }, false },
    // Protocol 9: HT6P20B chip protocol (inverted)
    { 450, { 23,  1 }, {  1,  2 }, {  2,  1 }, true },
    // Protocol 10: HS2303-PT (AUKEY Remote) protocol
    { 150, {  2, 62 }, {  1,  6 }, {  6,  1 }, false },
    // Protocol 11: Conrad RS-200 RX protocol
    { 200, {  3, 130}, {  7, 16 }, {  3,  16}, false },
    // Protocol 12: Conrad RS-200 TX protocol (
    { 200, { 130, 7 }, {  16, 7 }, { 16,  3 }, true },
    // Protocol 13: 1ByOne Doorbell protocol (inverted)
    { 365, { 18,  1 }, {  3,  1 }, {  1,  3 }, true },
    // Protocol 14: HT12E chip protocol (inverted)
    { 270, { 36,  1 }, {  1,  2 }, {  2,  1 }, true },
    // Protocol 15: SM5212 chip protocol (inverted)
    { 320, { 36,  1 }, {  1,  2 }, {  2,  1 }, true }
};

esp_err_t uint32_to_binary(uint32_t raw_code, char** binary_code, uint8_t bit_length) {
    if (*binary_code) {
        free(*binary_code);
        *binary_code = NULL;
    }

    static char buffer[64];
    memset(buffer, 0, sizeof(buffer));
    uint8_t pos = 0;

    // Convert raw value to binary string (LSB first)
    while (raw_code) {
        buffer[32 + pos] = (raw_code & 1) ? '1' : '0';
        raw_code >>= 1;
        pos++;
    }

    // Reverse and pad binary string to correct length
    for (uint8_t j = 0; j < bit_length; j++) {
        if (j >= bit_length - pos) buffer[j] = buffer[31 - j + bit_length];
        else buffer[j] = '0';
    }
    buffer[bit_length] = '\0';

    // Allocate and copy binary representation
    *binary_code = strdup(buffer);
    if (!*binary_code) {
        ESP_LOGE(RF_TAG, "Memory allocation failed for binary data");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t uint32_to_tristate(uint32_t raw_code, char** tristate_code, uint8_t bit_length) {
    if (*tristate_code) {
        free(*tristate_code);
        *tristate_code = NULL;
    }

    char buffer[bit_length / 2 + 1];
    memset(buffer, 0, sizeof(buffer));
    uint8_t sect, pos = 0;

    for (int i = bit_length - 2; i >= 0; i -= 2) {
        sect = (raw_code >> i) & 0x3;
        if (sect == 0) buffer[pos] = '0';
        else if (sect == 1) buffer[pos] = 'F';
        else if (sect == 3) buffer[pos] = '1';
        else {
            ESP_LOGE(RF_TAG, "Invalid data: %d", sect);
            return ESP_ERR_INVALID_ARG;
        }
        pos++;
    }
    buffer[pos] = '\0';

    *tristate_code = strdup(buffer);
    if (!*tristate_code) {
        ESP_LOGE(RF_TAG, "Memory allocation failed for tri-state data");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else

esp_err_t uint32_to_binary(uint32_t raw_code, char** binary_code, uint8_t bit_length) {
    (void)raw_code;
    (void)binary_code;
    (void)bit_length;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t uint32_to_tristate(uint32_t raw_code, char** tristate_code, uint8_t bit_length) {
    (void)raw_code;
    (void)tristate_code;
    (void)bit_length;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
