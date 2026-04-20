/**
 * @file rf_trigger.h
 * @brief RF Trigger Module - Match and Act Interface
 *
 * Declares the shared trigger-state APIs used to store the active RF code,
 * compare decoded frames, and drive the vibrator on a match.
 */

#ifndef RF_TRIGGER_H
#define RF_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "vibrator/vibrator.h"

#define RF_TRIGGER_TAG "RF_TRIGGER"

typedef struct {
    uint32_t code;
    uint8_t bits;
    uint32_t version;
} rf_trigger_snapshot_t;

esp_err_t rf_trigger_init(VibratorHandler *vibrator);
void rf_trigger_deinit(void);
void rf_trigger_stop_output(void);
void rf_trigger_set(uint32_t code, uint8_t bits, uint32_t version);
void rf_trigger_clear(void);
bool rf_trigger_has_code(void);
void rf_trigger_restore(uint32_t code, uint8_t bits, uint32_t version);
rf_trigger_snapshot_t rf_trigger_snapshot(void);
void rf_trigger_on_frame(uint32_t decoded, uint8_t decoded_bits, void *ctx);

#endif
