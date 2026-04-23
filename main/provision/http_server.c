/**
 * @file http_server.c
 * @brief Provisioning HTTP server and local recovery UI.
 */

#include "provision/http_server.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "http_parser.h"
#include "network/wifi.h"

#define HTTP_SERVER_TAG "HTTP_PROV"

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static httpd_handle_t s_server;
static provision_http_context_t s_ctx;

static const char *boot_state_string(device_boot_state_t state)
{
    switch (state) {
    case DEVICE_BOOT_STATE_UNPROVISIONED:
        return "UNPROVISIONED";
    case DEVICE_BOOT_STATE_PENDING_ACTIVATION:
        return "PENDING_ACTIVATION";
    case DEVICE_BOOT_STATE_RECOVERY_REQUIRED:
        return "RECOVERY_REQUIRED";
    case DEVICE_BOOT_STATE_OPERATIONAL:
        return "OPERATIONAL";
    default:
        return "UNKNOWN";
    }
}

const char *provision_reason_string(provision_reason_t reason)
{
    switch (reason) {
    case PROVISION_REASON_UNPROVISIONED:
        return "unprovisioned";
    case PROVISION_REASON_WIFI_FAILED:
        return "wifi_failed";
    case PROVISION_REASON_RECOVERY_REQUIRED:
        return "recovery_required";
    case PROVISION_REASON_BOOTSTRAP_FAILED:
        return "bootstrap_failed";
    default:
        return "unknown";
    }
}

static esp_err_t send_json(httpd_req_t *req, int code, cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG,
                        "cJSON_PrintUnformatted failed");

    httpd_resp_set_status(req, code == 200 ? "200 OK" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to create JSON");
    cJSON_AddBoolToObject(json, "ok", false);
    cJSON_AddStringToObject(json, "message", message);
    esp_err_t err = send_json(req, 400, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t read_request_body(httpd_req_t *req, char **out)
{
    ESP_RETURN_ON_FALSE(req != NULL && out != NULL, ESP_ERR_INVALID_ARG, HTTP_SERVER_TAG,
                        "invalid request");
    ESP_RETURN_ON_FALSE(req->content_len > 0, ESP_ERR_INVALID_SIZE, HTTP_SERVER_TAG,
                        "empty body");

    char *body = calloc(1, (size_t)req->content_len + 1U);
    ESP_RETURN_ON_FALSE(body != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to alloc body");

    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - (int)received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    body[received] = '\0';
    *out = body;
    return ESP_OK;
}

static bool copy_json_string(cJSON *parent, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    if (strlen(item->valuestring) >= out_len) {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static bool string_has_content(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static esp_err_t parse_provision_request(const char *body, device_provisioning_t *prov)
{
    cJSON *json = cJSON_Parse(body);
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_INVALID_ARG, HTTP_SERVER_TAG, "invalid JSON");

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema_version");
    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(json, "wifi");
    cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(json, "mqtt");
    cJSON *enrollment = cJSON_GetObjectItemCaseSensitive(json, "enrollment");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!cJSON_IsNumber(schema) || schema->valueint != DEVICE_CONFIG_SCHEMA_VERSION) {
        goto cleanup;
    }
    if (!cJSON_IsObject(wifi) || !cJSON_IsObject(mqtt) || !cJSON_IsObject(enrollment)) {
        goto cleanup;
    }
    if (!copy_json_string(wifi, "ssid", prov->wifi_ssid, sizeof(prov->wifi_ssid)) ||
        !copy_json_string(wifi, "password", prov->wifi_pwd, sizeof(prov->wifi_pwd)) ||
        !copy_json_string(mqtt, "broker_uri", prov->mqtt_uri, sizeof(prov->mqtt_uri)) ||
        !copy_json_string(mqtt, "username", prov->mqtt_user, sizeof(prov->mqtt_user)) ||
        !copy_json_string(mqtt, "password", prov->mqtt_pwd, sizeof(prov->mqtt_pwd)) ||
        !copy_json_string(enrollment, "token", prov->enroll_token, sizeof(prov->enroll_token))) {
        goto cleanup;
    }
    if (!string_has_content(prov->wifi_ssid) ||
        !string_has_content(prov->wifi_pwd) ||
        !string_has_content(prov->mqtt_uri) ||
        !string_has_content(prov->mqtt_user) ||
        !string_has_content(prov->mqtt_pwd) ||
        !string_has_content(prov->enroll_token)) {
        goto cleanup;
    }
    if (strncmp(prov->mqtt_uri, "mqtts://", 8) != 0) {
        goto cleanup;
    }
    err = ESP_OK;

cleanup:
    cJSON_Delete(json);
    return err;
}

static esp_err_t handle_index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req,
                           (const char *)index_html_gz_start,
                           (ssize_t)(index_html_gz_end - index_html_gz_start));
}

static esp_err_t handle_status_get(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to create JSON");

    device_boot_state_t boot_state = device_config_boot_state(s_ctx.cfg);
    cJSON_AddNumberToObject(json, "schema_version", DEVICE_CONFIG_SCHEMA_VERSION);
    cJSON_AddStringToObject(json, "recovery_reason", provision_reason_string(s_ctx.reason));
    cJSON_AddStringToObject(json, "boot_state", boot_state_string(boot_state));
    cJSON_AddStringToObject(json, "compiled_receiver_type",
                            device_config_receiver_type_string(device_config_compiled_receiver_type()));
    cJSON_AddStringToObject(json, "softap_ssid", wifi_get_softap_ssid());
    cJSON_AddBoolToObject(json, "has_enrollment_token", s_ctx.cfg->has_enroll_token);
    cJSON_AddBoolToObject(json, "has_public_id", s_ctx.cfg->has_public_id);
    cJSON_AddStringToObject(json, "public_id", s_ctx.cfg->has_public_id ? s_ctx.cfg->public_id : "");
    cJSON_AddStringToObject(json, "device_name", s_ctx.cfg->has_device_name ? s_ctx.cfg->device_name : "");
    cJSON_AddStringToObject(json, "op_state",
                            s_ctx.cfg->has_op_state ? device_config_op_state_string(s_ctx.cfg->op_state) : "NONE");

    esp_err_t err = send_json(req, 200, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t handle_provision_post(httpd_req_t *req)
{
    char *body = NULL;
    device_provisioning_t prov = { 0 };

    esp_err_t err = read_request_body(req, &body);
    if (err != ESP_OK) {
        return send_error(req, "request body missing");
    }
    err = parse_provision_request(body, &prov);
    free(body);
    if (err != ESP_OK) {
        return send_error(req, "invalid provisioning payload");
    }
    err = device_config_save_provisioning(s_ctx.cfg, &prov);
    if (err != ESP_OK) {
        return send_error(req, "failed to save provisioning");
    }

    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to create JSON");
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", "Configuration saved. Rebooting.");
    err = send_json(req, 200, json);
    cJSON_Delete(json);

    if (err == ESP_OK) {
        xEventGroupSetBits(s_ctx.events, PROVISION_EVENT_DONE);
    }
    return err;
}

static esp_err_t handle_retry_post(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to create JSON");
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", "Retry scheduled.");
    esp_err_t err = send_json(req, 200, json);
    cJSON_Delete(json);
    if (err == ESP_OK) {
        xEventGroupSetBits(s_ctx.events, PROVISION_EVENT_RETRY);
    }
    return err;
}

static esp_err_t handle_reset_post(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, HTTP_SERVER_TAG, "failed to create JSON");
    cJSON_AddBoolToObject(json, "ok", true);
    cJSON_AddStringToObject(json, "message", "Factory reset scheduled.");
    esp_err_t err = send_json(req, 200, json);
    cJSON_Delete(json);
    if (err == ESP_OK) {
        xEventGroupSetBits(s_ctx.events, PROVISION_EVENT_RESET);
    }
    return err;
}

esp_err_t provision_http_server_start(const provision_http_context_t *ctx)
{
    ESP_RETURN_ON_FALSE(ctx != NULL && ctx->events != NULL && ctx->cfg != NULL,
                        ESP_ERR_INVALID_ARG, HTTP_SERVER_TAG, "invalid server context");

    if (s_server != NULL) {
        return ESP_OK;
    }

    s_ctx = *ctx;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), HTTP_SERVER_TAG, "httpd_start failed");

    const httpd_uri_t handlers[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = handle_index_get,     .user_ctx = NULL },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = handle_status_get,    .user_ctx = NULL },
        { .uri = "/api/provision", .method = HTTP_POST, .handler = handle_provision_post, .user_ctx = NULL },
        { .uri = "/api/retry",   .method = HTTP_POST, .handler = handle_retry_post,    .user_ctx = NULL },
        { .uri = "/api/reset",   .method = HTTP_POST, .handler = handle_reset_post,    .user_ctx = NULL },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), HTTP_SERVER_TAG,
                            "failed to register URI handler");
    }

    return ESP_OK;
}

esp_err_t provision_http_server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    memset(&s_ctx, 0, sizeof(s_ctx));
    return err;
}
