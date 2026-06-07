#include "unity.h"

#include "fake_idf.h"
#include "touch.h"
#include "board.h"

/* Tier-T3 interaction test for touch.c: compile the real module against the
 * fake IDF/component APIs and assert its init wiring with no hardware. The
 * fake xTaskCreate does not run the polling loop, so this covers setup only;
 * the round-panel filter (touch_in_circle) is covered by test_touch_geom. */

void setUp(void) { faked_reset(); }
void tearDown(void) {}

static void assert_order(const char *before, const char *after) {
    int i = faked_index_of(before);
    int j = faked_index_of(after);
    TEST_ASSERT_MESSAGE(i >= 0, before);
    TEST_ASSERT_MESSAGE(j >= 0, after);
    TEST_ASSERT_TRUE_MESSAGE(i < j, "expected first event before second");
}

void test_touch_init_sequence_and_config(void) {
    int dummy_bus;
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)&dummy_bus;

    TEST_ASSERT_EQUAL(ESP_OK, touch_init(bus));

    /* I2C panel IO created, then the CST816 controller, then the INT callback is
     * registered, then the poll task — the INT is armed before the task starts so
     * no first-touch edge is lost. */
    assert_order("panel_io_i2c_new", "cst816s_new");
    assert_order("cst816s_new", "touch_register_isr");
    assert_order("touch_register_isr", "xTaskCreate");

    /* Touch config wired to the board: INT on the known GPIO, reset left to the
     * shared expander (-1), bounds set to the panel width. */
    TEST_ASSERT_EQUAL_INT(BOARD_TOUCH_INT_GPIO, faked_touch_int_gpio);
    TEST_ASSERT_EQUAL_INT(-1, faked_touch_rst_gpio);
    TEST_ASSERT_EQUAL_UINT16(BOARD_LCD_H_RES, faked_touch_x_max);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_touch_init_sequence_and_config);
    return UNITY_END();
}
