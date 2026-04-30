/**
 * @file http_server.c
 * @brief Provisioning HTTP Server - Implementation
 *
 * Implements the embedded provisioning UI, status endpoint, and local control
 * endpoints used during SoftAP setup and recovery flows.
 */

#include "provision/http_server.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"

#include "config/device_config.h"
#include "network/wifi.h"
#include "provision/recovery.h"

#define HTTP_SERVER_TAG "HTTP"
#ifndef PROJECT_VER
#define PROJECT_VER "dev"
#endif
#define PROV_BODY_MAX 2048

#define BIT_PROVISIONED BIT0
#define BIT_RETRY BIT1
#define BIT_RESET BIT2

static httpd_handle_t s_server;
static EventGroupHandle_t s_events;

extern const uint8_t provision_index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t provision_index_html_gz_end[] asm("_binary_index_html_gz_end");

static esp_err_t send_json(httpd_req_t *req, int status_code, cJSON *body)
{
    char *json = cJSON_PrintUnformatted(body);

    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_code == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

static esp_err_t send_error(httpd_req_t *req, const char *message)
{
    cJSON *body = cJSON_CreateObject();
    esp_err_t err;

    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "error", message);
    err = send_json(req, 400, body);
    cJSON_Delete(body);
    return err;
}

static esp_err_t index_get(httpd_req_t *req)
{
    size_t len = provision_index_html_gz_end - provision_index_html_gz_start;

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)provision_index_html_gz_start, len);
}

static esp_err_t status_get(httpd_req_t *req)
{
    device_config_t cfg;
    cJSON *body;
    char mac_label[7] = {0};
    const char *boot_state;
    const char *recovery_reason;
    esp_err_t err;

    device_config_init(&cfg);
    err = device_config_load(&cfg);
    if (err != ESP_OK) {
        return send_error(req, "config read failed");
    }

    wifi_get_mac_label(mac_label, sizeof(mac_label));

    body = cJSON_CreateObject();
    if (!body) {
        device_config_free(&cfg);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "mac_label", mac_label[0] ? mac_label : "000000");
    cJSON_AddStringToObject(body, "public_id", cfg.has_public_id ? cfg.public_id : "");
    cJSON_AddStringToObject(body, "firmware_version", PROJECT_VER);
    cJSON_AddStringToObject(body, "op_state", device_config_state_name(&cfg));
    cJSON_AddStringToObject(body, "wifi_ssid", cfg.has_wifi_ssid ? cfg.wifi_ssid : "");
    cJSON_AddBoolToObject(body, "has_config", device_config_has_any_data(&cfg));
    if (!device_config_is_provisioned(&cfg)) {
        boot_state = "UNPROVISIONED";
    } else if (device_config_is_recovery_required(&cfg)) {
        boot_state = "RECOVERY_REQUIRED";
    } else if (device_config_is_pending_activation(&cfg)) {
        boot_state = "PENDING_ACTIVATION";
    } else {
        boot_state = "OPERATIONAL";
    }
    cJSON_AddStringToObject(body, "boot_state", boot_state);
    cJSON_AddBoolToObject(body, "can_retry", device_config_is_provisioned(&cfg));
    recovery_reason = provision_recovery_get_active_reason();
    cJSON_AddStringToObject(body, "recovery_reason", recovery_reason ? recovery_reason : "");

    err = send_json(req, 200, body);
    cJSON_Delete(body);
    device_config_free(&cfg);
    return err;
}

static bool get_required_string(cJSON *obj, const char *key, const char **value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return false;
    }

    *value = item->valuestring;
    return true;
}

static esp_err_t read_request_body(httpd_req_t *req, char **out_body)
{
    char *body = NULL;
    int received = 0;

    if (req->content_len <= 0 || req->content_len > PROV_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    body = calloc(1, req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }

    *out_body = body;
    return ESP_OK;
}

static esp_err_t provision_post(httpd_req_t *req)
{
    char *body = NULL;
    cJSON *root = NULL;
    cJSON *wifi = NULL;
    cJSON *mqtt = NULL;
    cJSON *enrollment = NULL;
    device_provisioning_t prov = {0};
    esp_err_t err;

    err = read_request_body(req, &body);
    if (err != ESP_OK) {
        return send_error(req, "bad length");
    }

    root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_error(req, "invalid json");
    }

    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema_version")) ||
        cJSON_GetObjectItemCaseSensitive(root, "schema_version")->valueint != 1) {
        cJSON_Delete(root);
        return send_error(req, "schema_version");
    }

    wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    enrollment = cJSON_GetObjectItemCaseSensitive(root, "enrollment");
    if (!cJSON_IsObject(wifi) || !cJSON_IsObject(mqtt) || !cJSON_IsObject(enrollment)) {
        cJSON_Delete(root);
        return send_error(req, "missing sections");
    }

    if (!get_required_string(wifi, "ssid", &prov.wifi_ssid) ||
        !get_required_string(wifi, "password", &prov.wifi_pwd) ||
        !get_required_string(mqtt, "broker_uri", &prov.mqtt_uri) ||
        !get_required_string(mqtt, "username", &prov.mqtt_user) ||
        !get_required_string(mqtt, "password", &prov.mqtt_pwd) ||
        !get_required_string(enrollment, "token", &prov.enroll_token)) {
        cJSON_Delete(root);
        return send_error(req, "missing field");
    }

    if (strncmp(prov.mqtt_uri, "mqtts://", 8) != 0) {
        cJSON_Delete(root);
        return send_error(req, "mqtt must use tls");
    }

    err = device_config_store_provisioning(&prov);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_error(req, "persist failed");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, "{\"status\":\"ok\"}", strlen("{\"status\":\"ok\"}"));
    if (err == ESP_OK && s_events) {
        xEventGroupSetBits(s_events, BIT_PROVISIONED);
    }
    return err;
}

static esp_err_t retry_post(httpd_req_t *req)
{
    esp_err_t err;

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, "{\"status\":\"retrying\"}", strlen("{\"status\":\"retrying\"}"));
    if (err == ESP_OK && s_events) {
        xEventGroupSetBits(s_events, BIT_RETRY);
    }
    return err;
}

static esp_err_t reset_post(httpd_req_t *req)
{
    esp_err_t err;

    if (device_config_erase_runtime() != ESP_OK) {
        return send_error(req, "reset failed");
    }

    httpd_resp_set_type(req, "application/json");
    err = httpd_resp_send(req, "{\"status\":\"reset\"}", strlen("{\"status\":\"reset\"}"));
    if (err == ESP_OK && s_events) {
        xEventGroupSetBits(s_events, BIT_RESET);
    }
    return err;
}

esp_err_t http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err;
    static const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get,
    };
    static const httpd_uri_t provision_uri = {
        .uri = "/api/provision",
        .method = HTTP_POST,
        .handler = provision_post,
    };
    static const httpd_uri_t retry_uri = {
        .uri = "/api/retry",
        .method = HTTP_POST,
        .handler = retry_post,
    };
    static const httpd_uri_t reset_uri = {
        .uri = "/api/reset",
        .method = HTTP_POST,
        .handler = reset_post,
    };

    if (s_server) {
        return ESP_OK;
    }

    if (!s_events) {
        s_events = xEventGroupCreate();
        if (!s_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_events, BIT_PROVISIONED | BIT_RETRY | BIT_RESET);

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_register_uri_handler(s_server, &index_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &provision_uri);
    httpd_register_uri_handler(s_server, &retry_uri);
    httpd_register_uri_handler(s_server, &reset_uri);
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }

    if (httpd_stop(s_server) == ESP_OK) {
        s_server = NULL;
        return ESP_OK;
    }
    return ESP_FAIL;
}

http_server_action_t http_server_wait_for_action(TickType_t ticks_to_wait)
{
    EventBits_t bits;

    if (!s_events) {
        return HTTP_SERVER_ACTION_NONE;
    }

    bits = xEventGroupWaitBits(s_events,
                               BIT_PROVISIONED | BIT_RETRY | BIT_RESET,
                               pdTRUE,
                               pdFALSE,
                               ticks_to_wait);
    if (bits & BIT_PROVISIONED) {
        return HTTP_SERVER_ACTION_PROVISIONED;
    }
    if (bits & BIT_RETRY) {
        return HTTP_SERVER_ACTION_RETRY;
    }
    if (bits & BIT_RESET) {
        return HTTP_SERVER_ACTION_RESET;
    }
    return HTTP_SERVER_ACTION_NONE;
}
