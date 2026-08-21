idf_component_register(SRCS "launcher_main.c" "app_registry.c" "serial_push.c" "app_timer.c"
                       INCLUDE_DIRS "."
                       REQUIRES cap_lua display_service lua_module_lvgl fatfs mbedtls esp_hw_support esp_timer)
