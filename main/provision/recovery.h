/**
 * @file recovery.h
 * @brief Provisioning Recovery Mode - Interface
 *
 * Declares the helper that enters SoftAP recovery mode, serves the
 * provisioning UI, and returns the requested recovery outcome to main.
 */

#ifndef PROVISION_RECOVERY_H
#define PROVISION_RECOVERY_H

typedef enum {
    PROVISION_RECOVERY_RESULT_RESTART = 0,
    PROVISION_RECOVERY_RESULT_RETRY_EXISTING,
} provision_recovery_result_t;

typedef enum {
    PROVISION_RECOVERY_REASON_UNPROVISIONED = 0,
    PROVISION_RECOVERY_REASON_WIFI_FAILED,
    PROVISION_RECOVERY_REASON_MQTT_FAILED,
    PROVISION_RECOVERY_REASON_MISSING_ENROLL_TOKEN,
    PROVISION_RECOVERY_REASON_BOOTSTRAP_FAILED,
} provision_recovery_reason_t;

provision_recovery_result_t provision_run_recovery_mode(provision_recovery_reason_t reason);
const char *provision_recovery_reason_name(provision_recovery_reason_t reason);
const char *provision_recovery_get_active_reason(void);

#endif
