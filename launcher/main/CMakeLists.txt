idf_component_register(SRCS "launcher_main.c" "launcher_home.c" "launcher_face.c" "launcher_icons.c" "app_registry.c" "serial_push.c" "app_timer.c" "app_sandbox.c"
                       INCLUDE_DIRS "."
                       REQUIRES cap_lua display_service lua_module_lvgl lua_module_button lua_module_ui lua_module_store lua_module_prefs lua_module_voice lua_module_sensors lua_module_wifi fonts_lexend fatfs mbedtls esp_hw_support esp_timer esp_driver_gpio)
