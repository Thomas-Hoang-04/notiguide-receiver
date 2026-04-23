/**
 * @file main.c
 * @brief Receiver Application Entry Point and Orchestration
 *
 * Coordinates provisioning, activation, RF trigger restoration, and steady-
 * state receiver operation for the ESP-01 firmware.
 */

#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config/device_config.h"
#include "network/mqtt.h"
#include "network/wifi.h"
#include "provision/recovery.h"
#include "rf/rf_common.h"
#include "security/device_identity.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "vibrator/vibrator.h"

#ifndef PROJECT_VER
#define PROJECT_VER "v1"
#endif

#define VIBRATOR_GPIO GPIO_NUM_2

RFHandler g_rf = {0};
static VibratorHandler s_vibrator = {0};
static device_identity_t s_identity = {0};

void app_main(void)
{
    device_config_t cfg;
    esp_err_t err;

    device_config_init(&cfg);

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(vibrator_init(VIBRATOR_GPIO, &s_vibrator));
    ESP_ERROR_CHECK(rf_trigger_init(&s_vibrator));
    rf_recv_set_frame_callback(rf_trigger_on_frame, NULL);

    ESP_ERROR_CHECK(device_config_load(&cfg));
    if (cfg.has_rf_code && cfg.rf_code_bits > 0) {
        rf_trigger_restore(cfg.rf_code, cfg.rf_code_bits, cfg.rf_code_ver);
    }

    if (!device_config_is_provisioned(&cfg)) {
        provision_run_recovery_mode();
    }

    ESP_ERROR_CHECK(wifi_init());
    if (wifi_start_sta(cfg.wifi_ssid, cfg.wifi_pwd) != ESP_OK) {
        device_config_free(&cfg);
        provision_run_recovery_mode();
    }

    ESP_ERROR_CHECK(device_identity_init(&s_identity));
    ESP_ERROR_CHECK(mqtt_receiver_init(&s_identity, PROJECT_VER));
    if (mqtt_receiver_start(&cfg) != ESP_OK) {
        device_config_free(&cfg);
        provision_run_recovery_mode();
    }

    if (!cfg.has_public_id || !cfg.has_op_state) {
        if (!cfg.has_enroll_token) {
            device_config_free(&cfg);
            provision_run_recovery_mode();
        }

        if (mqtt_receiver_bootstrap_activate(&cfg) != ESP_OK) {
            device_config_free(&cfg);
            provision_run_recovery_mode();
        }

        device_config_free(&cfg);
        device_config_init(&cfg);
        ESP_ERROR_CHECK(device_config_load(&cfg));

        if (mqtt_receiver_restart_with_public_id(&cfg) != ESP_OK) {
            device_config_free(&cfg);
            provision_run_recovery_mode();
        }
    }

    ESP_ERROR_CHECK(mqtt_receiver_subscribe_commands(cfg.public_id));

    if (cfg.op_state == RECEIVER_OP_STATE_ACTIVE) {
        ESP_ERROR_CHECK(rf_sup_start());
    } else if (cfg.op_state == RECEIVER_OP_STATE_DECOMMISSIONED) {
        rf_sup_delete();
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
