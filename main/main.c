/**
 * @file main.c
 * @brief Receiver Application Entry Point
 *
 * Two-phase startup: NVS + config + vibrator init, then either ESP-NOW pairing
 * (if not yet paired) or direct RF listener bring-up.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "config/device_config.h"
#include "pair/espnow_pair.h"
#include "rf/rf_common.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "vibrator/vibrator.h"

#define TAG "APP"

RFHandler g_rf = { .rx_active = false, .rx_suspended = false, .rx_gpio = RF_GPIO_UNASSIGNED };
static device_config_t g_cfg = { .paired = false };
static VibratorHandler g_vibrator = { .vibrator_task_handle = NULL, .state_lock = NULL };

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init_or_recover());
    ESP_ERROR_CHECK(device_config_load(&g_cfg));

    if (!g_cfg.paired) {
        ESP_LOGI(TAG, "Not paired, entering ESP-NOW pair mode");
        ESP_ERROR_CHECK(vibrator_init((gpio_num_t)CONFIG_RECEIVER_VIBRATOR_GPIO, &g_vibrator));
        esp_err_t pulse_err = vibrator_pulse(&g_vibrator);
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_ERROR_CHECK(vibrator_deinit(&g_vibrator, VIBRATOR_DEINIT_HOLD_OFF));
        ESP_ERROR_CHECK(pulse_err);
        ESP_ERROR_CHECK(espnow_pair_wait(&g_cfg));
        ESP_LOGI(TAG, "Paired successfully");
    }

    ESP_ERROR_CHECK(vibrator_init((gpio_num_t)CONFIG_RECEIVER_VIBRATOR_GPIO, &g_vibrator));
    ESP_ERROR_CHECK(rf_trigger_init(&g_vibrator));
    rf_recv_set_frame_callback(rf_trigger_on_frame, NULL);
    rf_trigger_stop_output();

    if (g_cfg.paired && g_cfg.rf_code_bits > 0) {
        rf_trigger_restore(g_cfg.rf_code, g_cfg.rf_code_bits, 1);
    }

    ESP_ERROR_CHECK(rf_sup_start());
    ESP_LOGI(TAG, "433 MHz listener started, waiting for triggers");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
