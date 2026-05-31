/**
 * @file device_config.c
 * @brief Minimal NVS-backed pairing configuration.
 */

#include "config/device_config.h"

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define TAG "device_config"

static bool rf_code_shape_valid(size_t rf_code_len, uint8_t rf_bits, uint8_t rf_band)
{
    return (rf_band == 0 && rf_code_len == 4 && rf_bits > 0 && rf_bits <= 32) ||
           (rf_band == 1 && rf_code_len == 5 && rf_bits == 40);
}

esp_err_t nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t device_config_load(device_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(DEVICE_CONFIG_PAIR_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No pairing data, device is unpaired");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open pair namespace");

    ret = nvs_get_u8(handle, "slot_id", &cfg->slot_id);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
        goto cleanup;
    }
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "read slot_id");

    size_t mac_len = sizeof(cfg->hub_mac);
    ESP_GOTO_ON_ERROR(nvs_get_blob(handle, "hub_mac", cfg->hub_mac, &mac_len),
                      cleanup, TAG, "read hub_mac");
    ESP_GOTO_ON_FALSE(mac_len == sizeof(cfg->hub_mac), ESP_ERR_INVALID_SIZE,
                      cleanup, TAG, "invalid hub_mac length");

    size_t code_len = sizeof(cfg->rf_code);
    ESP_GOTO_ON_ERROR(nvs_get_blob(handle, "rf_code", cfg->rf_code, &code_len),
                      cleanup, TAG, "read rf_code");
    ESP_GOTO_ON_FALSE(code_len <= sizeof(cfg->rf_code), ESP_ERR_INVALID_SIZE,
                      cleanup, TAG, "invalid rf_code length");
    cfg->rf_code_len = code_len;

    ESP_GOTO_ON_ERROR(nvs_get_u8(handle, "rf_bits", &cfg->rf_code_bits),
                      cleanup, TAG, "read rf_bits");
    ESP_GOTO_ON_ERROR(nvs_get_u8(handle, "rf_band", &cfg->rf_band),
                      cleanup, TAG, "read rf_band");
    ESP_GOTO_ON_FALSE(cfg->slot_id > 0 &&
                      rf_code_shape_valid(cfg->rf_code_len, cfg->rf_code_bits, cfg->rf_band),
                      ESP_ERR_INVALID_ARG, cleanup, TAG, "invalid pairing data");

    ESP_GOTO_ON_ERROR(nvs_get_u32(handle, "rf_code_ver", &cfg->rf_code_ver),
                      cleanup, TAG, "read rf_code_ver");
    ESP_GOTO_ON_FALSE(cfg->rf_code_ver > 0, ESP_ERR_INVALID_ARG,
                      cleanup, TAG, "invalid rf_code_ver");

    cfg->paired = true;
    ESP_LOGI(TAG, "Loaded pairing: slot=%u band=%s bits=%u",
             cfg->slot_id, cfg->rf_band == 0 ? "433M" : "2.4G", cfg->rf_code_bits);

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t device_config_save_pairing(device_config_t *cfg,
                                     uint8_t slot_id,
                                     const uint8_t hub_mac[6],
                                     const uint8_t *rf_code,
                                     size_t rf_code_len,
                                     uint8_t rf_bits,
                                     uint8_t rf_band,
                                     uint32_t rf_code_ver)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(hub_mac != NULL, ESP_ERR_INVALID_ARG, TAG, "hub_mac is NULL");
    ESP_RETURN_ON_FALSE(rf_code != NULL, ESP_ERR_INVALID_ARG, TAG, "rf_code is NULL");
    ESP_RETURN_ON_FALSE(slot_id > 0, ESP_ERR_INVALID_ARG, TAG, "invalid slot_id");
    ESP_RETURN_ON_FALSE(rf_code_len > 0 && rf_code_len <= DEVICE_CONFIG_MAX_RF_CODE_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid rf_code length");
    ESP_RETURN_ON_FALSE(rf_code_shape_valid(rf_code_len, rf_bits, rf_band),
                        ESP_ERR_INVALID_ARG, TAG, "invalid RF code shape");

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_CONFIG_PAIR_NAMESPACE, NVS_READWRITE, &handle),
                        TAG, "open pair namespace");

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "slot_id", slot_id), cleanup, TAG, "write slot_id");
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, "hub_mac", hub_mac, 6), cleanup, TAG, "write hub_mac");
    ESP_GOTO_ON_ERROR(nvs_set_blob(handle, "rf_code", rf_code, rf_code_len), cleanup, TAG, "write rf_code");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "rf_bits", rf_bits), cleanup, TAG, "write rf_bits");
    ESP_GOTO_ON_ERROR(nvs_set_u8(handle, "rf_band", rf_band), cleanup, TAG, "write rf_band");
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, "rf_code_ver", rf_code_ver), cleanup, TAG, "write rf_code_ver");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), cleanup, TAG, "commit");

    memset(cfg, 0, sizeof(*cfg));
    cfg->slot_id = slot_id;
    memcpy(cfg->hub_mac, hub_mac, 6);
    memcpy(cfg->rf_code, rf_code, rf_code_len);
    cfg->rf_code_len = rf_code_len;
    cfg->rf_code_bits = rf_bits;
    cfg->rf_code_ver = rf_code_ver;
    cfg->rf_band = rf_band;
    cfg->paired = true;

    ESP_LOGI(TAG, "Pairing saved: slot=%u band=%s", slot_id, rf_band == 0 ? "433M" : "2.4G");

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t device_config_factory_reset(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(DEVICE_CONFIG_PAIR_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Factory reset skipped, pairing namespace absent");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open pair namespace");

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    ESP_LOGW(TAG, "Factory reset %s", err == ESP_OK ? "complete" : "failed");
    return err;
}

bool device_config_has_rf_code(const device_config_t *cfg)
{
    return cfg != NULL && cfg->paired && cfg->rf_code_ver > 0 &&
           rf_code_shape_valid(cfg->rf_code_len, cfg->rf_code_bits, cfg->rf_band);
}
