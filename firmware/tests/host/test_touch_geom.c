#include "unity.h"
#include "touch_geom.h"

/* The clawlexa panel is a 360x360 round display: center (180,180), radius 180.
 * touch_in_circle drops capacitive reports that land outside the glass. */

#define CX 180
#define CY 180
#define R  180

void setUp(void) {}
void tearDown(void) {}

void test_center_is_inside(void) {
    TEST_ASSERT_TRUE(touch_in_circle(CX, CY, CX, CY, R));
}

void test_boundary_is_inclusive(void) {
    /* Exactly on the rim along each axis. */
    TEST_ASSERT_TRUE(touch_in_circle(CX + R, CY, CX, CY, R));
    TEST_ASSERT_TRUE(touch_in_circle(CX - R, CY, CX, CY, R));
    TEST_ASSERT_TRUE(touch_in_circle(CX, CY + R, CX, CY, R));
}

void test_just_outside_rim_is_rejected(void) {
    TEST_ASSERT_FALSE(touch_in_circle(CX + R + 1, CY, CX, CY, R));
}

void test_corner_is_outside_round_panel(void) {
    /* (0,0) is a bounding-box corner — well outside the inscribed circle. */
    TEST_ASSERT_FALSE(touch_in_circle(0, 0, CX, CY, R));
}

/* INT gating: CST816 INT is active-low, so we read only when the pin reads 0. */
void test_int_active_low_asserted(void) {
    TEST_ASSERT_TRUE(touch_int_active(0, 0));   /* pin low, active-low -> touch */
}

void test_int_active_low_idle(void) {
    TEST_ASSERT_FALSE(touch_int_active(1, 0));  /* pin high, active-low -> idle */
}

void test_int_active_high_polarity(void) {
    /* The helper honors whatever active level it's told (active-high case). */
    TEST_ASSERT_TRUE(touch_int_active(1, 1));
    TEST_ASSERT_FALSE(touch_int_active(0, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_center_is_inside);
    RUN_TEST(test_boundary_is_inclusive);
    RUN_TEST(test_just_outside_rim_is_rejected);
    RUN_TEST(test_corner_is_outside_round_panel);
    RUN_TEST(test_int_active_low_asserted);
    RUN_TEST(test_int_active_low_idle);
    RUN_TEST(test_int_active_high_polarity);
    return UNITY_END();
}
