/**
 * @file device_identity.c
 * @brief Device Identity Module - Cryptographic Implementation
 *
 * Implements device EC key provisioning, public key export, activation
 * challenge signing, and backend command signature verification.
 */

#include "security/device_identity.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "mbedtls/ecp.h"
#include "mbedtls/error.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "sdkconfig.h"

#define IDENTITY_NAMESPACE "identity"
#define KEY_PUBKEY "pubkey"
#define KEY_PRIVKEY "privkey"

static esp_err_t base64_encode_alloc(const uint8_t *input, size_t input_len, char **out_b64)
{
    size_t output_len = 0;
    int ret;

    *out_b64 = NULL;
    ret = mbedtls_base64_encode(NULL, 0, &output_len, input, input_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    *out_b64 = calloc(1, output_len + 1);
    if (!*out_b64) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_encode((unsigned char *)*out_b64, output_len + 1, &output_len, input, input_len);
    if (ret != 0) {
        free(*out_b64);
        *out_b64 = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t base64_decode_alloc(const char *input, uint8_t **output, size_t *output_len)
{
    int ret;

    *output = NULL;
    *output_len = 0;
    ret = mbedtls_base64_decode(NULL, 0, output_len, (const unsigned char *)input, strlen(input));
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    *output = calloc(1, *output_len + 1);
    if (!*output) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(*output, *output_len, output_len, (const unsigned char *)input, strlen(input));
    if (ret != 0) {
        free(*output);
        *output = NULL;
        *output_len = 0;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t read_blob(nvs_handle_t handle, const char *key, uint8_t **out_blob, size_t *out_len)
{
    esp_err_t err;

    *out_blob = NULL;
    *out_len = 0;

    err = nvs_get_blob(handle, key, NULL, out_len);
    if (err != ESP_OK) {
        return err;
    }

    *out_blob = calloc(1, *out_len);
    if (!*out_blob) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, key, *out_blob, out_len);
    if (err != ESP_OK) {
        free(*out_blob);
        *out_blob = NULL;
        *out_len = 0;
    }

    return err;
}

static esp_err_t device_identity_load_or_generate(device_identity_t *identity)
{
    esp_err_t err;
    nvs_handle_t handle;
    uint8_t *priv_der = NULL;
    uint8_t *pub_der = NULL;
    size_t priv_len = 0;
    size_t pub_len = 0;

    err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = read_blob(handle, KEY_PRIVKEY, &priv_der, &priv_len);
    if (err == ESP_OK) {
        err = read_blob(handle, KEY_PUBKEY, &pub_der, &pub_len);
    }

    if (err == ESP_OK) {
        if (mbedtls_pk_parse_key(&identity->device_key, priv_der, priv_len, NULL, 0) != 0) {
            err = ESP_FAIL;
        } else {
            identity->device_key_ready = true;
        }
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t priv_buf[256];
        uint8_t pub_buf[160];
        int priv_written;
        int pub_written;

        if (mbedtls_pk_setup(&identity->device_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }
        if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                mbedtls_pk_ec(identity->device_key),
                                mbedtls_ctr_drbg_random,
                                &identity->drbg) != 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        priv_written = mbedtls_pk_write_key_der(&identity->device_key, priv_buf, sizeof(priv_buf));
        pub_written = mbedtls_pk_write_pubkey_der(&identity->device_key, pub_buf, sizeof(pub_buf));
        if (priv_written <= 0 || pub_written <= 0) {
            err = ESP_FAIL;
            goto cleanup;
        }

        err = nvs_set_blob(handle, KEY_PRIVKEY, priv_buf + sizeof(priv_buf) - priv_written, priv_written);
        if (err == ESP_OK) {
            err = nvs_set_blob(handle, KEY_PUBKEY, pub_buf + sizeof(pub_buf) - pub_written, pub_written);
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        if (err == ESP_OK) {
            identity->device_key_ready = true;
        }
    }

cleanup:
    free(priv_der);
    free(pub_der);
    nvs_close(handle);
    return err;
}

static esp_err_t load_backend_pubkey(device_identity_t *identity)
{
    uint8_t *der = NULL;
    size_t der_len = 0;
    esp_err_t err;

    if (!CONFIG_RECEIVER_BACKEND_PUBKEY_B64[0]) {
        identity->backend_key_ready = false;
        return ESP_OK;
    }

    err = base64_decode_alloc(CONFIG_RECEIVER_BACKEND_PUBKEY_B64, &der, &der_len);
    if (err != ESP_OK) {
        return err;
    }

    if (mbedtls_pk_parse_public_key(&identity->backend_key, der, der_len) != 0) {
        free(der);
        return ESP_FAIL;
    }

    free(der);
    identity->backend_key_ready = true;
    return ESP_OK;
}

esp_err_t device_identity_init(device_identity_t *identity)
{
    const char *pers = "receiver-esp01";

    if (!identity) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(identity, 0, sizeof(*identity));
    mbedtls_pk_init(&identity->device_key);
    mbedtls_pk_init(&identity->backend_key);
    mbedtls_entropy_init(&identity->entropy);
    mbedtls_ctr_drbg_init(&identity->drbg);

    if (mbedtls_ctr_drbg_seed(&identity->drbg,
                              mbedtls_entropy_func,
                              &identity->entropy,
                              (const unsigned char *)pers,
                              strlen(pers)) != 0) {
        return ESP_FAIL;
    }

    if (device_identity_load_or_generate(identity) != ESP_OK) {
        return ESP_FAIL;
    }

    return load_backend_pubkey(identity);
}

void device_identity_deinit(device_identity_t *identity)
{
    if (!identity) {
        return;
    }

    mbedtls_pk_free(&identity->device_key);
    mbedtls_pk_free(&identity->backend_key);
    mbedtls_ctr_drbg_free(&identity->drbg);
    mbedtls_entropy_free(&identity->entropy);
    memset(identity, 0, sizeof(*identity));
}

bool device_identity_backend_ready(const device_identity_t *identity)
{
    return identity && identity->backend_key_ready;
}

esp_err_t device_identity_get_public_key_b64(device_identity_t *identity, char **out_b64)
{
    uint8_t der[160];
    int written;

    if (!identity || !out_b64 || !identity->device_key_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    written = mbedtls_pk_write_pubkey_der(&identity->device_key, der, sizeof(der));
    if (written <= 0) {
        return ESP_FAIL;
    }

    return base64_encode_alloc(der + sizeof(der) - written, written, out_b64);
}

esp_err_t device_identity_make_registration_nonce(char **out_nonce)
{
    uint8_t raw[10];
    char *nonce = NULL;
    size_t i;
    esp_err_t err;

    if (!out_nonce) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_fill_random(raw, sizeof(raw));
    err = base64_encode_alloc(raw, sizeof(raw), &nonce);
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; nonce[i]; i++) {
        if (nonce[i] == '+') {
            nonce[i] = '-';
        } else if (nonce[i] == '/') {
            nonce[i] = '_';
        }
    }
    while (i > 0 && nonce[i - 1] == '=') {
        nonce[--i] = '\0';
    }

    *out_nonce = nonce;
    return ESP_OK;
}

esp_err_t device_identity_sign_activation_response(device_identity_t *identity,
                                                   const char *challenge_id,
                                                   const char *nonce,
                                                   const char *issued_at,
                                                   const char *expires_at,
                                                   char **out_signature_b64)
{
    char canonical[512];
    uint8_t hash[32];
    uint8_t signature[128];
    size_t sig_len = 0;
    int len;

    if (!identity || !challenge_id || !nonce || !issued_at || !expires_at || !out_signature_b64) {
        return ESP_ERR_INVALID_ARG;
    }

    len = snprintf(canonical,
                   sizeof(canonical),
                   "activate-v1|%s|%s|%s|%s",
                   challenge_id,
                   nonce,
                   issued_at,
                   expires_at);
    if (len <= 0 || len >= (int)sizeof(canonical)) {
        return ESP_ERR_INVALID_SIZE;
    }

    mbedtls_sha256((const unsigned char *)canonical, len, hash, 0);
    if (mbedtls_pk_sign(&identity->device_key,
                        MBEDTLS_MD_SHA256,
                        hash,
                        sizeof(hash),
                        signature,
                        &sig_len,
                        mbedtls_ctr_drbg_random,
                        &identity->drbg) != 0) {
        return ESP_FAIL;
    }

    return base64_encode_alloc(signature, sig_len, out_signature_b64);
}

esp_err_t device_identity_verify_signature(device_identity_t *identity,
                                           const char *canonical,
                                           const char *signature_b64,
                                           bool *verified)
{
    uint8_t *signature = NULL;
    size_t sig_len = 0;
    uint8_t hash[32];
    esp_err_t err;

    if (!identity || !canonical || !signature_b64 || !verified) {
        return ESP_ERR_INVALID_ARG;
    }

    *verified = false;
    if (!identity->backend_key_ready) {
        return ESP_OK;
    }

    err = base64_decode_alloc(signature_b64, &signature, &sig_len);
    if (err != ESP_OK) {
        return err;
    }

    mbedtls_sha256((const unsigned char *)canonical, strlen(canonical), hash, 0);
    *verified = mbedtls_pk_verify(&identity->backend_key,
                                  MBEDTLS_MD_SHA256,
                                  hash,
                                  sizeof(hash),
                                  signature,
                                  sig_len) == 0;

    free(signature);
    return ESP_OK;
}
