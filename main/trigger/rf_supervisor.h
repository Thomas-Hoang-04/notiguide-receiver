/**
 * @file rf_supervisor.h
 * @brief RF Supervisor Module - Lifecycle Interface
 *
 * Declares the thin lifecycle wrapper around the RF receiver module used to
 * start, suspend, resume, and delete the RF task safely.
 */

#ifndef RF_SUPERVISOR_H
#define RF_SUPERVISOR_H

#include "esp_err.h"

esp_err_t rf_sup_start(void);
esp_err_t rf_sup_suspend(void);
esp_err_t rf_sup_resume(void);
esp_err_t rf_sup_delete(void);

#endif
