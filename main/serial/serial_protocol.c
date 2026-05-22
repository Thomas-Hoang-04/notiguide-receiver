/**
 * @file serial_protocol.c
 * @brief USB Serial/JTAG JSON-line protocol — background FreeRTOS task.
 *
 * Runs alongside SoftAP+HTTP provisioning.  Commands that alter NVS
 * (provision, factory_reset, retry) persist to flash and call
 * esp_restart() directly.
 *
 * Wire format (newline-delimited JSON):
 *   Request:  {"id":"<uuid>","type":"<cmd>","payload":{...}}\n
 *   Response: {"id":"<uuid>","type":"response","ok":true,"payload":{...}}\n
 */

#include "serial/serial_protocol.h"

#include "config/device_config.h"
#include "network/wifi.h"
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "serial";

#define LINE_BUF_SIZE  1024
#define TX_BUF_SIZE    4096
#define RX_BUF_SIZE    4096

static TaskHandle_t s_task_handle;

/* -------------------------------------------------------------------------- */
/*  JSON helpers                                                              */
/* -------------------------------------------------------------------------- */

static const char *json_get_string(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

/* -------------------------------------------------------------------------- */
/*  Response writing                                                          */
/* -------------------------------------------------------------------------- */

static void send_response(const char *id, bool ok, cJSON *payload, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddBoolToObject(root, "ok", ok);
    if (ok && payload != NULL) {
        cJSON_AddItemToObject(root, "payload", payload);
    }
    if (!ok && error != NULL) {
        cJSON_AddStringToObject(root, "error", error);
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        size_t json_len = strlen(json);
        char *line = malloc(json_len + 2);
        if (line != NULL) {
            memcpy(line, json, json_len);
            line[json_len] = '\n';
            usb_serial_jtag_write_bytes(line, json_len + 1, pdMS_TO_TICKS(200));
            free(line);
        } else {
            ESP_LOGE(TAG, "malloc failed for serial response (%zu bytes)", json_len + 2);
        }
        free(json);
    }

    cJSON_Delete(root);
}

static void send_error(const char *id, const char *error)
{
    send_response(id, false, NULL, error);
}

/* -------------------------------------------------------------------------- */
/*  Command handlers                                                          */
/* -------------------------------------------------------------------------- */

static void handle_ping(const char *id)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "uptime_ms", esp_timer_get_time() / 1000.0);
    send_response(id, true, payload, NULL);
}

static void handle_identify(const char *id)
{
    device_config_t cfg;
    esp_err_t err = device_config_load(&cfg);
    if (err != ESP_OK) {
        send_error(id, "config_load_failed");
        return;
    }

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const char *device_kind =
        device_config_receiver_type_string(device_config_compiled_receiver_type());

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "public_id",
                            cfg.has_public_id ? cfg.public_id : "");
    cJSON_AddStringToObject(payload, "device_name",
                            cfg.has_device_name ? cfg.device_name : "");
    cJSON_AddStringToObject(payload, "device_kind", device_kind);
    cJSON_AddStringToObject(payload, "op_state",
                            cfg.has_op_state
                                ? device_config_op_state_string(cfg.op_state)
                                : "NONE");
    cJSON_AddStringToObject(payload, "firmware_version",
                            CONFIG_RECEIVER_FIRMWARE_VERSION);
    cJSON_AddStringToObject(payload, "mac", mac_str);
    send_response(id, true, payload, NULL);
}

static bool handle_provision(const char *id, const cJSON *cmd_payload)
{
    if (cmd_payload == NULL) {
        send_error(id, "missing_payload");
        return false;
    }

    const char *wifi_ssid = json_get_string(cmd_payload, "wifi_ssid");
    const char *wifi_pwd  = json_get_string(cmd_payload, "wifi_pwd");
    const char *mqtt_uri  = json_get_string(cmd_payload, "mqtt_uri");
    const char *mqtt_user = json_get_string(cmd_payload, "mqtt_user");
    const char *mqtt_pwd  = json_get_string(cmd_payload, "mqtt_pwd");
    const char *enroll    = json_get_string(cmd_payload, "enroll_token");

    if (wifi_ssid == NULL || mqtt_uri == NULL || mqtt_user == NULL ||
        mqtt_pwd == NULL || enroll == NULL) {
        send_error(id, "missing_fields");
        return false;
    }

    device_config_t cfg;
    esp_err_t err = device_config_load(&cfg);
    if (err != ESP_OK) {
        send_error(id, "config_load_failed");
        return false;
    }

    device_provisioning_t prov = { 0 };
    strncpy(prov.wifi_ssid, wifi_ssid, sizeof(prov.wifi_ssid) - 1);
    if (wifi_pwd != NULL) {
        strncpy(prov.wifi_pwd, wifi_pwd, sizeof(prov.wifi_pwd) - 1);
    }
    strncpy(prov.mqtt_uri, mqtt_uri, sizeof(prov.mqtt_uri) - 1);
    strncpy(prov.mqtt_user, mqtt_user, sizeof(prov.mqtt_user) - 1);
    strncpy(prov.mqtt_pwd, mqtt_pwd, sizeof(prov.mqtt_pwd) - 1);
    strncpy(prov.enroll_token, enroll, sizeof(prov.enroll_token) - 1);

    err = device_config_save_provisioning(&cfg, &prov);
    if (err != ESP_OK) {
        send_error(id, "nvs_write_failed");
        return false;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);

    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
    esp_restart();
    return false;
}

static void handle_test_wifi(const char *id, const cJSON *cmd_payload)
{
    if (cmd_payload == NULL) {
        send_error(id, "missing_payload");
        return;
    }

    if (wifi_is_sta_connected()) {
        send_error(id, "already_connected");
        return;
    }

    const char *ssid = json_get_string(cmd_payload, "wifi_ssid");
    if (ssid == NULL) {
        send_error(id, "missing_fields");
        return;
    }
    const char *pwd = json_get_string(cmd_payload, "wifi_pwd");

    esp_err_t err = wifi_start_sta_test(ssid, pwd, pdMS_TO_TICKS(15000));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "connected", err == ESP_OK);

    if (err == ESP_OK) {
        int rssi = 0;
        esp_wifi_sta_get_rssi(&rssi);
        cJSON_AddNumberToObject(resp, "rssi", rssi);

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                cJSON_AddStringToObject(resp, "ip", ip_str);
            }
        }
    }

    send_response(id, true, resp, NULL);
    wifi_stop();
}

static bool handle_factory_reset(const char *id)
{
    esp_err_t err = device_config_reprovision();
    if (err != ESP_OK) {
        send_error(id, "reset_failed");
        return false;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);
    return true;
}

static bool handle_retry(const char *id)
{
    device_config_t cfg;
    esp_err_t err = device_config_load(&cfg);
    if (err != ESP_OK) {
        send_error(id, "config_load_failed");
        return false;
    }

    device_boot_state_t state = device_config_boot_state(&cfg);
    if (state == DEVICE_BOOT_STATE_UNPROVISIONED) {
        send_error(id, "not_provisioned");
        return false;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Command dispatch                                                          */
/* -------------------------------------------------------------------------- */

static void dispatch_command(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return;
    }

    const cJSON *id_item   = cJSON_GetObjectItem(root, "id");
    const cJSON *type_item = cJSON_GetObjectItem(root, "type");

    if (!cJSON_IsString(id_item) || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return;
    }

    const char *id   = cJSON_GetStringValue(id_item);
    const char *type = cJSON_GetStringValue(type_item);
    const cJSON *payload = cJSON_GetObjectItem(root, "payload");

    bool needs_restart = false;

    if (strcmp(type, "ping") == 0) {
        handle_ping(id);
    } else if (strcmp(type, "identify") == 0) {
        handle_identify(id);
    } else if (strcmp(type, "provision") == 0) {
        needs_restart = handle_provision(id, payload);
    } else if (strcmp(type, "provision.test_wifi") == 0) {
        handle_test_wifi(id, payload);
    } else if (strcmp(type, "factory_reset") == 0) {
        needs_restart = handle_factory_reset(id);
    } else if (strcmp(type, "retry") == 0) {
        needs_restart = handle_retry(id);
    } else {
        send_error(id, "unknown_command");
    }

    cJSON_Delete(root);

    if (needs_restart) {
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
        esp_restart();
    }
}

/* -------------------------------------------------------------------------- */
/*  Read-loop task                                                            */
/* -------------------------------------------------------------------------- */

static void serial_read_task(void *arg)
{
    (void)arg;

    char line_buf[LINE_BUF_SIZE];
    int  line_pos = 0;
    bool line_overflow = false;

    for (;;) {
        uint8_t byte;
        int len = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        if (byte == '\n') {
            if (line_overflow) {
                ESP_LOGW(TAG, "Discarding oversized line (%d+ bytes)", LINE_BUF_SIZE);
                line_overflow = false;
            } else if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                dispatch_command(line_buf);
            }
            line_pos = 0;
        } else if (byte != '\r') {
            if (line_pos < LINE_BUF_SIZE - 1) {
                line_buf[line_pos++] = (char)byte;
            } else {
                line_overflow = true;
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t serial_protocol_init(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = TX_BUF_SIZE;
    cfg.rx_buffer_size = RX_BUF_SIZE;

    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB serial JTAG driver: %s",
                 esp_err_to_name(err));
        return err;
    }

    usb_serial_jtag_vfs_use_driver();

    BaseType_t task_ok = xTaskCreate(serial_read_task, "serial_proto",
                                     4096, NULL, 5, &s_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial protocol task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Serial protocol initialized");
    return ESP_OK;
}

void serial_protocol_stop(void)
{
    if (s_task_handle != NULL) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
        ESP_LOGI(TAG, "Serial protocol stopped");
    }
}
