#include "vibrator.h"
#include "esp_log.h"

static bool vibrator_lock(VibratorHandler *vibrator_handler)
{
    return vibrator_handler->state_lock != NULL &&
           xSemaphoreTake(vibrator_handler->state_lock, portMAX_DELAY) == pdTRUE;
}

static void vibrator_unlock(VibratorHandler *vibrator_handler)
{
    xSemaphoreGive(vibrator_handler->state_lock);
}

static void vibrator_pulse_task(void *arg)
{
    VibratorHandler *vibrator_handler = (VibratorHandler *)arg;

    while (1) {
        bool pulsing = false;
        if (vibrator_lock(vibrator_handler)) {
            pulsing = vibrator_handler->vibrator_pulsing;
            vibrator_unlock(vibrator_handler);
        }

        if (!pulsing) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (vibrator_lock(vibrator_handler)) {
            if (vibrator_handler->vibrator_pulsing && !vibrator_handler->vibrator_active) {
                gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_ON);
                vibrator_handler->vibrator_active = true;
            }
            vibrator_unlock(vibrator_handler);
        }
        vTaskDelay(pdMS_TO_TICKS(VIBRATOR_PULSE_MS));

        if (vibrator_lock(vibrator_handler)) {
            if (vibrator_handler->vibrator_active) {
                gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF);
                vibrator_handler->vibrator_active = false;
            }
            pulsing = vibrator_handler->vibrator_pulsing;
            vibrator_unlock(vibrator_handler);
        }
        if (!pulsing) {
            continue;
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
    vibrator_handler->state_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(vibrator_handler->state_lock != NULL, ESP_ERR_NO_MEM, VIBRATOR_TAG,
                        "Failed to create state lock");

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
        vSemaphoreDelete(vibrator_handler->state_lock);
        vibrator_handler->state_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(VIBRATOR_TAG, "Vibrator initialized");
    return ESP_OK;
}

esp_err_t vibrator_start(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_lock(vibrator_handler), ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Failed to lock state");
    if (!vibrator_handler->vibrator_initialized) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }
    if (vibrator_handler->vibrator_active) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }

    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_active = true;
    vibrator_unlock(vibrator_handler);
    ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_ON));
    ESP_LOGI(VIBRATOR_TAG, "Vibrator started");
    return ESP_OK;
}

esp_err_t vibrator_stop(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_lock(vibrator_handler), ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Failed to lock state");
    if (!vibrator_handler->vibrator_initialized) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }
    if (!vibrator_handler->vibrator_active && !vibrator_handler->vibrator_pulsing) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }

    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_active = false;
    vibrator_unlock(vibrator_handler);
    ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF));
    ESP_LOGI(VIBRATOR_TAG, "Vibrator stopped");
    return ESP_OK;
}

esp_err_t vibrator_pulse(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_lock(vibrator_handler), ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Failed to lock state");
    if (!vibrator_handler->vibrator_initialized) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }
    if (vibrator_handler->vibrator_pulsing) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }
    vibrator_unlock(vibrator_handler);

    esp_err_t err = vibrator_start(vibrator_handler);
    if (err != ESP_OK) {
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
    ESP_RETURN_ON_FALSE(vibrator_lock(vibrator_handler), ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Failed to lock state");
    if (!vibrator_handler->vibrator_initialized) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }

    vibrator_handler->vibrator_pulsing = enabled;
    bool force_off = !enabled && vibrator_handler->vibrator_active;
    if (force_off) {
        vibrator_handler->vibrator_active = false;
    }
    vibrator_unlock(vibrator_handler);

    if (force_off) {
        ESP_ERROR_CHECK(gpio_set_level(vibrator_handler->vibrator_gpio, VIBRATOR_OFF));
    }

    ESP_LOGI(VIBRATOR_TAG, "Continuous pulsing %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t vibrator_deinit(VibratorHandler* vibrator_handler)
{
    ESP_RETURN_ON_FALSE(vibrator_handler, ESP_ERR_INVALID_ARG, VIBRATOR_TAG, "Invalid vibrator handler");
    ESP_RETURN_ON_FALSE(vibrator_lock(vibrator_handler), ESP_ERR_INVALID_STATE, VIBRATOR_TAG, "Failed to lock state");
    if (!vibrator_handler->vibrator_initialized) {
        vibrator_unlock(vibrator_handler);
        return ESP_ERR_INVALID_STATE;
    }

    gpio_num_t gpio = vibrator_handler->vibrator_gpio;
    vibrator_handler->vibrator_gpio = GPIO_NUM_MAX;
    vibrator_handler->vibrator_pulsing = false;
    vibrator_handler->vibrator_initialized = false;
    vibrator_handler->vibrator_active = false;
    TaskHandle_t task_handle = vibrator_handler->vibrator_task_handle;
    vibrator_handler->vibrator_task_handle = NULL;
    SemaphoreHandle_t state_lock = vibrator_handler->state_lock;

    if (task_handle) {
        vTaskDelete(task_handle);
    }
    vibrator_handler->state_lock = NULL;
    xSemaphoreGive(state_lock);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << gpio),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    if (state_lock) {
        vSemaphoreDelete(state_lock);
    }

    ESP_LOGI(VIBRATOR_TAG, "Vibrator deinitialized");
    return ESP_OK;
}
