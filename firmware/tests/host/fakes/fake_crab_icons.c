/* Host-build stand-ins for the per-state crab image descriptors. The real
 * 280x280 RGB565A8 arrays under main/crab_icons are ~1.2 MB and irrelevant to the
 * display *sequence* test, so here they're just empty descriptors to satisfy the
 * link (display.c references &crab_idle etc. in display_set_state). */
#include "crab_icons.h"

const lv_image_dsc_t crab_idle = {0};
const lv_image_dsc_t crab_listening = {0};
const lv_image_dsc_t crab_thinking = {0};
const lv_image_dsc_t crab_speaking = {0};
const lv_image_dsc_t crab_error = {0};
