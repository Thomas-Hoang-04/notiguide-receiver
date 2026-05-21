/**
 * @file wifi.c
 * @brief Wi-Fi Module - Runtime Networking Implementation
 *
 * Implements STA association, MAC label formatting, and event-driven
 * connection state tracking for the receiver firmware.
 */

#include "network/wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_events;
static bool s_wifi_initialized;
static bool s_handlers_registered;
static int s_retry_num;

static bool is_auth_failure_reason(uint8_t reason)
{
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        system_event_sta_disconnected_t *event = (system_event_sta_disconnected_t *)data;
        uint8_t reason = event ? event->reason : 0;

        if (is_auth_failure_reason(reason)) {
            ESP_LOGW(WIFI_TAG, "Authentication failure, reason=%u", reason);
        }

        if (s_retry_num < CONFIG_RECEIVER_WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(WIFI_TAG, "Retry %d/%d", s_retry_num, CONFIG_RECEIVER_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_retry_num = 0;
        ESP_LOGI(WIFI_TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_register_handlers(void)
{
    esp_err_t err;

    if (s_handlers_registered) {
        return ESP_OK;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
        return err;
    }

    s_handlers_registered = true;
    return ESP_OK;
}

static void wifi_unregister_handlers(void)
{
    if (!s_handlers_registered) {
        return;
    }

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    s_handlers_registered = false;
}

esp_err_t wifi_init(void)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    if (s_wifi_initialized) {
        return ESP_OK;
    }

    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "WiFi subsystem initialized");
    s_wifi_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_get_mac_label(char *buf, size_t buf_len)
{
    uint8_t mac[6] = {0};
    esp_err_t err;

    if (!buf || buf_len < 5) {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(buf, buf_len, "%02X%02X", mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t wifi_start_sta(const char *ssid, const char *password)
{
    esp_err_t err;
    EventBits_t bits;
    wifi_config_t wifi_cfg = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold = {
                .rssi = -127,
                .authmode = WIFI_AUTH_WPA2_PSK,
            },
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    err = wifi_register_handlers();
    if (err != ESP_OK) {
        return err;
    }

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password);
#if CONFIG_ESP8266_WIFI_ENABLE_WPA3_SAE
    if (strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    }
#endif

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "set_mode STA failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "set_config STA failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "STA starting, connecting to %s", ssid);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "Waiting for connection result...");
    bits = xEventGroupWaitBits(s_wifi_events,
                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                               pdTRUE,
                               pdFALSE,
                               portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "STA connected, enabling power save");
        return esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    ESP_LOGE(WIFI_TAG, "STA connection failed after %d retries", CONFIG_RECEIVER_WIFI_MAX_RETRY);
    return ESP_FAIL;
}

esp_err_t wifi_start_sta_test(const char *ssid, const char *password, TickType_t timeout)
{
    esp_err_t err;
    EventBits_t bits;
    bool has_password = (password != NULL && password[0] != '\0');

    wifi_config_t wifi_cfg = {
        .sta = {
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold = {
                .rssi = -127,
                .authmode = has_password ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            },
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_init();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;

    err = wifi_register_handlers();
    if (err != ESP_OK) {
        return err;
    }

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    if (password != NULL) {
        snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password);
    }
#if CONFIG_ESP8266_WIFI_ENABLE_WPA3_SAE
    if (password != NULL && strlen(password) > 0) {
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    }
#endif

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "set_mode STA failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "set_config STA failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(WIFI_TAG, "STA test starting, connecting to %s", ssid);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    bits = xEventGroupWaitBits(s_wifi_events,
                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                               pdTRUE,
                               pdFALSE,
                               timeout);
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t wifi_stop(void)
{
    esp_err_t err;

    if (!s_wifi_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(WIFI_TAG, "Stopping WiFi");
    wifi_unregister_handlers();
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_stop failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
