/**
 * @file wifi.c
 * @brief ESP-IDF Wi-Fi helpers for station and SoftAP modes.
 */

#include "network/wifi.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

#define WIFI_TAG "WIFI"
#define WIFI_EVENT_CONNECTED_BIT BIT0
#define WIFI_EVENT_FAILED_BIT    BIT1

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static EventGroupHandle_t s_events;
static receiver_wifi_mode_t s_mode;
static bool s_initialized;
static bool s_sta_connected;
static int s_retry_count;
static char s_softap_ssid[33];

static void clear_runtime_state(void)
{
    if (s_events != NULL) {
        xEventGroupClearBits(s_events, WIFI_EVENT_CONNECTED_BIT | WIFI_EVENT_FAILED_BIT);
    }
    s_sta_connected = false;
    s_retry_count = 0;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (s_mode == RECEIVER_WIFI_MODE_STA) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        s_sta_connected = false;
        if (s_mode != RECEIVER_WIFI_MODE_STA) {
            break;
        }
        if (s_retry_count < CONFIG_RECEIVER_WIFI_MAX_RETRY) {
            s_retry_count++;
            esp_wifi_connect();
        } else if (s_events != NULL) {
            xEventGroupSetBits(s_events, WIFI_EVENT_FAILED_BIT);
        }
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_sta_connected = true;
        s_retry_count = 0;
        if (s_events != NULL) {
            xEventGroupSetBits(s_events, WIFI_EVENT_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_init_common(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events != NULL, ESP_ERR_NO_MEM, WIFI_TAG, "failed to create event group");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), WIFI_TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), WIFI_TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   &wifi_event_handler, NULL),
                        WIFI_TAG, "failed to register WIFI handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   &ip_event_handler, NULL),
                        WIFI_TAG, "failed to register IP handler");

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t wifi_prepare_mode(receiver_wifi_mode_t mode)
{
    clear_runtime_state();

    if (!s_initialized) {
        s_mode = mode;
        return ESP_OK;
    }

    if (s_mode == RECEIVER_WIFI_MODE_STA) {
        esp_wifi_disconnect();
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }

    s_mode = mode;
    return ESP_OK;
}

esp_err_t wifi_start_sta(const device_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, WIFI_TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(cfg->has_wifi, ESP_ERR_INVALID_STATE, WIFI_TAG, "Wi-Fi credentials missing");

    ESP_RETURN_ON_ERROR(wifi_init_common(), WIFI_TAG, "wifi init failed");
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, WIFI_TAG, "failed to create STA netif");
    }
    ESP_RETURN_ON_ERROR(wifi_prepare_mode(RECEIVER_WIFI_MODE_STA), WIFI_TAG,
                        "failed to prepare STA mode");

    wifi_config_t sta_cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
            .disable_wpa3_compatible_mode = 0,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .failure_retry_cnt = CONFIG_RECEIVER_WIFI_MAX_RETRY,
        },
    };

    strlcpy((char *)sta_cfg.sta.ssid, cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg->wifi_pwd, sizeof(sta_cfg.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), WIFI_TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), WIFI_TAG,
                        "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), WIFI_TAG, "esp_wifi_start failed");

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_EVENT_CONNECTED_BIT | WIFI_EVENT_FAILED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(30000));
    if ((bits & WIFI_EVENT_CONNECTED_BIT) == 0) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_MIN_MODEM), WIFI_TAG,
                        "esp_wifi_set_ps failed");
    return ESP_OK;
}

esp_err_t wifi_start_softap(void)
{
    ESP_RETURN_ON_ERROR(wifi_init_common(), WIFI_TAG, "wifi init failed");
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_FAIL, WIFI_TAG, "failed to create AP netif");
    }
    ESP_RETURN_ON_ERROR(wifi_prepare_mode(RECEIVER_WIFI_MODE_SOFTAP), WIFI_TAG,
                        "failed to prepare SoftAP mode");

    uint8_t mac[6] = { 0 };
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), WIFI_TAG, "esp_read_mac failed");

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_RECEIVER_AP_CHANNEL,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .wpa3_compatible_mode = 1,
        },
    };

    strlcpy((char *)ap_cfg.ap.password, CONFIG_RECEIVER_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    int written = snprintf(s_softap_ssid, sizeof(s_softap_ssid), "notiguide-recv-%02X%02X",
                           mac[4], mac[5]);
    ESP_RETURN_ON_FALSE(written > 0 && written < (int)sizeof(s_softap_ssid), ESP_FAIL, WIFI_TAG,
                        "failed to build SoftAP SSID");
    strlcpy((char *)ap_cfg.ap.ssid, s_softap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)written;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), WIFI_TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), WIFI_TAG,
                        "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), WIFI_TAG, "esp_wifi_start failed");
    return ESP_OK;
}

esp_err_t wifi_stop(void)
{
    if (!s_initialized) {
        s_mode = RECEIVER_WIFI_MODE_IDLE;
        return ESP_OK;
    }

    if (s_mode == RECEIVER_WIFI_MODE_STA) {
        esp_wifi_disconnect();
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        return err;
    }

    clear_runtime_state();
    s_mode = RECEIVER_WIFI_MODE_IDLE;
    return ESP_OK;
}

receiver_wifi_mode_t wifi_get_mode(void)
{
    return s_mode;
}

const char *wifi_get_softap_ssid(void)
{
    return s_softap_ssid;
}

bool wifi_is_sta_connected(void)
{
    return s_sta_connected;
}
