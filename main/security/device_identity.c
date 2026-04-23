/**
 * @file device_identity.c
 * @brief Device identity, signing, and signature verification helpers.
 */

#include "security/device_identity.h"
#include <stdint.h>
#include <string.h>
#include "config/device_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/psa_util.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "psa/crypto_sizes.h"
#include "sdkconfig.h"

#define DEVICE_IDENTITY_TAG "DEVICE_ID"
#define DEVICE_KEY_DER_BUF_LEN 256U
#define DEVICE_KEY_BITS 256U
#define DEVICE_SIGN_ALG PSA_ALG_ECDSA(PSA_ALG_SHA_256)
#define DEVICE_KEYPAIR_TYPE PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
#define DEVICE_PUBLIC_KEY_TYPE PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1)
#define DEVICE_RAW_SIGNATURE_BUF_LEN (2U * PSA_BITS_TO_BYTES(DEVICE_KEY_BITS))
#define DEVICE_DER_SIGNATURE_BUF_LEN MBEDTLS_ECDSA_DER_MAX_SIG_LEN(DEVICE_KEY_BITS)

static bool key_id_is_empty(mbedtls_svc_key_id_t key_id)
{
    return MBEDTLS_SVC_KEY_ID_GET_KEY_ID(key_id) == 0;
}

static void set_empty_key_id(mbedtls_svc_key_id_t *key_id)
{
    if (key_id != NULL) {
        *key_id = MBEDTLS_SVC_KEY_ID_INIT;
    }
}

static void destroy_key_if_present(mbedtls_svc_key_id_t *key_id, const char *label)
{
    if (key_id == NULL || key_id_is_empty(*key_id)) {
        return;
    }

    psa_status_t status = psa_destroy_key(*key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGW(DEVICE_IDENTITY_TAG, "failed to destroy %s PSA key: %d", label, (int)status);
    }
    set_empty_key_id(key_id);
}

static void clear_identity(device_identity_t *identity)
{
    if (identity == NULL) {
        return;
    }

    destroy_key_if_present(&identity->backend_key_id, "backend");
    destroy_key_if_present(&identity->device_key_id, "device");
    memset(identity, 0, sizeof(*identity));
    set_empty_key_id(&identity->device_key_id);
    set_empty_key_id(&identity->backend_key_id);
}

static esp_err_t device_identity_sha256(const char *message, uint8_t hash[32])
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ESP_RETURN_ON_FALSE(md_info != NULL, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "SHA-256 is unavailable");
    ESP_RETURN_ON_FALSE(message != NULL, ESP_ERR_INVALID_ARG, DEVICE_IDENTITY_TAG,
                        "message is NULL");
    ESP_RETURN_ON_FALSE(mbedtls_md(md_info, (const unsigned char *)message,
                                   strlen(message), hash) == 0,
                        ESP_FAIL, DEVICE_IDENTITY_TAG, "mbedtls_md failed");
    return ESP_OK;
}

static esp_err_t b64_encode(const uint8_t *input, size_t input_len, char *out, size_t out_len)
{
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_len, &written, input, input_len);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, DEVICE_IDENTITY_TAG, "base64 encode failed: %d", ret);
    ESP_RETURN_ON_FALSE(written < out_len, ESP_ERR_INVALID_SIZE, DEVICE_IDENTITY_TAG,
                        "base64 output buffer too small for terminator");
    out[written] = '\0';
    return ESP_OK;
}

static esp_err_t b64_decode(const char *input, uint8_t *out, size_t out_len, size_t *actual_len)
{
    size_t written = 0;
    int ret = mbedtls_base64_decode(out, out_len, &written,
                                    (const unsigned char *)input, strlen(input));
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, DEVICE_IDENTITY_TAG, "base64 decode failed: %d", ret);
    if (actual_len != NULL) {
        *actual_len = written;
    }
    return ESP_OK;
}

static esp_err_t export_public_key_der(mbedtls_pk_context *pk,
                                       uint8_t *out,
                                       size_t out_len,
                                       size_t *actual_len)
{
    int der_len = mbedtls_pk_write_pubkey_der(pk, out, out_len);
    ESP_RETURN_ON_FALSE(der_len > 0, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "failed to export public key: %d", der_len);

    size_t written = (size_t)der_len;
    memmove(out, &out[out_len - written], written);
    if (actual_len != NULL) {
        *actual_len = written;
    }
    return ESP_OK;
}

static esp_err_t export_private_key_der(mbedtls_pk_context *pk,
                                        uint8_t *out,
                                        size_t out_len,
                                        size_t *actual_len)
{
    int der_len = mbedtls_pk_write_key_der(pk, out, out_len);
    ESP_RETURN_ON_FALSE(der_len > 0, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "failed to export private key: %d", der_len);

    size_t written = (size_t)der_len;
    memmove(out, &out[out_len - written], written);
    if (actual_len != NULL) {
        *actual_len = written;
    }
    return ESP_OK;
}

static esp_err_t validate_key_attributes(const psa_key_attributes_t *attributes,
                                         psa_key_type_t expected_type,
                                         const char *label)
{
    psa_key_type_t key_type = psa_get_key_type(attributes);
    size_t key_bits = psa_get_key_bits(attributes);

    ESP_RETURN_ON_FALSE(key_type == expected_type, ESP_ERR_INVALID_RESPONSE, DEVICE_IDENTITY_TAG,
                        "%s has unexpected PSA type: 0x%04x", label, (unsigned)key_type);
    ESP_RETURN_ON_FALSE(key_bits == DEVICE_KEY_BITS, ESP_ERR_INVALID_RESPONSE, DEVICE_IDENTITY_TAG,
                        "%s is not %u-bit P-256", label, (unsigned)DEVICE_KEY_BITS);
    return ESP_OK;
}

static esp_err_t import_pk_into_psa(const mbedtls_pk_context *pk,
                                    psa_key_usage_t usage,
                                    psa_key_type_t expected_type,
                                    mbedtls_svc_key_id_t *key_id,
                                    const char *label)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    esp_err_t err = ESP_OK;
    int ret = mbedtls_pk_get_psa_attributes(pk, usage, &attributes);
    if (ret != 0) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to derive PSA attributes for %s: %d", label, ret);
        goto cleanup;
    }

    err = validate_key_attributes(&attributes, expected_type, label);
    if (err != ESP_OK) {
        goto cleanup;
    }

    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, DEVICE_SIGN_ALG);
    set_empty_key_id(key_id);
    ret = mbedtls_pk_import_into_psa(pk, &attributes, key_id);
    if (ret != 0) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to import %s into PSA: %d", label, ret);
        goto cleanup;
    }

cleanup:
    psa_reset_key_attributes(&attributes);
    return err;
}

static esp_err_t parse_backend_key(device_identity_t *identity)
{
    mbedtls_pk_context backend_pk;
    mbedtls_pk_init(&backend_pk);

    uint8_t der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    size_t der_len = 0;
    esp_err_t err = ESP_OK;

    if (CONFIG_RECEIVER_BACKEND_PUBKEY_B64[0] == '\0') {
        err = ESP_ERR_INVALID_STATE;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "CONFIG_RECEIVER_BACKEND_PUBKEY_B64 is empty");
        goto cleanup;
    }

    err = b64_decode(CONFIG_RECEIVER_BACKEND_PUBKEY_B64, der, sizeof(der), &der_len);
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to decode backend key");
        goto cleanup;
    }

    int ret = mbedtls_pk_parse_public_key(&backend_pk, der, der_len);
    if (ret != 0) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to parse backend public key: %d", ret);
        goto cleanup;
    }

    err = import_pk_into_psa(&backend_pk, PSA_KEY_USAGE_VERIFY_HASH,
                             DEVICE_PUBLIC_KEY_TYPE, &identity->backend_key_id,
                             "backend key");

cleanup:
    mbedtls_pk_free(&backend_pk);
    return err;
}

static esp_err_t load_key_blob(nvs_handle_t handle,
                               const char *key,
                               uint8_t *out,
                               size_t out_len,
                               size_t *actual_len,
                               bool *present)
{
    size_t required = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (present != NULL) {
            *present = false;
        }
        if (actual_len != NULL) {
            *actual_len = 0;
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, DEVICE_IDENTITY_TAG, "failed to query %s", key);
    ESP_RETURN_ON_FALSE(required <= out_len, ESP_ERR_NVS_INVALID_LENGTH, DEVICE_IDENTITY_TAG,
                        "%s exceeds buffer", key);
    ESP_RETURN_ON_ERROR(nvs_get_blob(handle, key, out, &required), DEVICE_IDENTITY_TAG,
                        "failed to read %s", key);
    if (present != NULL) {
        *present = true;
    }
    if (actual_len != NULL) {
        *actual_len = required;
    }
    return ESP_OK;
}

static esp_err_t persist_generated_keypair(device_identity_t *identity, nvs_handle_t handle)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_pk_context device_pk;
    mbedtls_pk_init(&device_pk);

    uint8_t pub_der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    uint8_t priv_der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    size_t pub_len = 0;
    size_t priv_len = 0;
    esp_err_t err = ESP_OK;

    psa_set_key_type(&attributes, DEVICE_KEYPAIR_TYPE);
    psa_set_key_bits(&attributes, DEVICE_KEY_BITS);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attributes, DEVICE_SIGN_ALG);

    psa_status_t status = psa_generate_key(&attributes, &identity->device_key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "psa_generate_key failed: %d", (int)status);
        goto cleanup;
    }

    int ret = mbedtls_pk_copy_from_psa(identity->device_key_id, &device_pk);
    if (ret != 0) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to copy generated key from PSA: %d", ret);
        goto cleanup;
    }

    err = export_public_key_der(&device_pk, pub_der, sizeof(pub_der), &pub_len);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = export_private_key_der(&device_pk, priv_der, sizeof(priv_der), &priv_len);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = nvs_set_blob(handle, "pubkey", pub_der, pub_len);
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to store public key");
        goto cleanup;
    }
    err = nvs_set_blob(handle, "privkey", priv_der, priv_len);
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to store private key");
        goto cleanup;
    }
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to commit keypair");
        goto cleanup;
    }

    err = b64_encode(pub_der, pub_len, identity->public_key_b64,
                     sizeof(identity->public_key_b64));

cleanup:
    mbedtls_pk_free(&device_pk);
    return err;
}

static esp_err_t load_existing_keypair(device_identity_t *identity,
                                       const uint8_t *priv_der,
                                       size_t priv_len,
                                       const uint8_t *stored_pub_der,
                                       size_t stored_pub_len,
                                       bool has_pub)
{
    mbedtls_pk_context device_pk;
    mbedtls_pk_init(&device_pk);

    uint8_t derived_pub_der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    size_t derived_pub_len = 0;
    esp_err_t err = ESP_OK;

    int ret = mbedtls_pk_parse_key(&device_pk, priv_der, priv_len, NULL, 0);
    if (ret != 0) {
        err = ESP_FAIL;
        ESP_LOGE(DEVICE_IDENTITY_TAG, "failed to parse stored private key: %d", ret);
        goto cleanup;
    }

    err = export_public_key_der(&device_pk, derived_pub_der, sizeof(derived_pub_der), &derived_pub_len);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = b64_encode(derived_pub_der, derived_pub_len, identity->public_key_b64,
                     sizeof(identity->public_key_b64));
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (!has_pub) {
        ESP_LOGW(DEVICE_IDENTITY_TAG, "stored public key missing, derived public key from private key");
    } else if (stored_pub_len != derived_pub_len ||
               memcmp(stored_pub_der, derived_pub_der, derived_pub_len) != 0) {
        ESP_LOGW(DEVICE_IDENTITY_TAG, "stored public key does not match private key; using derived public key");
    }

    err = import_pk_into_psa(&device_pk, PSA_KEY_USAGE_SIGN_HASH,
                             DEVICE_KEYPAIR_TYPE, &identity->device_key_id,
                             "device key");

cleanup:
    mbedtls_pk_free(&device_pk);
    return err;
}

static esp_err_t load_or_generate_keypair(device_identity_t *identity)
{
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(DEVICE_IDENTITY_NAMESPACE, NVS_READWRITE, &handle),
                        DEVICE_IDENTITY_TAG, "failed to open identity namespace");

    uint8_t priv_der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    uint8_t pub_der[DEVICE_KEY_DER_BUF_LEN] = { 0 };
    size_t priv_len = 0;
    size_t pub_len = 0;
    bool has_priv = false;
    bool has_pub = false;

    esp_err_t err = load_key_blob(handle, "privkey", priv_der, sizeof(priv_der), &priv_len, &has_priv);
    if (err == ESP_OK) {
        err = load_key_blob(handle, "pubkey", pub_der, sizeof(pub_der), &pub_len, &has_pub);
    }

    if (err == ESP_OK) {
        if (has_priv) {
            err = load_existing_keypair(identity, priv_der, priv_len, pub_der, pub_len, has_pub);
        } else {
            err = persist_generated_keypair(identity, handle);
        }
    }

    nvs_close(handle);
    return err;
}

esp_err_t device_identity_init(device_identity_t *identity)
{
    ESP_RETURN_ON_FALSE(identity != NULL, ESP_ERR_INVALID_ARG, DEVICE_IDENTITY_TAG,
                        "identity is NULL");

    memset(identity, 0, sizeof(*identity));
    set_empty_key_id(&identity->device_key_id);
    set_empty_key_id(&identity->backend_key_id);

    psa_status_t status = psa_crypto_init();
    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "psa_crypto_init failed: %d", (int)status);

    esp_err_t err = load_or_generate_keypair(identity);
    if (err != ESP_OK) {
        clear_identity(identity);
        return err;
    }

    err = parse_backend_key(identity);
    if (err != ESP_OK) {
        clear_identity(identity);
        return err;
    }

    identity->initialized = true;
    return ESP_OK;
}

void device_identity_deinit(device_identity_t *identity)
{
    clear_identity(identity);
}

const char *device_identity_public_key_b64(const device_identity_t *identity)
{
    return identity != NULL ? identity->public_key_b64 : "";
}

esp_err_t device_identity_sign_message_b64(device_identity_t *identity,
                                           const char *message,
                                           char *out_b64,
                                           size_t out_len)
{
    ESP_RETURN_ON_FALSE(identity != NULL && message != NULL && out_b64 != NULL,
                        ESP_ERR_INVALID_ARG, DEVICE_IDENTITY_TAG, "invalid sign input");
    ESP_RETURN_ON_FALSE(identity->initialized, ESP_ERR_INVALID_STATE, DEVICE_IDENTITY_TAG,
                        "identity not initialized");

    uint8_t hash[32] = { 0 };
    uint8_t raw_signature[DEVICE_RAW_SIGNATURE_BUF_LEN] = { 0 };
    uint8_t der_signature[DEVICE_DER_SIGNATURE_BUF_LEN] = { 0 };
    size_t raw_signature_len = 0;
    size_t der_signature_len = 0;

    ESP_RETURN_ON_ERROR(device_identity_sha256(message, hash), DEVICE_IDENTITY_TAG,
                        "failed to hash signing payload");

    psa_status_t status = psa_sign_hash(identity->device_key_id, DEVICE_SIGN_ALG,
                                        hash, sizeof(hash),
                                        raw_signature, sizeof(raw_signature),
                                        &raw_signature_len);
    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "psa_sign_hash failed: %d", (int)status);
    ESP_RETURN_ON_FALSE(raw_signature_len == sizeof(raw_signature), ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "unexpected raw signature length: %u", (unsigned)raw_signature_len);

    int ret = mbedtls_ecdsa_raw_to_der(DEVICE_KEY_BITS,
                                       raw_signature, raw_signature_len,
                                       der_signature, sizeof(der_signature),
                                       &der_signature_len);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "failed to DER-encode signature: %d", ret);

    return b64_encode(der_signature, der_signature_len, out_b64, out_len);
}

esp_err_t device_identity_verify_message_b64(const device_identity_t *identity,
                                             const char *message,
                                             const char *signature_b64)
{
    ESP_RETURN_ON_FALSE(identity != NULL && message != NULL && signature_b64 != NULL,
                        ESP_ERR_INVALID_ARG, DEVICE_IDENTITY_TAG, "invalid verify input");
    ESP_RETURN_ON_FALSE(identity->initialized, ESP_ERR_INVALID_STATE, DEVICE_IDENTITY_TAG,
                        "identity not initialized");

    uint8_t hash[32] = { 0 };
    uint8_t der_signature[DEVICE_DER_SIGNATURE_BUF_LEN] = { 0 };
    uint8_t raw_signature[DEVICE_RAW_SIGNATURE_BUF_LEN] = { 0 };
    size_t der_signature_len = 0;
    size_t raw_signature_len = 0;

    ESP_RETURN_ON_ERROR(b64_decode(signature_b64, der_signature, sizeof(der_signature),
                                   &der_signature_len),
                        DEVICE_IDENTITY_TAG, "failed to decode signature");
    ESP_RETURN_ON_ERROR(device_identity_sha256(message, hash), DEVICE_IDENTITY_TAG,
                        "failed to hash verification payload");

    int ret = mbedtls_ecdsa_der_to_raw(DEVICE_KEY_BITS,
                                       der_signature, der_signature_len,
                                       raw_signature, sizeof(raw_signature),
                                       &raw_signature_len);
    ESP_RETURN_ON_FALSE(ret == 0, ESP_ERR_INVALID_RESPONSE, DEVICE_IDENTITY_TAG,
                        "failed to decode DER signature: %d", ret);

    psa_status_t status = psa_verify_hash(identity->backend_key_id, DEVICE_SIGN_ALG,
                                          hash, sizeof(hash),
                                          raw_signature, raw_signature_len);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        ESP_LOGW(DEVICE_IDENTITY_TAG, "signature verification failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_RETURN_ON_FALSE(status == PSA_SUCCESS, ESP_FAIL, DEVICE_IDENTITY_TAG,
                        "psa_verify_hash failed: %d", (int)status);
    return ESP_OK;
}

esp_err_t device_identity_generate_nonce(device_identity_t *identity,
                                         size_t random_bytes,
                                         char *out_nonce,
                                         size_t out_len)
{
    ESP_RETURN_ON_FALSE(identity != NULL && out_nonce != NULL, ESP_ERR_INVALID_ARG,
                        DEVICE_IDENTITY_TAG, "invalid nonce input");
    ESP_RETURN_ON_FALSE(random_bytes > 0 && random_bytes <= 24, ESP_ERR_INVALID_ARG,
                        DEVICE_IDENTITY_TAG, "invalid nonce size");

    uint8_t random_buf[24] = { 0 };
    char b64_buf[48] = { 0 };
    esp_fill_random(random_buf, random_bytes);
    ESP_RETURN_ON_ERROR(b64_encode(random_buf, random_bytes, b64_buf, sizeof(b64_buf)),
                        DEVICE_IDENTITY_TAG, "base64 encode failed");

    size_t j = 0;
    for (size_t i = 0; b64_buf[i] != '\0'; ++i) {
        char c = b64_buf[i];
        if (c == '=') {
            break;
        }
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
        ESP_RETURN_ON_FALSE(j + 1 < out_len, ESP_ERR_INVALID_SIZE, DEVICE_IDENTITY_TAG,
                            "nonce buffer too small");
        out_nonce[j++] = c;
    }
    out_nonce[j] = '\0';
    return ESP_OK;
}
