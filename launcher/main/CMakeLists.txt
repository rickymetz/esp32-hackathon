idf_component_register(SRCS "launcher_main.c" "app_registry.c"
                       INCLUDE_DIRS "."
                       REQUIRES cap_lua display_service lua_module_lvgl fatfs)
