/**
 * @file main.c
 * @brief Receiver Application Entry Point and Orchestration
 *
 * Coordinates provisioning, activation, RF trigger restoration, and steady-
 * state receiver operation for the ESP-01 firmware.
 */

#include "esp_event.h"
#include "esp_log.h"
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

#define MAIN_TAG "MAIN"
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

    ESP_LOGI(MAIN_TAG, "NVS init");
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(MAIN_TAG, "NVS partition dirty, erasing");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "NVS erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(MAIN_TAG, "TCP/IP adapter init");
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Netif init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Event loop failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(MAIN_TAG, "Vibrator init on GPIO %d", VIBRATOR_GPIO);
    err = vibrator_init(VIBRATOR_GPIO, &s_vibrator);
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Vibrator init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(MAIN_TAG, "RF trigger init");
    err = rf_trigger_init(&s_vibrator);
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "RF trigger init failed: %s", esp_err_to_name(err));
        return err;
    }

    rf_recv_set_frame_callback(rf_trigger_on_frame, NULL);

    ESP_LOGI(MAIN_TAG, "WiFi init");
    err = wifi_init();
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(MAIN_TAG, "Device identity init");
    err = device_identity_init(&s_identity);
    if (err != ESP_OK) {
        ESP_LOGE(MAIN_TAG, "Identity init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(MAIN_TAG, "MQTT init");
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

    ESP_LOGI(MAIN_TAG, "receiver-8266 %s starting", PROJECT_VER);

    device_config_init(&cfg);
    ESP_ERROR_CHECK(init_platform_once());
    ESP_LOGI(MAIN_TAG, "Platform init complete");

    for (;;) {
        err = load_runtime_config(&cfg);
        ESP_ERROR_CHECK(err);
        ESP_LOGI(MAIN_TAG, "Config state: %s", device_config_state_name(&cfg));

        restore_rf_snapshot(&cfg);

        if (!device_config_is_provisioned(&cfg)) {
            ESP_LOGW(MAIN_TAG, "Not provisioned, entering recovery");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_UNPROVISIONED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        if (device_config_is_recovery_required(&cfg)) {
            ESP_LOGW(MAIN_TAG, "Recovery required (missing enroll token)");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        ESP_LOGI(MAIN_TAG, "Connecting to WiFi SSID: %s", cfg.wifi_ssid ? cfg.wifi_ssid : "?");
        if (attempt_wifi_stage(&cfg) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "WiFi connection failed");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_WIFI_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }
        ESP_LOGI(MAIN_TAG, "WiFi connected");

        ESP_LOGI(MAIN_TAG, "Starting MQTT to %s", cfg.mqtt_uri ? cfg.mqtt_uri : "?");
        if (attempt_mqtt_stage(&cfg) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "MQTT connection failed");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MQTT_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }
        ESP_LOGI(MAIN_TAG, "MQTT connected");

        ESP_LOGI(MAIN_TAG, "Activation stage");
        err = attempt_activation_stage(&cfg);
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(MAIN_TAG, "Missing enroll token for activation");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Bootstrap activation failed: %s", esp_err_to_name(err));
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_BOOTSTRAP_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        ESP_LOGI(MAIN_TAG, "Subscribing to command topics");
        if (subscribe_command_topics(&cfg) != ESP_OK) {
            ESP_LOGE(MAIN_TAG, "Command topic subscription failed");
            provision_recovery_result_t result =
                provision_run_recovery_mode(PROVISION_RECOVERY_REASON_MQTT_FAILED);
            device_config_free(&cfg);
            handle_recovery_result(result);
            continue;
        }

        ESP_LOGI(MAIN_TAG, "Entering operational state: %s", device_config_state_name(&cfg));
        enter_operational_state(&cfg);
        device_config_free(&cfg);
        break;
    }

    ESP_LOGI(MAIN_TAG, "Operational, idle loop running");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
