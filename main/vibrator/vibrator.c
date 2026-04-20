#include "vibrator.h"
#include "esp_log.h"

static void vibrator_pulse_task(void *arg)
{
    VibratorHandler *vibrator_handler = (VibratorHandler *)arg;

    while (1) {
        if (!vibrator_handler->vibrator_pulsing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!vibrator_handler->vibrator_active) {
            gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_ON);
            vibrator_handler->vibrator_active = true;
        }
        vTaskDelay(pdMS_TO_TICKS(VIBRATOR_PULSE_MS));

        if (!vibrator_handler->vibrator_pulsing) {
            continue;
        }

        if (vibrator_handler->vibrator_active) {
            gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF);
            vibrator_handler->vibrator_active = false;
        }
        vTaskDelay(pdMS_TO_TICKS(VIBRATOR_GAP_MS));
    }
}

esp_err_t vibrator_init(gpio_num_t vibrator_gpio, VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");

    vibrator_handler->vibrator_initialized = false;
    vibrator_handler->vibrator_active = false;
    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_gpio = GPIO_NUM_MAX;
    vibrator_handler->vibrator_task_handle = NULL;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << vibrator_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(vibrator_gpio, VIBRATOR_OFF));
    vibrator_handler->vibrator_gpio = vibrator_gpio;
    ESP_LOGI(VIBRATOR_TAG, "GPIO %d configured for vibrator", vibrator_gpio);

    vibrator_handler->vibrator_initialized = true;

    if (xTaskCreate(vibrator_pulse_task,
                    "vibrator_task",
                    2048,
                    vibrator_handler,
                    4,
                    &vibrator_handler->vibrator_task_handle) != pdPASS) {
        gpio_set_level(vibrator_gpio, VIBRATOR_OFF);
        vibrator_handler->vibrator_initialized = false;
        vibrator_handler->vibrator_gpio = GPIO_NUM_MAX;
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(VIBRATOR_TAG, "Vibrator initialized");
    return ESP_OK;
}

esp_err_t vibrator_start(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_initialized, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not initialized");
    ESP_RETURN_ON_FALSE(!vibrator_handler->vibrator_active, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is already active");

    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_active = true;
    ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_ON));
    ESP_LOGI(VIBRATOR_TAG, "Vibrator started");
    return ESP_OK;
}

esp_err_t vibrator_stop(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_initialized, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not initialized");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_active, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not active");

    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_active = false;
    ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF));
    ESP_LOGI(VIBRATOR_TAG, "Vibrator stopped");
    return ESP_OK;
}

esp_err_t vibrator_pulse(VibratorHandler* vibrator_handler)
{
    esp_err_t err = vibrator_start(vibrator_handler);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(VIBRATOR_PULSE_MS));

    if (!vibrator_handler->vibrator_active) {
        return ESP_OK;
    }

    return vibrator_stop(vibrator_handler);
}

esp_err_t vibrator_set_pulsing(VibratorHandler* vibrator_handler, bool enabled)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_initialized, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not initialized");

    vibrator_handler->vibrator_pulsing = enabled;
    if (!enabled && vibrator_handler->vibrator_active) {
        vibrator_handler->vibrator_active = false;
        ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF));
    }

    ESP_LOGI(VIBRATOR_TAG, "Continuous pulsing %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t vibrator_toggle_pulsing(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_initialized, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not initialized");

    return vibrator_set_pulsing(vibrator_handler, !vibrator_handler->vibrator_pulsing);
}

esp_err_t vibrator_deinit(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_handler->vibrator_initialized, ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Vibrator is not initialized");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << vibrator_handler->vibrator_gpio),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    vibrator_handler->vibrator_gpio = GPIO_NUM_MAX;

    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_initialized = false;
    vibrator_handler->vibrator_active = false;

    if (vibrator_handler->vibrator_task_handle) {
        vTaskDelete(vibrator_handler->vibrator_task_handle);
        vibrator_handler->vibrator_task_handle = NULL;
    }

    ESP_LOGI(VIBRATOR_TAG, "Vibrator deinitialized");
    return ESP_OK;
}
