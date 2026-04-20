/**
 * @file device_identity.h
 * @brief Device Identity Module - Cryptographic Interface
 *
 * Declares the device key management and backend signature verification APIs
 * used by activation and signed operational command handling.
 */

#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"

#define DEVICE_IDENTITY_TAG "IDENTITY"

typedef struct {
    mbedtls_pk_context device_key;
    mbedtls_pk_context backend_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    bool device_key_ready;
    bool backend_key_ready;
} device_identity_t;

esp_err_t device_identity_init(device_identity_t *identity);
void device_identity_deinit(device_identity_t *identity);
bool device_identity_backend_ready(const device_identity_t *identity);
esp_err_t device_identity_get_public_key_b64(device_identity_t *identity, char **out_b64);
esp_err_t device_identity_make_registration_nonce(char **out_nonce);
esp_err_t device_identity_sign_activation_response(device_identity_t *identity,
                                                   const char *challenge_id,
                                                   const char *nonce,
                                                   const char *issued_at,
                                                   const char *expires_at,
                                                   char **out_signature_b64);
esp_err_t device_identity_verify_signature(device_identity_t *identity,
                                           const char *canonical,
                                           const char *signature_b64,
                                           bool *verified);

#endif
