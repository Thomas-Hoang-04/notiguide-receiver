/**
 * @file device_identity.h
 * @brief Device identity, signing, and signature verification helpers.
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "psa/crypto.h"

#define DEVICE_IDENTITY_MAX_PUBKEY_B64_LEN     512U
#define DEVICE_IDENTITY_MAX_SIGNATURE_B64_LEN  256U

typedef struct {
    bool initialized;
    char public_key_b64[DEVICE_IDENTITY_MAX_PUBKEY_B64_LEN];
    mbedtls_svc_key_id_t device_key_id;
    mbedtls_svc_key_id_t backend_key_id;
} device_identity_t;

/**
 * @brief Initialize the device identity context.
 *
 * Loads or generates the device P-256 keypair, imports it into the PSA
 * keystore for signing, exports the device public key in base64-DER form,
 * and imports the firmware-pinned backend public key into the PSA keystore
 * for signature verification.
 *
 * @param identity Device identity context to initialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_identity_init(device_identity_t *identity);

/**
 * @brief Deinitialize the device identity context.
 *
 * @param identity Device identity context to clear
 */
void device_identity_deinit(device_identity_t *identity);

/**
 * @brief Get the base64-DER encoded device public key.
 *
 * @param identity Device identity context
 * @return Null-terminated public key string, or an empty string when unavailable
 */
const char *device_identity_public_key_b64(const device_identity_t *identity);

/**
 * @brief Sign a canonical message and return the signature in base64.
 *
 * @param identity Device identity context
 * @param message Canonical message to sign
 * @param out_b64 Output buffer for the base64 signature
 * @param out_len Size of @p out_b64 in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_identity_sign_message_b64(device_identity_t *identity,
                                           const char *message,
                                           char *out_b64,
                                           size_t out_len);

/**
 * @brief Verify a backend-signed canonical message.
 *
 * @param identity Device identity context
 * @param message Canonical message to verify
 * @param signature_b64 Base64-encoded signature to validate
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_identity_verify_message_b64(const device_identity_t *identity,
                                             const char *message,
                                             const char *signature_b64);

/**
 * @brief Generate a URL-safe base64 nonce.
 *
 * @param identity Device identity context
 * @param random_bytes Number of raw random bytes to encode
 * @param out_nonce Output buffer for the base64url nonce
 * @param out_len Size of @p out_nonce in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t device_identity_generate_nonce(device_identity_t *identity,
                                         size_t random_bytes,
                                         char *out_nonce,
                                         size_t out_len);

#endif /* DEVICE_IDENTITY_H */
