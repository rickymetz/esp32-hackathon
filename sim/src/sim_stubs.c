/* Empty replacements for the two binding modules the sim excludes:
 *   - lua_lvgl_eaf.c   (Espressif Animation Format player -- a component)
 *   - lua_lvgl_demos.c (bundled lv_demos, disabled in the sim's lv_conf)
 * lua_module_lvgl.c registers these tables unconditionally, so empty (but
 * present) tables keep the module surface intact: the `lvgl` table simply gains
 * no eaf/demo functions. */
#include "lua_lvgl_private.h"

const luaL_Reg lua_lvgl_eaf_module_funcs[]  = { { NULL, NULL } };
const luaL_Reg lua_lvgl_demo_module_funcs[] = { { NULL, NULL } };

/* lua_lvgl_methods.c wires these into the eaf-widget method table, so they must
 * exist as symbols even though the sim has no EAF player. Each is a hard Lua
 * error: an app that reaches for EAF playback is doing something the sim can't
 * simulate, and a clear error beats a silent no-op. */
#define SIM_EAF_STUB(fn) \
    int fn(lua_State *L) { return luaL_error(L, "%s: EAF playback is not available in the simulator", #fn); }

SIM_EAF_STUB(lua_lvgl_eaf_set_src)
SIM_EAF_STUB(lua_lvgl_eaf_set_src_data)
SIM_EAF_STUB(lua_lvgl_eaf_restart)
SIM_EAF_STUB(lua_lvgl_eaf_pause)
SIM_EAF_STUB(lua_lvgl_eaf_resume)
SIM_EAF_STUB(lua_lvgl_eaf_is_loaded)
SIM_EAF_STUB(lua_lvgl_eaf_get_loop_count)
SIM_EAF_STUB(lua_lvgl_eaf_set_loop_count)
SIM_EAF_STUB(lua_lvgl_eaf_get_total_frames)
SIM_EAF_STUB(lua_lvgl_eaf_get_current_frame)
SIM_EAF_STUB(lua_lvgl_eaf_set_frame_delay)
SIM_EAF_STUB(lua_lvgl_eaf_get_frame_delay)
SIM_EAF_STUB(lua_lvgl_eaf_set_loop_enabled)
SIM_EAF_STUB(lua_lvgl_eaf_get_loop_enabled)
