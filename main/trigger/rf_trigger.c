/**
 * @file rf_trigger.c
 * @brief Trigger matcher shared by the 433 MHz and nRF24 receivers.
 */

#include "trigger/rf_trigger.h"
#include <stdint.h>
#include <string.h>
#include "config/device_config.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vibrator/vibrator.h"

#define RF_TRIGGER_TAG "RF_TRIGGER"

typedef struct {
    uint8_t code[DEVICE_CONFIG_MAX_RF_CODE_LEN];
    size_t code_len;
    uint8_t bits;
    uint32_t version;
    bool initialized;
} rf_trigger_state_t;

static SemaphoreHandle_t s_mutex;
static VibratorHandler s_vibrator;
static rf_trigger_state_t s_trigger;

static bool copy_state(rf_trigger_state_t *out)
{
    if (s_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    *out = s_trigger;
    xSemaphoreGive(s_mutex);
    return true;
}

esp_err_t rf_trigger_init(gpio_num_t vibrator_gpio)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, RF_TRIGGER_TAG,
                            "failed to create trigger mutex");
    }

    if (s_trigger.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(vibrator_init(vibrator_gpio, &s_vibrator), RF_TRIGGER_TAG,
                        "failed to initialize vibrator");
    s_trigger.initialized = true;
    return ESP_OK;
}

esp_err_t rf_trigger_deinit(void)
{
    if (!s_trigger.initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(vibrator_deinit(&s_vibrator), RF_TRIGGER_TAG,
                        "failed to deinitialize vibrator");
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    memset(&s_trigger, 0, sizeof(s_trigger));
    return ESP_OK;
}

esp_err_t rf_trigger_set(const uint8_t *code, size_t code_len, uint8_t bits, uint32_t version)
{
    ESP_RETURN_ON_FALSE(code != NULL, ESP_ERR_INVALID_ARG, RF_TRIGGER_TAG, "code is NULL");
    ESP_RETURN_ON_FALSE(code_len > 0 && code_len <= sizeof(s_trigger.code), ESP_ERR_INVALID_ARG,
                        RF_TRIGGER_TAG, "invalid code length");
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_INVALID_STATE, RF_TRIGGER_TAG,
                        "trigger not initialized");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_trigger.code, 0, sizeof(s_trigger.code));
    memcpy(s_trigger.code, code, code_len);
    s_trigger.code_len = code_len;
    s_trigger.bits = bits;
    s_trigger.version = version;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void rf_trigger_restore_from_cfg(const device_config_t *cfg)
{
    if (cfg == NULL || !device_config_has_rf_code(cfg) || s_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(s_trigger.code, 0, sizeof(s_trigger.code));
    memcpy(s_trigger.code, cfg->rf_code, cfg->rf_code_len);
    s_trigger.code_len = cfg->rf_code_len;
    s_trigger.bits = cfg->rf_code_bits;
    s_trigger.version = cfg->rf_code_ver;
    xSemaphoreGive(s_mutex);
}

bool rf_trigger_has_code(void)
{
    if (s_mutex == NULL) {
        return false;
    }

    bool has_code = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    has_code = s_trigger.code_len > 0 && s_trigger.bits > 0;
    xSemaphoreGive(s_mutex);
    return has_code;
}

esp_err_t rf_trigger_stop_output(void)
{
    if (!s_trigger.initialized) {
        return ESP_OK;
    }
    return vibrator_set_pulsing(&s_vibrator, false);
}

void rf_trigger_on_frame(uint32_t value, uint8_t value_bits)
{
    rf_trigger_state_t state = { 0 };
    if (!copy_state(&state) || !state.initialized || state.bits == 0 || state.bits > 32) {
        return;
    }
    if (value_bits < state.bits) {
        return;
    }

    uint32_t expected = (uint32_t)state.code[0]
                      | ((uint32_t)state.code[1] << 8)
                      | ((uint32_t)state.code[2] << 16)
                      | ((uint32_t)state.code[3] << 24);
    uint32_t mask = state.bits >= 32 ? UINT32_MAX : ((1UL << state.bits) - 1UL);

    if ((value & mask) == (expected & mask)) {
        vibrator_toggle_pulsing(&s_vibrator);
    }
}

void rf_trigger_on_packet(const uint8_t *pkt, size_t pkt_len)
{
    rf_trigger_state_t state = { 0 };
    if (pkt == NULL || !copy_state(&state) || !state.initialized || state.bits == 0) {
        return;
    }
    if ((state.bits % 8U) != 0 || pkt_len < state.code_len) {
        return;
    }

    if (memcmp(pkt, state.code, state.code_len) == 0) {
        vibrator_toggle_pulsing(&s_vibrator);
    }
}
