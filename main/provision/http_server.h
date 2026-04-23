/**
 * @file http_server.h
 * @brief Provisioning HTTP server and local recovery UI.
 */

#ifndef RECEIVER_HTTP_SERVER_H
#define RECEIVER_HTTP_SERVER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "config/device_config.h"

typedef enum {
    PROVISION_REASON_UNPROVISIONED = 0,
    PROVISION_REASON_WIFI_FAILED,
    PROVISION_REASON_RECOVERY_REQUIRED,
    PROVISION_REASON_BOOTSTRAP_FAILED,
} provision_reason_t;

/**
 * @brief Recovery web server context shared with the provisioning task.
 */
typedef struct {
    EventGroupHandle_t events;
    device_config_t *cfg;
    provision_reason_t reason;
} provision_http_context_t;

enum {
    PROVISION_EVENT_DONE = BIT0,
    PROVISION_EVENT_RETRY = BIT1,
    PROVISION_EVENT_RESET = BIT2,
};

/**
 * @brief Start the local provisioning HTTP server.
 *
 * @param ctx Server context with event bits, config mirror, and reason
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t provision_http_server_start(const provision_http_context_t *ctx);

/**
 * @brief Stop the local provisioning HTTP server.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t provision_http_server_stop(void);

/**
 * @brief Convert a provisioning reason into a stable diagnostic string.
 *
 * @param reason Recovery reason to stringify
 * @return Stable string representation
 */
const char *provision_reason_string(provision_reason_t reason);

#endif /* RECEIVER_HTTP_SERVER_H */
