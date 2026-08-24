/*
 * Launcher: BSP + LVGL + Lua.
 *
 * Lists the Lua apps found on the SD card and runs the one you tap. Each app
 * gets its own Lua VM on its own task; the launcher keeps its own screen and
 * restores it when the app stops.
 *
 * Three things here are non-obvious, all established by testing on hardware:
 *
 * 1. bsp_display_start() alone leaves the panel dark. BSP_LCD_RST /
 *    BSP_LCD_TOUCH_RST / BSP_LCD_BACKLIGHT are all GPIO_NUM_NC on this board --
 *    the reset lines hang off the TCA9554 IO expander, which the BSP never
 *    initialises, so the panel sits held in reset with no error reported.
 *
 * 2. Lua event callbacks only queue. The LVGL event trampoline in
 *    lua_module_lvgl enqueues; something must call lvgl.process_events() to
 *    drain it. The launcher pumps it so apps stay declarative.
 *
 * 3. That pump needs a positive timeout AND a yield. A zero timeout returns
 *    immediately and starves the idle task into a watchdog reset.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_io_expander.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cap_lua.h"
#include "display_service.h"
#include "lua_module_lvgl.h"
#include "app_registry.h"
#include "launcher_home.h"
#include "launcher_face.h"
#include "app_sandbox.h"
#include "app_timer.h"
#include "app_button.h"
#include "lv_font_lexend.h"
#include "lua_module_ui.h"
#include "lua_module_store.h"
#include "lua_module_prefs.h"
#include <sys/stat.h>
#include "app_voice.h"
#include "app_sensors.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "driver/gpio.h"
#include "launcher_main.h"
#include "serial_push.h"

static const char *TAG = "launcher";

/* Expander lines, from the vendor's reference sketch.
 * EXIO1 / EXIO2 = LCD and touch reset. EXIO4 = PWR button, EXIO5 = PMU IRQ. */
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_2
#define EXIO_PWR_BTN    IO_EXPANDER_PIN_NUM_4
#define EXIO_PMU_IRQ    IO_EXPANDER_PIN_NUM_5

#define APP_TASK_STACK  (32 * 1024)
#define EVENT_PUMP_MS   100

/* Touch on this panel is not pixel-accurate, so small targets get missed.
 * Verified on hardware: a 240x120 button catches every tap where a 180x56
 * one dropped roughly half. Both watchOS (44pt) and Wear OS (48dp) land on
 * 88-96px at this panel's 2x scale; 104px is Wear OS's standard Chip height
 * and comfortable here -- see docs/DESIGN_GUIDE.md. Keep launcher rows at
 * least this tall. */
#define ROW_HEIGHT      104

/* I3: caps rows actually rendered, independent of APP_MAX_COUNT (32). Only
 * Bounds refresh_clicked()'s peak widget count (it builds the new screen
 * before deleting the old one, so peak is ~2x one screen's rows). The old
 * cap of 16 guarded a fixed internal LVGL pool that no longer exists --
 * ff9acc2 moved LVGL's heap to PSRAM, where 2x64 rows is noise -- and it
 * ended up hiding real apps behind a "more not shown" label on a card with
 * 23. The list scrolls; 64 is a pathological-card backstop, not a UI
 * limit. A truncated list still says so rather than silently hiding apps. */
#define MAX_VISIBLE_ROWS 64

static lv_obj_t *s_launcher_screen;

/* ---- Shell navigation ---------------------------------------------------
 * Home is the watch face, not the app list: a watch shows you the time and
 * makes apps somewhere you navigate to. BOOT is the only navigation control
 * and it is a three-way toggle:
 *
 *     running an app  ->  home      (the app is stopped first)
 *     home (face)     ->  app list
 *     app list        ->  home
 *
 * That keeps the escape guarantee BOOT has always carried -- it is hardware,
 * no app can consume it, so a misbehaving app is always escapable -- while
 * making the face, not the launcher, the place you land.
 *
 * s_shell_view tracks which of the two shell surfaces is up. It is only
 * meaningful when no app is running; the app-exit path always returns to the
 * face, so it is set there rather than inferred. */
typedef enum {
    SHELL_VIEW_FACE = 0,
    SHELL_VIEW_APPS = 1,
} shell_view_t;

static shell_view_t s_shell_view = SHELL_VIEW_FACE;
/* Defined with the other shell surfaces below; declared here because
 * lua_app_task's exit path (above them) returns to the face. */
static void show_face_screen(void);
static void show_apps_screen(void);
static lv_obj_t    *s_face_screen;
static lv_timer_t  *s_face_tick;
static lv_display_t *s_disp;   /* for re-applying the theme after app exit */
static launcher_view_t s_view_mode = LAUNCHER_VIEW_LIST;   /* list <-> grid */
static void apply_persisted_font_scale(lv_display_t *disp)
{
    /* NVS, not the card: the UI scale is a device setting and must apply with
     * no card in the slot -- the same reason the face style and timezone live
     * there. Settings writes "font_pct" through the `prefs` Lua module, which
     * shares this NVS namespace. Absent or out-of-range leaves the default. */
    int32_t pct = shell_nvs_get_i32("font_pct", 0);
    if (pct >= 70 && pct <= 130) {
        lua_module_lvgl_set_font_scale((float)pct / 100.0f);
    }

    bsp_display_lock(0);
    lv_display_set_theme(disp,
        lv_theme_default_init(disp,
                              lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED),
                              true /* dark */,
                              lua_module_lvgl_scaled_builtin_font(32)));
    bsp_display_unlock();
}

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d / %s\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LUA_RELEASE);

    /* Created before anything that can launch an app (UI build, serial
     * task): app_row_clicked, launcher_run_app_by_name, launcher_stop_app,
     * and lua_app_task's own exit path all take this. */
    s_app_mutex = xSemaphoreCreateMutex();
    if (s_app_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create app mutex");
        return;
    }

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    s_disp = disp;
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    /* Lexend as the theme default font. LVGL's Kconfig LV_FONT_DEFAULT
     * choice only offers its bundled faces, so the swap happens here via
     * the theme instead: every widget on this display -- launcher and app
     * screens alike -- resolves Lexend 32 unless a style overrides it.
     * LV_FONT_DEFAULT stays Montserrat 14 (sdkconfig) purely so the macro
     * still resolves; nothing renders it. Locked: the LVGL task is already
     * live once bsp_display_start() returns. */
    bsp_display_lock(0);
    lv_display_set_theme(disp,
        lv_theme_default_init(disp,
                              lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED),
                              true /* dark */,
                              /* Theme default follows the global font scale
                               * (default 1.0); apply_persisted_font_scale()
                               * re-themes once the SD's saved value is read. */
                              lua_module_lvgl_scaled_builtin_font(32)));
    bsp_display_unlock();

    /* Synthetic touch indev for serial TAP/SWIPE -- same event pipeline
     * as the real digitizer, so widgets cannot tell the difference. */
    bsp_display_lock(0);
    {
        lv_indev_t *synth = lv_indev_create();
        lv_indev_set_type(synth, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(synth, synth_indev_read);
    }
    bsp_display_unlock();

    /* Hand the BSP display to the service the Lua LVGL binding talks to. */
    display_service_attach(disp);
    ESP_ERROR_CHECK(lua_module_lvgl_register_with_data_root(BSP_SD_MOUNT_POINT));
    /* Register the D: card filesystem now so the home screen can load app
     * icons (D:/apps/<id>/icon.bin) before any app runs its own lvgl.init(). */
    ESP_ERROR_CHECK(lua_module_lvgl_register_fs());
    ESP_ERROR_CHECK(app_timer_register());
    ESP_ERROR_CHECK(app_button_register());
    /* Order matters: cap_lua opens modules in registration order, and
     * ui.lua/keyboard.lua require() lvgl, timer, and voice at load --
     * so voice must register BEFORE the embedded-Lua modules. */
    ESP_ERROR_CHECK(app_audio_register());   /* touches no hardware */
    ESP_ERROR_CHECK(app_voice_register());
    ESP_ERROR_CHECK(app_sensors_register());
    ESP_ERROR_CHECK(app_wifi_register());
    ESP_ERROR_CHECK(lua_module_store_register());
    ESP_ERROR_CHECK(lua_module_prefs_register());   /* no deps; order is free */
    ESP_ERROR_CHECK(lua_module_ui_register());

    /* BOOT (GPIO0) is the Home button. Input + pull-up matches its idle
     * state; it is only special during reset, where the ROM samples it as a
     * strapping pin -- configuring it as an input afterwards is free. */
    {
        const gpio_config_t boot_cfg = {
            .pin_bit_mask = 1ULL << GPIO_NUM_0,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&boot_cfg));
    }

    app_registry_scan();
    apply_persisted_font_scale(disp);   /* after SD mount, before UI build */
    /* Boot to the watch face, not the app list: home is the watch. The app
     * list is one BOOT press away (see the shell navigation notes above).
     * The style is whatever was last swiped to; an out-of-range or absent
     * value falls back to the digital face, which is also the fallback for a
     * failed home app. */
    {
        int32_t saved = shell_nvs_get_i32(SHELL_NVS_FACE, LAUNCHER_FACE_DIGITAL);
        if (saved < 0 || saved >= LAUNCHER_FACE_COUNT) {
            saved = LAUNCHER_FACE_DIGITAL;
        }
        s_face_style = (launcher_face_style_t)saved;
    }
    show_face_screen();
    xTaskCreate(button_poll_task, "buttons", 3072, NULL, 6, NULL);
    serial_push_start();

    /* Last, and deliberately after the UI is up: a network that is slow,
     * absent or misconfigured must never delay the launcher appearing.
     * Non-blocking -- it returns immediately whatever the radio does. */
    app_wifi_autostart();

    /* Structural proof the theme font actually took (no board display to
     * look at from here). Advisor review caught that logging
     * lv_font_lexend_32's own line_height proves nothing about the THEME --
     * and the row-label check sets its font explicitly, so it doesn't
     * either. This probes an UNSTYLED label: whatever font it resolves is
     * what the theme hands every widget. Want lexend_32 (line_height 36);
     * montserrat_14 (16) here means the theme init did not take. */
    bsp_display_lock(0);
    {
        lv_obj_t *probe = lv_label_create(lv_screen_active());
        const lv_font_t *resolved = lv_obj_get_style_text_font(probe, LV_PART_MAIN);
        /* Expectation tracks the font scale (the PR review caught the
         * probe reporting a mismatch on every healthy boot after the
         * scale landed). */
        const lv_font_t *want = lua_module_lvgl_scaled_builtin_font(32);
        ESP_LOGI(TAG, "theme probe: resolved=%p want=%p line_height=%d (want %d)",
                 (const void *)resolved, (const void *)want,
                 (int)lv_font_get_line_height(resolved),
                 (int)lv_font_get_line_height(want));
        lv_obj_delete(probe);
    }
    bsp_display_unlock();

    ESP_LOGI(TAG, "ready: %u app(s), internal free=%u psram free=%u",
             (unsigned)app_registry_count(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
