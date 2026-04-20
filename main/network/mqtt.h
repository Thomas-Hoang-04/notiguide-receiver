/**
 * @file mqtt.h
 * @brief MQTT Module - Secure Messaging Interface
 *
 * Declares the TLS-backed MQTT client APIs used for bootstrap activation,
 * operational command subscriptions, and upstream acknowledgements.
 */

#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>

#include "esp_err.h"

#include "config/device_config.h"

#define MQTT_TAG "MQTT"

#define MQTT_SCHEME_SSL "mqtts://"
#define MQTT_CA_CERT_HEADER "-----BEGIN CERTIFICATE-----"

typedef void (*mqtt_message_callback_t)(const char *topic,
                                        const char *payload,
                                        int payload_len,
                                        void *ctx);

esp_err_t mqtt_start(const device_config_t *cfg,
                     mqtt_message_callback_t message_cb,
                     void *message_ctx);
esp_err_t mqtt_stop(void);
bool mqtt_is_connected(void);
esp_err_t mqtt_subscribe(const char *topic, int qos);
esp_err_t mqtt_unsubscribe(const char *topic);
esp_err_t mqtt_publish(const char *topic,
                       const char *payload,
                       int qos,
                       int retain);

#endif
