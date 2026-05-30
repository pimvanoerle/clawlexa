#pragma once

#include <stdint.h>

/* The ESP32-S3-Touch-LCD-1.85C ships two ST77916 panel variants that need
 * different init sequences. Waveshare's demo tells them apart by reading panel
 * register 0x04. This decision is pure (no IDF/IO) so it can be unit-tested
 * host-side; display.c does the actual register read and array selection. */
typedef enum {
    ST77916_VARIANT_DEFAULT = 0,  /* register 0x04 == {00,7F,7F,7F} (or unknown) */
    ST77916_VARIANT_NEW,          /* register 0x04 == {00,02,7F,7F}; needs INVON */
} st77916_variant_t;

/* Map the 4-byte register-0x04 readout to a panel variant. Unrecognized or
 * all-zero (failed-read) ids fall back to DEFAULT. */
st77916_variant_t st77916_variant_from_id(const uint8_t id[4]);
