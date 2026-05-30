idf_component_register(
    SRCS
        "main.c"
        "app_version.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        log
        esp_system
        freertos
)

# Stamp the build with a version string so boot logs are traceable.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    APP_VERSION="${CMAKE_PROJECT_VERSION}"
)
