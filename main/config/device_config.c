/**
 * @file device_config.c
 * @brief Persistent device configuration and NVS helpers.
 */

#include "config/device_config.h"
#include <stdint.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define DEVICE_CONFIG_TAG "DEVICE_CFG"

static void device_config_reset(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->schema_ver = DEVICE_CONFIG_SCHEMA_VERSION;
}

static esp_err_t load_string(nvs_handle_t handle,
                             const char *key,
                             char *out,
                             size_t out_len,
                             bool *present)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (present) {
            *present = false;
        }
        if (out_len > 0) {
            out[0] = '\0';
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to query key %s", key);
    ESP_RETURN_ON_FALSE(required <= out_len, ESP_ERR_NVS_INVALID_LENGTH, DEVICE_CONFIG_TAG,
                        "value for %s exceeds buffer", key);
    ESP_RETURN_ON_ERROR(nvs_get_str(handle, key, out, &required), DEVICE_CONFIG_TAG,
                        "failed to read key %s", key);
    if (present) {
        *present = true;
    }
    return ESP_OK;
}

static esp_err_t load_u8(nvs_handle_t handle, const char *key, uint8_t *out, bool *present)
{
    esp_err_t err = nvs_get_u8(handle, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (present) {
            *present = false;
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to read key %s", key);
    if (present) {
        *present = true;
    }
    return ESP_OK;
}

static esp_err_t load_u32(nvs_handle_t handle, const char *key, uint32_t *out, bool *present)
{
    esp_err_t err = nvs_get_u32(handle, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (present) {
            *present = false;
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to read key %s", key);
    if (present) {
        *present = true;
    }
    return ESP_OK;
}

static esp_err_t load_blob(nvs_handle_t handle,
                           const char *key,
                           void *out,
                           size_t out_len,
                           size_t *actual_len,
                           bool *present)
{
    size_t required = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (present) {
            *present = false;
        }
        if (actual_len) {
            *actual_len = 0;
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to query key %s", key);
    ESP_RETURN_ON_FALSE(required <= out_len, ESP_ERR_NVS_INVALID_LENGTH, DEVICE_CONFIG_TAG,
                        "blob for %s exceeds buffer", key);
    ESP_RETURN_ON_ERROR(nvs_get_blob(handle, key, out, &required), DEVICE_CONFIG_TAG,
                        "failed to read key %s", key);
    if (present) {
        *present = true;
    }
    if (actual_len) {
        *actual_len = required;
    }
    return ESP_OK;
}

static esp_err_t erase_optional_key(nvs_handle_t handle, const char *key)
{
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

receiver_type_t device_config_compiled_receiver_type(void)
{
#if CONFIG_RECEIVER_RADIO_2_4G
    return RECEIVER_TYPE_2_4G;
#else
    return RECEIVER_TYPE_433M;
#endif
}

const char *device_config_receiver_type_string(receiver_type_t type)
{
    switch (type) {
    case RECEIVER_TYPE_433M:
        return "RECEIVER_433M";
    case RECEIVER_TYPE_2_4G:
        return "RECEIVER_2_4G";
    default:
        return "RECEIVER_UNKNOWN";
    }
}

const char *device_config_op_state_string(receiver_op_state_t state)
{
    switch (state) {
    case OP_STATE_PENDING_RF_CODE:
        return "PENDING_RF_CODE";
    case OP_STATE_ACTIVE:
        return "ACTIVE";
    case OP_STATE_SUSPENDED:
        return "SUSPENDED";
    case OP_STATE_DECOMMISSIONED:
        return "DECOMMISSIONED";
    default:
        return "NONE";
    }
}

bool device_config_has_rf_code(const device_config_t *cfg)
{
    return cfg != NULL && cfg->has_rf_code && cfg->rf_code_len > 0 && cfg->rf_code_bits > 0;
}

device_boot_state_t device_config_boot_state(const device_config_t *cfg)
{
    if (cfg == NULL || !cfg->has_wifi) {
        return DEVICE_BOOT_STATE_UNPROVISIONED;
    }

    if (strncmp(cfg->mqtt_uri, "mqtts://", 8) != 0) {
        return DEVICE_BOOT_STATE_RECOVERY_REQUIRED;
    }

    if (!cfg->has_public_id) {
        return cfg->has_enroll_token ? DEVICE_BOOT_STATE_PENDING_ACTIVATION
                                     : DEVICE_BOOT_STATE_RECOVERY_REQUIRED;
    }

    if (!cfg->has_op_state || !cfg->has_rx_type || cfg->rx_type != device_config_compiled_receiver_type()) {
        return DEVICE_BOOT_STATE_RECOVERY_REQUIRED;
    }

    return DEVICE_BOOT_STATE_OPERATIONAL;
}

esp_err_t nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), DEVICE_CONFIG_TAG, "failed to erase NVS");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t device_config_load(device_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG, "cfg is NULL");

    device_config_reset(cfg);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to open config namespace");

    bool present = false;
    uint8_t u8_value = 0;

    err = load_u8(handle, "schema_ver", &cfg->schema_ver, NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid), &cfg->has_wifi);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "wifi_pwd", cfg->wifi_pwd, sizeof(cfg->wifi_pwd), NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "mqtt_uri", cfg->mqtt_uri, sizeof(cfg->mqtt_uri), NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "mqtt_user", cfg->mqtt_user, sizeof(cfg->mqtt_user), NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "mqtt_pwd", cfg->mqtt_pwd, sizeof(cfg->mqtt_pwd), NULL);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "enroll_token", cfg->enroll_token, sizeof(cfg->enroll_token),
                      &cfg->has_enroll_token);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "public_id", cfg->public_id, sizeof(cfg->public_id), &cfg->has_public_id);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "device_name", cfg->device_name, sizeof(cfg->device_name),
                      &cfg->has_device_name);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_string(handle, "last_deact_id", cfg->last_deact_id, sizeof(cfg->last_deact_id),
                      &cfg->has_last_deact_id);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_u8(handle, "rx_type", &u8_value, &cfg->has_rx_type);
    if (err != ESP_OK) {
        goto cleanup;
    }
    cfg->rx_type = (receiver_type_t)u8_value;
    err = load_u8(handle, "op_state", &u8_value, &cfg->has_op_state);
    if (err != ESP_OK) {
        goto cleanup;
    }
    cfg->op_state = (receiver_op_state_t)u8_value;
    err = load_blob(handle, "rf_code", cfg->rf_code, sizeof(cfg->rf_code), &cfg->rf_code_len,
                    &cfg->has_rf_code);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = load_u8(handle, "rf_code_bits", &cfg->rf_code_bits, &present);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!present) {
        cfg->rf_code_bits = 0;
    }
    err = load_u32(handle, "rf_code_ver", &cfg->rf_code_ver, &present);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (!present) {
        cfg->rf_code_ver = 0;
    }

cleanup:
    nvs_close(handle);
    return err;
}

esp_err_t device_config_save_provisioning(device_config_t *cfg, const device_provisioning_t *prov)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && prov != NULL, ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG,
                        "invalid provisioning input");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");

    esp_err_t err = nvs_set_u8(handle, "schema_ver", DEVICE_CONFIG_SCHEMA_VERSION);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_ssid", prov->wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "wifi_pwd", prov->wifi_pwd);
    if (err == ESP_OK) err = nvs_set_str(handle, "mqtt_uri", prov->mqtt_uri);
    if (err == ESP_OK) err = nvs_set_str(handle, "mqtt_user", prov->mqtt_user);
    if (err == ESP_OK) err = nvs_set_str(handle, "mqtt_pwd", prov->mqtt_pwd);
    if (err == ESP_OK) err = nvs_set_str(handle, "enroll_token", prov->enroll_token);
    if (err == ESP_OK) err = erase_optional_key(handle, "public_id");
    if (err == ESP_OK) err = erase_optional_key(handle, "device_name");
    if (err == ESP_OK) err = erase_optional_key(handle, "rx_type");
    if (err == ESP_OK) err = erase_optional_key(handle, "rf_code");
    if (err == ESP_OK) err = erase_optional_key(handle, "rf_code_bits");
    if (err == ESP_OK) err = erase_optional_key(handle, "rf_code_ver");
    if (err == ESP_OK) err = erase_optional_key(handle, "op_state");
    if (err == ESP_OK) err = erase_optional_key(handle, "last_deact_id");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to persist provisioning");

    return device_config_load(cfg);
}

esp_err_t device_config_commit_activation(device_config_t *cfg,
                                          const char *public_id,
                                          const char *device_name,
                                          receiver_type_t rx_type)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && public_id != NULL && device_name != NULL,
                        ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG, "invalid activation data");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");

    esp_err_t err = nvs_set_str(handle, "public_id", public_id);
    if (err == ESP_OK) err = nvs_set_str(handle, "device_name", device_name);
    if (err == ESP_OK) err = nvs_set_u8(handle, "rx_type", (uint8_t)rx_type);
    if (err == ESP_OK) err = nvs_set_u8(handle, "op_state", (uint8_t)OP_STATE_PENDING_RF_CODE);
    if (err == ESP_OK) err = erase_optional_key(handle, "enroll_token");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to persist activation");

    strlcpy(cfg->public_id, public_id, sizeof(cfg->public_id));
    strlcpy(cfg->device_name, device_name, sizeof(cfg->device_name));
    cfg->has_public_id = true;
    cfg->has_device_name = true;
    cfg->rx_type = rx_type;
    cfg->has_rx_type = true;
    cfg->op_state = OP_STATE_PENDING_RF_CODE;
    cfg->has_op_state = true;
    cfg->enroll_token[0] = '\0';
    cfg->has_enroll_token = false;
    return ESP_OK;
}

esp_err_t device_config_commit_rf_code(device_config_t *cfg,
                                       const uint8_t *code,
                                       size_t code_len,
                                       uint8_t bits,
                                       uint32_t version,
                                       bool promote_active)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && code != NULL, ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG,
                        "invalid RF code");
    ESP_RETURN_ON_FALSE(code_len > 0 && code_len <= sizeof(cfg->rf_code), ESP_ERR_INVALID_ARG,
                        DEVICE_CONFIG_TAG, "invalid RF code length");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");

    esp_err_t err = nvs_set_blob(handle, "rf_code", code, code_len);
    if (err == ESP_OK) err = nvs_set_u8(handle, "rf_code_bits", bits);
    if (err == ESP_OK) err = nvs_set_u32(handle, "rf_code_ver", version);
    if (err == ESP_OK && promote_active) {
        err = nvs_set_u8(handle, "op_state", (uint8_t)OP_STATE_ACTIVE);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to persist RF code");

    memcpy(cfg->rf_code, code, code_len);
    cfg->rf_code_len = code_len;
    cfg->rf_code_bits = bits;
    cfg->rf_code_ver = version;
    cfg->has_rf_code = true;
    if (promote_active) {
        cfg->op_state = OP_STATE_ACTIVE;
        cfg->has_op_state = true;
    }
    return ESP_OK;
}

esp_err_t device_config_commit_op_state(device_config_t *cfg,
                                        receiver_op_state_t op_state,
                                        const char *last_deact_id)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG, "cfg is NULL");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");

    esp_err_t err = nvs_set_u8(handle, "op_state", (uint8_t)op_state);
    if (err == ESP_OK && last_deact_id != NULL) {
        err = nvs_set_str(handle, "last_deact_id", last_deact_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to persist operational state");

    cfg->op_state = op_state;
    cfg->has_op_state = true;
    if (last_deact_id != NULL) {
        strlcpy(cfg->last_deact_id, last_deact_id, sizeof(cfg->last_deact_id));
        cfg->has_last_deact_id = true;
    }
    return ESP_OK;
}

esp_err_t device_config_clear_enroll_token(device_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, DEVICE_CONFIG_TAG, "cfg is NULL");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");
    esp_err_t err = erase_optional_key(handle, "enroll_token");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(err, DEVICE_CONFIG_TAG, "failed to erase enroll token");

    cfg->enroll_token[0] = '\0';
    cfg->has_enroll_token = false;
    return ESP_OK;
}

esp_err_t device_config_reprovision(void)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_NAMESPACE, NVS_READWRITE, &handle), DEVICE_CONFIG_TAG,
                        "failed to open config namespace");
    esp_err_t err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
