/**
 * @file main.c
 * @brief Receiver firmware boot: pair if needed, then RF listen + vibrate.
 */

#include "config/device_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pair/espnow_pair.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "vibrator/vibrator.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "APP"

static device_config_t g_cfg;

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init_or_recover());
    ESP_ERROR_CHECK(device_config_load(&g_cfg));

    if (!g_cfg.paired) {
        ESP_LOGI(TAG, "Not paired, entering ESP-NOW pair mode");

        VibratorHandler pair_vib = { 0 };
        if (vibrator_init((gpio_num_t)CONFIG_RECEIVER_VIBRATOR_GPIO, &pair_vib) == ESP_OK) {
            vibrator_pulse(&pair_vib);
            vTaskDelay(pdMS_TO_TICKS(500));
            vibrator_deinit(&pair_vib);
        }

        ESP_ERROR_CHECK(espnow_pair_wait(&g_cfg));
        ESP_LOGI(TAG, "Paired successfully, continuing to RF listen");
    }

    ESP_ERROR_CHECK(rf_trigger_init((gpio_num_t)CONFIG_RECEIVER_VIBRATOR_GPIO));
    rf_trigger_stop_output();

    if (device_config_has_rf_code(&g_cfg)) {
        ESP_ERROR_CHECK(rf_trigger_set(g_cfg.rf_code, g_cfg.rf_code_len,
                                       g_cfg.rf_code_bits, g_cfg.rf_code_ver));
    }

    ESP_ERROR_CHECK(rf_sup_start(&g_cfg));
    ESP_LOGI(TAG, "RF listener started, waiting for triggers");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
