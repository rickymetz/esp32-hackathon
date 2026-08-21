/*
 * Minimal display_service implementation, backed by the Waveshare BSP.
 *
 * The vendored lua_module_lvgl (from espressif/esp-claw) talks to the display
 * through this service rather than to LVGL directly. esp-claw's own
 * implementation is ~1100 LOC and manages display ownership across a whole OS;
 * we only need the seven entry points the LVGL module actually calls, mapped
 * onto bsp_display_lock()/unlock() and the BSP's lv_display_t.
 *
 * Ownership model, which is what makes the launcher safe: one session at a
 * time owns the screen. Closing a session deletes the screen it loaded, so an
 * app cannot leak widgets into the launcher's UI.
 */

#include <string.h>
#include <stdlib.h>
#include "display_service.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "display_service";

struct display_service_session_t {
    char owner_name[DISPLAY_SERVICE_OWNER_NAME_LEN];
    display_service_mode_t mode;
    uint32_t flags;
    display_service_session_cleanup_cb_t cleanup_cb;
    void *user_ctx;
    lv_obj_t *screen;       /* screen this session loaded, deleted on close */
    lv_obj_t *prev_screen;  /* screen to restore on close */
    bool active;
};

static lv_display_t *s_display;
static struct display_service_session_t *s_session;  /* single active session */

void display_service_attach(lv_display_t *disp)
{
    s_display = disp;
}

bool display_service_is_started(void)
{
    return s_display != NULL;
}

esp_err_t display_service_lock(void)
{
    /* 0 == wait forever in esp_lvgl_port. */
    return bsp_display_lock(0) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_service_unlock(void)
{
    bsp_display_unlock();
}

esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session)
{
    if (config == NULL || ret_session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_display == NULL) {
        ESP_LOGE(TAG, "display not attached");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session != NULL) {
        ESP_LOGE(TAG, "session already open (owner '%s')", s_session->owner_name);
        return ESP_ERR_INVALID_STATE;
    }

    struct display_service_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(s->owner_name, sizeof(s->owner_name), "%s",
             config->owner_name ? config->owner_name : "app");
    s->mode       = config->mode;
    s->flags      = config->flags;
    s->cleanup_cb = config->cleanup_cb;
    s->user_ctx   = config->user_ctx;
    s->active     = true;

    if (display_service_lock() == ESP_OK) {
        s->prev_screen = lv_display_get_screen_active(s_display);
        display_service_unlock();
    }

    s_session = s;
    *ret_session = s;
    ESP_LOGI(TAG, "session opened for '%s'", s->owner_name);
    return ESP_OK;
}

esp_err_t display_service_close(display_service_session_handle_t session)
{
    if (session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Take the LVGL lock for the WHOLE teardown, including the cleanup
     * callback. The callback deletes the app's LVGL objects, and the LVGL
     * port task is concurrently running lv_timer_handler on the other core.
     * LVGL is built without LV_USE_OS, so it does no internal locking --
     * calling the callback outside the lock is a data race on the object
     * tree, on every single app exit.
     *
     * The lock is recursive, so the callback re-acquiring it is fine. */
    esp_err_t locked = display_service_lock();

    if (session->cleanup_cb) {
        session->cleanup_cb(session, session->user_ctx);
    }

    /* Restore the previous screen and delete the app's, which takes every
     * widget the app parented to it with it.
     *
     * session->screen may already have been deleted by cleanup_cb above
     * (lua_lvgl_session_cleanup_cb deletes the runtime's root screen, which
     * is the same lv_obj_t) -- cleanup_cb clears session->screen once it
     * has, but guard with lv_obj_is_valid() too rather than trust that
     * alone, since this pointer must never be handed to lv_obj_delete()
     * twice. Without both, this was a use-after-free/double-delete masked
     * only by LVGL 9's is_deleting bit surviving in freed memory by luck. */
    if (session->prev_screen != NULL) {
        lv_screen_load(session->prev_screen);
    }
    if (session->screen != NULL && session->screen != session->prev_screen &&
            lv_obj_is_valid(session->screen)) {
        lv_obj_delete(session->screen);
    }
    session->screen = NULL;

    if (locked == ESP_OK) {
        display_service_unlock();
    }

    ESP_LOGI(TAG, "session closed for '%s'", session->owner_name);
    if (s_session == session) {
        s_session = NULL;
    }
    free(session);
    return ESP_OK;
}

bool display_service_session_is_valid(display_service_session_handle_t session)
{
    return session != NULL && session == s_session;
}

bool display_service_session_is_active(display_service_session_handle_t session)
{
    return display_service_session_is_valid(session) && session->active;
}

display_service_mode_t display_service_session_mode(display_service_session_handle_t session)
{
    return session ? session->mode : DISPLAY_SERVICE_MODE_SHARED_LVGL;
}

const char *display_service_session_owner_name(display_service_session_handle_t session)
{
    return session ? session->owner_name : NULL;
}

lv_display_t *display_service_session_display(display_service_session_handle_t session)
{
    (void)session;
    return s_display;
}

/* Caller already holds the display lock. */
esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session,
                                                     lv_obj_t *screen)
{
    if (session == NULL || screen == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    session->screen = screen;
    lv_screen_load(screen);
    return ESP_OK;
}

void display_service_session_clear_screen(display_service_session_handle_t session)
{
    if (session != NULL) {
        session->screen = NULL;
    }
}

esp_err_t display_service_session_load_screen(display_service_session_handle_t session,
                                              lv_obj_t *screen)
{
    esp_err_t err = display_service_lock();
    if (err != ESP_OK) {
        return err;
    }
    err = display_service_session_load_screen_locked(session, screen);
    display_service_unlock();
    return err;
}
