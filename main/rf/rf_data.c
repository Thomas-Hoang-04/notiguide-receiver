/**
 * @file rf_data.c
 * @brief RF Data Conversion Functions
 *
 * Implements functions for converting between different data formats,
 * including tri-state data, binary data, and RF pulse sequences.
 */

#include <stdlib.h>
#include <string.h>
#include "rf_common.h"
#include "esp_log.h"
#include "esp_check.h"

char* tristate_code = NULL;

uint8_t chn_count, proto_idx, repeat_count;

const char* chn_2[2] = {"01", "10"};
const char* chn_3[3] = {"001", "010", "100"};
const char* chn_4[4] = {"0001", "0010", "0100", "1000"};

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

esp_err_t tristate_to_pulses(const char* data, RFPulse** pulses, size_t* pulse_count_per_chn, Protocol* proto) {
    ESP_RETURN_ON_FALSE(proto, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid protocol");
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid transmit code");

    const char* log_data = data;

    // Free any existing pulse data
    if (*pulses) {
        free(*pulses);
        *pulses = NULL;
    }

    // Calculate total pulses needed: 4 pulses per character + 2 sync pulses
    *pulse_count_per_chn = strlen(data) * 4 + 2;
    *pulses = malloc(*pulse_count_per_chn * sizeof(RFPulse));
    int idx = 0;

    // Determine logic levels based on protocol inversion setting
    uint8_t firstLogic = proto->inverted ? 0 : 1;
    uint8_t secondLogic = proto->inverted ? 1 : 0;

    // Process each character in the tri-state string
    while (*data) {
        if (*data == '0') {
            // Encode logic '0' as two identical zero pulse pairs
            (*pulses)[idx].level = firstLogic;
            (*pulses)[idx].pulse_length = proto->zero.high * proto->pulse_length;
            (*pulses)[idx + 1].level = secondLogic;
            (*pulses)[idx + 1].pulse_length = proto->zero.low * proto->pulse_length;

            (*pulses)[idx + 2].level = firstLogic;
            (*pulses)[idx + 2].pulse_length = proto->zero.high * proto->pulse_length;
            (*pulses)[idx + 3].level = secondLogic;
            (*pulses)[idx + 3].pulse_length = proto->zero.low * proto->pulse_length;
        } else if (*data == '1') {
            // Encode logic '1' as two identical one pulse pairs
            (*pulses)[idx].level = firstLogic;
            (*pulses)[idx].pulse_length = proto->one.high * proto->pulse_length;
            (*pulses)[idx + 1].level = secondLogic;
            (*pulses)[idx + 1].pulse_length = proto->one.low * proto->pulse_length;

            (*pulses)[idx + 2].level = firstLogic;
            (*pulses)[idx + 2].pulse_length = proto->one.high * proto->pulse_length;
            (*pulses)[idx + 3].level = secondLogic;
            (*pulses)[idx + 3].pulse_length = proto->one.low * proto->pulse_length;
        } else if (*data == 'F') {
            // Encode 'F' (floating) as zero pulse pair + one pulse pair
            (*pulses)[idx].level = firstLogic;
            (*pulses)[idx].pulse_length = proto->zero.high * proto->pulse_length;
            (*pulses)[idx + 1].level = secondLogic;
            (*pulses)[idx + 1].pulse_length = proto->zero.low * proto->pulse_length;

            (*pulses)[idx + 2].level = firstLogic;
            (*pulses)[idx + 2].pulse_length = proto->one.high * proto->pulse_length;
            (*pulses)[idx + 3].level = secondLogic;
            (*pulses)[idx + 3].pulse_length = proto->one.low * proto->pulse_length;
        } else {
            ESP_LOGE(RF_TAG, "Invalid data: %c", *data);
            return ESP_ERR_INVALID_ARG;
        }
        idx += 4;
        data++;
    }

    // Add synchronization pulses at the end
    (*pulses)[idx].level = firstLogic;
    (*pulses)[idx].pulse_length = proto->sync_factor.high * proto->pulse_length;
    (*pulses)[idx + 1].level = secondLogic;
    (*pulses)[idx + 1].pulse_length = proto->sync_factor.low * proto->pulse_length;

    ESP_LOGI(RF_TAG, "Data %s translated to %d pulses", log_data, *pulse_count_per_chn);

    return ESP_OK;
}

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

esp_err_t tristate_to_uint32(const char* tristate_code, uint32_t* raw_code) {
    ESP_RETURN_ON_FALSE(tristate_code, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid tri-state code");
    ESP_RETURN_ON_FALSE(raw_code, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid integer code pointer");

    uint32_t value = 0;

    // Process from right to left (LSB first)
    for (uint8_t pos = 0; tristate_code[pos]; pos++) {
        value <<= 2;
        if (tristate_code[pos] == '0')
            value |= 0b00;
        else if (tristate_code[pos] == 'F')
            value |= 0b01;
        else if (tristate_code[pos] == '1')
            value |= 0b11;
        else {
            ESP_LOGE(RF_TAG, "Invalid data: %c", tristate_code[pos]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    *raw_code = value;
    return ESP_OK;
}

esp_err_t load_rf_chn_pulses(RFHandler* rf_rmt) {
    static const uint8_t code_len = 16;
    char chn_code[code_len];
    rf_rmt->chn_count = chn_count;
    for (int i = 0; i < chn_count; i++) {
        snprintf(chn_code, code_len, "%s%s", tristate_code, chn_count == 2 ? chn_2[i] : (chn_count == 3 ? chn_3[i] : chn_4[i]));
        ESP_LOGI(RF_TAG, "Channel %d code: %s", i + 1, chn_code);
        tristate_to_pulses(chn_code, &rf_rmt->pulses[i], &rf_rmt->pulse_count_per_chn, rf_rmt->proto);
        ESP_LOGI(RF_TAG, "Pulse loaded for channel %d. Pulse count: %u", i + 1, rf_rmt->pulse_count_per_chn);
    }
    return ESP_OK;
}