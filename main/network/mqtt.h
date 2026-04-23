/**
 * @file mqtt.h
 * @brief MQTT Module - Secure Messaging Interface
 *
 * Declares the TLS-backed MQTT transport APIs plus the receiver-specific
 * bootstrap and command helpers built on top of that transport.
 */

#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>

#include "esp_err.h"

#include "config/device_config.h"
#include "security/device_identity.h"

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
esp_err_t mqtt_receiver_init(device_identity_t *identity, const char *firmware_version);
esp_err_t mqtt_receiver_start(const device_config_t *cfg);
esp_err_t mqtt_receiver_restart_with_public_id(const device_config_t *cfg);
esp_err_t mqtt_receiver_subscribe_commands(const char *public_id);
esp_err_t mqtt_receiver_bootstrap_activate(const device_config_t *cfg);

#endif
