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

# --- test_touch_geom ----------------------------------------------------
# Pure round-panel geometry, no IDF/IO deps. Self-contained runner.
add_executable(test_touch_geom
    test_touch_geom.c
    "${FIRMWARE_MAIN}/touch_geom.c"
)
target_include_directories(test_touch_geom PRIVATE
    "${FIRMWARE_MAIN}/include"
)
target_link_libraries(test_touch_geom PRIVATE unity)
add_test(NAME test_touch_geom COMMAND test_touch_geom)
# -----------------------------------------------------------------------

# --- test_st77916_variant -----------------------------------------------
# Pure panel-variant decision (the DT-3 footgun core), no IDF/IO deps.
add_executable(test_st77916_variant
    test_st77916_variant.c
    "${FIRMWARE_MAIN}/st77916_variant.c"
)
target_include_directories(test_st77916_variant PRIVATE
    "${FIRMWARE_MAIN}/include"
)
target_link_libraries(test_st77916_variant PRIVATE unity)
add_test(NAME test_st77916_variant COMMAND test_st77916_variant)
# -----------------------------------------------------------------------

# --- test_display_sequence (tier T3: mocked-IO interaction test) ---------
# Compiles the real display.c against fake IDF/component headers (fakes/) and
# asserts the bring-up call sequence + variant selection with no hardware.
add_executable(test_display_sequence
    test_display_sequence.c
    fakes/fake_idf.c
    "${FIRMWARE_MAIN}/display.c"
    "${FIRMWARE_MAIN}/st77916_variant.c"
)
target_include_directories(test_display_sequence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
    "${FIRMWARE_MAIN}/include"
)
target_link_libraries(test_display_sequence PRIVATE unity)
add_test(NAME test_display_sequence COMMAND test_display_sequence)
# -----------------------------------------------------------------------

# --- test_touch_sequence (tier T3: mocked-IO interaction test) -----------
add_executable(test_touch_sequence
    test_touch_sequence.c
    fakes/fake_idf.c
    "${FIRMWARE_MAIN}/touch.c"
    "${FIRMWARE_MAIN}/touch_geom.c"
)
target_include_directories(test_touch_sequence PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fakes"
    "${FIRMWARE_MAIN}/include"
)
target_link_libraries(test_touch_sequence PRIVATE unity)
add_test(NAME test_touch_sequence COMMAND test_touch_sequence)
# -----------------------------------------------------------------------
