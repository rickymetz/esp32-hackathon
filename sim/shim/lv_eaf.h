/* Host shim for lv_eaf.h (Espressif Animation Format player).
 *
 * The EAF player is a hardware/component feature the sim does not implement.
 * lua_lvgl_private.h includes this header unconditionally, but the sim excludes
 * lua_lvgl_eaf.c and registers an empty eaf function table (see sim_stubs.c),
 * so no EAF types are actually needed here. */
#ifndef SIM_LV_EAF_H
#define SIM_LV_EAF_H

typedef struct sim_esp_lv_eaf_player *esp_lv_eaf_player_handle_t;

#endif /* SIM_LV_EAF_H */
