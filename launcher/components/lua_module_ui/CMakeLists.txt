idf_component_register(
    SRCS "lua_module_ui.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "ui.lua" "keyboard.lua"
    REQUIRES cap_lua espressif__lua
)
