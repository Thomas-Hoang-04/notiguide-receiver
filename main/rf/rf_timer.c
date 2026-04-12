/**
 * @file rf_timer.c
 * @brief RF Timer Implementation
 *
 * Implements precise timing control for RF signal generation using ESP32
 * general purpose timer. Handles pulse sequence timing and transmission
 * repetition control through interrupt-driven callbacks.
 */

#include "rf_common.h"
#include "esp_log.h"

static bool IRAM_ATTR rf_timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    RFHandler* rf_rmt = (RFHandler*)arg;

    if (rf_rmt->tx_active) {
        if (rf_rmt->current_pulse < rf_rmt->pulse_count_per_chn) {
            // Continue with next pulse in sequence
            gpio_set_level(rf_rmt->tx_gpio, rf_rmt->pulses[rf_rmt->current_chn][rf_rmt->current_pulse].level);

            // Set timer alarm for next pulse duration
            gptimer_alarm_config_t next_alarm = {
                .alarm_count = edata->alarm_value + rf_rmt->pulses[rf_rmt->current_chn][rf_rmt->current_pulse].pulse_length,
            };
            gptimer_set_alarm_action(timer, &next_alarm);

            rf_rmt->current_pulse++;
        } else {
            // End of pulse sequence reached
            gptimer_stop(timer);
            gptimer_set_raw_count(timer, 0);  // Reset timer counter
            gpio_set_level(rf_rmt->tx_gpio, 0);  // Ensure GPIO is low
            rf_rmt->current_pulse = 0;
            rf_rmt->current_rep++;

            if (rf_rmt->current_rep < rf_rmt->repeat_count) {
                // Start next repetition
                gpio_set_level(rf_rmt->tx_gpio, 1);  // Start with high pulse
                gptimer_alarm_config_t alarm_config = {
                    .alarm_count = rf_rmt->pulses[rf_rmt->current_chn][rf_rmt->current_pulse].pulse_length,
                };
                gptimer_set_alarm_action(timer, &alarm_config);
                gptimer_start(timer);
                rf_rmt->current_pulse++;
            } else {
                // All repetitions completed, reset transmission state
                rf_rmt->current_rep = 0;

                // Notify waiting task that transmission is complete
                if (rf_rmt->rf_trans_handle)
                    vTaskNotifyGiveFromISR(rf_rmt->rf_trans_handle, &xHigherPriorityTaskWoken);
            }
        }
    }

    return (xHigherPriorityTaskWoken == pdTRUE);
}

esp_err_t rf_timer_init(RFHandler* rf_rmt) {
    // Configure timer with 1MHz resolution for microsecond precision
    gptimer_config_t rf_timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = DEFAULT_RESOLUTION,  // 1MHz = 1μs resolution
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&rf_timer_config, &rf_rmt->timer));
    ESP_LOGI(RF_TAG, "Timer created");

    // Register interrupt callback for timer alarms
    gptimer_event_callbacks_t rf_timer_cb = {
        .on_alarm = rf_timer_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(rf_rmt->timer, &rf_timer_cb, rf_rmt));
    ESP_LOGI(RF_TAG, "Timer callbacks registered");

    // Enable timer for operation
    ESP_ERROR_CHECK(gptimer_enable(rf_rmt->timer));
    ESP_LOGI(RF_TAG, "Timer enabled");

    return ESP_OK;
}

esp_err_t rf_timer_deinit(gptimer_handle_t timer) {
    gptimer_stop(timer);                    // Stop timer operation
    ESP_ERROR_CHECK(gptimer_disable(timer)); // Disable timer
    ESP_ERROR_CHECK(gptimer_del_timer(timer)); // Delete timer and free resources
    ESP_LOGI(RF_TAG, "Timer deleted");
    return ESP_OK;
}

esp_err_t rf_timer_reset(RFHandler* rf_rmt) {
    gptimer_stop(rf_rmt->timer); // Stop timer
    // Reset counter to 0
    ESP_ERROR_CHECK(gptimer_set_raw_count(rf_rmt->timer, 0));

    // Reset transmission state
    rf_rmt->current_pulse = 0;
    rf_rmt->current_rep = 0;
    rf_rmt->tx_active = false;

    // Ensure GPIO is in idle state
    gpio_set_level(rf_rmt->tx_gpio, 0);

    ESP_LOGI(RF_TAG, "Timer reset");
    return ESP_OK;
}