idf_component_register(
    SRCS "app_button.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_lua espressif__lua esp_timer lua_module_lvgl
)
