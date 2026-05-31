/**
 * @file espnow_pair.c
 * @brief ESP-NOW pairing state machine for locally-paired receivers.
 */

#include "pair/espnow_pair.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#define TAG "pair"

#define PAIR_MSG_REQUEST    0x01
#define PAIR_MSG_CHALLENGE  0x02
#define PAIR_MSG_RESPONSE   0x03
#define PAIR_MSG_OFFER      0x04
#define PAIR_MSG_ACK        0x05
#define PAIR_MSG_CONFIRM    0x06
#define PAIR_MSG_SAVED      0x07

#define PAIR_BIT_CHALLENGE  BIT0
#define PAIR_BIT_OFFER      BIT1
#define PAIR_BIT_CONFIRM    BIT2
#define PAIR_BIT_SAVED_SENT BIT3

#define PSK_LEN             32
#define NONCE_LEN           16
#define HMAC_LEN            32
#define ESPNOW_CCM_KEY_LEN  ESP_NOW_KEY_LEN
#define WIFI_CHANNEL_MIN    1
#define WIFI_CHANNEL_MAX    13
#define OFFER_TIMEOUT_MS    15000
#define CONFIRM_TIMEOUT_MS  10000
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
    uint8_t rf_code[DEVICE_CONFIG_MAX_RF_CODE_LEN];
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
static esp_netif_t *s_pair_netif;
static uint8_t s_hub_mac[ESP_NOW_ETH_ALEN];
static uint8_t s_nonce[NONCE_LEN];
static pair_offer_t s_offer;
static volatile bool s_waiting_for_saved_send;

static bool mac_equal(const uint8_t lhs[ESP_NOW_ETH_ALEN], const uint8_t rhs[ESP_NOW_ETH_ALEN])
{
    return memcmp(lhs, rhs, ESP_NOW_ETH_ALEN) == 0;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (hex == NULL || out == NULL) {
        return false;
    }

    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) {
        return false;
    }

    for (size_t i = 0; i < out_len; i++) {
        if (!isxdigit((unsigned char)hex[i * 2]) ||
            !isxdigit((unsigned char)hex[(i * 2) + 1])) {
            return false;
        }

        unsigned int byte = 0;
        if (sscanf(&hex[i * 2], "%2x", &byte) != 1) {
            return false;
        }
        out[i] = (uint8_t)byte;
    }
    return true;
}

static esp_err_t compute_hmac(const uint8_t *nonce, size_t nonce_len,
                              const uint8_t *psk, size_t psk_len,
                              uint8_t *hmac_out)
{
    psa_status_t status = psa_crypto_init();
    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, TAG, "psa init failed");

    psa_algorithm_t alg = PSA_ALG_HMAC(PSA_ALG_SHA_256);
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;

    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, alg);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, psk_len * 8);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_VOLATILE);

    status = psa_import_key(&attributes, psk, psk_len, &key_id);
    psa_reset_key_attributes(&attributes);
    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, TAG, "psa import failed");

    size_t hmac_len = 0;
    status = psa_mac_compute(key_id, alg, nonce, nonce_len, hmac_out, HMAC_LEN, &hmac_len);
    psa_destroy_key(key_id);

    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS && hmac_len == HMAC_LEN,
                        ESP_FAIL, TAG, "psa hmac failed");
    return ESP_OK;
}

static uint8_t compiled_band(void)
{
#if CONFIG_RECEIVER_RADIO_2_4G
    return 1;
#else
    return 0;
#endif
}

static bool offer_is_valid(const pair_offer_t *offer)
{
    if (offer->slot == 0 || offer->rf_band != compiled_band()) {
        return false;
    }

    if (offer->rf_code_len == 0 || offer->rf_code_len > DEVICE_CONFIG_MAX_RF_CODE_LEN) {
        return false;
    }

    if (offer->rf_band == 0) {
        return offer->rf_code_len == 4 && offer->rf_bits > 0 && offer->rf_bits <= 32;
    }

    return offer->rf_code_len == 5 && offer->rf_bits == 40;
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || info->src_addr == NULL || data == NULL || len < 1 || s_pair_events == NULL) {
        return;
    }

    switch (data[0]) {
    case PAIR_MSG_CHALLENGE:
        if ((size_t)len >= sizeof(pair_challenge_t)) {
            const pair_challenge_t *ch = (const pair_challenge_t *)data;
            memcpy(s_nonce, ch->nonce, NONCE_LEN);
            memcpy(s_hub_mac, info->src_addr, ESP_NOW_ETH_ALEN);
            xEventGroupSetBits(s_pair_events, PAIR_BIT_CHALLENGE);
        }
        break;

    case PAIR_MSG_OFFER:
        if ((size_t)len >= sizeof(pair_offer_t) && mac_equal(s_hub_mac, info->src_addr)) {
            memcpy(&s_offer, data, sizeof(pair_offer_t));
            xEventGroupSetBits(s_pair_events, PAIR_BIT_OFFER);
        }
        break;

    case PAIR_MSG_CONFIRM:
        if ((size_t)len >= sizeof(pair_confirm_t) && mac_equal(s_hub_mac, info->src_addr)) {
            xEventGroupSetBits(s_pair_events, PAIR_BIT_CONFIRM);
        }
        break;

    default:
        break;
    }
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (info == NULL || s_pair_events == NULL || !s_waiting_for_saved_send ||
        !mac_equal(s_hub_mac, info->des_addr)) {
        return;
    }

    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "PAIR_SAVED send status failed");
    }
    xEventGroupSetBits(s_pair_events, PAIR_BIT_SAVED_SENT);
}

static esp_err_t wifi_init_sta_no_connect(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");

    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG,
                        "event loop");

    s_pair_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_pair_netif != NULL, ESP_FAIL, TAG, "create sta netif");

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
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

    if (s_pair_netif != NULL) {
        esp_netif_destroy_default_wifi(s_pair_netif);
        s_pair_netif = NULL;
    }

    esp_netif_deinit();
    esp_event_loop_delete_default();
}

static esp_err_t add_broadcast_peer(void)
{
    esp_now_peer_info_t peer = { 0 };
    memset(peer.peer_addr, 0xFF, ESP_NOW_ETH_ALEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    return esp_now_add_peer(&peer);
}

static esp_err_t add_hub_peer(const uint8_t hub_mac[ESP_NOW_ETH_ALEN], const uint8_t lmk[ESPNOW_CCM_KEY_LEN])
{
    if (esp_now_is_peer_exist(hub_mac)) {
        return ESP_OK;
    }

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, hub_mac, ESP_NOW_ETH_ALEN);
    memcpy(peer.lmk, lmk, ESPNOW_CCM_KEY_LEN);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    return esp_now_add_peer(&peer);
}

esp_err_t espnow_pair_wait(device_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");

    uint8_t psk[PSK_LEN] = { 0 };
    const char *psk_hex = CONFIG_RECEIVER_PAIR_PSK;
    if (!hex_to_bytes(psk_hex, psk, PSK_LEN)) {
        ESP_LOGE(TAG, "Invalid PSK in Kconfig (need 64 hex chars)");
        return ESP_ERR_INVALID_ARG;
    }

    s_pair_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_pair_events != NULL, ESP_ERR_NO_MEM, TAG, "pair events");

    esp_err_t ret = wifi_init_sta_no_connect();
    if (ret != ESP_OK) {
        goto cleanup;
    }

    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "espnow init failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // PSK split: bytes 0-15 → PMK (encrypts LMK exchange), bytes 16-31 → LMK (encrypts unicast data)
    ESP_GOTO_ON_ERROR(esp_now_set_pmk(psk), cleanup, TAG, "set pmk");
    ESP_GOTO_ON_ERROR(esp_now_register_recv_cb(recv_cb), cleanup, TAG, "register recv cb");
    ESP_GOTO_ON_ERROR(esp_now_register_send_cb(send_cb), cleanup, TAG, "register send cb");
    ESP_GOTO_ON_ERROR(add_broadcast_peer(), cleanup, TAG, "add broadcast peer");

    ESP_LOGI(TAG, "Entering pair mode, scanning channels...");

    uint8_t my_mac[ESP_NOW_ETH_ALEN] = { 0 };
    ESP_GOTO_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, my_mac), cleanup, TAG, "get sta mac");

    pair_request_t req = {
        .msg_type = PAIR_MSG_REQUEST,
        .rx_band = compiled_band(),
    };
    memcpy(req.rx_mac, my_mac, sizeof(req.rx_mac));

    const uint32_t scan_ms = CONFIG_RECEIVER_PAIR_CHANNEL_SCAN_MS;

    for (;;) {
        for (uint8_t ch = WIFI_CHANNEL_MIN; ch <= WIFI_CHANNEL_MAX; ch++) {
            bool hub_peer_added = false;
            xEventGroupClearBits(s_pair_events,
                                 PAIR_BIT_CHALLENGE | PAIR_BIT_OFFER | PAIR_BIT_CONFIRM);
            ret = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "channel %u set failed: %s", ch, esp_err_to_name(ret));
                continue;
            }
            esp_err_t send_err = esp_now_send(NULL, (const uint8_t *)&req, sizeof(req));
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_REQUEST send failed on ch=%u: %s",
                         ch, esp_err_to_name(send_err));
            }

            EventBits_t bits = xEventGroupWaitBits(
                s_pair_events, PAIR_BIT_CHALLENGE,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(scan_ms));

            if ((bits & PAIR_BIT_CHALLENGE) == 0) {
                continue;
            }

            ESP_LOGI(TAG, "Challenge received on ch=%u from %02x:%02x:%02x:%02x:%02x:%02x",
                     ch, s_hub_mac[0], s_hub_mac[1], s_hub_mac[2],
                     s_hub_mac[3], s_hub_mac[4], s_hub_mac[5]);

            ESP_GOTO_ON_ERROR(add_hub_peer(s_hub_mac, &psk[ESPNOW_CCM_KEY_LEN]),
                              cleanup, TAG, "add hub peer");
            hub_peer_added = true;

            xEventGroupClearBits(s_pair_events, PAIR_BIT_OFFER | PAIR_BIT_CONFIRM);

            uint8_t hmac[HMAC_LEN] = { 0 };
            if (compute_hmac(s_nonce, NONCE_LEN, psk, PSK_LEN, hmac) != ESP_OK) {
                ESP_LOGE(TAG, "HMAC computation failed");
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            pair_response_t resp = { .msg_type = PAIR_MSG_RESPONSE };
            memcpy(resp.hmac, hmac, HMAC_LEN);
            send_err = esp_now_send(s_hub_mac, (const uint8_t *)&resp, sizeof(resp));
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_RESPONSE send failed: %s", esp_err_to_name(send_err));
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            bits = xEventGroupWaitBits(
                s_pair_events, PAIR_BIT_OFFER,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(OFFER_TIMEOUT_MS));

            if ((bits & PAIR_BIT_OFFER) == 0) {
                ESP_LOGW(TAG, "No PAIR_OFFER within %u ms, resuming scan", OFFER_TIMEOUT_MS);
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            if (!offer_is_valid(&s_offer)) {
                ESP_LOGW(TAG, "Invalid offer: slot=%u band=%u len=%u bits=%u",
                         s_offer.slot, s_offer.rf_band, s_offer.rf_code_len, s_offer.rf_bits);
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            ESP_LOGI(TAG, "Offer received: slot=%u band=%u bits=%u",
                     s_offer.slot, s_offer.rf_band, s_offer.rf_bits);

            pair_ack_t ack = { .msg_type = PAIR_MSG_ACK, .slot = s_offer.slot };
            memcpy(ack.nonce_tag, s_nonce, sizeof(ack.nonce_tag));
            xEventGroupClearBits(s_pair_events, PAIR_BIT_CONFIRM);
            send_err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_ACK send failed: %s", esp_err_to_name(send_err));
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            bits = xEventGroupWaitBits(
                s_pair_events, PAIR_BIT_CONFIRM,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(CONFIRM_TIMEOUT_MS));

            if ((bits & PAIR_BIT_CONFIRM) == 0) {
                ESP_LOGW(TAG, "No PAIR_CONFIRM within %u ms, resuming scan without saving",
                         CONFIRM_TIMEOUT_MS);
                if (hub_peer_added) {
                    (void)esp_now_del_peer(s_hub_mac);
                }
                continue;
            }

            esp_err_t save_err = ESP_FAIL;
            for (int attempt = 0; attempt < 3; attempt++) {
                save_err = device_config_save_pairing(
                    cfg, s_offer.slot, s_hub_mac,
                    s_offer.rf_code, s_offer.rf_code_len,
                    s_offer.rf_bits, s_offer.rf_band, 1);
                if (save_err == ESP_OK) {
                    break;
                }
                ESP_LOGW(TAG, "NVS save attempt %d failed: %s",
                         attempt + 1, esp_err_to_name(save_err));
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if (save_err != ESP_OK) {
                ESP_LOGE(TAG, "All NVS save attempts failed");
                ret = save_err;
                goto cleanup;
            }

            ESP_LOGI(TAG, "Pairing complete, slot=%u", s_offer.slot);
            pair_saved_t saved = { .msg_type = PAIR_MSG_SAVED, .slot = s_offer.slot };
            memcpy(saved.nonce_tag, s_nonce, sizeof(saved.nonce_tag));
            xEventGroupClearBits(s_pair_events, PAIR_BIT_SAVED_SENT);
            s_waiting_for_saved_send = true;
            esp_err_t saved_err = esp_now_send(s_hub_mac, (const uint8_t *)&saved, sizeof(saved));
            if (saved_err != ESP_OK) {
                ESP_LOGW(TAG, "PAIR_SAVED send failed: %s", esp_err_to_name(saved_err));
                s_waiting_for_saved_send = false;
            } else {
                bits = xEventGroupWaitBits(s_pair_events, PAIR_BIT_SAVED_SENT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(SAVED_SEND_TIMEOUT_MS));
                s_waiting_for_saved_send = false;
                if ((bits & PAIR_BIT_SAVED_SENT) == 0) {
                    ESP_LOGW(TAG, "PAIR_SAVED send callback timeout");
                }
            }

            ret = ESP_OK;
            goto cleanup;
        }
    }

cleanup:
    memset(psk, 0, sizeof(psk));
    if (s_pair_events != NULL) {
        vEventGroupDelete(s_pair_events);
        s_pair_events = NULL;
    }
    wifi_deinit_all();
    return ret;
}
