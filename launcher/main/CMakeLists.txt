idf_component_register(SRCS "launcher_main.c" "app_registry.c" "serial_push.c" "app_timer.c" "app_sandbox.c"
                       INCLUDE_DIRS "."
                       REQUIRES cap_lua display_service lua_module_lvgl lua_module_button lua_module_ui fonts_lexend fatfs mbedtls esp_hw_support esp_timer esp_driver_gpio)
