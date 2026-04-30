/**
 * @file main.c
 * @brief Receiver Application Entry Point and Orchestration
 *
 * Coordinates provisioning, activation, RF trigger restoration, and steady-
 * state receiver operation for the ESP-01 firmware.
 */

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
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

static void restart_after_response_flush(void)
{
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static esp_err_t init_platform_once(void)
{
    esp_err_t err;

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    err = vibrator_init(VIBRATOR_GPIO, &s_vibrator);
    if (err != ESP_OK) {
        return err;
    }

    err = rf_trigger_init(&s_vibrator);
    if (err != ESP_OK) {
        return err;
    }

    rf_recv_set_frame_callback(rf_trigger_on_frame, NULL);

    err = wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    err = device_identity_init(&s_identity);
    if (err != ESP_OK) {
        return err;
    }

    return mqtt_receiver_init(&s_identity, PROJECT_VER);
}

static esp_err_t load_runtime_config(device_config_t *cfg)
{
    device_config_free(cfg);
    return device_config_load(cfg);
}

static void restore_rf_snapshot(const device_config_t *cfg)
{
    if (cfg->has_rf_code && cfg->rf_code_bits > 0) {
        rf_trigger_restore(cfg->rf_code, cfg->rf_code_bits, cfg->rf_code_ver);
    }
}

static esp_err_t attempt_wifi_stage(const device_config_t *cfg)
{
    return wifi_start_sta(cfg->wifi_ssid, cfg->wifi_pwd);
}

static esp_err_t attempt_mqtt_stage(const device_config_t *cfg)
{
    return mqtt_receiver_start(cfg);
}

static esp_err_t attempt_activation_stage(device_config_t *cfg)
{
    esp_err_t err;

    if (cfg->has_public_id && cfg->has_op_state) {
        return ESP_OK;
    }

    if (!cfg->has_enroll_token || !cfg->enroll_token) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mqtt_receiver_bootstrap_activate(cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = load_runtime_config(cfg);
    if (err != ESP_OK) {
        return err;
    }

    if (!cfg->has_public_id || !cfg->public_id || !cfg->has_op_state) {
        return ESP_FAIL;
    }

    return mqtt_receiver_restart_with_public_id(cfg);
}

static esp_err_t subscribe_command_topics(const device_config_t *cfg)
{
    if (!cfg->has_public_id || !cfg->public_id) {
        return ESP_ERR_INVALID_STATE;
    }

    return mqtt_receiver_subscribe_commands(cfg->public_id);
}

static void enter_operational_state(const device_config_t *cfg)
{
    if (cfg->op_state == RECEIVER_OP_STATE_ACTIVE) {
        ESP_ERROR_CHECK(rf_sup_start());
    } else if (cfg->op_state == RECEIVER_OP_STATE_DECOMMISSIONED) {
        rf_sup_delete();
    }
}

static void handle_recovery_result(provision_recovery_result_t result)
{
    if (result == PROVISION_RECOVERY_RESULT_RESTART) {
        restart_after_response_flush();
    }
}

void app_main(void)
{
    device_config_t cfg;
    esp_err_t err;

    device_config_init(&cfg);
    ESP_ERROR_CHECK(init_platform_once());

    for (;;) {
        err = load_runtime_config(&cfg);
        ESP_ERROR_CHECK(err);

        restore_rf_snapshot(&cfg);

        if (!device_config_is_provisioned(&cfg)) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_UNPROVISIONED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        if (device_config_is_recovery_required(&cfg)) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        if (attempt_wifi_stage(&cfg) != ESP_OK) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_WIFI_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        if (attempt_mqtt_stage(&cfg) != ESP_OK) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MQTT_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        err = attempt_activation_stage(&cfg);
        if (err == ESP_ERR_INVALID_STATE) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }
        if (err != ESP_OK) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_BOOTSTRAP_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        if (subscribe_command_topics(&cfg) != ESP_OK) {
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MQTT_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        enter_operational_state(&cfg);
        device_config_free(&cfg);
        break;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
