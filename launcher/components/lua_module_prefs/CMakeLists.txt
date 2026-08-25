idf_component_register(
    SRCS "lua_module_prefs.c"
    INCLUDE_DIRS "include"
    REQUIRES cap_lua espressif__lua nvs_flash
)
