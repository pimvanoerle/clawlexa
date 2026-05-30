#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Pure geometry helpers for the round panel — no IDF/IO deps so they can be
 * unit-tested host-side (see firmware/tests/host). */

/* True if (x, y) lies within the circular active area centered at (cx, cy)
 * with the given radius (boundary inclusive). The CST816 can report points
 * just outside the visible glass; callers use this to drop them. */
bool touch_in_circle(int16_t x, int16_t y, int16_t cx, int16_t cy,
                     uint16_t radius);
