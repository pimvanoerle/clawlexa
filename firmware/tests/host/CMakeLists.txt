cmake_minimum_required(VERSION 3.16)
project(clawlexa_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict-ish compile flags — host tests are cheap, catch bugs early.
add_compile_options(
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wpointer-arith
    -Wstrict-prototypes
)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(unity)

enable_testing()

# Path to firmware source we're testing.
set(FIRMWARE_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/../../main")

# --- test_app_version ---------------------------------------------------
add_executable(test_app_version
    test_runner.c
    test_app_version.c
    "${FIRMWARE_MAIN}/app_version.c"
)
target_include_directories(test_app_version PRIVATE
    "${FIRMWARE_MAIN}/include"
)
target_compile_definitions(test_app_version PRIVATE
    APP_VERSION="test-1.2.3"
)
target_link_libraries(test_app_version PRIVATE unity)
add_test(NAME test_app_version COMMAND test_app_version)
# -----------------------------------------------------------------------
