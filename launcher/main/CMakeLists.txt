idf_component_register(SRCS "launcher_main.c" "app_registry.c" "serial_push.c" "app_timer.c"
                       INCLUDE_DIRS "."
                       REQUIRES cap_lua display_service lua_module_lvgl fatfs mbedtls esp_hw_support esp_timer)

# The vendored espressif/lua component only defines LUA_32BITS for its own
# sources (CMakeLists.txt there uses target_compile_definitions(... PRIVATE
# LUA_32BITS=1)), so lua_Integer/lua_Number in every consuming component
# silently resolve to the 64-bit default (long long/double) instead of the
# core's actual 32-bit int/float. That is a real ABI mismatch, not just a
# luaL_checkversion_() false alarm: app_timer.c calls luaL_checkinteger()
# across that boundary, so without this the returned lua_Integer is corrupt.
# Defining it here (matching the core's own flag) fixes app_timer.c without
# touching the vendored component.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)
