#include <string.h>

#include "unity.h"
#include "app_version.h"

/* Sanity check: the compile-time APP_VERSION macro round-trips through
 * app_version(). The host CMake sets APP_VERSION="test-1.2.3" for this
 * binary; if this ever fails, the build system probably stopped wiring
 * the project version into compile flags. */
void test_app_version_returns_compiled_value(void) {
    const char *v = app_version();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("test-1.2.3", v);
}
