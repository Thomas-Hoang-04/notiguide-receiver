/**
 * @file device_config.h
 * @brief Persistent device configuration and NVS helpers.
 */

#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define DEVICE_CONFIG_SCHEMA_VERSION          1U
#define DEVICE_CONFIG_NAMESPACE               "device_cfg"
#define DEVICE_IDENTITY_NAMESPACE             "identity"
#define DEVICE_CONFIG_MAX_WIFI_SSID_LEN       64U
#define DEVICE_CONFIG_MAX_WIFI_PASSWORD_LEN   128U
#define DEVICE_CONFIG_MAX_MQTT_URI_LEN        191U
#define DEVICE_CONFIG_MAX_MQTT_USER_LEN       64U
#define DEVICE_CONFIG_MAX_MQTT_PASSWORD_LEN   128U
#define DEVICE_CONFIG_MAX_TOKEN_LEN           128U
#define DEVICE_CONFIG_MAX_PUBLIC_ID_LEN       256U
#define DEVICE_CONFIG_MAX_DEVICE_NAME_LEN     256U
#define DEVICE_CONFIG_MAX_COMMAND_ID_LEN      63U
#define DEVICE_CONFIG_MAX_RF_CODE_LEN         32U

typedef enum {
    RECEIVER_TYPE_433M = 0,
    RECEIVER_TYPE_2_4G = 1,
} receiver_type_t;

typedef enum {
    OP_STATE_NONE = 0,
    OP_STATE_PENDING_RF_CODE = 1,
    OP_STATE_ACTIVE = 2,
    OP_STATE_SUSPENDED = 3,
    OP_STATE_DECOMMISSIONED = 4,
} receiver_op_state_t;

typedef enum {
    DEVICE_BOOT_STATE_UNPROVISIONED = 0,
    DEVICE_BOOT_STATE_PENDING_ACTIVATION,
    DEVICE_BOOT_STATE_RECOVERY_REQUIRED,
    DEVICE_BOOT_STATE_OPERATIONAL,
} device_boot_state_t;

/**
 * @brief In-RAM mirror of the persisted `device_cfg` namespace.
 */
typedef struct {
    char wifi_ssid[DEVICE_CONFIG_MAX_WIFI_SSID_LEN + 1];
    char wifi_pwd[DEVICE_CONFIG_MAX_WIFI_PASSWORD_LEN + 1];
    char mqtt_uri[DEVICE_CONFIG_MAX_MQTT_URI_LEN + 1];
    char mqtt_user[DEVICE_CONFIG_MAX_MQTT_USER_LEN + 1];
    char mqtt_pwd[DEVICE_CONFIG_MAX_MQTT_PASSWORD_LEN + 1];
    char enroll_token[DEVICE_CONFIG_MAX_TOKEN_LEN + 1];
    char public_id[DEVICE_CONFIG_MAX_PUBLIC_ID_LEN + 1];
    char device_name[DEVICE_CONFIG_MAX_DEVICE_NAME_LEN + 1];
    char last_deact_id[DEVICE_CONFIG_MAX_COMMAND_ID_LEN + 1];
    uint8_t rf_code[DEVICE_CONFIG_MAX_RF_CODE_LEN];
    size_t rf_code_len;
    uint8_t schema_ver;
    uint8_t rf_code_bits;
    uint32_t rf_code_ver;
    receiver_type_t rx_type;
    receiver_op_state_t op_state;
    bool has_wifi;
    bool has_enroll_token;
    bool has_public_id;
    bool has_device_name;
    bool has_last_deact_id;
    bool has_rf_code;
    bool has_rx_type;
    bool has_op_state;
} device_config_t;

/**
 * @brief Provisioning bundle accepted from the local recovery UI.
 */
typedef struct {
    char wifi_ssid[DEVICE_CONFIG_MAX_WIFI_SSID_LEN + 1];
    char wifi_pwd[DEVICE_CONFIG_MAX_WIFI_PASSWORD_LEN + 1];
    char mqtt_uri[DEVICE_CONFIG_MAX_MQTT_URI_LEN + 1];
    char mqtt_user[DEVICE_CONFIG_MAX_MQTT_USER_LEN + 1];
    char mqtt_pwd[DEVICE_CONFIG_MAX_MQTT_PASSWORD_LEN + 1];
    char enroll_token[DEVICE_CONFIG_MAX_TOKEN_LEN + 1];
} device_provisioning_t;

/**
 * @brief Initialize NVS or recover from truncated/old pages.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t nvs_init_or_recover(void);

/**
 * @brief Load the current persisted device configuration into RAM.
 *
 * @param cfg Configuration structure to populate
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_load(device_config_t *cfg);

/**
 * @brief Persist a fresh provisioning bundle and clear activation state.
 *
 * @param cfg In-RAM configuration mirror to refresh
 * @param prov Provisioning data to save
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_save_provisioning(device_config_t *cfg, const device_provisioning_t *prov);

/**
 * @brief Persist backend activation metadata and enter `PENDING_RF_CODE`.
 *
 * @param cfg In-RAM configuration mirror to update
 * @param public_id Backend-minted operational identifier
 * @param device_name Backend-assigned human-readable name
 * @param rx_type Receiver type confirmed by activation
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_commit_activation(device_config_t *cfg,
                                          const char *public_id,
                                          const char *device_name,
                                          receiver_type_t rx_type);

/**
 * @brief Persist an RF trigger code bundle.
 *
 * @param cfg In-RAM configuration mirror to update
 * @param code Raw RF code bytes
 * @param code_len Number of valid bytes in @p code
 * @param bits Width of the code in bits
 * @param version Monotonic version within the current `public_id`
 * @param promote_active True to atomically switch `op_state` to `ACTIVE`
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_commit_rf_code(device_config_t *cfg,
                                       const uint8_t *code,
                                       size_t code_len,
                                       uint8_t bits,
                                       uint32_t version,
                                       bool promote_active);

/**
 * @brief Persist a new operational state and optional command id.
 *
 * @param cfg In-RAM configuration mirror to update
 * @param op_state New operational state
 * @param last_deact_id Optional last processed deactivation command id
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_commit_op_state(device_config_t *cfg,
                                        receiver_op_state_t op_state,
                                        const char *last_deact_id);

/**
 * @brief Erase the enrollment token after bootstrap completion or timeout.
 *
 * @param cfg In-RAM configuration mirror to update
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_clear_enroll_token(device_config_t *cfg);

/**
 * @brief Erase the runtime configuration namespace while preserving identity.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_config_reprovision(void);

/**
 * @brief Return the receiver type compiled into this firmware image.
 *
 * @return Compiled receiver type
 */
receiver_type_t device_config_compiled_receiver_type(void);

/**
 * @brief Convert a receiver type enum to its wire-format string.
 *
 * @param type Receiver type to stringify
 * @return Stable string representation
 */
const char *device_config_receiver_type_string(receiver_type_t type);

/**
 * @brief Convert an operational state enum to a diagnostic string.
 *
 * @param state Operational state to stringify
 * @return Stable string representation
 */
const char *device_config_op_state_string(receiver_op_state_t state);

/**
 * @brief Derive the boot state from the persisted configuration.
 *
 * @param cfg Configuration to evaluate
 * @return Derived boot state
 */
device_boot_state_t device_config_boot_state(const device_config_t *cfg);

/**
 * @brief Report whether a complete RF code bundle is present in RAM.
 *
 * @param cfg Configuration to inspect
 * @return True when the RF code is populated and valid
 */
bool device_config_has_rf_code(const device_config_t *cfg);

#endif /* DEVICE_CONFIG_H */
