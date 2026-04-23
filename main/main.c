/**
 * @file main.c
 * @brief Receiver firmware boot orchestration.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "network/mqtt.h"
#include "network/wifi.h"
#include "provision/http_server.h"
#include "security/device_identity.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "sdkconfig.h"
#include <stddef.h>

#define APP_TAG "APP"

static device_config_t g_cfg;
static device_identity_t g_identity;
static EventGroupHandle_t g_provision_events;

static void restart_after_delay(void)
{
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
}

static void run_provisioning_mode(provision_reason_t reason)
{
    ESP_LOGW(APP_TAG, "Entering provisioning mode: %s", provision_reason_string(reason));

    mqtt_stop();
    xEventGroupClearBits(g_provision_events,
                         PROVISION_EVENT_DONE | PROVISION_EVENT_RETRY | PROVISION_EVENT_RESET);
    ESP_ERROR_CHECK(wifi_start_softap());

    provision_http_context_t http_ctx = {
        .events = g_provision_events,
        .cfg = &g_cfg,
        .reason = reason,
    };
    ESP_ERROR_CHECK(provision_http_server_start(&http_ctx));

    EventBits_t bits = xEventGroupWaitBits(g_provision_events,
                                           PROVISION_EVENT_DONE |
                                           PROVISION_EVENT_RETRY |
                                           PROVISION_EVENT_RESET,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    ESP_ERROR_CHECK(provision_http_server_stop());
    ESP_ERROR_CHECK(wifi_stop());

    if ((bits & PROVISION_EVENT_RESET) != 0) {
        ESP_ERROR_CHECK(device_config_reprovision());
        ESP_ERROR_CHECK(device_config_load(&g_cfg));
    }

    restart_after_delay();
}

static void restore_trigger_state(void)
{
    if (device_config_has_rf_code(&g_cfg)) {
        rf_trigger_restore_from_cfg(&g_cfg);
    }
}

static void dispatch_operational_state(void)
{
    if (!g_cfg.has_op_state) {
        return;
    }

    switch (g_cfg.op_state) {
    case OP_STATE_PENDING_RF_CODE:
        rf_trigger_stop_output();
        break;
    case OP_STATE_ACTIVE:
        if (device_config_has_rf_code(&g_cfg)) {
            ESP_ERROR_CHECK(rf_sup_start());
        } else {
            ESP_LOGW(APP_TAG, "ACTIVE without stored RF code, remaining idle");
        }
        break;
    case OP_STATE_SUSPENDED:
        rf_trigger_stop_output();
        break;
    case OP_STATE_DECOMMISSIONED:
        rf_trigger_stop_output();
        break;
    default:
        break;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_init_or_recover());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(device_config_load(&g_cfg));

    g_provision_events = xEventGroupCreate();
    ESP_ERROR_CHECK(g_provision_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    switch (device_config_boot_state(&g_cfg)) {
        case DEVICE_BOOT_STATE_UNPROVISIONED:
            run_provisioning_mode(PROVISION_REASON_UNPROVISIONED);
            break;
        case DEVICE_BOOT_STATE_RECOVERY_REQUIRED:
            run_provisioning_mode(PROVISION_REASON_RECOVERY_REQUIRED);
            break;
        case DEVICE_BOOT_STATE_PENDING_ACTIVATION:
        case DEVICE_BOOT_STATE_OPERATIONAL:
            break;
    }

    ESP_ERROR_CHECK(rf_trigger_init((gpio_num_t)CONFIG_RECEIVER_VIBRATOR_GPIO));
    restore_trigger_state();

    if (wifi_start_sta(&g_cfg) != ESP_OK) {
        run_provisioning_mode(PROVISION_REASON_WIFI_FAILED);
    }

    ESP_ERROR_CHECK(device_identity_init(&g_identity));
    if (mqtt_start(&g_cfg, &g_identity) != ESP_OK) {
        run_provisioning_mode(PROVISION_REASON_BOOTSTRAP_FAILED);
    }

    if (device_config_boot_state(&g_cfg) == DEVICE_BOOT_STATE_PENDING_ACTIVATION) {
        char registration_nonce[32] = { 0 };
        ESP_ERROR_CHECK(device_identity_generate_nonce(&g_identity, 10,
                                                       registration_nonce,
                                                       sizeof(registration_nonce)));

        esp_err_t err = mqtt_bootstrap_activate(&g_cfg, &g_identity,
                                                registration_nonce,
                                                pdMS_TO_TICKS(CONFIG_RECEIVER_BOOTSTRAP_TIMEOUT_MS));
        if (err != ESP_OK) {
            mqtt_stop();
            device_config_clear_enroll_token(&g_cfg);
            run_provisioning_mode(PROVISION_REASON_BOOTSTRAP_FAILED);
        }
    }

    restore_trigger_state();
    dispatch_operational_state();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
