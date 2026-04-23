/**
 * @file recovery.h
 * @brief Provisioning Recovery Mode - Interface
 *
 * Declares the helper that enters SoftAP recovery mode, serves the
 * provisioning UI, and reboots once the user completes an action.
 */

#ifndef PROVISION_RECOVERY_H
#define PROVISION_RECOVERY_H

void provision_run_recovery_mode(void);

#endif
