/**
 * @file rf_supervisor.c
 * @brief Radio-variant dispatcher for receiver lifecycle operations.
 */

#include "trigger/rf_supervisor.h"
#include "config/device_config.h"
#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>

#if CONFIG_RECEIVER_RADIO_433M
#include "rf/rf_common.h"
static RFHandler s_rf;
#elif CONFIG_RECEIVER_RADIO_2_4G
#include "nrf24/nrf24_receiver.h"
static nrf24_handle_t s_nrf;
#endif

static bool s_started;
static bool s_suspended;

esp_err_t rf_sup_start(const device_config_t *cfg)
{
    if (s_started && !s_suspended) {
        return ESP_OK;
    }
    if (s_started && s_suspended) {
        return rf_sup_resume();
    }

#if CONFIG_RECEIVER_RADIO_433M
    (void)cfg;
    esp_err_t err = rf_recv_start_task((gpio_num_t)CONFIG_RECEIVER_RF433_RX_GPIO, &s_rf);
#else
    if (cfg == NULL || !device_config_has_rf_code(cfg) || cfg->rf_code_len != 5U ||
        cfg->rf_code_bits != 40U) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = nrf24_recv_start_task(&s_nrf, cfg->rf_code);
#endif
    if (err == ESP_OK) {
        s_started = true;
        s_suspended = false;
    }
    return err;
}

esp_err_t rf_sup_apply_rx_address(const uint8_t *addr, size_t addr_len)
{
#if CONFIG_RECEIVER_RADIO_433M
    (void)addr;
    (void)addr_len;
    return ESP_OK;
#else
    if (addr == NULL || addr_len != 5U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_OK;
    }
    return nrf24_recv_set_rx_address(&s_nrf, addr);
#endif
}

esp_err_t rf_sup_suspend(void)
{
    if (!s_started || s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_RECEIVER_RADIO_433M
    esp_err_t err = rf_recv_suspend(&s_rf);
#else
    esp_err_t err = nrf24_recv_suspend(&s_nrf);
#endif
    if (err == ESP_OK) {
        s_suspended = true;
    }
    return err;
}

esp_err_t rf_sup_resume(void)
{
    if (!s_started || !s_suspended) {
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_RECEIVER_RADIO_433M
    esp_err_t err = rf_recv_resume(&s_rf);
#else
    esp_err_t err = nrf24_recv_resume(&s_nrf);
#endif
    if (err == ESP_OK) {
        s_suspended = false;
    }
    return err;
}

esp_err_t rf_sup_delete(void)
{
    if (!s_started) {
        return ESP_OK;
    }

#if CONFIG_RECEIVER_RADIO_433M
    esp_err_t err = rf_recv_deinit(&s_rf);
#else
    esp_err_t err = nrf24_recv_deinit(&s_nrf);
#endif
    if (err == ESP_OK) {
        s_started = false;
        s_suspended = false;
    }
    return err;
}

bool rf_sup_is_started(void)
{
    return s_started;
}

bool rf_sup_is_suspended(void)
{
    return s_started && s_suspended;
}
