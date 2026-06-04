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

/* True when the touch INT pin reads its configured active level (a touch is
 * being signaled). The poll loop gates its I2C read on this so the CST816 is
 * never read while idle — the chip NACKs reads in standby, which otherwise
 * floods the console with I2C errors. */
bool touch_int_active(int int_level, int active_level);
