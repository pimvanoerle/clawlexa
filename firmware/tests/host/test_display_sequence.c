#include <string.h>

#include "unity.h"

#include "fake_idf.h"
#include "display.h"
#include "st77916_waveshare_init.h"  /* for the expected init-array sizes */

/* Tier-T3 interaction test: compile the real display.c against fake IDF/driver
 * APIs and assert the *sequence and arguments* of the bring-up — the ordering
 * footguns from DT-3 — with no hardware. */

#define NEW_CMDS_SIZE     (sizeof(st77916_init_new) / sizeof(st77916_init_new[0]))
#define DEFAULT_CMDS_SIZE (sizeof(st77916_init_default) / sizeof(st77916_init_default[0]))

void setUp(void) { faked_reset(); }
void tearDown(void) {}

/* Assert event `before` was recorded and precedes event `after`. */
static void assert_order(const char *before, const char *after) {
    int i = faked_index_of(before);
    int j = faked_index_of(after);
    TEST_ASSERT_MESSAGE(i >= 0, before);
    TEST_ASSERT_MESSAGE(j >= 0, after);
    TEST_ASSERT_TRUE_MESSAGE(i < j, "expected first event before second");
}

/* With the "new" panel id, display_init must pick the new init array and run
 * the bring-up in the right order. */
void test_new_variant_full_sequence(void) {
    const uint8_t new_id[4] = {0x00, 0x02, 0x7F, 0x7F};
    memcpy(faked_reg04, new_id, sizeof(new_id));

    i2c_master_bus_handle_t bus = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, display_init(&bus));

    /* correct variant array selected (by size — array contents come from the
     * shared header, so size is a faithful proxy that doesn't break across TUs) */
    TEST_ASSERT_EQUAL_UINT16(NEW_CMDS_SIZE, faked_init_cmds_size);

    /* reset is driven via the expander, so the panel driver must get -1 */
    TEST_ASSERT_EQUAL_INT(-1, faked_panel_reset_gpio);

    /* expander reset pulse: assert low then release high, before the panel inits */
    assert_order("expander_reset_assert", "expander_reset_release");
    assert_order("expander_reset_release", "panel_init");

    /* variant probe happens on a throwaway IO that is torn down before the
     * real panel is created */
    assert_order("panel_io_rx_param", "panel_io_del");
    assert_order("panel_io_del", "st77916_new");

    /* GRAM is cleared before LVGL takes over */
    assert_order("panel_draw_bitmap", "lvgl_port_init");
    assert_order("lvgl_port_init", "lvgl_port_add_disp");
}

/* The "default" panel id selects the other array. */
void test_default_variant_selected(void) {
    const uint8_t def_id[4] = {0x00, 0x7F, 0x7F, 0x7F};
    memcpy(faked_reg04, def_id, sizeof(def_id));

    TEST_ASSERT_EQUAL(ESP_OK, display_init(NULL));
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_CMDS_SIZE, faked_init_cmds_size);
}

/* A failed register read must not crash and must fall back to the default
 * array (not pick "new" off a garbage/zeroed buffer). */
void test_failed_probe_falls_back_to_default(void) {
    faked_reg04_fail = true;

    TEST_ASSERT_EQUAL(ESP_OK, display_init(NULL));
    TEST_ASSERT_EQUAL_UINT16(DEFAULT_CMDS_SIZE, faked_init_cmds_size);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_new_variant_full_sequence);
    RUN_TEST(test_default_variant_selected);
    RUN_TEST(test_failed_probe_falls_back_to_default);
    return UNITY_END();
}
