/**
 * @file http_server.h
 * @brief Provisioning HTTP Server - Interface
 *
 * Declares the SoftAP-only HTTP server used to serve the provisioning UI and
 * signal provisioning, retry, and factory-reset actions back to the main task.
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef enum {
    HTTP_SERVER_ACTION_NONE = 0,
    HTTP_SERVER_ACTION_PROVISIONED,
    HTTP_SERVER_ACTION_RETRY,
    HTTP_SERVER_ACTION_RESET,
} http_server_action_t;

esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
http_server_action_t http_server_wait_for_action(TickType_t ticks_to_wait);

#endif
