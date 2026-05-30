#include "unity.h"

void test_app_version_returns_compiled_value(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_app_version_returns_compiled_value);
    return UNITY_END();
}
