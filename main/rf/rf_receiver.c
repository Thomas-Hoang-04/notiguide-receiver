/**
 * @file rf_receiver.c
 * @brief RF Receiver Implementation
 *
 * Implements RF signal reception and decoding functionality. Captures timing
 * data from GPIO interrupts, analyzes pulse patterns to detect protocols,
 * and decodes received data into binary and tri-state formats.
 */

#include "rf_common.h"
#include "sdkconfig.h"

#if CONFIG_RECEIVER_RADIO_433M

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "trigger/rf_trigger.h"

static inline uint32_t diff(uint32_t a, uint32_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static bool IRAM_ATTR recv_proto(RFHandler* rf_rmt, uint8_t proto_idx, uint32_t edge_count) {
    const Protocol curr_proto = proto[proto_idx];

    uint32_t code = 0;
    // Determine sync pulse length (use longer of high/low sync factors)
    const uint32_t sync_len_in_pulses = curr_proto.sync_factor.low > curr_proto.sync_factor.high
        ? curr_proto.sync_factor.low : curr_proto.sync_factor.high;

    // Calculate expected pulse delay from sync timing
    const uint32_t delay = rf_rmt->recv_timings[0] / sync_len_in_pulses;
    const uint32_t delay_tolerance = delay * rf_rmt->recv_tolerance / 100;

    // Determine starting position based on protocol inversion
    const uint8_t first_data_timing = curr_proto.inverted ? 2 : 1;

    // Decode each bit pair according to protocol timing
    for (uint8_t i = first_data_timing; i < edge_count - 1; i += 2) {
        code <<= 1;  // Shift previous bits left

        // Check if timing matches '0' bit pattern
        if (diff(rf_rmt->recv_timings[i], delay * curr_proto.zero.high) < delay_tolerance &&
                diff(rf_rmt->recv_timings[i + 1], delay * curr_proto.zero.low) < delay_tolerance) {
            code |= 0;  // Add '0' bit
        }
        // Check if timing matches '1' bit pattern
        else if (diff(rf_rmt->recv_timings[i], delay * curr_proto.one.high) < delay_tolerance &&
                diff(rf_rmt->recv_timings[i + 1], delay * curr_proto.one.low) < delay_tolerance) {
            code |= 1;  // Add '1' bit
        } else {
            return false;  // Timing doesn't match protocol
        }
    }

    // Ignore very short transmissions (presumably noise)
    if (edge_count > 7) {
        rf_rmt->recv_value = code;
        rf_rmt->recv_bit_length = (edge_count - 1) / 2;
        rf_rmt->recv_delay = delay;
        rf_rmt->recv_proto = proto_idx;
        rf_rmt->recv_pending = true;
        return true;
    }

    return false;
}

static void IRAM_ATTR rf_recv_notify_task(RFHandler *rf_rmt)
{
    TaskHandle_t task = rf_rmt->rf_recv_handle;
    if (task == NULL) {
        return;
    }

    BaseType_t higher_priority_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_woken);
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR rf_recv_isr_handler(void* arg) {
    RFHandler* rf_rmt = (RFHandler*)arg;

    // Static variables maintain state between ISR calls
    static uint32_t edge_count = 0;
    static uint64_t last_time = 0;
    static uint32_t repeat_count = 0;

    const int64_t time = esp_timer_get_time();
    const uint32_t duration = (uint32_t)(time - last_time);

    if (duration > rf_rmt->separation_limit) {
        // Long stretch without signal level change -> presumably gap between transmissions
        if ((repeat_count == 0) || (diff(duration, rf_rmt->recv_timings[0]) < 200)) {
            /** Assuming sender transmits signal multiple times with roughly the same gap period.
             * This long signal is close in length to the signal which started previous records
             * -> potentially confirming it being a gap between two transmissions
             *
             * If the gap is not detected, the signal is likely a single transmission.
             * */
            repeat_count++;
            if (repeat_count == 2) {
                // Try to decode using all available protocols
                for (uint8_t i = 0; i < PROTO_COUNT; i++) {
                    if (recv_proto(rf_rmt, i, edge_count)) {
                        rf_recv_notify_task(rf_rmt);
                        break;
                    }
                }
                repeat_count = 0;
            }
        }

        edge_count = 0;  // Reset for new transmission
    }

    // Prevent buffer overflow
    if (edge_count >= MAX_EDGES) {
        edge_count = 0;
        repeat_count = 0;
    }

    // Store timing data and update timestamp
    rf_rmt->recv_timings[edge_count++] = duration;
    last_time = (uint64_t)time;
}

esp_err_t rf_recv_init(gpio_num_t rx_gpio, RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");

    rf_rmt->rx_active = false;
    rf_rmt->rx_suspended = false;
    rf_rmt->rx_gpio = GPIO_NUM_NC;
    rf_rmt->rf_recv_handle = NULL;

    // Configure GPIO for input with pull-up and edge interrupts
    gpio_config_t io_conf_rx = {
        .pin_bit_mask = (1ULL << rx_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,    // Enable pull-up for stable idle state
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,      // Trigger on both rising and falling edges
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf_rx));
    rf_rmt->rx_gpio = rx_gpio;
    ESP_LOGI(RF_TAG, "GPIO %d configured for RF reception", rx_gpio);

    // Install ISR service (may already be installed)
    esp_err_t isr_service_check = gpio_install_isr_service(0);
    if (isr_service_check == ESP_ERR_INVALID_STATE)
        ESP_LOGI(RF_TAG, "ISR service already installed");
    else {
        ESP_ERROR_CHECK(isr_service_check);
        ESP_LOGI(RF_TAG, "ISR service installed");
    }

    // Add ISR handler for this specific GPIO
    ESP_ERROR_CHECK(gpio_isr_handler_add(rx_gpio, rf_recv_isr_handler, rf_rmt));
    ESP_LOGI(RF_TAG, "ISR handler added for GPIO %d", rx_gpio);

    // Initialize reception parameters
    rf_rmt->recv_value = 0;
    rf_rmt->recv_bit_length = 0;
    rf_rmt->recv_delay = 0;
    rf_rmt->recv_proto = 0;
    rf_rmt->recv_pending = false;
    rf_rmt->separation_limit = SEPARATION_LIMIT;
    rf_rmt->recv_tolerance = RECV_TOLERANCE;

    rf_rmt->rx_active = true;

    ESP_LOGI(RF_TAG, "RF receiver initialized");
    return ESP_OK;
}

esp_err_t rf_recv_deinit(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_active && rf_rmt->rx_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not active");

    // Remove ISR handler and reset GPIO
    esp_err_t isr_status = gpio_isr_handler_remove(rf_rmt->rx_gpio);
    if (isr_status == ESP_ERR_INVALID_STATE)
        ESP_LOGI(RF_TAG, "ISR handler already removed for GPIO %d", rf_rmt->rx_gpio);
    else {
        ESP_ERROR_CHECK(isr_status);
        ESP_LOGI(RF_TAG, "ISR handler removed for GPIO %d", rf_rmt->rx_gpio);
    }

    // Reset GPIO pin and restore default state
    gpio_reset_pin(rf_rmt->rx_gpio);
    rf_rmt->rx_gpio = GPIO_NUM_NC;

    // Uninstall ISR service
    gpio_uninstall_isr_service();

    // Reset reception state
    rf_rmt->rx_active = false;
    rf_rmt->rx_suspended = false;

    if (rf_rmt->rf_recv_handle) {
        vTaskDelete(rf_rmt->rf_recv_handle);
        rf_rmt->rf_recv_handle = NULL;
    }

    ESP_LOGI(RF_TAG, "RF receiver deinitialized");
    return ESP_OK;
}

esp_err_t rf_recv_suspend(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_active && rf_rmt->rx_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not active");
    ESP_RETURN_ON_FALSE(!rf_rmt->rx_suspended, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is already suspended");

    // Remove ISR handler
    ESP_ERROR_CHECK(gpio_intr_disable(rf_rmt->rx_gpio));
    esp_err_t isr_status = gpio_isr_handler_remove(rf_rmt->rx_gpio);
    if (isr_status == ESP_ERR_INVALID_STATE)
        ESP_LOGI(RF_TAG, "ISR handler already removed for GPIO %d", rf_rmt->rx_gpio);
    else {
        ESP_ERROR_CHECK(isr_status);
        ESP_LOGI(RF_TAG, "ISR handler removed for GPIO %d", rf_rmt->rx_gpio);
    }

    // Suspend receiver task
    vTaskSuspend(rf_rmt->rf_recv_handle);
    while (eTaskGetState(rf_rmt->rf_recv_handle) != eSuspended)
        vTaskDelay(pdMS_TO_TICKS(5));

    rf_rmt->rx_suspended = true;
    return ESP_OK;
}

esp_err_t rf_recv_resume(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_active && rf_rmt->rx_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not active");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_suspended, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not suspended");

    // Enable interrupt and set interrupt type
    ESP_ERROR_CHECK(gpio_intr_enable(rf_rmt->rx_gpio));
    ESP_ERROR_CHECK(gpio_set_intr_type(rf_rmt->rx_gpio, GPIO_INTR_ANYEDGE));
    // Add ISR handler
    esp_err_t isr_status = gpio_isr_handler_add(rf_rmt->rx_gpio, rf_recv_isr_handler, rf_rmt);
    if (isr_status == ESP_ERR_INVALID_STATE)
        ESP_LOGI(RF_TAG, "ISR handler already added for GPIO %d", rf_rmt->rx_gpio);
    else {
        ESP_ERROR_CHECK(isr_status);
        ESP_LOGI(RF_TAG, "ISR handler added for GPIO %d", rf_rmt->rx_gpio);
    }

    // Resume receiver task
    rf_rmt->rx_suspended = false;
    vTaskDelay(pdMS_TO_TICKS(5));
    vTaskResume(rf_rmt->rf_recv_handle);

    return ESP_OK;
}

esp_err_t reset_recv(RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_active && rf_rmt->rx_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not active");
    ESP_RETURN_ON_FALSE(!rf_rmt->rx_suspended, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is suspended");

    rf_rmt->recv_value = 0;
    rf_rmt->recv_bit_length = 0;
    rf_rmt->recv_pending = false;
    return ESP_OK;
}

static void rf_recv_task(void* arg) {
    RFHandler* rf_rmt = (RFHandler*)arg;
    RFRecvData recv_data = { 0 };

    for (;;) {
        if (!rf_rmt->recv_pending) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        if (rf_rmt->recv_pending) {
            if (output_recv(rf_rmt, &recv_data) == ESP_OK) {
                rf_trigger_on_frame(recv_data.original_value, rf_rmt->recv_bit_length);
            }
            reset_recv(rf_rmt);
        }
    }
}

esp_err_t rf_recv_start_task(gpio_num_t rx_gpio, RFHandler* rf_rmt) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");

    if (rf_rmt->rx_active) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(rf_recv_init(rx_gpio, rf_rmt), RF_TAG, "rf_recv_init failed");
    if (xTaskCreate(rf_recv_task, "rf_rx", 4096, rf_rmt, 5, &rf_rmt->rf_recv_handle) != pdPASS) {
        rf_recv_deinit(rf_rmt);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t decode_recv(RFHandler* rf_rmt, RFRecvData* recv_data) {
    ESP_RETURN_ON_FALSE(rf_rmt, ESP_ERR_INVALID_ARG, RF_TAG, "Invalid RF module");
    ESP_RETURN_ON_FALSE(rf_rmt->rx_active && rf_rmt->rx_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is not active");
    ESP_RETURN_ON_FALSE(!rf_rmt->rx_suspended, ESP_ERR_INVALID_STATE, RF_TAG, "RF receiver is suspended");

    uint32_t code = rf_rmt->recv_value;
    const uint8_t recv_len = rf_rmt->recv_bit_length;
    recv_data->original_value = code;
    if (recv_data->binary) {
        free(recv_data->binary);
        recv_data->binary = NULL;
    }
    if (recv_data->tri_state) {
        free(recv_data->tri_state);
        recv_data->tri_state = NULL;
    }

    // Convert raw value to binary string
    esp_err_t ret = uint32_to_binary(code, &recv_data->binary, recv_len);
    if (ret != ESP_OK) {
        ESP_LOGE(RF_TAG, "Invalid data. Failed to convert raw value to binary string");
        if (recv_data->binary) {
            free(recv_data->binary);
            recv_data->binary = NULL;
        }
    }

    // Convert raw value to tri-state string
    ret = uint32_to_tristate(code, &recv_data->tri_state, recv_len);
    if (ret != ESP_OK) {
        ESP_LOGE(RF_TAG, "Invalid data. Failed to convert raw value to tri-state string");
        if (recv_data->tri_state) {
            free(recv_data->tri_state);
            recv_data->tri_state = NULL;
        }
    }

    ESP_LOGI(RF_TAG, "Decode completed. Status: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t output_recv(RFHandler* rf_rmt, RFRecvData* recv_data) {
    esp_err_t ret = decode_recv(rf_rmt, recv_data);
    if (ret != ESP_OK) {
        ESP_LOGE(RF_TAG, "Failed to decode received data");
        return ret;
    }

    ESP_LOGI(RF_TAG, "Received data!");
    ESP_LOGI(RF_TAG, "Original value: %lu", recv_data->original_value);
    ESP_LOGI(RF_TAG, "Hexadecimal: 0x%lx", recv_data->original_value);
    if (recv_data->binary)
        ESP_LOGI(RF_TAG, "Binary: %s", recv_data->binary);
    else
        ESP_LOGE(RF_TAG, "No binary data available");
    if (recv_data->tri_state)
        ESP_LOGI(RF_TAG, "Tri-state: %s", recv_data->tri_state);
    else
        ESP_LOGE(RF_TAG, "No tri-state data available");
    ESP_LOGI(RF_TAG, "Pulse length: %lu", rf_rmt->recv_delay);
    return ESP_OK;
}

#else

esp_err_t rf_recv_init(gpio_num_t rx_gpio, RFHandler* rf_rmt) {
    (void)rx_gpio;
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rf_recv_deinit(RFHandler* rf_rmt) {
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rf_recv_suspend(RFHandler* rf_rmt) {
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rf_recv_resume(RFHandler* rf_rmt) {
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rf_recv_start_task(gpio_num_t rx_gpio, RFHandler* rf_rmt) {
    (void)rx_gpio;
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t reset_recv(RFHandler* rf_rmt) {
    (void)rf_rmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t output_recv(RFHandler* rf_rmt, RFRecvData* recv_data) {
    (void)rf_rmt;
    (void)recv_data;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
