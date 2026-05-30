#pragma once

/* Compile-time version string, set in main/CMakeLists.txt from the project
 * version in firmware/CMakeLists.txt. Single source of truth. */
extern const char *app_version(void);
