#pragma once

#include "lvgl.h"

/* Per-state crab sprites (280x280 RGB565A8, transparent background) shown on the
 * display's mood-colour ring. Generated from PNGs by firmware/tools/process_crabs.py
 * + LVGL's LVGLImage.py; one descriptor per display state. */
extern const lv_image_dsc_t crab_idle;
extern const lv_image_dsc_t crab_listening;
extern const lv_image_dsc_t crab_thinking;
extern const lv_image_dsc_t crab_speaking;
extern const lv_image_dsc_t crab_error;
