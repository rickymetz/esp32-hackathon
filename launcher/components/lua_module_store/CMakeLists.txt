idf_component_register(
    SRCS "lua_module_store.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "store.lua"
    REQUIRES cap_lua espressif__lua
)
