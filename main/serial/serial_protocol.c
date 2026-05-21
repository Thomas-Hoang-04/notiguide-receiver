/**
 * @file serial_protocol.c
 * @brief Serial Provisioning Protocol - Implementation
 *
 * Synchronous blocking serial protocol for the ESP-01 receiver.  Reads
 * newline-delimited JSON commands from UART0, dispatches them, and returns
 * when a provisioning action completes.  No background task is created —
 * the caller's task blocks inside serial_protocol_run_blocking().
 */

#include "serial/serial_protocol.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tcpip_adapter.h"

#include "config/device_config.h"
#include "network/wifi.h"

#ifndef PROJECT_VER
#define PROJECT_VER "v1"
#endif

static const char *TAG = "serial";

#define LINE_BUF_SIZE 1024
#define UART_PORT     UART_NUM_0
#define UART_BAUD     115200
#define UART_RX_BUF   2048

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                      */
/* ------------------------------------------------------------------ */

static const char *json_get_string(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

/* ------------------------------------------------------------------ */
/*  UART send helpers                                                 */
/* ------------------------------------------------------------------ */

static void send_json(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        size_t len = strlen(json);
        uart_write_bytes(UART_PORT, json, len);
        uart_write_bytes(UART_PORT, "\n", 1);
        uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(500));
        free(json);
    }
    cJSON_Delete(root);
}

static void send_response(const char *id, bool ok, cJSON *payload,
                           const char *error)
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
    send_json(root);
}

static void send_error(const char *id, const char *error)
{
    send_response(id, false, NULL, error);
}

/* ------------------------------------------------------------------ */
/*  Command handlers                                                  */
/* ------------------------------------------------------------------ */

static void handle_ping(const char *id)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "uptime_ms",
                            esp_timer_get_time() / 1000.0);
    send_response(id, true, payload, NULL);
}

static void handle_identify(const char *id, const device_config_t *cfg,
                             const char *recovery_reason)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "public_id",
                            (cfg->has_public_id && cfg->public_id)
                                ? cfg->public_id : "");
    cJSON_AddStringToObject(payload, "device_name",
                            (cfg->has_device_name && cfg->device_name)
                                ? cfg->device_name : "");
    cJSON_AddStringToObject(payload, "device_kind", "RECEIVER_433M");
    cJSON_AddStringToObject(payload, "op_state",
                            device_config_state_name(cfg));
    cJSON_AddStringToObject(payload, "firmware_version", PROJECT_VER);
    cJSON_AddStringToObject(payload, "mac", mac_str);
    cJSON_AddBoolToObject(payload, "provisioned",
                          device_config_is_provisioned(cfg));
    if (recovery_reason != NULL) {
        cJSON_AddStringToObject(payload, "recovery_reason", recovery_reason);
    }
    send_response(id, true, payload, NULL);
}

static serial_prov_result_t handle_provision(const char *id,
                                             const cJSON *payload)
{
    if (payload == NULL) {
        send_error(id, "missing_payload");
        return SERIAL_PROV_RESULT_ERROR;
    }

    const char *wifi_ssid = json_get_string(payload, "wifi_ssid");
    const char *wifi_pwd  = json_get_string(payload, "wifi_pwd");
    const char *mqtt_uri  = json_get_string(payload, "mqtt_uri");
    const char *mqtt_user = json_get_string(payload, "mqtt_user");
    const char *mqtt_pwd  = json_get_string(payload, "mqtt_pwd");
    const char *enroll    = json_get_string(payload, "enroll_token");

    if (wifi_ssid == NULL || mqtt_uri == NULL || mqtt_user == NULL ||
        mqtt_pwd == NULL || enroll == NULL) {
        send_error(id, "missing_fields");
        return SERIAL_PROV_RESULT_ERROR;
    }

    device_provisioning_t prov = {
        .wifi_ssid    = wifi_ssid,
        .wifi_pwd     = wifi_pwd ? wifi_pwd : "",
        .mqtt_uri     = mqtt_uri,
        .mqtt_user    = mqtt_user,
        .mqtt_pwd     = mqtt_pwd,
        .enroll_token = enroll,
    };

    esp_err_t err = device_config_store_provisioning(&prov);
    if (err != ESP_OK) {
        send_error(id, "nvs_write_failed");
        return SERIAL_PROV_RESULT_ERROR;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);

    return SERIAL_PROV_RESULT_PROVISIONED;
}

static void handle_test_wifi(const char *id, const cJSON *cmd_payload)
{
    if (cmd_payload == NULL) {
        send_error(id, "missing_payload");
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
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            cJSON_AddNumberToObject(resp, "rssi", ap.rssi);
        }
        tcpip_adapter_ip_info_t ip_info;
        if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK) {
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            cJSON_AddStringToObject(resp, "ip", ip_str);
        }
    }

    send_response(id, true, resp, NULL);
    wifi_stop();
}

static serial_prov_result_t handle_factory_reset(const char *id)
{
    esp_err_t err = device_config_erase_runtime();
    if (err != ESP_OK) {
        send_error(id, "reset_failed");
        return SERIAL_PROV_RESULT_ERROR;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);

    return SERIAL_PROV_RESULT_RESET;
}

static serial_prov_result_t handle_retry(const char *id,
                                         const device_config_t *cfg)
{
    if (!device_config_is_provisioned(cfg)) {
        send_error(id, "not_provisioned");
        return SERIAL_PROV_RESULT_ERROR;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "restarting", true);
    send_response(id, true, resp, NULL);

    return SERIAL_PROV_RESULT_RETRY;
}

/* ------------------------------------------------------------------ */
/*  Command dispatch                                                  */
/* ------------------------------------------------------------------ */

static serial_prov_result_t dispatch_command(const char *line,
                                             const device_config_t *cfg,
                                             const char *recovery_reason,
                                             bool *should_break)
{
    serial_prov_result_t result = SERIAL_PROV_RESULT_ERROR;
    *should_break = false;

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        return result;
    }

    cJSON *id_item   = cJSON_GetObjectItem(root, "id");
    cJSON *type_item = cJSON_GetObjectItem(root, "type");

    if (!cJSON_IsString(id_item) || !cJSON_IsString(type_item)) {
        cJSON_Delete(root);
        return result;
    }

    const char *id   = cJSON_GetStringValue(id_item);
    const char *type = cJSON_GetStringValue(type_item);
    const cJSON *payload = cJSON_GetObjectItem(root, "payload");

    if (strcmp(type, "ping") == 0) {
        handle_ping(id);
    } else if (strcmp(type, "identify") == 0) {
        handle_identify(id, cfg, recovery_reason);
    } else if (strcmp(type, "provision") == 0) {
        result = handle_provision(id, payload);
        if (result == SERIAL_PROV_RESULT_PROVISIONED) {
            *should_break = true;
        }
    } else if (strcmp(type, "provision.test_wifi") == 0) {
        handle_test_wifi(id, payload);
    } else if (strcmp(type, "factory_reset") == 0) {
        result = handle_factory_reset(id);
        if (result == SERIAL_PROV_RESULT_RESET) {
            *should_break = true;
        }
    } else if (strcmp(type, "retry") == 0) {
        result = handle_retry(id, cfg);
        if (result == SERIAL_PROV_RESULT_RETRY) {
            *should_break = true;
        }
    } else {
        send_error(id, "unknown_command");
    }

    cJSON_Delete(root);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

esp_err_t serial_protocol_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    esp_err_t err = uart_param_config(UART_PORT, &uart_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(UART_PORT, UART_RX_BUF, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "UART%d initialized at %d baud", UART_PORT, UART_BAUD);
    return ESP_OK;
}

serial_prov_result_t serial_protocol_run_blocking(const device_config_t *cfg,
                                                  const char *recovery_reason)
{
    char line_buf[LINE_BUF_SIZE];
    int line_pos = 0;
    bool line_overflow = false;
    serial_prov_result_t result = SERIAL_PROV_RESULT_ERROR;

    ESP_LOGI(TAG, "Blocking serial loop started (reason=%s)",
             recovery_reason ? recovery_reason : "none");

    for (;;) {
        uint8_t byte;
        int len = uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) {
            continue;
        }

        if (byte == '\n') {
            if (line_overflow) {
                ESP_LOGW(TAG, "Discarding oversized line (%d+ bytes)",
                         LINE_BUF_SIZE);
                line_overflow = false;
            } else if (line_pos > 0) {
                line_buf[line_pos] = '\0';

                bool should_break = false;
                result = dispatch_command(line_buf, cfg, recovery_reason,
                                          &should_break);
                if (should_break) {
                    return result;
                }
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
