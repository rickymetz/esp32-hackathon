cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The vendored espressif/lua component builds its core with LUA_32BITS=1 but
# only applies it PRIVATE, so any component that includes lua.h would default
# to 64-bit lua_Integer/lua_Number and mismatch the core's ABI -- which aborts
# in luaL_checkversion_ at runtime. Set it build-wide so every current and
# future capability module agrees with the core.
idf_build_set_property(COMPILE_DEFINITIONS "LUA_32BITS=1" APPEND)

project(launcher)
