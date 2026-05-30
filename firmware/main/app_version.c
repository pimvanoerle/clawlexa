#include "app_version.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0-unknown"
#endif

const char *app_version(void) {
    return APP_VERSION;
}
