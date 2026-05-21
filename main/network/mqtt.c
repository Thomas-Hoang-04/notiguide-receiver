/**
 * @file mqtt.c
 * @brief MQTT Module - Secure Messaging Implementation
 *
 * Implements the MQTTS client lifecycle, topic reassembly, and the
 * receiver-specific bootstrap and operational command protocol.
 */

#include "network/mqtt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"

#define TOPIC_PREFIX CONFIG_RECEIVER_MQTT_TOPIC_PREFIX "/"

#define MQTT_CONNECTED_BIT BIT0
#define MQTT_FAILED_BIT BIT1
#define MQTT_CONNECT_TIMEOUT_MS (45 * 1000)

#define BOOTSTRAP_TIMEOUT_MS (15 * 60 * 1000)
#define BOOTSTRAP_PENDING_BIT BIT0
#define BOOTSTRAP_REJECTED_BIT BIT1
#define BOOTSTRAP_RESULT_BIT BIT2

typedef struct {
    char *registration_nonce;
    char *challenge_id;
    char *public_id;
    char *device_name;
} bootstrap_session_t;

static esp_mqtt_client_handle_t s_client;
static mqtt_message_callback_t s_message_callback;
static void *s_message_ctx;
static EventGroupHandle_t s_mqtt_events;
static char *s_rx_topic;
static char *s_rx_payload;
static int s_rx_total_len;
static device_identity_t *s_receiver_identity;
static const char *s_receiver_firmware_version = "v1";
static EventGroupHandle_t s_bootstrap_events;
static bootstrap_session_t s_bootstrap;

extern const uint8_t mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");
extern const uint8_t mqtt_ca_pem_end[] asm("_binary_mqtt_ca_pem_end");

static bool broker_uses_scheme(const char *uri, const char *scheme)
{
    return strncmp(uri, scheme, strlen(scheme)) == 0;
}

static void bootstrap_session_reset(void)
{
    free(s_bootstrap.registration_nonce);
    free(s_bootstrap.challenge_id);
    free(s_bootstrap.public_id);
    free(s_bootstrap.device_name);
    memset(&s_bootstrap, 0, sizeof(s_bootstrap));
    if (s_bootstrap_events) {
        xEventGroupClearBits(s_bootstrap_events,
                             BOOTSTRAP_PENDING_BIT | BOOTSTRAP_REJECTED_BIT | BOOTSTRAP_RESULT_BIT);
    }
}

static char *dup_json_string(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsString(item) || !item->valuestring) {
        return NULL;
    }

    return strdup(item->valuestring);
}

static bool json_number_value(cJSON *obj, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static const char *mqtt_ca_pem_body(void)
{
    const char *pem = (const char *)mqtt_ca_pem_start;

    while (*pem == ' ' || *pem == '\t' || *pem == '\n' || *pem == '\r') {
        pem++;
    }

    return pem;
}

static bool mqtt_ca_pem_configured(void)
{
    const char *pem = mqtt_ca_pem_body();
    return strncmp(pem, MQTT_CA_CERT_HEADER, strlen(MQTT_CA_CERT_HEADER)) == 0;
}

static size_t mqtt_ca_pem_len(void)
{
    return (size_t)((const char *)mqtt_ca_pem_end - mqtt_ca_pem_body());
}

static void mqtt_reset_rx_buffer(void)
{
    free(s_rx_topic);
    free(s_rx_payload);
    s_rx_topic = NULL;
    s_rx_payload = NULL;
    s_rx_total_len = 0;
}

static void format_applied_at(char *buf, size_t buf_len, const char *fallback)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (buf_len == 0) {
        return;
    }

    if (now <= 0 || gmtime_r(&now, &tm_now) == NULL) {
        snprintf(buf, buf_len, "%s", fallback ? fallback : "1970-01-01T00:00:00Z");
        return;
    }

    strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%SZ", &tm_now);
}

static esp_err_t publish_json(const char *topic, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    esp_err_t err;

    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    err = mqtt_publish(topic, payload, 1, 0);
    free(payload);
    return err;
}

static esp_err_t publish_rf_ack(const char *public_id,
                                uint32_t code_version,
                                const char *status,
                                const char *fallback_time)
{
    cJSON *root = cJSON_CreateObject();
    char topic[128];
    char applied_at[32];
    esp_err_t err;

    if (!root || !public_id || !status) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    format_applied_at(applied_at, sizeof(applied_at), fallback_time);
    snprintf(topic, sizeof(topic), TOPIC_PREFIX "receiver/device/%s/ack", public_id);

    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "ack_for", "rf_code");
    cJSON_AddNumberToObject(root, "code_version", code_version);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "applied_at", applied_at);
    err = publish_json(topic, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t publish_deact_ack(const char *public_id,
                                   const char *command_id,
                                   const char *action,
                                   const char *status,
                                   const char *fallback_time)
{
    cJSON *root = cJSON_CreateObject();
    char topic[128];
    char applied_at[32];
    esp_err_t err;

    if (!root || !public_id || !command_id || !action || !status) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    format_applied_at(applied_at, sizeof(applied_at), fallback_time);
    snprintf(topic, sizeof(topic), TOPIC_PREFIX "receiver/device/%s/ack", public_id);

    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "ack_for", "deact");
    cJSON_AddStringToObject(root, "command_id", command_id);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "applied_at", applied_at);
    err = publish_json(topic, root);
    cJSON_Delete(root);
    return err;
}

static bool parse_rf_code_hex(const char *hex, uint32_t *value_out)
{
    size_t len;
    uint32_t value = 0;

    if (!hex || !value_out) {
        return false;
    }

    len = strlen(hex);
    if (len == 0 || len > 8) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char ch = hex[i];

        if (!isxdigit((unsigned char)ch)) {
            return false;
        }

        value <<= 4;
        if (isdigit((unsigned char)ch)) {
            value |= (uint32_t)(ch - '0');
        } else {
            value |= (uint32_t)(toupper((unsigned char)ch) - 'A' + 10);
        }
    }

    *value_out = value;
    return true;
}

static void uppercase_copy(char *dst, size_t dst_len, const char *src)
{
    size_t i;

    if (!dst_len) {
        return;
    }

    for (i = 0; src[i] && i + 1 < dst_len; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static esp_err_t publish_bootstrap_registration(const device_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    char *public_key_b64 = NULL;
    esp_err_t err;

    if (!root || !cfg || !cfg->enroll_token || !s_bootstrap.registration_nonce || !s_receiver_identity) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    err = device_identity_get_public_key_b64(s_receiver_identity, &public_key_b64);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "hardware_model", "ESP-01");
    cJSON_AddStringToObject(root, "receiver_type", "RECEIVER_433M");
    cJSON_AddStringToObject(root, "firmware_version", s_receiver_firmware_version);
    cJSON_AddStringToObject(root, "public_key_b64", public_key_b64);
    cJSON_AddStringToObject(root, "enrollment_token", cfg->enroll_token);
    cJSON_AddStringToObject(root, "registration_nonce", s_bootstrap.registration_nonce);

    err = publish_json(TOPIC_PREFIX "receiver/bootstrap/register", root);
    cJSON_Delete(root);
    free(public_key_b64);
    return err;
}

static void handle_bootstrap_message(cJSON *root)
{
    char *type = dup_json_string(root, "type");
    char *challenge_id = dup_json_string(root, "challenge_id");

    if (!type || !challenge_id) {
        goto cleanup;
    }

    if (strcmp(type, "response") == 0) {
        goto cleanup;
    }

    if (strcmp(type, "pending") == 0 || strcmp(type, "challenge") == 0) {
        char *registration_nonce = dup_json_string(root, "registration_nonce");

        if (!registration_nonce || !s_bootstrap.registration_nonce ||
            strcmp(registration_nonce, s_bootstrap.registration_nonce) != 0) {
            free(registration_nonce);
            goto cleanup;
        }
        free(registration_nonce);

        if (!s_bootstrap.challenge_id) {
            char wildcard_topic[] = TOPIC_PREFIX "receiver/bootstrap/+";
            char exact_topic[128];

            s_bootstrap.challenge_id = strdup(challenge_id);
            snprintf(exact_topic, sizeof(exact_topic), TOPIC_PREFIX "receiver/bootstrap/%s", challenge_id);
            mqtt_unsubscribe(wildcard_topic);
            mqtt_subscribe(exact_topic, 1);
        }
        xEventGroupSetBits(s_bootstrap_events, BOOTSTRAP_PENDING_BIT);
    }

    if (strcmp(type, "rejected") == 0) {
        xEventGroupSetBits(s_bootstrap_events, BOOTSTRAP_REJECTED_BIT);
        goto cleanup;
    }

    if (strcmp(type, "challenge") == 0) {
        char *nonce = dup_json_string(root, "nonce");
        char *issued_at = dup_json_string(root, "issued_at");
        char *expires_at = dup_json_string(root, "expires_at");
        char *signature_b64 = NULL;
        cJSON *response = NULL;
        char exact_topic[128];

        if (!nonce || !issued_at || !expires_at || !s_bootstrap.challenge_id ||
            strcmp(s_bootstrap.challenge_id, challenge_id) != 0) {
            free(nonce);
            free(issued_at);
            free(expires_at);
            goto cleanup;
        }

        if (device_identity_sign_activation_response(s_receiver_identity,
                                                     challenge_id,
                                                     nonce,
                                                     issued_at,
                                                     expires_at,
                                                     &signature_b64) == ESP_OK) {
            response = cJSON_CreateObject();
            if (response) {
                snprintf(exact_topic, sizeof(exact_topic), TOPIC_PREFIX "receiver/bootstrap/%s", challenge_id);
                cJSON_AddNumberToObject(response, "schema_version", 1);
                cJSON_AddStringToObject(response, "type", "response");
                cJSON_AddStringToObject(response, "challenge_id", challenge_id);
                cJSON_AddStringToObject(response, "signature_b64", signature_b64);
                publish_json(exact_topic, response);
            }
        }

        cJSON_Delete(response);
        free(signature_b64);
        free(nonce);
        free(issued_at);
        free(expires_at);
    }

    if (strcmp(type, "result") == 0) {
        if (!s_bootstrap.challenge_id || strcmp(s_bootstrap.challenge_id, challenge_id) != 0) {
            goto cleanup;
        }
        s_bootstrap.public_id = dup_json_string(root, "public_id");
        s_bootstrap.device_name = dup_json_string(root, "assigned_device_name");
        if (s_bootstrap.public_id && s_bootstrap.device_name) {
            xEventGroupSetBits(s_bootstrap_events, BOOTSTRAP_RESULT_BIT);
        }
    }

cleanup:
    free(type);
    free(challenge_id);
}

static void handle_rf_code_command(cJSON *root)
{
    device_config_t cfg;
    char *rf_code_hex = NULL;
    char *issued_at = NULL;
    char *signature_b64 = NULL;
    char canonical[256];
    char rf_code_hex_upper[9];
    bool verified = false;
    uint32_t rf_code = 0;
    int rf_code_bits = 0;
    int code_version = 0;

    device_config_init(&cfg);
    if (device_config_load(&cfg) != ESP_OK || !cfg.has_public_id) {
        goto cleanup;
    }

    if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(root, "schema_version")) ||
        cJSON_GetObjectItemCaseSensitive(root, "schema_version")->valueint != 1) {
        publish_rf_ack(cfg.public_id, 0, "rejected", NULL);
        goto cleanup;
    }

    rf_code_hex = dup_json_string(root, "rf_code_hex");
    issued_at = dup_json_string(root, "issued_at");
    signature_b64 = dup_json_string(root, "signature_b64");
    if (!rf_code_hex || !issued_at || !signature_b64 ||
        !json_number_value(root, "rf_code_bits", &rf_code_bits) ||
        !json_number_value(root, "code_version", &code_version) ||
        rf_code_bits < 1 || rf_code_bits > 32 ||
        !parse_rf_code_hex(rf_code_hex, &rf_code)) {
        publish_rf_ack(cfg.public_id, 0, "rejected", issued_at);
        goto cleanup;
    }

    uppercase_copy(rf_code_hex_upper, sizeof(rf_code_hex_upper), rf_code_hex);
    snprintf(canonical,
             sizeof(canonical),
             "rf-code-v1|%s|%d|%s|%d|%s",
             cfg.public_id,
             code_version,
             rf_code_hex_upper,
             rf_code_bits,
             issued_at);
    if (device_identity_verify_signature(s_receiver_identity, canonical, signature_b64, &verified) != ESP_OK ||
        !verified) {
        publish_rf_ack(cfg.public_id, (uint32_t)code_version, "rejected", issued_at);
        goto cleanup;
    }

    if (cfg.has_rf_code && (uint32_t)code_version < cfg.rf_code_ver) {
        publish_rf_ack(cfg.public_id, (uint32_t)code_version, "rejected", issued_at);
        goto cleanup;
    }
    if (cfg.has_rf_code && (uint32_t)code_version == cfg.rf_code_ver) {
        publish_rf_ack(cfg.public_id, (uint32_t)code_version, "unchanged", issued_at);
        goto cleanup;
    }

    if (cfg.has_op_state && cfg.op_state == RECEIVER_OP_STATE_PENDING_RF_CODE) {
        if (rf_sup_start() != ESP_OK) {
            publish_rf_ack(cfg.public_id, (uint32_t)code_version, "rejected", issued_at);
            goto cleanup;
        }
    }

    if (device_config_store_rf_code(rf_code,
                                    (uint8_t)rf_code_bits,
                                    (uint32_t)code_version,
                                    cfg.has_op_state && cfg.op_state == RECEIVER_OP_STATE_PENDING_RF_CODE,
                                    RECEIVER_OP_STATE_ACTIVE) != ESP_OK) {
        publish_rf_ack(cfg.public_id, (uint32_t)code_version, "rejected", issued_at);
        goto cleanup;
    }

    rf_trigger_stop_output();
    rf_trigger_set(rf_code, (uint8_t)rf_code_bits, (uint32_t)code_version);
    publish_rf_ack(cfg.public_id, (uint32_t)code_version, "applied", issued_at);

cleanup:
    free(rf_code_hex);
    free(issued_at);
    free(signature_b64);
    device_config_free(&cfg);
}

static void handle_deact_command(cJSON *root)
{
    device_config_t cfg;
    char *action = NULL;
    char *command_id = NULL;
    char *issued_at = NULL;
    char *signature_b64 = NULL;
    char canonical[256];
    bool verified = false;
    receiver_op_state_t next_state;
    esp_err_t err = ESP_OK;

    device_config_init(&cfg);
    if (device_config_load(&cfg) != ESP_OK || !cfg.has_public_id) {
        goto cleanup;
    }

    action = dup_json_string(root, "action");
    command_id = dup_json_string(root, "command_id");
    issued_at = dup_json_string(root, "issued_at");
    signature_b64 = dup_json_string(root, "signature_b64");
    if (!action || !command_id || !issued_at || !signature_b64) {
        publish_deact_ack(cfg.public_id, command_id ? command_id : "", action ? action : "", "rejected", issued_at);
        goto cleanup;
    }

    snprintf(canonical,
             sizeof(canonical),
             "deact-v1|%s|%s|%s|%s",
             cfg.public_id,
             command_id,
             action,
             issued_at);
    if (device_identity_verify_signature(s_receiver_identity, canonical, signature_b64, &verified) != ESP_OK ||
        !verified) {
        publish_deact_ack(cfg.public_id, command_id, action, "rejected", issued_at);
        goto cleanup;
    }

    if (cfg.has_last_deact_id && strcmp(cfg.last_deact_id, command_id) == 0) {
        publish_deact_ack(cfg.public_id, command_id, action, "ok", issued_at);
        goto cleanup;
    }

    if (cfg.has_op_state && cfg.op_state == RECEIVER_OP_STATE_DECOMMISSIONED &&
        strcmp(action, "decommission") != 0) {
        publish_deact_ack(cfg.public_id, command_id, action, "ignored", issued_at);
        goto cleanup;
    }

    if (strcmp(action, "suspend") == 0) {
        err = rf_sup_suspend();
        next_state = RECEIVER_OP_STATE_SUSPENDED;
    } else if (strcmp(action, "resume") == 0) {
        if (cfg.has_rf_code && cfg.rf_code_bits > 0) {
            err = rf_sup_resume();
            next_state = RECEIVER_OP_STATE_ACTIVE;
        } else {
            next_state = RECEIVER_OP_STATE_PENDING_RF_CODE;
        }
    } else if (strcmp(action, "decommission") == 0) {
        err = rf_sup_delete();
        next_state = RECEIVER_OP_STATE_DECOMMISSIONED;
    } else {
        publish_deact_ack(cfg.public_id, command_id, action, "rejected", issued_at);
        goto cleanup;
    }

    if (err != ESP_OK || device_config_store_deactivation(next_state, command_id) != ESP_OK) {
        publish_deact_ack(cfg.public_id, command_id, action, "rejected", issued_at);
        goto cleanup;
    }

    if (next_state != RECEIVER_OP_STATE_ACTIVE) {
        rf_trigger_stop_output();
    }
    publish_deact_ack(cfg.public_id, command_id, action, "ok", issued_at);

cleanup:
    free(action);
    free(command_id);
    free(issued_at);
    free(signature_b64);
    device_config_free(&cfg);
}

static void on_mqtt_message(const char *topic, const char *payload, int payload_len, void *ctx)
{
    cJSON *root;

    (void)payload_len;
    (void)ctx;

    root = cJSON_Parse(payload);
    if (!root) {
        return;
    }

    if (strncmp(topic, TOPIC_PREFIX "receiver/bootstrap/", sizeof(TOPIC_PREFIX "receiver/bootstrap/") - 1) == 0) {
        handle_bootstrap_message(root);
    } else if (strstr(topic, "/cmd/rf_code") != NULL) {
        handle_rf_code_command(root);
    } else if (strstr(topic, "/cmd/deact") != NULL) {
        handle_deact_command(root);
    }

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)args;
    (void)base;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(MQTT_TAG, "MQTT connected");
        if (s_mqtt_events) {
            xEventGroupClearBits(s_mqtt_events, MQTT_FAILED_BIT);
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(MQTT_TAG, "MQTT disconnected");
        if (s_mqtt_events) {
            EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            if ((bits & MQTT_CONNECTED_BIT) == 0) {
                xEventGroupSetBits(s_mqtt_events, MQTT_FAILED_BIT);
            }
        }
        mqtt_reset_rx_buffer();
        break;
    case MQTT_EVENT_DATA: {
        int final_offset;

        if (event->current_data_offset == 0) {
            mqtt_reset_rx_buffer();
            s_rx_topic = calloc(1, event->topic_len + 1);
            s_rx_payload = calloc(1, event->total_data_len + 1);
            if (!s_rx_topic || !s_rx_payload) {
                mqtt_reset_rx_buffer();
                return;
            }
            memcpy(s_rx_topic, event->topic, event->topic_len);
            s_rx_total_len = event->total_data_len;
        }

        if (!s_rx_payload || event->current_data_offset + event->data_len > s_rx_total_len) {
            mqtt_reset_rx_buffer();
            return;
        }

        memcpy(s_rx_payload + event->current_data_offset, event->data, event->data_len);
        final_offset = event->current_data_offset + event->data_len;
        if (final_offset == s_rx_total_len && s_message_callback) {
            s_message_callback(s_rx_topic, s_rx_payload, s_rx_total_len, s_message_ctx);
            mqtt_reset_rx_buffer();
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(MQTT_TAG, "MQTT error type=%d tls=0x%x stack=%d conn=%d",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->connect_return_code);
        }
        if (s_mqtt_events) {
            EventBits_t bits = xEventGroupGetBits(s_mqtt_events);
            if ((bits & MQTT_CONNECTED_BIT) == 0) {
                xEventGroupSetBits(s_mqtt_events, MQTT_FAILED_BIT);
            }
        }
        break;
    default:
        break;
    }
}

esp_err_t mqtt_start(const device_config_t *cfg,
                     mqtt_message_callback_t message_cb,
                     void *message_ctx)
{
    if (!cfg || !cfg->mqtt_uri || !cfg->mqtt_user || !cfg->mqtt_pwd) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!broker_uses_scheme(cfg->mqtt_uri, MQTT_SCHEME_SSL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mqtt_ca_pem_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_message_callback = message_cb;
    s_message_ctx = message_ctx;

    if (!s_mqtt_events) {
        s_mqtt_events = xEventGroupCreate();
        if (!s_mqtt_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_FAILED_BIT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = cfg->mqtt_uri,
        .client_id = cfg->has_public_id ? cfg->public_id : NULL,
        .username = cfg->mqtt_user,
        .password = cfg->mqtt_pwd,
        .transport = MQTT_TRANSPORT_OVER_SSL,
        .cert_pem = mqtt_ca_pem_body(),
        .cert_len = mqtt_ca_pem_len(),
        .keepalive = 120,
        .buffer_size = 2048,
        .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .reconnect_timeout_ms = 5000,
    };

    ESP_LOGI(MQTT_TAG, "Connecting to %s (client_id=%s)",
             cfg->mqtt_uri, cfg->has_public_id ? cfg->public_id : "auto");
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(MQTT_TAG, "Client init failed");
        return ESP_FAIL;
    }

    if (esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(MQTT_TAG, "Event registration failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(MQTT_TAG, "Client start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    {
        EventBits_t bits = xEventGroupWaitBits(s_mqtt_events,
                                               MQTT_CONNECTED_BIT | MQTT_FAILED_BIT,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(MQTT_CONNECT_TIMEOUT_MS));
        if ((bits & MQTT_CONNECTED_BIT) != 0) {
            return ESP_OK;
        }

        ESP_LOGE(MQTT_TAG, "Connection %s (timeout=%dms)",
                 (bits & MQTT_FAILED_BIT) ? "rejected" : "timed out",
                 MQTT_CONNECT_TIMEOUT_MS);
        mqtt_stop();
        return (bits & MQTT_FAILED_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }
}

esp_err_t mqtt_stop(void)
{
    if (!s_client) {
        bootstrap_session_reset();
        return ESP_OK;
    }

    mqtt_reset_rx_buffer();
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_message_callback = NULL;
    s_message_ctx = NULL;
    if (s_mqtt_events) {
        xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_FAILED_BIT);
    }
    bootstrap_session_reset();
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return s_mqtt_events && (xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT);
}

esp_err_t mqtt_subscribe(const char *topic, int qos)
{
    if (!s_client || !topic) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_subscribe(s_client, topic, qos) >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_unsubscribe(const char *topic)
{
    if (!s_client || !topic) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_unsubscribe(s_client, topic) >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_publish(const char *topic,
                       const char *payload,
                       int qos,
                       int retain)
{
    if (!s_client || !topic || !payload) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain) >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_receiver_init(device_identity_t *identity, const char *firmware_version)
{
    if (!identity) {
        return ESP_ERR_INVALID_ARG;
    }

    s_receiver_identity = identity;
    s_receiver_firmware_version = firmware_version ? firmware_version : "dev";

    if (!s_bootstrap_events) {
        s_bootstrap_events = xEventGroupCreate();
        if (!s_bootstrap_events) {
            return ESP_ERR_NO_MEM;
        }
    }

    bootstrap_session_reset();
    return ESP_OK;
}

esp_err_t mqtt_receiver_start(const device_config_t *cfg)
{
    if (!s_receiver_identity || !s_bootstrap_events) {
        return ESP_ERR_INVALID_STATE;
    }

    return mqtt_start(cfg, on_mqtt_message, NULL);
}

/* ESP-MQTT only applies client_id during init, so activation requires a new session. */
esp_err_t mqtt_receiver_restart_with_public_id(const device_config_t *cfg)
{
    if (!cfg || !cfg->has_public_id || !cfg->public_id) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(MQTT_TAG, "Restarting MQTT session with public_id");
    if (mqtt_stop() != ESP_OK) {
        return ESP_FAIL;
    }

    return mqtt_receiver_start(cfg);
}

esp_err_t mqtt_receiver_subscribe_commands(const char *public_id)
{
    char topic[128];

    if (!public_id) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(topic, sizeof(topic), TOPIC_PREFIX "receiver/device/%s/cmd/#", public_id);
    ESP_LOGI(MQTT_TAG, "Subscribing: %s", topic);
    return mqtt_subscribe(topic, 1);
}

esp_err_t mqtt_receiver_bootstrap_activate(const device_config_t *cfg)
{
    EventBits_t bits;

    if (!cfg || !s_receiver_identity || !s_bootstrap_events) {
        return ESP_ERR_INVALID_STATE;
    }

    bootstrap_session_reset();
    if (device_identity_make_registration_nonce(&s_bootstrap.registration_nonce) != ESP_OK) {
        return ESP_FAIL;
    }

    if (mqtt_subscribe(TOPIC_PREFIX "receiver/bootstrap/+", 1) != ESP_OK) {
        return ESP_FAIL;
    }
    if (publish_bootstrap_registration(cfg) != ESP_OK) {
        return ESP_FAIL;
    }

    bits = xEventGroupWaitBits(s_bootstrap_events,
                               BOOTSTRAP_REJECTED_BIT | BOOTSTRAP_RESULT_BIT,
                               pdTRUE,
                               pdFALSE,
                               pdMS_TO_TICKS(BOOTSTRAP_TIMEOUT_MS));
    if (bits & BOOTSTRAP_RESULT_BIT) {
        esp_err_t err = device_config_store_activation(s_bootstrap.public_id,
                                                       s_bootstrap.device_name,
                                                       RECEIVER_OP_STATE_PENDING_RF_CODE);
        char challenge_topic[128];

        snprintf(challenge_topic, sizeof(challenge_topic), TOPIC_PREFIX "receiver/bootstrap/%s", s_bootstrap.challenge_id);
        mqtt_unsubscribe(challenge_topic);
        bootstrap_session_reset();
        return err;
    }

    device_config_clear_enroll_token();
    bootstrap_session_reset();
    return ESP_ERR_TIMEOUT;
}
