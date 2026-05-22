/**
 * @file mqtt.c
 * @brief MQTT bootstrap and operational command handling.
 */

#include "network/mqtt.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "security/device_identity.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "sdkconfig.h"

#define MQTT_TAG "MQTT"
#define TOPIC_PREFIX CONFIG_RECEIVER_MQTT_TOPIC_PREFIX "/"
#define MQTT_CONNECTED_BIT          BIT0
#define MQTT_BOOTSTRAP_DONE_BIT     BIT1
#define MQTT_BOOTSTRAP_REJECTED_BIT BIT2
#define MQTT_BOOTSTRAP_FAILED_BIT   BIT3

/* Buffer large enough for any MQTT topic that embeds `public_id` (up to
 * DEVICE_CONFIG_MAX_PUBLIC_ID_LEN) plus our longest fixed prefix/suffix
 * (e.g. TOPIC_PREFIX "receiver/device/<public_id>/cmd/rf_code"). */
#define MQTT_TOPIC_BUF_LEN          (DEVICE_CONFIG_MAX_PUBLIC_ID_LEN + 64U)

/* Buffer large enough for the deact-v1 canonical string, which embeds
 * `public_id` plus a command_id, action and issued_at from the payload. */
#define MQTT_DEACT_CANONICAL_LEN    (DEVICE_CONFIG_MAX_PUBLIC_ID_LEN + 128U)

extern const uint8_t mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");

typedef enum {
    MQTT_PHASE_BOOTSTRAP = 0,
    MQTT_PHASE_OPERATIONAL,
} receiver_mqtt_phase_t;

typedef struct {
    char *topic;
    char *payload;
    int total_len;
} mqtt_fragment_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    EventGroupHandle_t events;
    TaskHandle_t ctl_task;
    device_config_t *cfg;
    device_identity_t *identity;
    receiver_mqtt_phase_t phase;
    mqtt_fragment_t fragment;
    char registration_nonce[32];
    char challenge_id[64];
    char bootstrap_topic[128];
    bool connected;
    bool pending_operational_restart;
    bool time_base_valid;
    int64_t time_base_us;
    int64_t time_base_epoch;
} receiver_mqtt_ctx_t;

static receiver_mqtt_ctx_t s_mqtt;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

static char *mqtt_dup_span(const char *src, size_t len)
{
    char *copy = calloc(1, len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

static void mqtt_clear_fragment(void)
{
    free(s_mqtt.fragment.topic);
    free(s_mqtt.fragment.payload);
    memset(&s_mqtt.fragment, 0, sizeof(s_mqtt.fragment));
}

static bool is_leap_year(int year)
{
    return ((year % 4) == 0 && (year % 100) != 0) || ((year % 400) == 0);
}

static bool parse_decimal_2(const char *in, int *out)
{
    if (in[0] < '0' || in[0] > '9' || in[1] < '0' || in[1] > '9') {
        return false;
    }
    *out = (in[0] - '0') * 10 + (in[1] - '0');
    return true;
}

static bool parse_decimal_4(const char *in, int *out)
{
    for (int i = 0; i < 4; ++i) {
        if (in[i] < '0' || in[i] > '9') {
            return false;
        }
    }
    *out = (in[0] - '0') * 1000 + (in[1] - '0') * 100 + (in[2] - '0') * 10 + (in[3] - '0');
    return true;
}

static bool parse_iso8601_utc(const char *timestamp, int64_t *epoch_seconds)
{
    static const int days_before_month[2][12] = {
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
        { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
    };

    if (timestamp == NULL || strlen(timestamp) != 20 ||
        timestamp[4] != '-' || timestamp[7] != '-' || timestamp[10] != 'T' ||
        timestamp[13] != ':' || timestamp[16] != ':' || timestamp[19] != 'Z') {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_decimal_4(timestamp, &year) ||
        !parse_decimal_2(&timestamp[5], &month) ||
        !parse_decimal_2(&timestamp[8], &day) ||
        !parse_decimal_2(&timestamp[11], &hour) ||
        !parse_decimal_2(&timestamp[14], &minute) ||
        !parse_decimal_2(&timestamp[17], &second)) {
        return false;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }

    int64_t days = 0;
    for (int y = 1970; y < year; ++y) {
        days += is_leap_year(y) ? 366 : 365;
    }
    days += days_before_month[is_leap_year(year) ? 1 : 0][month - 1];
    days += day - 1;

    *epoch_seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

static void mqtt_update_time_base(const char *issued_at)
{
    int64_t epoch = 0;
    if (parse_iso8601_utc(issued_at, &epoch)) {
        s_mqtt.time_base_epoch = epoch;
        s_mqtt.time_base_us = esp_timer_get_time();
        s_mqtt.time_base_valid = true;
    }
}

static void mqtt_format_now_iso8601(char *out, size_t out_len)
{
    if (!s_mqtt.time_base_valid) {
        strlcpy(out, "1970-01-01T00:00:00Z", out_len);
        return;
    }

    int64_t epoch = s_mqtt.time_base_epoch + ((esp_timer_get_time() - s_mqtt.time_base_us) / 1000000LL);
    time_t now = (time_t)epoch;
    struct tm tm_info = { 0 };
    gmtime_r(&now, &tm_info);
    strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_info);
}

static esp_err_t mqtt_publish_json(const char *topic, cJSON *json, int qos, int retain)
{
    ESP_RETURN_ON_FALSE(s_mqtt.client != NULL, ESP_ERR_INVALID_STATE, MQTT_TAG,
                        "MQTT client is not running");

    char *payload = cJSON_PrintUnformatted(json);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, MQTT_TAG, "failed to serialize JSON");
    int msg_id = esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, qos, retain);
    cJSON_free(payload);
    return msg_id >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t mqtt_publish_register(void)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, MQTT_TAG, "failed to create JSON");

    cJSON_AddNumberToObject(json, "schema_version", DEVICE_CONFIG_SCHEMA_VERSION);
    cJSON_AddStringToObject(json, "hardware_model", "ESP32-C3");
    cJSON_AddStringToObject(json, "receiver_type",
                            device_config_receiver_type_string(device_config_compiled_receiver_type()));
    cJSON_AddStringToObject(json, "firmware_version", CONFIG_RECEIVER_FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "public_key_b64",
                            device_identity_public_key_b64(s_mqtt.identity));
    cJSON_AddStringToObject(json, "enrollment_token", s_mqtt.cfg->enroll_token);
    cJSON_AddStringToObject(json, "registration_nonce", s_mqtt.registration_nonce);

    esp_err_t err = mqtt_publish_json(TOPIC_PREFIX "receiver/bootstrap/register", json, 1, 0);
    cJSON_Delete(json);
    return err;
}

static esp_err_t mqtt_publish_bootstrap_response(const char *signature_b64)
{
    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, MQTT_TAG, "failed to create JSON");

    cJSON_AddNumberToObject(json, "schema_version", DEVICE_CONFIG_SCHEMA_VERSION);
    cJSON_AddStringToObject(json, "type", "response");
    cJSON_AddStringToObject(json, "challenge_id", s_mqtt.challenge_id);
    cJSON_AddStringToObject(json, "signature_b64", signature_b64);

    esp_err_t err = mqtt_publish_json(s_mqtt.bootstrap_topic, json, 1, 0);
    cJSON_Delete(json);
    return err;
}

static esp_err_t mqtt_publish_rf_ack(const char *status, uint32_t version)
{
    char ack_topic[MQTT_TOPIC_BUF_LEN];
    char applied_at[32];
    mqtt_format_now_iso8601(applied_at, sizeof(applied_at));
    snprintf(ack_topic, sizeof(ack_topic), TOPIC_PREFIX "receiver/device/%s/ack", s_mqtt.cfg->public_id);

    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, MQTT_TAG, "failed to create JSON");
    cJSON_AddNumberToObject(json, "schema_version", DEVICE_CONFIG_SCHEMA_VERSION);
    cJSON_AddStringToObject(json, "ack_for", "rf_code");
    cJSON_AddNumberToObject(json, "code_version", (double)version);
    cJSON_AddStringToObject(json, "status", status);
    cJSON_AddStringToObject(json, "applied_at", applied_at);

    esp_err_t err = mqtt_publish_json(ack_topic, json, 1, 0);
    cJSON_Delete(json);
    return err;
}

static esp_err_t mqtt_publish_deact_ack(const char *command_id, const char *action, const char *status)
{
    char ack_topic[MQTT_TOPIC_BUF_LEN];
    char applied_at[32];
    mqtt_format_now_iso8601(applied_at, sizeof(applied_at));
    snprintf(ack_topic, sizeof(ack_topic), TOPIC_PREFIX "receiver/device/%s/ack", s_mqtt.cfg->public_id);

    cJSON *json = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, MQTT_TAG, "failed to create JSON");
    cJSON_AddNumberToObject(json, "schema_version", DEVICE_CONFIG_SCHEMA_VERSION);
    cJSON_AddStringToObject(json, "ack_for", "deact");
    cJSON_AddStringToObject(json, "command_id", command_id);
    cJSON_AddStringToObject(json, "action", action);
    cJSON_AddStringToObject(json, "status", status);
    cJSON_AddStringToObject(json, "applied_at", applied_at);

    esp_err_t err = mqtt_publish_json(ack_topic, json, 1, 0);
    cJSON_Delete(json);
    return err;
}

static bool mqtt_get_string(cJSON *parent, const char *key, const char **out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static bool mqtt_get_u32(cJSON *parent, const char *key, uint32_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static esp_err_t mqtt_decode_hex(const char *hex, uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t hex_len = strlen(hex);
    ESP_RETURN_ON_FALSE((hex_len % 2U) == 0U, ESP_ERR_INVALID_ARG, MQTT_TAG, "hex length must be even");
    ESP_RETURN_ON_FALSE((hex_len / 2U) <= out_size, ESP_ERR_INVALID_SIZE, MQTT_TAG, "hex too long");

    for (size_t i = 0; i < hex_len / 2U; ++i) {
        unsigned int byte = 0;
        if (sscanf(&hex[i * 2U], "%2x", &byte) != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)byte;
    }
    *out_len = hex_len / 2U;
    return ESP_OK;
}

static void mqtt_subscribe_current_phase(void)
{
    if (s_mqtt.client == NULL) {
        return;
    }

    if (s_mqtt.phase == MQTT_PHASE_BOOTSTRAP) {
        const char *topic = s_mqtt.bootstrap_topic[0] != '\0'
                          ? s_mqtt.bootstrap_topic
                          : TOPIC_PREFIX "receiver/bootstrap/+";
        esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    } else {
        char topic[MQTT_TOPIC_BUF_LEN];
        snprintf(topic, sizeof(topic), TOPIC_PREFIX "receiver/device/%s/cmd/#", s_mqtt.cfg->public_id);
        esp_mqtt_client_subscribe(s_mqtt.client, topic, 1);
    }
}

static esp_err_t mqtt_start_internal(void)
{
    ESP_RETURN_ON_FALSE(s_mqtt.cfg != NULL, ESP_ERR_INVALID_STATE, MQTT_TAG, "config not bound");

    s_mqtt.phase = s_mqtt.cfg->has_public_id ? MQTT_PHASE_OPERATIONAL : MQTT_PHASE_BOOTSTRAP;
    xEventGroupClearBits(s_mqtt.events,
                         MQTT_CONNECTED_BIT | MQTT_BOOTSTRAP_DONE_BIT |
                         MQTT_BOOTSTRAP_REJECTED_BIT | MQTT_BOOTSTRAP_FAILED_BIT);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {
                .uri = s_mqtt.cfg->mqtt_uri,
            },
            .verification = {
                .certificate = (const char *)mqtt_ca_pem_start,
                .certificate_len = 0,
            },
        },
        .credentials = {
            .username = s_mqtt.cfg->mqtt_user,
            .client_id = s_mqtt.cfg->has_public_id ? s_mqtt.cfg->public_id : NULL,
            .set_null_client_id = false,
            .authentication = {
                .password = s_mqtt.cfg->mqtt_pwd,
            },
        },
        .session = {
            .keepalive = 120,
            .protocol_ver = MQTT_PROTOCOL_V_5,
            .message_retransmit_timeout = 1000,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms = 15000,
            .refresh_connection_after_ms = 5 * 60 * 1000,
        },
        .task = {
            .priority = 7,
            .stack_size = 5120,
        },
        .buffer = {
            .size = 2048,
        },
    };

    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_mqtt.client != NULL, ESP_FAIL, MQTT_TAG, "esp_mqtt_client_init failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID,
                                                       (esp_event_handler_t)mqtt_event_handler, NULL),
                        MQTT_TAG, "failed to register MQTT events");
    return esp_mqtt_client_start(s_mqtt.client);
}

static void mqtt_destroy_client(void)
{
    mqtt_clear_fragment();

    if (s_mqtt.client != NULL) {
        esp_mqtt_client_stop(s_mqtt.client);
        esp_mqtt_client_destroy(s_mqtt.client);
        s_mqtt.client = NULL;
    }
    s_mqtt.connected = false;
    xEventGroupClearBits(s_mqtt.events, MQTT_CONNECTED_BIT);
}

static void mqtt_control_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        mqtt_destroy_client();
        if (mqtt_start_internal() != ESP_OK) {
            xEventGroupSetBits(s_mqtt.events, MQTT_BOOTSTRAP_FAILED_BIT);
        }
    }
}

static void mqtt_handle_pending(cJSON *json)
{
    const char *challenge_id = NULL;
    const char *registration_nonce = NULL;

    if (!mqtt_get_string(json, "challenge_id", &challenge_id) ||
        !mqtt_get_string(json, "registration_nonce", &registration_nonce)) {
        return;
    }
    if (strcmp(registration_nonce, s_mqtt.registration_nonce) != 0) {
        return;
    }

    strlcpy(s_mqtt.challenge_id, challenge_id, sizeof(s_mqtt.challenge_id));
    snprintf(s_mqtt.bootstrap_topic, sizeof(s_mqtt.bootstrap_topic), TOPIC_PREFIX "receiver/bootstrap/%s",
             challenge_id);
    esp_mqtt_client_unsubscribe(s_mqtt.client, TOPIC_PREFIX "receiver/bootstrap/+");
    esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.bootstrap_topic, 1);
}

static void mqtt_handle_rejected(cJSON *json)
{
    const char *challenge_id = NULL;
    if (!mqtt_get_string(json, "challenge_id", &challenge_id)) {
        return;
    }
    if (s_mqtt.challenge_id[0] != '\0' && strcmp(challenge_id, s_mqtt.challenge_id) != 0) {
        return;
    }

    device_config_clear_enroll_token(s_mqtt.cfg);
    xEventGroupSetBits(s_mqtt.events, MQTT_BOOTSTRAP_REJECTED_BIT);
}

static void mqtt_handle_challenge(cJSON *json)
{
    const char *challenge_id = NULL;
    const char *registration_nonce = NULL;
    const char *nonce = NULL;
    const char *issued_at = NULL;
    const char *expires_at = NULL;
    const char *purpose = NULL;

    if (!mqtt_get_string(json, "challenge_id", &challenge_id) ||
        !mqtt_get_string(json, "registration_nonce", &registration_nonce) ||
        !mqtt_get_string(json, "nonce", &nonce) ||
        !mqtt_get_string(json, "issued_at", &issued_at) ||
        !mqtt_get_string(json, "expires_at", &expires_at) ||
        !mqtt_get_string(json, "purpose", &purpose)) {
        return;
    }
    if (strcmp(registration_nonce, s_mqtt.registration_nonce) != 0 ||
        strcmp(purpose, "activate-v1") != 0) {
        return;
    }
    if (s_mqtt.challenge_id[0] != '\0' && strcmp(challenge_id, s_mqtt.challenge_id) != 0) {
        return;
    }

    mqtt_update_time_base(issued_at);
    char canonical[256];
    snprintf(canonical, sizeof(canonical), "activate-v1|%s|%s|%s|%s",
             challenge_id, nonce, issued_at, expires_at);

    char signature_b64[DEVICE_IDENTITY_MAX_SIGNATURE_B64_LEN] = { 0 };
    if (device_identity_sign_message_b64(s_mqtt.identity, canonical,
                                         signature_b64, sizeof(signature_b64)) == ESP_OK) {
        mqtt_publish_bootstrap_response(signature_b64);
    }
}

static void mqtt_handle_challenge_result(cJSON *json)
{
    const char *challenge_id = NULL;
    const char *status = NULL;
    const char *public_id = NULL;
    const char *device_name = NULL;

    if (!mqtt_get_string(json, "challenge_id", &challenge_id) ||
        !mqtt_get_string(json, "status", &status) ||
        !mqtt_get_string(json, "public_id", &public_id) ||
        !mqtt_get_string(json, "assigned_device_name", &device_name)) {
        return;
    }
    if (strcmp(status, "active") != 0) {
        xEventGroupSetBits(s_mqtt.events, MQTT_BOOTSTRAP_REJECTED_BIT);
        return;
    }
    if (s_mqtt.challenge_id[0] != '\0' && strcmp(challenge_id, s_mqtt.challenge_id) != 0) {
        return;
    }

    if (device_config_commit_activation(s_mqtt.cfg, public_id, device_name,
                                        device_config_compiled_receiver_type()) != ESP_OK) {
        xEventGroupSetBits(s_mqtt.events, MQTT_BOOTSTRAP_FAILED_BIT);
        return;
    }

    s_mqtt.pending_operational_restart = true;
    xTaskNotifyGive(s_mqtt.ctl_task);
}

static void mqtt_handle_bootstrap_message(const char *topic, const char *payload)
{
    (void)topic;

    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        return;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema_version");
    const char *type = NULL;
    if (!cJSON_IsNumber(schema) || schema->valueint != DEVICE_CONFIG_SCHEMA_VERSION ||
        !mqtt_get_string(json, "type", &type)) {
        cJSON_Delete(json);
        return;
    }

    if (strcmp(type, "pending") == 0) {
        mqtt_handle_pending(json);
    } else if (strcmp(type, "rejected") == 0) {
        mqtt_handle_rejected(json);
    } else if (strcmp(type, "challenge") == 0) {
        mqtt_handle_challenge(json);
    } else if (strcmp(type, "result") == 0) {
        mqtt_handle_challenge_result(json);
    }

    cJSON_Delete(json);
}

static void mqtt_handle_rf_code(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        return;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema_version");
    const char *rf_code_hex = NULL;
    const char *issued_at = NULL;
    const char *signature_b64 = NULL;
    uint32_t code_version = 0;
    uint32_t rf_code_bits = 0;

    if (!cJSON_IsNumber(schema) || schema->valueint != DEVICE_CONFIG_SCHEMA_VERSION ||
        !mqtt_get_string(json, "rf_code_hex", &rf_code_hex) ||
        !mqtt_get_u32(json, "rf_code_bits", &rf_code_bits) ||
        !mqtt_get_u32(json, "code_version", &code_version) ||
        !mqtt_get_string(json, "issued_at", &issued_at) ||
        !mqtt_get_string(json, "signature_b64", &signature_b64)) {
        mqtt_publish_rf_ack("rejected", code_version);
        cJSON_Delete(json);
        return;
    }

    mqtt_update_time_base(issued_at);

    uint8_t code[DEVICE_CONFIG_MAX_RF_CODE_LEN] = { 0 };
    size_t code_len = 0;
    if (mqtt_decode_hex(rf_code_hex, code, sizeof(code), &code_len) != ESP_OK) {
        mqtt_publish_rf_ack("rejected", code_version);
        cJSON_Delete(json);
        return;
    }

    bool width_ok = false;
    if (device_config_compiled_receiver_type() == RECEIVER_TYPE_433M) {
        size_t expected_len = ((size_t)rf_code_bits + 7U) / 8U;
        width_ok = rf_code_bits >= 1U && rf_code_bits <= 32U && expected_len == code_len;
    } else {
        width_ok = rf_code_bits == 40U && code_len == 5U;
    }
    if (!width_ok) {
        mqtt_publish_rf_ack("rejected", code_version);
        cJSON_Delete(json);
        return;
    }

    char canonical[320];
    snprintf(canonical, sizeof(canonical), "rf-code-v1|%s|%" PRIu32 "|%s|%" PRIu32 "|%s",
             s_mqtt.cfg->public_id, code_version, rf_code_hex, rf_code_bits, issued_at);
    if (device_identity_verify_message_b64(s_mqtt.identity, canonical, signature_b64) != ESP_OK) {
        mqtt_publish_rf_ack("rejected", code_version);
        cJSON_Delete(json);
        return;
    }

    if (code_version < s_mqtt.cfg->rf_code_ver) {
        mqtt_publish_rf_ack("rejected", code_version);
        cJSON_Delete(json);
        return;
    }
    if (code_version == s_mqtt.cfg->rf_code_ver) {
        mqtt_publish_rf_ack("unchanged", code_version);
        cJSON_Delete(json);
        return;
    }

    bool first_code = !device_config_has_rf_code(s_mqtt.cfg);
    bool promote_active = first_code && s_mqtt.cfg->op_state == OP_STATE_PENDING_RF_CODE;

    if (promote_active) {
        uint8_t previous_code[DEVICE_CONFIG_MAX_RF_CODE_LEN];
        memcpy(previous_code, s_mqtt.cfg->rf_code, sizeof(previous_code));
        size_t previous_code_len = s_mqtt.cfg->rf_code_len;
        uint8_t previous_code_bits = s_mqtt.cfg->rf_code_bits;
        uint32_t previous_code_ver = s_mqtt.cfg->rf_code_ver;
        bool previous_has_rf_code = s_mqtt.cfg->has_rf_code;

        memset(s_mqtt.cfg->rf_code, 0, sizeof(s_mqtt.cfg->rf_code));
        memcpy(s_mqtt.cfg->rf_code, code, code_len);
        s_mqtt.cfg->rf_code_len = code_len;
        s_mqtt.cfg->rf_code_bits = (uint8_t)rf_code_bits;
        s_mqtt.cfg->rf_code_ver = code_version;
        s_mqtt.cfg->has_rf_code = true;

        if (rf_sup_start(s_mqtt.cfg) != ESP_OK) {
            memcpy(s_mqtt.cfg->rf_code, previous_code, sizeof(s_mqtt.cfg->rf_code));
            s_mqtt.cfg->rf_code_len = previous_code_len;
            s_mqtt.cfg->rf_code_bits = previous_code_bits;
            s_mqtt.cfg->rf_code_ver = previous_code_ver;
            s_mqtt.cfg->has_rf_code = previous_has_rf_code;
            mqtt_publish_rf_ack("rejected", code_version);
            cJSON_Delete(json);
            return;
        }

        if (device_config_commit_rf_code(s_mqtt.cfg, code, code_len, (uint8_t)rf_code_bits,
                                         code_version, true) != ESP_OK) {
            rf_sup_delete();
            memcpy(s_mqtt.cfg->rf_code, previous_code, sizeof(s_mqtt.cfg->rf_code));
            s_mqtt.cfg->rf_code_len = previous_code_len;
            s_mqtt.cfg->rf_code_bits = previous_code_bits;
            s_mqtt.cfg->rf_code_ver = previous_code_ver;
            s_mqtt.cfg->has_rf_code = previous_has_rf_code;
            mqtt_publish_rf_ack("rejected", code_version);
            cJSON_Delete(json);
            return;
        }
    } else {
        if (device_config_commit_rf_code(s_mqtt.cfg, code, code_len, (uint8_t)rf_code_bits,
                                         code_version, false) != ESP_OK) {
            mqtt_publish_rf_ack("rejected", code_version);
            cJSON_Delete(json);
            return;
        }

        if (rf_sup_apply_rx_address(code, code_len) != ESP_OK) {
            mqtt_publish_rf_ack("rejected", code_version);
            cJSON_Delete(json);
            return;
        }
    }

    rf_trigger_set(code, code_len, (uint8_t)rf_code_bits, code_version);
    mqtt_publish_rf_ack("applied", code_version);
    cJSON_Delete(json);
}

static void mqtt_handle_deact(const char *payload)
{
    cJSON *json = cJSON_Parse(payload);
    if (json == NULL) {
        return;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(json, "schema_version");
    const char *action = NULL;
    const char *command_id = NULL;
    const char *issued_at = NULL;
    const char *signature_b64 = NULL;

    if (!cJSON_IsNumber(schema) || schema->valueint != DEVICE_CONFIG_SCHEMA_VERSION ||
        !mqtt_get_string(json, "action", &action) ||
        !mqtt_get_string(json, "command_id", &command_id) ||
        !mqtt_get_string(json, "issued_at", &issued_at) ||
        !mqtt_get_string(json, "signature_b64", &signature_b64)) {
        cJSON_Delete(json);
        return;
    }

    mqtt_update_time_base(issued_at);

    char canonical[MQTT_DEACT_CANONICAL_LEN];
    snprintf(canonical, sizeof(canonical), "deact-v1|%s|%s|%s|%s",
             s_mqtt.cfg->public_id, command_id, action, issued_at);
    if (device_identity_verify_message_b64(s_mqtt.identity, canonical, signature_b64) != ESP_OK) {
        mqtt_publish_deact_ack(command_id, action, "rejected");
        cJSON_Delete(json);
        return;
    }

    if (s_mqtt.cfg->has_last_deact_id && strcmp(command_id, s_mqtt.cfg->last_deact_id) == 0) {
        const char *status = (s_mqtt.cfg->op_state == OP_STATE_DECOMMISSIONED &&
                              strcmp(action, "decommission") != 0) ? "ignored" : "ok";
        mqtt_publish_deact_ack(command_id, action, status);
        cJSON_Delete(json);
        return;
    }

    if (s_mqtt.cfg->op_state == OP_STATE_DECOMMISSIONED && strcmp(action, "decommission") != 0) {
        if (device_config_commit_op_state(s_mqtt.cfg, OP_STATE_DECOMMISSIONED, command_id) == ESP_OK) {
            mqtt_publish_deact_ack(command_id, action, "ignored");
        } else {
            mqtt_publish_deact_ack(command_id, action, "rejected");
        }
        cJSON_Delete(json);
        return;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(action, "suspend") == 0) {
        rf_trigger_stop_output();
        if (rf_sup_is_started() && !rf_sup_is_suspended()) {
            err = rf_sup_suspend();
        }
        if (err == ESP_OK) {
            err = device_config_commit_op_state(s_mqtt.cfg, OP_STATE_SUSPENDED, command_id);
        }
        mqtt_publish_deact_ack(command_id, action, err == ESP_OK ? "ok" : "rejected");
    } else if (strcmp(action, "resume") == 0) {
        receiver_op_state_t target_state = OP_STATE_PENDING_RF_CODE;
        if (device_config_has_rf_code(s_mqtt.cfg)) {
            if (rf_sup_is_started()) {
                err = rf_sup_is_suspended() ? rf_sup_resume() : ESP_OK;
            } else {
                err = rf_sup_start(s_mqtt.cfg);
            }
            target_state = OP_STATE_ACTIVE;
        } else if (rf_sup_is_started()) {
            rf_sup_delete();
        }
        if (err == ESP_OK) {
            err = device_config_commit_op_state(s_mqtt.cfg, target_state, command_id);
        }
        mqtt_publish_deact_ack(command_id, action, err == ESP_OK ? "ok" : "rejected");
    } else if (strcmp(action, "decommission") == 0) {
        rf_trigger_stop_output();
        if (rf_sup_is_started()) {
            err = rf_sup_delete();
        }
        if (err == ESP_OK) {
            err = device_config_commit_op_state(s_mqtt.cfg, OP_STATE_DECOMMISSIONED, command_id);
        }
        mqtt_publish_deact_ack(command_id, action, err == ESP_OK ? "ok" : "rejected");
    } else {
        mqtt_publish_deact_ack(command_id, action, "rejected");
    }

    cJSON_Delete(json);
}

static void mqtt_handle_operational_message(const char *topic, const char *payload)
{
    char rf_topic[MQTT_TOPIC_BUF_LEN];
    char deact_topic[MQTT_TOPIC_BUF_LEN];
    snprintf(rf_topic, sizeof(rf_topic), TOPIC_PREFIX "receiver/device/%s/cmd/rf_code", s_mqtt.cfg->public_id);
    snprintf(deact_topic, sizeof(deact_topic), TOPIC_PREFIX "receiver/device/%s/cmd/deact", s_mqtt.cfg->public_id);

    if (strcmp(topic, rf_topic) == 0) {
        mqtt_handle_rf_code(payload);
    } else if (strcmp(topic, deact_topic) == 0) {
        mqtt_handle_deact(payload);
    }
}

static void mqtt_dispatch(const char *topic, const char *payload)
{
    if (s_mqtt.phase == MQTT_PHASE_BOOTSTRAP) {
        mqtt_handle_bootstrap_message(topic, payload);
    } else {
        mqtt_handle_operational_message(topic, payload);
    }
}

static void mqtt_process_data_event(esp_mqtt_event_handle_t event)
{
    if (event->total_data_len <= event->data_len && event->current_data_offset == 0) {
        char *topic = mqtt_dup_span(event->topic, (size_t)event->topic_len);
        char *payload = mqtt_dup_span(event->data, (size_t)event->data_len);
        if (topic != NULL && payload != NULL) {
            mqtt_dispatch(topic, payload);
        }
        free(topic);
        free(payload);
        return;
    }

    if (event->current_data_offset == 0) {
        mqtt_clear_fragment();
        s_mqtt.fragment.topic = mqtt_dup_span(event->topic, (size_t)event->topic_len);
        s_mqtt.fragment.payload = calloc(1, (size_t)event->total_data_len + 1U);
        s_mqtt.fragment.total_len = event->total_data_len;
    }
    if (s_mqtt.fragment.payload == NULL || s_mqtt.fragment.topic == NULL) {
        mqtt_clear_fragment();
        return;
    }

    memcpy(s_mqtt.fragment.payload + event->current_data_offset, event->data, (size_t)event->data_len);
    if (event->current_data_offset + event->data_len == event->total_data_len) {
        s_mqtt.fragment.payload[s_mqtt.fragment.total_len] = '\0';
        mqtt_dispatch(s_mqtt.fragment.topic, s_mqtt.fragment.payload);
        mqtt_clear_fragment();
    }
}

void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt.connected = true;
        xEventGroupSetBits(s_mqtt.events, MQTT_CONNECTED_BIT);
        mqtt_subscribe_current_phase();
        if (s_mqtt.pending_operational_restart && s_mqtt.phase == MQTT_PHASE_OPERATIONAL) {
            s_mqtt.pending_operational_restart = false;
            xEventGroupSetBits(s_mqtt.events, MQTT_BOOTSTRAP_DONE_BIT);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt.connected = false;
        xEventGroupClearBits(s_mqtt.events, MQTT_CONNECTED_BIT);
        break;
    case MQTT_EVENT_DATA:
        mqtt_process_data_event(event);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(MQTT_TAG, "MQTT error type=%d", event->error_handle != NULL
                 ? event->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

esp_err_t mqtt_start(device_config_t *cfg, device_identity_t *identity)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && identity != NULL, ESP_ERR_INVALID_ARG, MQTT_TAG,
                        "invalid MQTT start args");
    ESP_RETURN_ON_FALSE(strncmp(cfg->mqtt_uri, "mqtts://", 8) == 0, ESP_ERR_INVALID_ARG,
                        MQTT_TAG, "mqtt_uri must use mqtts://");

    s_mqtt.cfg = cfg;
    s_mqtt.identity = identity;

    if (s_mqtt.events == NULL) {
        s_mqtt.events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_mqtt.events != NULL, ESP_ERR_NO_MEM, MQTT_TAG,
                            "failed to create MQTT event group");
    }
    if (s_mqtt.ctl_task == NULL) {
        if (xTaskCreate(mqtt_control_task, "mqtt_ctl", 6144, NULL, 6, &s_mqtt.ctl_task) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_mqtt.client == NULL) {
        ESP_RETURN_ON_ERROR(mqtt_start_internal(), MQTT_TAG, "failed to start MQTT");
    }

    EventBits_t bits = xEventGroupWaitBits(s_mqtt.events, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(30000));
    return (bits & MQTT_CONNECTED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mqtt_stop(void)
{
    if (s_mqtt.client == NULL) {
        return ESP_OK;
    }
    mqtt_destroy_client();
    return ESP_OK;
}

esp_err_t mqtt_bootstrap_activate(device_config_t *cfg,
                                  device_identity_t *identity,
                                  const char *registration_nonce,
                                  TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && identity != NULL && registration_nonce != NULL,
                        ESP_ERR_INVALID_ARG, MQTT_TAG, "invalid bootstrap args");
    ESP_RETURN_ON_FALSE(s_mqtt.client != NULL && s_mqtt.connected, ESP_ERR_INVALID_STATE,
                        MQTT_TAG, "MQTT client is not connected");

    memset(s_mqtt.challenge_id, 0, sizeof(s_mqtt.challenge_id));
    memset(s_mqtt.bootstrap_topic, 0, sizeof(s_mqtt.bootstrap_topic));
    strlcpy(s_mqtt.registration_nonce, registration_nonce, sizeof(s_mqtt.registration_nonce));
    xEventGroupClearBits(s_mqtt.events,
                         MQTT_BOOTSTRAP_DONE_BIT | MQTT_BOOTSTRAP_REJECTED_BIT | MQTT_BOOTSTRAP_FAILED_BIT);

    ESP_RETURN_ON_ERROR(mqtt_publish_register(), MQTT_TAG, "failed to publish registration");

    EventBits_t bits = xEventGroupWaitBits(s_mqtt.events,
                                           MQTT_BOOTSTRAP_DONE_BIT |
                                           MQTT_BOOTSTRAP_REJECTED_BIT |
                                           MQTT_BOOTSTRAP_FAILED_BIT,
                                           pdFALSE, pdFALSE, timeout_ticks);
    if ((bits & MQTT_BOOTSTRAP_DONE_BIT) != 0) {
        return ESP_OK;
    }
    if ((bits & MQTT_BOOTSTRAP_REJECTED_BIT) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((bits & MQTT_BOOTSTRAP_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

bool mqtt_is_connected(void)
{
    return s_mqtt.connected;
}
