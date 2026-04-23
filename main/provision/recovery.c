/**
 * @file recovery.c
 * @brief Provisioning Recovery Mode - Implementation
 *
 * Owns the recovery-mode handoff that stops normal connectivity, starts the
 * provisioning SoftAP and HTTP server, and reboots after a local action.
 */

#include "provision/recovery.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network/mqtt.h"
#include "network/wifi.h"
#include "provision/http_server.h"

#define RECOVERY_TAG "provision"

void provision_run_recovery_mode(void)
{
    char ssid[32] = {0};

    mqtt_stop();
    wifi_stop();
    ESP_ERROR_CHECK(wifi_start_softap(ssid, sizeof(ssid)));
    ESP_ERROR_CHECK(http_server_start());
    ESP_LOGI(RECOVERY_TAG, "SoftAP ready: %s", ssid);

    for (;;) {
        http_server_action_t action = http_server_wait_for_action(portMAX_DELAY);

        if (action == HTTP_SERVER_ACTION_NONE) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}
