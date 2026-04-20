/**
 * @file device_config.h
 * @brief Device Configuration Module - Persistent State Interface
 *
 * Defines the NVS-backed configuration schema and helper APIs used to load,
 * persist, and erase receiver runtime state.
 */

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_CONFIG_TAG "DEVICE_CFG"
#define DEVICE_CONFIG_SCHEMA_VERSION 1

typedef enum {
    RECEIVER_OP_STATE_NONE = 0,
    RECEIVER_OP_STATE_PENDING_RF_CODE = 1,
    RECEIVER_OP_STATE_ACTIVE = 2,
    RECEIVER_OP_STATE_SUSPENDED = 3,
    RECEIVER_OP_STATE_DECOMMISSIONED = 4,
} receiver_op_state_t;

typedef struct {
    bool has_schema_ver;
    bool has_wifi_ssid;
    bool has_wifi_pwd;
    bool has_mqtt_uri;
    bool has_mqtt_user;
    bool has_mqtt_pwd;
    bool has_enroll_token;
    bool has_public_id;
    bool has_device_name;
    bool has_rf_code;
    bool has_op_state;
    bool has_last_deact_id;

    uint8_t schema_ver;
    char *wifi_ssid;
    char *wifi_pwd;
    char *mqtt_uri;
    char *mqtt_user;
    char *mqtt_pwd;
    char *enroll_token;
    char *public_id;
    char *device_name;
    uint32_t rf_code;
    uint8_t rf_code_bits;
    uint32_t rf_code_ver;
    receiver_op_state_t op_state;
    char *last_deact_id;
} device_config_t;

typedef struct {
    const char *wifi_ssid;
    const char *wifi_pwd;
    const char *mqtt_uri;
    const char *mqtt_user;
    const char *mqtt_pwd;
    const char *enroll_token;
} device_provisioning_t;

void device_config_init(device_config_t *cfg);
void device_config_free(device_config_t *cfg);
esp_err_t device_config_load(device_config_t *cfg);
bool device_config_has_any_data(const device_config_t *cfg);
bool device_config_is_provisioned(const device_config_t *cfg);
bool device_config_is_pending_activation(const device_config_t *cfg);
bool device_config_is_recovery_required(const device_config_t *cfg);
const char *device_config_state_name(const device_config_t *cfg);

esp_err_t device_config_store_provisioning(const device_provisioning_t *prov);
esp_err_t device_config_store_activation(const char *public_id,
                                         const char *device_name,
                                         receiver_op_state_t op_state);
esp_err_t device_config_clear_enroll_token(void);
esp_err_t device_config_store_rf_code(uint32_t rf_code,
                                      uint8_t rf_code_bits,
                                      uint32_t rf_code_ver,
                                      bool update_state,
                                      receiver_op_state_t new_state);
esp_err_t device_config_store_op_state(receiver_op_state_t op_state);
esp_err_t device_config_store_deactivation(receiver_op_state_t op_state,
                                           const char *last_deact_id);
esp_err_t device_config_erase_runtime(void);

#endif
