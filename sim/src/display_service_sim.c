/* Host implementation of the launcher's display_service.
 *
 * The real display_service owns the panel, the LVGL display, and the touch
 * indev, brought up once at boot (bsp_display_start); each app then opens a
 * session on it via lvgl.init(). We mirror that split:
 *
 *   sim_display_service_boot()  -- create display + synthetic indev + theme,
 *                                  once, like the launcher's app_main().
 *   display_service_open()      -- open a per-app session on the live display.
 *   display_service_close()     -- end the session; the display stays up.
 *
 * so lua_lvgl_runtime.c compiles and runs unchanged. Only the entry points the
 * bindings actually call are implemented.
 */
#include "display_service.h"
#include "sim_display.h"
#include "sim_input.h"
#include "lv_font_lexend.h"
#include "lua_module_lvgl.h"

#include <stdbool.h>

struct display_service_session_t {
    int active;
};

static bool s_booted;
static bool s_session_active;
static lv_display_t *s_display;
static lv_indev_t   *s_indev;
static struct display_service_session_t s_session;

void sim_display_service_boot(void)
{
    if (s_booted) return;

    s_display = sim_display_init();
    lv_display_set_default(s_display);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, sim_input_read_cb);
    lv_indev_set_display(s_indev, s_display);

    /* The launcher applies Lexend 32 as the display theme so every widget --
     * launcher and app alike -- gets it for free (launcher_main.c app_main). */
    /* Theme default follows the global font scale, so plain labels (no explicit
     * font) scale with everything else. */
    lv_display_set_theme(s_display,
        lv_theme_default_init(s_display,
                              lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED),
                              true /* dark */,
                              lua_module_lvgl_scaled_builtin_font(32)));
    s_booted = true;
}

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session)
{
    (void)config;
    if (!ret_session) return ESP_ERR_INVALID_ARG;
    if (!s_booted) return ESP_ERR_INVALID_STATE;
    if (s_session_active) return ESP_ERR_INVALID_STATE;

    s_session_active = true;
    s_session.active = 1;
    *ret_session = &s_session;
    return ESP_OK;
}

esp_err_t display_service_close(display_service_session_handle_t session)
{
    if (session != &s_session || !s_session_active) return ESP_ERR_INVALID_STATE;
    s_session_active = false;
    s_session.active = 0;
    return ESP_OK;
}

/* True once the display exists: gates the (no-op) display_service lock path in
 * the binding, so the lock/unlock pairs stay balanced. */
bool display_service_is_started(void) { return s_booted; }

esp_err_t display_service_lock(void)   { return ESP_OK; }  /* single-threaded */
void      display_service_unlock(void) { }

lv_display_t *display_service_session_display(display_service_session_handle_t session)
{
    return (session == &s_session) ? s_display : NULL;
}

esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session,
                                                     lv_obj_t *screen)
{
    if (session != &s_session || !s_session_active) return ESP_ERR_INVALID_STATE;
    if (!screen) return ESP_ERR_INVALID_ARG;
    lv_screen_load(screen);
    return ESP_OK;
}

void display_service_session_clear_screen(display_service_session_handle_t session)
{
    (void)session; /* nothing to restore in the sim */
}
