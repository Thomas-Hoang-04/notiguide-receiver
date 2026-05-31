/**
 * @file espnow_pair.c
 * @brief ESP-NOW pairing state machine for ESP8266 receivers.
 */

#include "pair/espnow_pair.h"

#include <string.h>

#include "config/device_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"
#include "tcpip_adapter.h"

#ifndef ESP_RETURN_ON_ERROR
#define ESP_RETURN_ON_ERROR(x, tag, msg) do {                  \
        esp_err_t __err_rc = (x);                              \
        if (__err_rc != ESP_OK) {                              \
            ESP_LOGE((tag), "%s failed: %s",                  \
                     (msg), esp_err_to_name(__err_rc));        \
            return __err_rc;                                   \
        }                                                      \
    } while (0)
#endif

#define TAG "pair"

#define PAIR_MSG_REQUEST    0x01
#define PAIR_MSG_CHALLENGE  0x02
#define PAIR_MSG_RESPONSE   0x03
#define PAIR_MSG_OFFER      0x04
#define PAIR_MSG_ACK        0x05
#define PAIR_MSG_CONFIRM    0x06
#define PAIR_MSG_SAVED      0x07

#define PAIR_BIT_CHALLENGE BIT0
#define PAIR_BIT_OFFER     BIT1
#define PAIR_BIT_CONFIRM   BIT2
#define PAIR_BIT_SAVED_SENT BIT3

#define PSK_LEN    32
#define PMK_LEN    16
#define LMK_LEN    16
#define NONCE_LEN  16
#define HMAC_LEN   32
#define RF_CODE_MAX_LEN 4
#define SAVED_SEND_TIMEOUT_MS 1000

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t rx_mac[6];
    uint8_t rx_band;
} pair_request_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t nonce[NONCE_LEN];
} pair_challenge_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t hmac[HMAC_LEN];
} pair_response_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t slot;
    uint8_t rf_band;
    uint8_t rf_code[16];
    uint8_t rf_code_len;
    uint8_t rf_bits;
} pair_offer_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t slot;
    uint8_t nonce_tag[4];
} pair_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
} pair_confirm_t;

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t slot;
    uint8_t nonce_tag[4];
} pair_saved_t;

static EventGroupHandle_t s_pair_events;
static uint8_t s_hub_mac[6];
static uint8_t s_nonce[NONCE_LEN];
static pair_offer_t s_offer;
static volatile bool s_waiting_for_saved_send;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static esp_err_t hex_to_bytes(uint8_t out[PSK_LEN])
{
    const char *psk_hex = CONFIG_RECEIVER_PAIR_PSK;

    if (strlen(psk_hex) != PSK_LEN * 2) {
        ESP_LOGE(TAG, "Pairing PSK must be exactly 64 hex characters");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < PSK_LEN; i++) {
        int high = hex_value(psk_hex[i * 2]);
        int low = hex_value(psk_hex[i * 2 + 1]);

        if (high < 0 || low < 0) {
            ESP_LOGE(TAG, "Pairing PSK contains non-hex characters");
            return ESP_ERR_INVALID_ARG;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }

    return ESP_OK;
}

static esp_err_t compute_hmac(const uint8_t psk[PSK_LEN],
                              const uint8_t nonce[NONCE_LEN],
                              uint8_t output[HMAC_LEN])
{
    const mbedtls_md_info_t *md_info =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (!md_info) {
        return ESP_FAIL;
    }

    if (mbedtls_md_hmac(md_info, psk, PSK_LEN, nonce, NONCE_LEN, output) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void recv_cb(const uint8_t *mac_addr, const uint8_t *data, int data_len)
{
    if (!s_pair_events || !mac_addr || !data || data_len < 1) {
        return;
    }

    switch (data[0]) {
    case PAIR_MSG_CHALLENGE:
        if (data_len >= (int)sizeof(pair_challenge_t)) {
            const pair_challenge_t *challenge = (const pair_challenge_t *)data;

            memcpy(s_hub_mac, mac_addr, sizeof(s_hub_mac));
            memcpy(s_nonce, challenge->nonce, sizeof(s_nonce));
            xEventGroupSetBits(s_pair_events, PAIR_BIT_CHALLENGE);
        }
        break;

    case PAIR_MSG_OFFER:
        if (data_len >= (int)sizeof(pair_offer_t) &&
            memcmp(mac_addr, s_hub_mac, sizeof(s_hub_mac)) == 0) {
            memcpy(&s_offer, data, sizeof(s_offer));
            xEventGroupSetBits(s_pair_events, PAIR_BIT_OFFER);
        }
        break;

    case PAIR_MSG_CONFIRM:
        if (data_len >= (int)sizeof(pair_confirm_t) &&
            memcmp(mac_addr, s_hub_mac, sizeof(s_hub_mac)) == 0) {
            xEventGroupSetBits(s_pair_events, PAIR_BIT_CONFIRM);
        }
        break;

    default:
        break;
    }
}

static void send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (!s_pair_events || !s_waiting_for_saved_send || !mac_addr ||
        memcmp(mac_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) {
        return;
    }

    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "PAIR_SAVED send status failed");
    }
    xEventGroupSetBits(s_pair_events, PAIR_BIT_SAVED_SENT);
}

static esp_err_t wifi_init_sta_no_connect(void)
{
    esp_err_t err;
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();

    tcpip_adapter_init();

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    return ESP_OK;
}

static void wifi_deinit_all(void)
{
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
}

static esp_err_t add_broadcast_peer(void)
{
    static const uint8_t broadcast_mac[6] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    esp_now_peer_info_t peer;

    if (esp_now_is_peer_exist(broadcast_mac)) {
        return ESP_OK;
    }

    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, broadcast_mac, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    return esp_now_add_peer(&peer);
}

static esp_err_t add_hub_peer(const uint8_t lmk[LMK_LEN])
{
    esp_now_peer_info_t peer;

    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    memcpy(peer.lmk, lmk, LMK_LEN);

    if (esp_now_is_peer_exist(s_hub_mac)) {
        return esp_now_mod_peer(&peer);
    }

    return esp_now_add_peer(&peer);
}

static uint32_t bytes_to_u32(const uint8_t *bytes, uint8_t len)
{
    uint32_t value = 0;
    uint8_t count = len < RF_CODE_MAX_LEN ? len : RF_CODE_MAX_LEN;

    for (uint8_t i = 0; i < count; i++) {
        value = (value << 8) | bytes[i];
    }

    return value;
}

static esp_err_t validate_offer(const pair_offer_t *offer)
{
    if (offer->slot == 0) {
        ESP_LOGE(TAG, "Pairing offer has invalid slot 0");
        return ESP_ERR_INVALID_ARG;
    }
    if (offer->rf_band != 0) {
        ESP_LOGE(TAG, "Pairing offer has unsupported band=%u "
                 "(ESP8266 is 433M only)", offer->rf_band);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (offer->rf_bits != 32) {
        ESP_LOGE(TAG, "Pairing offer has unsupported rf_bits=%u", offer->rf_bits);
        return ESP_ERR_INVALID_ARG;
    }
    if (offer->rf_code_len == 0 || offer->rf_code_len > RF_CODE_MAX_LEN) {
        ESP_LOGE(TAG, "Pairing offer has invalid rf_code_len=%u",
                 offer->rf_code_len);
        return ESP_ERR_INVALID_ARG;
    }
    if (offer->rf_code_len != RF_CODE_MAX_LEN) {
        ESP_LOGW(TAG, "Pairing offer rf_code_len=%u for 32-bit code",
                 offer->rf_code_len);
    }

    return ESP_OK;
}

esp_err_t espnow_pair_wait(device_config_t *cfg)
{
    esp_err_t err = ESP_OK;
    uint8_t psk[PSK_LEN];
    uint8_t my_mac[6];
    pair_request_t request;

    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    err = hex_to_bytes(psk);
    if (err != ESP_OK) {
        return err;
    }

    s_pair_events = xEventGroupCreate();
    if (!s_pair_events) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(s_pair_events, PAIR_BIT_CHALLENGE | PAIR_BIT_OFFER | PAIR_BIT_CONFIRM);
    memset(s_hub_mac, 0, sizeof(s_hub_mac));
    memset(s_nonce, 0, sizeof(s_nonce));
    memset(&s_offer, 0, sizeof(s_offer));

    err = wifi_init_sta_no_connect();
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp-now init failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_now_set_pmk(psk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp-now set PMK failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_now_register_recv_cb(recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp-now recv callback failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_now_register_send_cb(send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp-now send callback failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = add_broadcast_peer();
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "broadcast peer add failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_get_mac(WIFI_IF_STA, my_mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station mac read failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    memset(&request, 0, sizeof(request));
    request.msg_type = PAIR_MSG_REQUEST;
    memcpy(request.rx_mac, my_mac, sizeof(request.rx_mac));
    request.rx_band = 0;

    ESP_LOGI(TAG, "Entering pair mode, scanning channels...");
    for (;;) {
        for (uint8_t channel = 1; channel <= 13; channel++) {
            EventBits_t bits;
            bool hub_peer_added = false;

            xEventGroupClearBits(s_pair_events,
                                 PAIR_BIT_CHALLENGE | PAIR_BIT_OFFER | PAIR_BIT_CONFIRM);

            err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "channel %u set failed: %s",
                         channel, esp_err_to_name(err));
                continue;
            }

            err = esp_now_send(NULL, (const uint8_t *)&request, sizeof(request));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_REQUEST send failed on ch=%u: %s",
                         channel, esp_err_to_name(err));
            }

            bits = xEventGroupWaitBits(s_pair_events, PAIR_BIT_CHALLENGE, pdTRUE,
                                       pdFALSE,
                                       pdMS_TO_TICKS(CONFIG_RECEIVER_PAIR_CHANNEL_SCAN_MS));
            if (!(bits & PAIR_BIT_CHALLENGE)) {
                continue;
            }

            ESP_LOGI(TAG, "Challenge received on ch=%u from %02x:%02x:%02x:%02x:%02x:%02x",
                     channel, s_hub_mac[0], s_hub_mac[1], s_hub_mac[2],
                     s_hub_mac[3], s_hub_mac[4], s_hub_mac[5]);

            err = add_hub_peer(psk + PMK_LEN);
            if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGW(TAG, "hub peer add failed: %s, resuming scan",
                         esp_err_to_name(err));
                continue;
            }
            hub_peer_added = true;

            xEventGroupClearBits(s_pair_events, PAIR_BIT_OFFER | PAIR_BIT_CONFIRM);

            pair_response_t response = {
                .msg_type = PAIR_MSG_RESPONSE,
            };

            err = compute_hmac(psk, s_nonce, response.hmac);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "HMAC computation failed, resuming scan");
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            err = esp_now_send(s_hub_mac, (const uint8_t *)&response,
                               sizeof(response));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_RESPONSE send failed: %s, resuming scan",
                         esp_err_to_name(err));
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            bits = xEventGroupWaitBits(s_pair_events, PAIR_BIT_OFFER, pdTRUE,
                                       pdFALSE, pdMS_TO_TICKS(15000));
            if (!(bits & PAIR_BIT_OFFER)) {
                ESP_LOGW(TAG, "No PAIR_OFFER within 15s, resuming scan");
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            err = validate_offer(&s_offer);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Invalid offer, resuming scan");
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            ESP_LOGI(TAG, "Offer received: slot=%u bits=%u", s_offer.slot, s_offer.rf_bits);

            pair_ack_t ack = {
                .msg_type = PAIR_MSG_ACK,
                .slot = s_offer.slot,
            };
            memcpy(ack.nonce_tag, s_nonce, sizeof(ack.nonce_tag));

            xEventGroupClearBits(s_pair_events, PAIR_BIT_CONFIRM);
            err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_ACK send failed: %s, resuming scan",
                         esp_err_to_name(err));
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            bits = xEventGroupWaitBits(s_pair_events, PAIR_BIT_CONFIRM, pdTRUE,
                                       pdFALSE, pdMS_TO_TICKS(10000));
            if (!(bits & PAIR_BIT_CONFIRM)) {
                ESP_LOGW(TAG, "No PAIR_CONFIRM within 10s, resuming scan without saving");
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            uint32_t rf_code = bytes_to_u32(s_offer.rf_code,
                                            s_offer.rf_code_len);

            err = device_config_save_pairing(cfg, s_offer.slot, s_hub_mac,
                                             rf_code, s_offer.rf_bits);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "save pairing failed: %s", esp_err_to_name(err));
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            ESP_LOGI(TAG, "Pairing complete, slot=%u", s_offer.slot);
            pair_saved_t saved = {
                .msg_type = PAIR_MSG_SAVED,
                .slot = s_offer.slot,
            };
            memcpy(saved.nonce_tag, s_nonce, sizeof(saved.nonce_tag));
            xEventGroupClearBits(s_pair_events, PAIR_BIT_SAVED_SENT);
            s_waiting_for_saved_send = true;
            esp_err_t saved_err = esp_now_send(s_hub_mac,
                                               (const uint8_t *)&saved,
                                               sizeof(saved));
            if (saved_err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_SAVED send failed: %s",
                         esp_err_to_name(saved_err));
                s_waiting_for_saved_send = false;
            } else {
                bits = xEventGroupWaitBits(s_pair_events, PAIR_BIT_SAVED_SENT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(SAVED_SEND_TIMEOUT_MS));
                s_waiting_for_saved_send = false;
                if (!(bits & PAIR_BIT_SAVED_SENT)) {
                    ESP_LOGW(TAG, "PAIR_SAVED send callback timeout");
                }
            }

            err = ESP_OK;
            goto cleanup;
        }
    }

cleanup:
    memset(psk, 0, sizeof(psk));
    wifi_deinit_all();
    vEventGroupDelete(s_pair_events);
    s_pair_events = NULL;
    return err;
}
