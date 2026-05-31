idf_component_register(
    SRCS
        "main.c"
        "app_version.c"
        "display.c"
        "st77916_variant.c"
        "touch.c"
        "touch_geom.c"
        "audio.c"
        "audio_tone.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        log
        esp_system
        esp_psram
        freertos
        driver
        esp_lcd
        esp_lcd_st77916
        esp_io_expander_tca9554
        esp_lvgl_port
        esp_lcd_touch_cst816s
)

# Stamp the build with a version string so boot logs are traceable.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_VERSION="${CMAKE_PROJECT_VERSION}"
)
