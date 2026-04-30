/**
 * @file recovery.c
 * @brief Provisioning Recovery Mode - Implementation
 *
 * Owns the recovery-mode handoff that stops normal connectivity, starts the
 * provisioning SoftAP and HTTP server, and returns the requested outcome.
 */

#include "provision/recovery.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "network/mqtt.h"
#include "network/wifi.h"
#include "provision/http_server.h"

#define RECOVERY_TAG "provision"

static const char *s_active_recovery_reason;

const char *provision_recovery_reason_name(provision_recovery_reason_t reason)
{
    switch (reason) {
    case PROVISION_RECOVERY_REASON_UNPROVISIONED:
        return "UNPROVISIONED";
    case PROVISION_RECOVERY_REASON_WIFI_FAILED:
        return "WIFI_FAILED";
    case PROVISION_RECOVERY_REASON_MQTT_FAILED:
        return "MQTT_FAILED";
    case PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN:
        return "MISSING_ENROLL_TOKEN";
    case PROVISION_RECOVERY_REASON_BOOTSTRAP_FAILED:
        return "BOOTSTRAP_FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *provision_recovery_get_active_reason(void)
{
    return s_active_recovery_reason;
}

provision_recovery_result_t provision_run_recovery_mode(provision_recovery_reason_t reason)
{
    char ssid[32] = {0};
    const char *reason_name = provision_recovery_reason_name(reason);

    mqtt_stop();
    wifi_stop();
    ESP_ERROR_CHECK(wifi_start_softap(ssid, sizeof(ssid)));
    ESP_ERROR_CHECK(http_server_start());
    s_active_recovery_reason = reason_name;
    ESP_LOGI(RECOVERY_TAG, "SoftAP ready: %s (reason=%s)", ssid, reason_name);

    for (;;) {
        http_server_action_t action = http_server_wait_for_action(portMAX_DELAY);

        if (action == HTTP_SERVER_ACTION_NONE) {
            continue;
        }

        if (http_server_stop() != ESP_OK) {
            ESP_LOGW(RECOVERY_TAG, "Failed to stop HTTP server cleanly");
        }

        if (action == HTTP_SERVER_ACTION_RETRY) {
            ESP_ERROR_CHECK(wifi_stop());
            s_active_recovery_reason = NULL;
            return PROVISION_RECOVERY_RESULT_RETRY_EXISTING;
        }

        s_active_recovery_reason = NULL;
        return PROVISION_RECOVERY_RESULT_RESTART;
    }
}
