/**
 * @file rf_trigger.c
 * @brief RF Trigger Module - Match and Act Implementation
 *
 * Implements the mutex-protected RF trigger state that bridges decoded RF
 * frames from the receiver to vibrator pulses on matching codes.
 */

#include "trigger/rf_trigger.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define RF_TRIGGER_DEBOUNCE_US (1200 * 1000)

static SemaphoreHandle_t s_trigger_mutex;
static VibratorHandler *s_vibrator;
static rf_trigger_snapshot_t s_trigger;
static int64_t s_last_trigger_us;

esp_err_t rf_trigger_init(VibratorHandler *vibrator)
{
    if (!vibrator) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_trigger_mutex) {
        s_trigger_mutex = xSemaphoreCreateMutex();
        if (!s_trigger_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    s_vibrator = vibrator;
    rf_trigger_clear();
    ESP_LOGI(RF_TRIGGER_TAG, "RF trigger initialized");
    return ESP_OK;
}

void rf_trigger_deinit(void)
{
    rf_trigger_stop_output();
    if (s_trigger_mutex) {
        vSemaphoreDelete(s_trigger_mutex);
        s_trigger_mutex = NULL;
    }
    s_vibrator = NULL;
    s_trigger.code = 0;
    s_trigger.bits = 0;
    s_trigger.version = 0;
    s_last_trigger_us = 0;
}

void rf_trigger_stop_output(void)
{
    if (s_vibrator) {
        vibrator_set_pulsing(s_vibrator, false);
    }
    s_last_trigger_us = 0;
}

void rf_trigger_set(uint32_t code, uint8_t bits, uint32_t version)
{
    if (!s_trigger_mutex) {
        return;
    }

    xSemaphoreTake(s_trigger_mutex, portMAX_DELAY);
    s_trigger.code = code;
    s_trigger.bits = bits;
    s_trigger.version = version;
    xSemaphoreGive(s_trigger_mutex);
}

void rf_trigger_clear(void)
{
    rf_trigger_set(0, 0, 0);
}

bool rf_trigger_has_code(void)
{
    return rf_trigger_snapshot().bits != 0;
}

void rf_trigger_restore(uint32_t code, uint8_t bits, uint32_t version)
{
    if (bits == 0) {
        rf_trigger_clear();
        return;
    }

    ESP_LOGI(RF_TRIGGER_TAG, "Restored RF code: %u bits, version %u", bits, version);
    rf_trigger_set(code, bits, version);
}

rf_trigger_snapshot_t rf_trigger_snapshot(void)
{
    rf_trigger_snapshot_t snapshot = {0};

    if (!s_trigger_mutex) {
        return snapshot;
    }

    xSemaphoreTake(s_trigger_mutex, portMAX_DELAY);
    snapshot = s_trigger;
    xSemaphoreGive(s_trigger_mutex);

    return snapshot;
}

void rf_trigger_on_frame(uint32_t decoded, uint8_t decoded_bits, void *ctx)
{
    bool start;
    uint8_t action_bit;
    uint32_t code_only;
    uint32_t exp_masked;
    uint32_t mask;
    rf_trigger_snapshot_t snapshot = rf_trigger_snapshot();

    (void)ctx;

    if (!snapshot.bits || !s_vibrator) {
        return;
    }
    if (decoded_bits < snapshot.bits) {
        return;
    }

    action_bit = (decoded >> 31) & 1u;
    code_only = decoded & 0x7FFFFFFFu;
    exp_masked = snapshot.code & 0x7FFFFFFFu;
    mask = (snapshot.bits >= 32) ? 0x7FFFFFFFu : ((1u << snapshot.bits) - 1u);

    if ((code_only & mask) != (exp_masked & mask)) {
        return;
    }

    if (esp_timer_get_time() - s_last_trigger_us < RF_TRIGGER_DEBOUNCE_US) {
        return;
    }

    start = (action_bit == 0);
    ESP_LOGI(RF_TRIGGER_TAG, "RF match on %u bits, action=%s", decoded_bits, start ? "start" : "stop");
    if (vibrator_set_pulsing(s_vibrator, start) != ESP_OK) {
        ESP_LOGE(RF_TRIGGER_TAG, "Failed to set vibrator pulsing");
        return;
    }
    s_last_trigger_us = esp_timer_get_time();
}
