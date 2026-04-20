/**
 * @file main.c
 * @brief Receiver Application Entry Point and Orchestration
 *
 * Coordinates provisioning, activation, secure command handling, RF trigger
 * restoration, and steady-state receiver operation for the ESP-01 firmware.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "config/device_config.h"
#include "network/mqtt.h"
#include "network/wifi.h"
#include "provision/http_server.h"
#include "rf/rf_common.h"
#include "security/device_identity.h"
#include "trigger/rf_supervisor.h"
#include "trigger/rf_trigger.h"
#include "vibrator/vibrator.h"

#define TAG "main"
#ifndef PROJECT_VER
#define PROJECT_VER "dev"
#endif
#define BOOTSTRAP_TIMEOUT_MS (15 * 60 * 1000)

#define RF_RX_GPIO GPIO_NUM_0
#define VIBRATOR_GPIO GPIO_NUM_2

#define BOOTSTRAP_PENDING_BIT BIT0
#define BOOTSTRAP_REJECTED_BIT BIT1
#define BOOTSTRAP_RESULT_BIT BIT2

RFHandler g_rf = {0};
static VibratorHandler s_vibrator = {0};
static device_identity_t s_identity = {0};
static EventGroupHandle_t s_bootstrap_events;

typedef struct {
    char *registration_nonce;
    char *challenge_id;
    char *public_id;
    char *device_name;
} bootstrap_session_t;

static bootstrap_session_t s_bootstrap = {0};

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
    snprintf(topic, sizeof(topic), "receiver/device/%s/ack", public_id);

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
    snprintf(topic, sizeof(topic), "receiver/device/%s/ack", public_id);

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

static esp_err_t ensure_operational_subscription(const char *public_id)
{
    char topic[128];

    if (!public_id) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(topic, sizeof(topic), "receiver/device/%s/cmd/#", public_id);
    return mqtt_subscribe(topic, 1);
}

static esp_err_t publish_bootstrap_registration(const device_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    char *public_key_b64 = NULL;
    esp_err_t err;

    if (!root || !cfg->enroll_token || !s_bootstrap.registration_nonce) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    err = device_identity_get_public_key_b64(&s_identity, &public_key_b64);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    cJSON_AddNumberToObject(root, "schema_version", 1);
    cJSON_AddStringToObject(root, "hardware_model", "ESP-01");
    cJSON_AddStringToObject(root, "receiver_type", "RECEIVER_433M");
    cJSON_AddStringToObject(root, "firmware_version", PROJECT_VER);
    cJSON_AddStringToObject(root, "public_key_b64", public_key_b64);
    cJSON_AddStringToObject(root, "enrollment_token", cfg->enroll_token);
    cJSON_AddStringToObject(root, "registration_nonce", s_bootstrap.registration_nonce);

    err = publish_json("receiver/bootstrap/register", root);
    cJSON_Delete(root);
    free(public_key_b64);
    return err;
}

static void handle_bootstrap_message(const char *topic, cJSON *root)
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
            char wildcard_topic[] = "receiver/bootstrap/+";
            char exact_topic[128];

            s_bootstrap.challenge_id = strdup(challenge_id);
            snprintf(exact_topic, sizeof(exact_topic), "receiver/bootstrap/%s", challenge_id);
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

        if (device_identity_sign_activation_response(&s_identity,
                                                     challenge_id,
                                                     nonce,
                                                     issued_at,
                                                     expires_at,
                                                     &signature_b64) == ESP_OK) {
            response = cJSON_CreateObject();
            if (response) {
                snprintf(exact_topic, sizeof(exact_topic), "receiver/bootstrap/%s", challenge_id);
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
    if (device_identity_verify_signature(&s_identity, canonical, signature_b64, &verified) != ESP_OK || !verified) {
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
    if (device_identity_verify_signature(&s_identity, canonical, signature_b64, &verified) != ESP_OK || !verified) {
        publish_deact_ack(cfg.public_id, command_id, action, "rejected", issued_at);
        goto cleanup;
    }

    if (cfg.has_last_deact_id && strcmp(cfg.last_deact_id, command_id) == 0) {
        publish_deact_ack(cfg.public_id, command_id, action, "ok", issued_at);
        goto cleanup;
    }

    if (cfg.has_op_state && cfg.op_state == RECEIVER_OP_STATE_DECOMMISSIONED && strcmp(action, "decommission") != 0) {
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

    if (strncmp(topic, "receiver/bootstrap/", 19) == 0) {
        handle_bootstrap_message(topic, root);
    } else if (strstr(topic, "/cmd/rf_code") != NULL) {
        handle_rf_code_command(root);
    } else if (strstr(topic, "/cmd/deact") != NULL) {
        handle_deact_command(root);
    }

    cJSON_Delete(root);
}

static esp_err_t bootstrap_activate(device_config_t *cfg)
{
    EventBits_t bits;

    bootstrap_session_reset();
    if (device_identity_make_registration_nonce(&s_bootstrap.registration_nonce) != ESP_OK) {
        return ESP_FAIL;
    }

    if (mqtt_subscribe("receiver/bootstrap/+", 1) != ESP_OK) {
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

        snprintf(challenge_topic, sizeof(challenge_topic), "receiver/bootstrap/%s", s_bootstrap.challenge_id);
        mqtt_unsubscribe(challenge_topic);
        bootstrap_session_reset();
        return err;
    }

    device_config_clear_enroll_token();
    bootstrap_session_reset();
    return ESP_ERR_TIMEOUT;
}

static void run_softap_recovery(void)
{
    char ssid[32] = {0};

    mqtt_stop();
    wifi_stop();
    ESP_ERROR_CHECK(wifi_start_softap(ssid, sizeof(ssid)));
    ESP_ERROR_CHECK(http_server_start());
    ESP_LOGI(TAG, "SoftAP ready: %s", ssid);

    while (1) {
        http_server_action_t action = http_server_wait_for_action(portMAX_DELAY);

        if (action == HTTP_SERVER_ACTION_NONE) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}

void app_main(void)
{
    device_config_t cfg;
    esp_err_t err;

    device_config_init(&cfg);
    s_bootstrap_events = xEventGroupCreate();

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(vibrator_init(VIBRATOR_GPIO, &s_vibrator));
    ESP_ERROR_CHECK(rf_trigger_init(&s_vibrator));
    rf_recv_set_frame_callback(rf_trigger_on_frame, NULL);

    ESP_ERROR_CHECK(device_config_load(&cfg));
    if (cfg.has_rf_code && cfg.rf_code_bits > 0) {
        rf_trigger_restore(cfg.rf_code, cfg.rf_code_bits, cfg.rf_code_ver);
    }

    if (!device_config_is_provisioned(&cfg)) {
        run_softap_recovery();
    }

    ESP_ERROR_CHECK(wifi_init());
    if (wifi_start_sta(cfg.wifi_ssid, cfg.wifi_pwd) != ESP_OK) {
        device_config_free(&cfg);
        run_softap_recovery();
    }

    ESP_ERROR_CHECK(device_identity_init(&s_identity));
    if (mqtt_start(&cfg, on_mqtt_message, NULL) != ESP_OK) {
        device_config_free(&cfg);
        run_softap_recovery();
    }

    if (!cfg.has_public_id || !cfg.has_op_state) {
        if (!cfg.has_enroll_token) {
            device_config_free(&cfg);
            run_softap_recovery();
        }

        if (bootstrap_activate(&cfg) != ESP_OK) {
            device_config_free(&cfg);
            run_softap_recovery();
        }

        device_config_free(&cfg);
        device_config_init(&cfg);
        ESP_ERROR_CHECK(device_config_load(&cfg));
    }

    ESP_ERROR_CHECK(ensure_operational_subscription(cfg.public_id));

    if (cfg.op_state == RECEIVER_OP_STATE_ACTIVE) {
        ESP_ERROR_CHECK(rf_sup_start());
    } else if (cfg.op_state == RECEIVER_OP_STATE_DECOMMISSIONED) {
        rf_sup_delete();
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
