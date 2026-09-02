idf_component_register(
    SRCS "lua_module_store.c" "store_exit_flush.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "store.lua"
    REQUIRES cap_lua espressif__lua
)
