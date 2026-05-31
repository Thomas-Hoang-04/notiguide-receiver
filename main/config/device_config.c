/**
 * @file device_config.c
 * @brief Minimal NVS-backed pairing configuration for ESP8266 receivers.
 */

#include "config/device_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define KEY_SLOT_ID "slot_id"
#define KEY_HUB_MAC "hub_mac"
#define KEY_RF_CODE "rf_code"
#define KEY_RF_BITS "rf_bits"

static void reset_pairing_config(device_config_t *cfg)
{
    *cfg = (device_config_t){
        .slot_id = 0,
        .hub_mac = {0},
        .rf_code = 0,
        .rf_code_bits = 0,
        .paired = false,
    };
}

esp_err_t nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(DEVICE_CONFIG_TAG, "NVS init failed (%s), erasing flash",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }

    return err;
}

esp_err_t device_config_load(device_config_t *cfg)
{
    esp_err_t err;
    nvs_handle_t handle;
    size_t mac_len = 6;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    reset_pairing_config(cfg);

    err = nvs_open(DEVICE_CONFIG_PAIR_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(DEVICE_CONFIG_TAG, "No pairing data in NVS");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(handle, KEY_SLOT_ID, &cfg->slot_id);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_get_blob(handle, KEY_HUB_MAC, cfg->hub_mac, &mac_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        reset_pairing_config(cfg);
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(DEVICE_CONFIG_TAG, "Invalid hub MAC length, treating as unpaired");
        nvs_close(handle);
        reset_pairing_config(cfg);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (mac_len != sizeof(cfg->hub_mac)) {
        ESP_LOGW(DEVICE_CONFIG_TAG, "Invalid hub MAC length, treating as unpaired");
        nvs_close(handle);
        reset_pairing_config(cfg);
        return ESP_OK;
    }

    err = nvs_get_u32(handle, KEY_RF_CODE, &cfg->rf_code);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        reset_pairing_config(cfg);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_get_u8(handle, KEY_RF_BITS, &cfg->rf_code_bits);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        reset_pairing_config(cfg);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);

    if (cfg->slot_id == 0 || cfg->rf_code_bits != 32) {
        ESP_LOGW(DEVICE_CONFIG_TAG, "Invalid pairing record, treating as unpaired");
        reset_pairing_config(cfg);
        return ESP_OK;
    }

    cfg->paired = true;
    ESP_LOGI(DEVICE_CONFIG_TAG, "Loaded pairing: slot=%u rf_bits=%u",
             cfg->slot_id, cfg->rf_code_bits);
    return ESP_OK;
}

esp_err_t device_config_save_pairing(device_config_t *cfg,
                                     uint8_t slot_id,
                                     const uint8_t hub_mac[6],
                                     uint32_t rf_code,
                                     uint8_t rf_bits)
{
    esp_err_t err;
    nvs_handle_t handle;

    if (!cfg || !hub_mac) {
        return ESP_ERR_INVALID_ARG;
    }
    if (slot_id == 0 || rf_bits != 32) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(DEVICE_CONFIG_PAIR_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, KEY_SLOT_ID, slot_id);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, KEY_HUB_MAC, hub_mac, 6);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(handle, KEY_RF_CODE, rf_code);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, KEY_RF_BITS, rf_bits);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    cfg->slot_id = slot_id;
    memcpy(cfg->hub_mac, hub_mac, sizeof(cfg->hub_mac));
    cfg->rf_code = rf_code;
    cfg->rf_code_bits = rf_bits;
    cfg->paired = true;

    ESP_LOGI(DEVICE_CONFIG_TAG, "Saved pairing: slot=%u", cfg->slot_id);
    return ESP_OK;
}

esp_err_t device_config_factory_reset(void)
{
    esp_err_t err = nvs_flash_erase();

    if (err == ESP_OK) {
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(DEVICE_CONFIG_TAG, "Factory reset complete");
    } else {
        ESP_LOGE(DEVICE_CONFIG_TAG, "Factory reset failed: %s", esp_err_to_name(err));
    }

    return err;
}
