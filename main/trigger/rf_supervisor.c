/**
 * @file rf_supervisor.c
 * @brief RF Supervisor Module - Lifecycle Implementation
 *
 * Implements the ESP-01 specific RF lifecycle wrapper that delegates task
 * ownership and ISR management to the underlying RF receiver module.
 */

#include "trigger/rf_supervisor.h"

#include "rf/rf_common.h"

#define RF_RX_GPIO GPIO_NUM_0

extern RFHandler g_rf;

esp_err_t rf_sup_start(void)
{
    return rf_recv_start_task(RF_RX_GPIO, &g_rf);
}

esp_err_t rf_sup_suspend(void)
{
    if (!g_rf.rx_active) {
        return ESP_OK;
    }
    if (g_rf.rx_suspended) {
        return ESP_OK;
    }
    return rf_recv_suspend(&g_rf);
}

esp_err_t rf_sup_resume(void)
{
    if (!g_rf.rx_active) {
        return rf_sup_start();
    }
    if (!g_rf.rx_suspended) {
        return ESP_OK;
    }
    return rf_recv_resume(&g_rf);
}

esp_err_t rf_sup_delete(void)
{
    if (!g_rf.rx_active) {
        return ESP_OK;
    }
    return rf_recv_deinit(&g_rf);
}
