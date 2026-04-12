/**
 * @file rf_transmitter.c
 * @brief RF Transmitter Implementation
 *
 * Implements RF signal transmission functionality including tri-state data
 * encoding, pulse generation, and transmission control using ESP32 GPIO
 * and timer peripherals.
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"

#include "rf_common.h"
#include "esp_log.h"
#include "esp_check.h"
#include "Controls/controls.h"

volatile uint8_t rf_cmp_changed = 0;

esp_err_t rf_trans_init(gpio_num_t tx_gpio, int8_t repeat_count, Protocol* tx_proto, RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(tx_proto, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF protocol");

    // Configure GPIO pin for output
    gpio_config_t io_conf_tx = {
        .pin_bit_mask = (1ULL << tx_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_tx));
    ESP_ERROR_CHECK(gpio_set_level(tx_gpio, 0));  // Start with low level
    rf_rmt->tx_gpio = tx_gpio;
    ESP_LOGI(RF_TAG, "GPIO %d configured for RF transmission", tx_gpio);

    // Initialize transmission parameters
    rf_rmt->chn_count = 0;
    rf_rmt->pulse_count_per_chn = 0;
    rf_rmt->current_chn = 0;
    rf_rmt->current_pulse = 0;
    rf_rmt->current_rep = 0;
    rf_rmt->repeat_count = repeat_count;
    rf_rmt->proto = tx_proto;

    // Initialize pulse data storage
    for (int i = 0; i < 4; i++)
        rf_rmt->pulses[i] = NULL;

    rf_rmt->tx_active = false;

    // Initialize hardware timer for pulse timing
    ESP_ERROR_CHECK(rf_timer_init(rf_rmt));

    ESP_LOGI(RF_TAG, "RF transmitter initialized");

    return ESP_OK;
}

esp_err_t rf_trans_deinit(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->tx_active, ESP_ERR_INVALID_STATE, RF_TAG, "RF transmitter is not active");

    // Free pulse data memory
    for (int i = 0; i < 4; i++)
        if (rf_rmt->pulses[i]) {
            free(rf_rmt->pulses[i]);
            rf_rmt->pulses[i] = NULL;
        }

    rf_rmt->pulse_count_per_chn = 0;
    rf_rmt->current_chn = 0;
    rf_rmt->current_pulse = 0;

    // Reset transmission state
    rf_rmt->current_rep = 0;
    rf_rmt->repeat_count = 0;
    rf_rmt->tx_active = false;

    // Clear protocol reference
    rf_rmt->proto = NULL;

    // Deinitialize hardware timer
    ESP_ERROR_CHECK(rf_timer_deinit(rf_rmt->timer));

    // Reset GPIO pin and restore default state
    ESP_ERROR_CHECK(gpio_set_level(rf_rmt->tx_gpio, 0));
    gpio_reset_pin(rf_rmt->tx_gpio);
    rf_rmt->tx_gpio = GPIO_NUM_NC;

    // Delete transmitter task
    vTaskDelete(rf_rmt->rf_trans_handle);

    ESP_LOGI(RF_TAG, "RF module deinitialized");
    return ESP_OK;
}

esp_err_t rf_trans_suspend(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");

    // Wait for current transmission to complete
    while (rf_rmt->tx_active)
        vTaskDelay(pdMS_TO_TICKS(5));

    rf_rmt->tx_active = false;
    up_down_active = 0;

    // Suspend transmitter task
    vTaskSuspend(rf_rmt->rf_trans_handle);
    while (eTaskGetState(rf_rmt->rf_trans_handle) != eSuspended)
        vTaskDelay(pdMS_TO_TICKS(5));

    return ESP_OK;
}

esp_err_t rf_trans_resume(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");

    // Resume transmitter task
    vTaskResume(rf_rmt->rf_trans_handle);
    if (rf_cmp_changed) {
        if ((rf_cmp_changed >> 2) & 1)
            rf_trans_proto_select(rf_rmt, &proto[proto_idx]);
        rf_trans_event_t rf_recompute_chns = TRANS_RECOMPUTE_CHNS;
        if (xQueueSend(rf_trans_event_queue, &rf_recompute_chns, portMAX_DELAY) != pdTRUE)
            ESP_LOGE(RF_TAG, "Failed to send TRANS_RECOMPUTE_CHNS to queue");
        rf_cmp_changed = 0;
    }

    chn_lock_active = chn_count == CHN_COUNT_MAX;

    return ESP_OK;
}

esp_err_t rf_send(RFHandler* rf_rmt, uint8_t chn_idx) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(!rf_rmt->tx_active, ESP_ERR_INVALID_STATE, RF_TAG, "RF transmitter is already transmitting");
    ESP_RETURN_ON_FALSE(rf_rmt->pulses[chn_idx] && rf_rmt->pulse_count_per_chn > 0, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF data");

    ESP_LOGI(RF_TAG, "Starting RF transmission");

    // Temporarily disable receiver during transmission to avoid interference
    if (rf_rmt->rx_active && !rf_rmt->rx_suspended)
        ESP_ERROR_CHECK(rf_recv_suspend(rf_rmt));

    // Initialize transmission state
    rf_rmt->tx_active = true;
    rf_rmt->current_rep = 0;
    rf_rmt->current_chn = chn_idx;
    rf_rmt->current_pulse = 0;

    // Start transmission with first pulse
    gpio_set_level(rf_rmt->tx_gpio, rf_rmt->pulses[chn_idx][rf_rmt->current_pulse].level);
    gptimer_alarm_config_t alarm_config = {
        .alarm_count = rf_rmt->pulses[chn_idx][rf_rmt->current_pulse].pulse_length,
    };
    gptimer_set_alarm_action(rf_rmt->timer, &alarm_config);
    gptimer_start(rf_rmt->timer);
    rf_rmt->current_pulse++;

    return ESP_OK;
}

esp_err_t rf_trans_proto_select(RFHandler* rf_rmt, Protocol* tx_proto) {
    ESP_RETURN_ON_FALSE(rf_rmt && tx_proto, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF handler");
    rf_rmt->proto = tx_proto;
    return ESP_OK;
}

#pragma GCC diagnostic pop