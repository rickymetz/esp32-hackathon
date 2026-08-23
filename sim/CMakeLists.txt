cmake_minimum_required(VERSION 3.16)
project(esp32_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- LVGL (host build) --------------------------------------------------
# Point LVGL at our lv_conf.h and drop the parts we don't need so the build
# stays fast.
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/lvgl)

# --- Lua 5.5 core (host build) ------------------------------------------
# Same upstream core the device's espressif/lua component wraps. Build every
# core+stdlib .c except the standalone mains (lua.c, luac.c), the amalgamation
# (onelua.c), and the internal test harness (ltests.c).
file(GLOB LUA_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/external/lua/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(lua STATIC ${LUA_SRC})
target_include_directories(lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/external/lua)
# Lua uses libm and (for io/os) standard POSIX; enable them.
target_link_libraries(lua PUBLIC m)

# --- Slice 1: LVGL renders our panel to a PNG ---------------------------
add_executable(sim_slice1
    src/main_slice1.c
    src/sim_display.c
    src/sim_tick.c
    src/png_write.c
)
target_include_directories(sim_slice1 PRIVATE src)
target_link_libraries(sim_slice1 PRIVATE lvgl)

# --- Slice 2: Lua 5.5 runs on host --------------------------------------
add_executable(sim_slice2 src/main_slice2.c)
target_link_libraries(sim_slice2 PRIVATE lua)

# --- Real launcher bindings, compiled against host LVGL/Lua --------------
set(LAUNCHER ${CMAKE_CURRENT_SOURCE_DIR}/../launcher)
set(LVGL_BINDING_SRC ${LAUNCHER}/components/lua_module_lvgl/src)

# Every binding TU except the two hardware/component-only ones (eaf, demos),
# which sim_stubs.c replaces with empty module tables.
file(GLOB BINDING_SRC CONFIGURE_DEPENDS ${LVGL_BINDING_SRC}/*.c)
list(FILTER BINDING_SRC EXCLUDE REGEX "lua_lvgl_(eaf|demos)\\.c$")

# fonts_lexend: the five baked-in faces lvgl.font(size) resolves to.
file(GLOB LEXEND_SRC CONFIGURE_DEPENDS ${LAUNCHER}/components/fonts_lexend/*.c)

add_library(bindings STATIC
    ${BINDING_SRC}
    ${LEXEND_SRC}
    ${LAUNCHER}/components/cap_lua/cap_lua.c
    ${LAUNCHER}/components/cap_lua/claw_paths.c
    # Reused launcher modules -- all portable against the shims.
    ${LAUNCHER}/main/app_timer.c
    ${LAUNCHER}/main/app_sandbox.c
    ${LAUNCHER}/main/launcher_home.c
    ${LAUNCHER}/components/lua_module_button/app_button.c
    # Sim-specific pieces.
    src/display_service_sim.c
    src/sim_stubs.c
    src/sim_voice.c
    src/sim_audio.c
    src/sim_wifi.c
    src/sim_sensors.c
    src/sim_module_ui.c
    shim/esp_shim.c
)
target_include_directories(bindings PUBLIC
    shim
    ${LVGL_BINDING_SRC}
    ${LAUNCHER}/main
    ${LAUNCHER}/components/cap_lua/include
    ${LAUNCHER}/components/display_service/include
    ${LAUNCHER}/components/fonts_lexend/include
    ${LAUNCHER}/components/lua_module_button/include
    ${LAUNCHER}/components/lua_module_voice/include
    ${LAUNCHER}/components/lua_module_audio/include
    ${LAUNCHER}/components/lua_module_wifi/include
    ${LAUNCHER}/components/lua_module_sensors/include
    ${LAUNCHER}/components/lua_module_ui/include
)
target_compile_definitions(bindings PUBLIC
    UI_LUA_DIR="${LAUNCHER}/components/lua_module_ui"
)
# The device forces LUA_32BITS build-wide; on host we keep native 64-bit Lua,
# and both the core and these bindings agree because neither defines it.
target_link_libraries(bindings PUBLIC lvgl lua)

# --- Slice 3: the real bindings render a Lua-built screen ---------------
add_executable(sim_slice3
    src/main_slice3.c
    src/sim_display.c
    src/sim_input.c
    src/sim_tick.c
    src/png_write.c
)
target_include_directories(sim_slice3 PRIVATE src)
target_link_libraries(sim_slice3 PRIVATE bindings)

# --- The simulator: run apps, drive them, screenshot ---------------------
add_executable(sim
    src/sim_main.c
    src/sim_display.c
    src/sim_input.c
    src/sim_tick.c
    src/png_write.c
)
target_include_directories(sim PRIVATE src)
target_link_libraries(sim PRIVATE bindings)

# --- Unit tests for the sim's own hand-written C ------------------------
add_executable(sim_tests
    test/test_units.c
    src/png_write.c
    src/sim_input.c
    src/sim_tick.c
    shim/esp_shim.c
)
target_include_directories(sim_tests PRIVATE src shim)
target_link_libraries(sim_tests PRIVATE lvgl m)
