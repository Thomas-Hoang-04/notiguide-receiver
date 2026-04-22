/**
 * @file mqtt.c
 * @brief MQTT Module - Secure Messaging Implementation
 *
 * Implements the MQTTS client lifecycle, message reassembly, topic
 * subscriptions, and publish helpers for receiver command handling.
 */

#include "network/mqtt.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

static esp_mqtt_client_handle_t s_client;
static mqtt_message_callback_t s_message_callback;
static void *s_message_ctx;
static EventGroupHandle_t s_mqtt_events;
static char *s_rx_topic;
static char *s_rx_payload;
static int s_rx_total_len;

extern const uint8_t mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");
extern const uint8_t mqtt_ca_pem_end[] asm("_binary_mqtt_ca_pem_end");

#define MQTT_CONNECTED_BIT BIT0

static bool broker_uses_scheme(const char *uri, const char *scheme)
{
    return strncmp(uri, scheme, strlen(scheme)) == 0;
}

static const char *mqtt_ca_pem_body(void)
{
    const char *pem = (const char *)mqtt_ca_pem_start;

    while (*pem == ' ' || *pem == '\t' || *pem == '\n' || *pem == '\r') {
        pem++;
    }

    return pem;
}

static bool mqtt_ca_pem_configured(void)
{
    const char *pem = mqtt_ca_pem_body();
    return strncmp(pem, MQTT_CA_CERT_HEADER, strlen(MQTT_CA_CERT_HEADER)) == 0;
}

static size_t mqtt_ca_pem_len(void)
{
    return (size_t)((const char *)mqtt_ca_pem_end - mqtt_ca_pem_body());
}

static void mqtt_reset_rx_buffer(void)
{
    free(s_rx_topic);
    free(s_rx_payload);
    s_rx_topic = NULL;
    s_rx_payload = NULL;
    s_rx_total_len = 0;
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)args;
    (void)base;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(MQTT_TAG, "MQTT connected");
        if (s_mqtt_events) {
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(MQTT_TAG, "MQTT disconnected");
        if (s_mqtt_events) {
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
        }
        mqtt_reset_rx_buffer();
        break;
    case MQTT_EVENT_DATA: {
        int final_offset;

        if (event->current_data_offset == 0) {
            mqtt_reset_rx_buffer();
            s_rx_topic = calloc(1, event->topic_len + 1);
            s_rx_payload = calloc(1, event->total_data_len + 1);
            if (!s_rx_topic || !s_rx_payload) {
                mqtt_reset_rx_buffer();
                return;
            }
            memcpy(s_rx_topic, event->topic, event->topic_len);
            s_rx_total_len = event->total_data_len;
        }

        if (!s_rx_payload || event->current_data_offset + event->data_len > s_rx_total_len) {
            mqtt_reset_rx_buffer();
            return;
        }

        memcpy(s_rx_payload + event->current_data_offset, event->data, event->data_len);
        final_offset = event->current_data_offset + event->data_len;
        if (final_offset == s_rx_total_len && s_message_callback) {
            s_message_callback(s_rx_topic, s_rx_payload, s_rx_total_len, s_message_ctx);
            mqtt_reset_rx_buffer();
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            ESP_LOGE(MQTT_TAG, "MQTT error type=%d tls=0x%x stack=%d conn=%d",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_tls_stack_err,
                     event->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }

}

esp_err_t mqtt_start(const device_config_t *cfg,
                     mqtt_message_callback_t message_cb,
                     void *message_ctx)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    if (!cfg || !cfg->mqtt_uri || !cfg->mqtt_user || !cfg->mqtt_pwd) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!broker_uses_scheme(cfg->mqtt_uri, MQTT_SCHEME_SSL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!mqtt_ca_pem_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_message_callback = message_cb;
    s_message_ctx = message_ctx;

    if (!s_mqtt_events) {
        s_mqtt_events = xEventGroupCreate();
        if (!s_mqtt_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);

    mqtt_cfg.uri = cfg->mqtt_uri;
    if (cfg->has_public_id) {
        mqtt_cfg.client_id = cfg->public_id;
    }
    mqtt_cfg.username = cfg->mqtt_user;
    mqtt_cfg.password = cfg->mqtt_pwd;
    mqtt_cfg.transport = MQTT_TRANSPORT_OVER_SSL;
    mqtt_cfg.cert_pem = mqtt_ca_pem_body();
    mqtt_cfg.cert_len = mqtt_ca_pem_len();
    mqtt_cfg.keepalive = 120;
    mqtt_cfg.buffer_size = 2048;
    mqtt_cfg.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
    mqtt_cfg.reconnect_timeout_ms = 5000;

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        return ESP_FAIL;
    }

    if (esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    xEventGroupWaitBits(s_mqtt_events, MQTT_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    return ESP_OK;
}

esp_err_t mqtt_stop(void)
{
    if (!s_client) {
        return ESP_OK;
    }

    mqtt_reset_rx_buffer();
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return s_mqtt_events && (xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT);
}

esp_err_t mqtt_subscribe(const char *topic, int qos)
{
    if (!s_client || !topic) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_subscribe(s_client, topic, qos) >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_unsubscribe(const char *topic)
{
    if (!s_client || !topic) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_unsubscribe(s_client, topic) >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_publish(const char *topic,
                       const char *payload,
                       int qos,
                       int retain)
{
    if (!s_client || !topic || !payload) {
        return ESP_ERR_INVALID_STATE;
    }

    return (esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain) >= 0) ? ESP_OK : ESP_FAIL;
}
