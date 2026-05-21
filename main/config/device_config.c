/**
 * @file device_config.c
 * @brief Device Configuration Module - Persistent State Implementation
 *
 * Implements NVS-backed storage for provisioning data, activation state,
 * RF trigger state, and deactivation bookkeeping.
 */

#include "config/device_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#define IDENTITY_NAMESPACE "identity"
#define DEVICE_CFG_NAMESPACE "device_cfg"

#define KEY_SCHEMA_VER "schema_ver"
#define KEY_WIFI_SSID "wifi_ssid"
#define KEY_WIFI_PWD "wifi_pwd"
#define KEY_MQTT_URI "mqtt_uri"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PWD "mqtt_pwd"
#define KEY_ENROLL_TOKEN "enroll_token"
#define KEY_PUBLIC_ID "public_id"
#define KEY_DEVICE_NAME "device_name"
#define KEY_RF_CODE "rf_code"
#define KEY_RF_CODE_BITS "rf_code_bits"
#define KEY_RF_CODE_VER "rf_code_ver"
#define KEY_OP_STATE "op_state"
#define KEY_LAST_DEACT_ID "last_deact_id"

static esp_err_t read_string(nvs_handle_t handle, const char *key, char **out_value, bool *has_value)
{
    esp_err_t err;
    size_t len = 0;

    *out_value = NULL;
    *has_value = false;

    err = nvs_get_str(handle, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    *out_value = calloc(1, len);
    if (!*out_value) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_str(handle, key, *out_value, &len);
    if (err != ESP_OK) {
        free(*out_value);
        *out_value = NULL;
        return err;
    }

    *has_value = true;
    return ESP_OK;
}

static esp_err_t open_device_cfg(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(DEVICE_CFG_NAMESPACE, mode, handle);
}

static esp_err_t commit_and_close(nvs_handle_t handle, esp_err_t status)
{
    if (status == ESP_OK) {
        status = nvs_commit(handle);
    }
    nvs_close(handle);
    return status;
}

void device_config_init(device_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

void device_config_free(device_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    free(cfg->wifi_ssid);
    free(cfg->wifi_pwd);
    free(cfg->mqtt_uri);
    free(cfg->mqtt_user);
    free(cfg->mqtt_pwd);
    free(cfg->enroll_token);
    free(cfg->public_id);
    free(cfg->device_name);
    free(cfg->last_deact_id);
    device_config_init(cfg);
}

esp_err_t device_config_load(device_config_t *cfg)
{
    esp_err_t err;
    nvs_handle_t handle;
    uint8_t value_u8 = 0;
    uint32_t value_u32 = 0;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    device_config_free(cfg);

    err = open_device_cfg(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(DEVICE_CONFIG_TAG, "No config in NVS (first boot)");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_CONFIG_TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    if (nvs_get_u8(handle, KEY_SCHEMA_VER, &value_u8) == ESP_OK) {
        cfg->schema_ver = value_u8;
        cfg->has_schema_ver = true;
    }
    if (nvs_get_u8(handle, KEY_OP_STATE, &value_u8) == ESP_OK) {
        cfg->op_state = (receiver_op_state_t)value_u8;
        cfg->has_op_state = true;
    }
    if (nvs_get_u32(handle, KEY_RF_CODE, &value_u32) == ESP_OK) {
        cfg->rf_code = value_u32;
        cfg->has_rf_code = true;
    }
    if (nvs_get_u8(handle, KEY_RF_CODE_BITS, &value_u8) == ESP_OK) {
        cfg->rf_code_bits = value_u8;
        cfg->has_rf_code = true;
    }
    if (nvs_get_u32(handle, KEY_RF_CODE_VER, &value_u32) == ESP_OK) {
        cfg->rf_code_ver = value_u32;
        cfg->has_rf_code = true;
    }

    err = read_string(handle, KEY_WIFI_SSID, &cfg->wifi_ssid, &cfg->has_wifi_ssid);
    if (err == ESP_OK) err = read_string(handle, KEY_WIFI_PWD, &cfg->wifi_pwd, &cfg->has_wifi_pwd);
    if (err == ESP_OK) err = read_string(handle, KEY_MQTT_URI, &cfg->mqtt_uri, &cfg->has_mqtt_uri);
    if (err == ESP_OK) err = read_string(handle, KEY_MQTT_USER, &cfg->mqtt_user, &cfg->has_mqtt_user);
    if (err == ESP_OK) err = read_string(handle, KEY_MQTT_PWD, &cfg->mqtt_pwd, &cfg->has_mqtt_pwd);
    if (err == ESP_OK) err = read_string(handle, KEY_ENROLL_TOKEN, &cfg->enroll_token, &cfg->has_enroll_token);
    if (err == ESP_OK) err = read_string(handle, KEY_PUBLIC_ID, &cfg->public_id, &cfg->has_public_id);
    if (err == ESP_OK) err = read_string(handle, KEY_DEVICE_NAME, &cfg->device_name, &cfg->has_device_name);
    if (err == ESP_OK) err = read_string(handle, KEY_LAST_DEACT_ID, &cfg->last_deact_id, &cfg->has_last_deact_id);

    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_CONFIG_TAG, "Config load failed: %s", esp_err_to_name(err));
        device_config_free(cfg);
        return err;
    }

    ESP_LOGI(DEVICE_CONFIG_TAG, "Config loaded: state=%s provisioned=%s",
             device_config_state_name(cfg),
             device_config_is_provisioned(cfg) ? "yes" : "no");
    return ESP_OK;
}

bool device_config_has_any_data(const device_config_t *cfg)
{
    return cfg && (cfg->has_schema_ver || cfg->has_wifi_ssid || cfg->has_mqtt_uri ||
                   cfg->has_enroll_token || cfg->has_public_id || cfg->has_op_state ||
                   cfg->has_rf_code || cfg->has_last_deact_id);
}

bool device_config_is_provisioned(const device_config_t *cfg)
{
    return cfg && cfg->has_wifi_ssid && cfg->has_mqtt_uri && cfg->has_mqtt_user && cfg->has_mqtt_pwd;
}

bool device_config_is_pending_activation(const device_config_t *cfg)
{
    return device_config_is_provisioned(cfg) && !cfg->has_op_state && cfg->has_enroll_token;
}

bool device_config_is_recovery_required(const device_config_t *cfg)
{
    return device_config_is_provisioned(cfg) && !cfg->has_op_state && !cfg->has_enroll_token;
}

const char *device_config_state_name(const device_config_t *cfg)
{
    if (!cfg || !device_config_is_provisioned(cfg)) {
        return "UNPROVISIONED";
    }
    if (!cfg->has_op_state) {
        if (cfg->has_enroll_token) {
            return "PENDING_ACTIVATION";
        }
        return "RECOVERY_REQUIRED";
    }

    switch (cfg->op_state) {
    case RECEIVER_OP_STATE_PENDING_RF_CODE:
        return "PENDING_RF_CODE";
    case RECEIVER_OP_STATE_ACTIVE:
        return "ACTIVE";
    case RECEIVER_OP_STATE_SUSPENDED:
        return "SUSPENDED";
    case RECEIVER_OP_STATE_DECOMMISSIONED:
        return "DECOMMISSIONED";
    default:
        return "UNKNOWN";
    }
}

esp_err_t device_config_store_provisioning(const device_provisioning_t *prov)
{
    esp_err_t err;
    nvs_handle_t handle;

    if (!prov || !prov->wifi_ssid || !prov->wifi_pwd || !prov->mqtt_uri ||
        !prov->mqtt_user || !prov->mqtt_pwd || !prov->enroll_token) {
        return ESP_ERR_INVALID_ARG;
    }

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(DEVICE_CONFIG_TAG, "Storing provisioning data (SSID=%s)", prov->wifi_ssid);
    err = nvs_set_u8(handle, KEY_SCHEMA_VER, DEVICE_CONFIG_SCHEMA_VERSION);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WIFI_SSID, prov->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_WIFI_PWD, prov->wifi_pwd);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_URI, prov->mqtt_uri);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_USER, prov->mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_MQTT_PWD, prov->mqtt_pwd);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_ENROLL_TOKEN, prov->enroll_token);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_PUBLIC_ID);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_DEVICE_NAME);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_RF_CODE);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_RF_CODE_BITS);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_RF_CODE_VER);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_OP_STATE);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_LAST_DEACT_ID);

    return commit_and_close(handle, err);
}

esp_err_t device_config_store_activation(const char *public_id,
                                         const char *device_name,
                                         receiver_op_state_t op_state)
{
    esp_err_t err;
    nvs_handle_t handle;

    if (!public_id || !device_name) {
        return ESP_ERR_INVALID_ARG;
    }

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_PUBLIC_ID, public_id);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_DEVICE_NAME, device_name);
    if (err == ESP_OK) err = nvs_set_u8(handle, KEY_OP_STATE, (uint8_t)op_state);
    if (err == ESP_OK) err = nvs_erase_key(handle, KEY_ENROLL_TOKEN);

    return commit_and_close(handle, err);
}

esp_err_t device_config_clear_enroll_token(void)
{
    esp_err_t err;
    nvs_handle_t handle;

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, KEY_ENROLL_TOKEN);
    return commit_and_close(handle, err);
}

esp_err_t device_config_store_rf_code(uint32_t rf_code,
                                      uint8_t rf_code_bits,
                                      uint32_t rf_code_ver,
                                      bool update_state,
                                      receiver_op_state_t new_state)
{
    esp_err_t err;
    nvs_handle_t handle;

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, KEY_RF_CODE, rf_code);
    if (err == ESP_OK) err = nvs_set_u8(handle, KEY_RF_CODE_BITS, rf_code_bits);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_RF_CODE_VER, rf_code_ver);
    if (err == ESP_OK && update_state) err = nvs_set_u8(handle, KEY_OP_STATE, (uint8_t)new_state);

    return commit_and_close(handle, err);
}

esp_err_t device_config_store_op_state(receiver_op_state_t op_state)
{
    esp_err_t err;
    nvs_handle_t handle;

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, KEY_OP_STATE, (uint8_t)op_state);
    return commit_and_close(handle, err);
}

esp_err_t device_config_store_deactivation(receiver_op_state_t op_state,
                                           const char *last_deact_id)
{
    esp_err_t err;
    nvs_handle_t handle;

    if (!last_deact_id) {
        return ESP_ERR_INVALID_ARG;
    }

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, KEY_OP_STATE, (uint8_t)op_state);
    if (err == ESP_OK) err = nvs_set_str(handle, KEY_LAST_DEACT_ID, last_deact_id);

    return commit_and_close(handle, err);
}

esp_err_t device_config_erase_runtime(void)
{
    esp_err_t err;
    nvs_handle_t handle;

    err = open_device_cfg(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    return commit_and_close(handle, err);
}
