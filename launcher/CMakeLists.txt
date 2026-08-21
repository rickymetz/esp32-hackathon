cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# I3: force-include lv_assert_override.h ahead of every translation unit so
# it can #define LV_ASSERT_HANDLER before lvgl's own lv_conf_internal.h
# falls back to `while(1);` -- see that file for why this isn't done via
# sdkconfig.defaults or a plain -D compile definition (both were tried and
# both silently lost the ';' the macro's body needs).
# Restricted to C (COMPILE_LANGUAGE genex) -- applied unconditionally this
# also reaches assembler (.S) sources, which choke trying to assemble a C
# header (confirmed on hardware: an unrestricted -include broke the ASM
# build with "unknown opcode" errors from esp_system.h's C declarations).
idf_build_set_property(COMPILE_OPTIONS
    "$<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_LIST_DIR}/lv_assert_override.h>" APPEND)

# The vendored espressif/lua component builds its core with LUA_32BITS=1 but
# only applies it PRIVATE, so any component that includes lua.h would default
# to 64-bit lua_Integer/lua_Number and mismatch the core's ABI -- which aborts
# in luaL_checkversion_ at runtime. Set it build-wide so every current and
# future capability module agrees with the core.
idf_build_set_property(COMPILE_DEFINITIONS "LUA_32BITS=1" APPEND)

project(launcher)
