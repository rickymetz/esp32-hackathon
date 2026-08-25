#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "serial_push.h"
#include "app_registry.h"
#include "launcher_main.h"
#include "app_button.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_push";

/* Bumped when a command is added or a reply format changes, so a host
 * tool can tell whether this firmware speaks what it needs. Reported by
 * PING. */
#define LAUNCHER_PROTOCOL_VERSION "1"

/* NAME_MAX covers a full app id (APP_ID_MAX is 128): a shorter cap would let
 * the registry list and launch an app by tap that RUN/DELETE then reject as
 * "bad_name". LINE_MAX must hold the longest command line ("DELETE " + id). */
#define LINE_MAX     256
#define NAME_MAX     128
#define PAYLOAD_MAX (64 * 1024)

/* SHOT payload chunking -- see handle_shot(). A multiple of 3 encodes to
 * exactly 4/3 the bytes with no padding; +4 covers the NUL and rounding. */
#define SHOT_CHUNK   720
#define SHOT_B64_MAX (((SHOT_CHUNK + 2) / 3) * 4 + 4)

/* Stringify NAME_MAX for the sscanf field width, so the width can never drift
 * from the buffer size (name[NAME_MAX + 1]). */
#define STR2(x) #x
#define STR(x)  STR2(x)

/* None of these are a security boundary -- anyone with USB can already reflash
 * the board -- but they stop a stray path from writing somewhere surprising and
 * turning into a confusing afternoon. `..` and a leading '/' are always out. */
static bool has_dotdot_or_absolute(const char *name)
{
    return name[0] == '/' || strstr(name, "..") != NULL;
}

/* A RUN/DELETE target: an app id. A flat app's id is its "counter.lua" file
 * basename; a folder app's is its bare "mygame" directory name. Either way it
 * names one entry directly under apps/, so no '/' and no '..'. */
static bool id_is_safe(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX) {
        return false;
    }
    return !has_dotdot_or_absolute(name) && strchr(name, '/') == NULL;
}

/* A PUSH target: a path under apps/. Either a flat "counter.lua", or one
 * directory deep for a folder app ("mygame/main.lua", "mygame/icon.bin").
 * At most one '/', and it must have a non-empty folder and file on each side. */
static bool push_path_is_safe(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX || has_dotdot_or_absolute(name)) {
        return false;
    }
    const char *slash = strchr(name, '/');
    if (slash == NULL) {
        return true;   /* flat file directly under apps/ */
    }
    if (slash == name || slash[1] == '\0') {
        return false;  /* "/foo" or "foo/" -- need dir and file both */
    }
    return strchr(slash + 1, '/') == NULL;   /* only one level deep */
}

/* Whether `id` matches an app currently known to the registry. Used by
 * RUN to tell "not_found" apart from "already_running" without either
 * duplicating launcher_main's task-state tracking or widening the
 * launcher_run_app_by_name() API beyond the bool it was specified as. */
static bool registry_has_id(const char *id)
{
    app_entry_t app;
    return app_registry_find_by_id(id, &app);
}

/* True for a line that is almost certainly an orphaned PUSH body line rather
 * than a mistyped command: long, and made only of base64 characters.
 *
 * The length floor is load-bearing. Base64 is [A-Za-z0-9+/=], so a bare
 * alphabetic typo like "FROB" is *also* valid base64 -- testing the charset
 * alone would silently swallow exactly the typos the unknown-command reply
 * exists to make loud. tools/push.py emits 76-character lines (57 bytes per
 * line), so 32 sits far above any plausible hand-typed command and far below
 * a real body line. A short trailing chunk from a desynced stream can still
 * draw one reply; that is the acceptable side of the trade, and
 * drain_push_payload() means it should not arise in the first place. */
#define BODY_LINE_MIN 32

static bool looks_like_base64(const char *line)
{
    size_t n = 0;
    for (const char *p = line; *p; p++, n++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '+' || *p == '/' || *p == '=';
        if (!ok) {
            return false;
        }
    }
    return n >= BODY_LINE_MIN;
}

static bool read_line(char *out, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out[n] = '\0';
            return true;
        }
        out[n++] = (char)c;
    }
    out[cap - 1] = '\0';

    /* Oversized line: cap-1 bytes were consumed but the rest of this
     * physical line is still sitting in the stream. Drain and discard up
     * to the next '\n' (or EOF) so every call to read_line consumes exactly
     * one whole line and the next call starts at the next line, not mid-line. */
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n') {
            break;
        }
    }
    return false;
}

/* Swallow the body of a PUSH we are rejecting, up to and including ENDPUSH.
 *
 * Every early PUSH_ERR return used to leave the base64 body sitting in the
 * stream. That was invisible while unrecognised lines were silently dropped --
 * but the new unknown-command reply answers each one, so a rejected 64 KB push
 * (1150 lines at push.py's 57 bytes/line) produced ~1150 "ERR unknown_command
 * <base64>" replies, ~60 KB of console spew. push.py writes the whole body
 * before reading any reply, so this is the normal shape of a rejection, not a
 * corner case. A 4-character final chunk could even literally be "PING" and
 * draw a spurious PONG.
 *
 * Bounded so a malformed stream cannot pin the task here forever: PAYLOAD_MAX
 * base64 lines is already far more than any legal push. */
static void drain_push_payload(void)
{
    char line[LINE_MAX];
    for (unsigned i = 0; i < (PAYLOAD_MAX / 3) + 16; i++) {
        if (!read_line(line, sizeof(line))) {
            continue;   /* oversized line; read_line already resynced */
        }
        if (strcmp(line, "ENDPUSH") == 0) {
            return;
        }
    }
    ESP_LOGW(TAG, "drain: no ENDPUSH after a rejected push");
}

static void handle_push(const char *header)
{
    char name[NAME_MAX + 1];
    unsigned expect_len = 0, expect_crc = 0;

    if (sscanf(header, "PUSH %" STR(NAME_MAX) "s %u %x", name, &expect_len, &expect_crc) != 3) {
        drain_push_payload();
        printf("PUSH_ERR bad_header\n");
        return;
    }
    if (!push_path_is_safe(name)) {
        drain_push_payload();
        printf("PUSH_ERR bad_name\n");
        return;
    }
    if (expect_len == 0 || expect_len > PAYLOAD_MAX) {
        drain_push_payload();
        printf("PUSH_ERR bad_length\n");
        return;
    }

    uint8_t *buf = malloc(expect_len);
    if (buf == NULL) {
        drain_push_payload();
        printf("PUSH_ERR no_memory\n");
        return;
    }

    size_t got = 0;
    char line[LINE_MAX];
    bool ok = true;

    while (read_line(line, sizeof(line))) {
        if (strcmp(line, "ENDPUSH") == 0) {
            break;
        }
        size_t produced = 0;
        if (mbedtls_base64_decode(buf + got, expect_len - got, &produced,
                                  (const unsigned char *)line, strlen(line)) != 0) {
            ok = false;
            break;
        }
        got += produced;
    }

    if (!ok || got != expect_len) {
        free(buf);
        /* A decode failure `break`s out of the loop above with the rest of the
         * body still queued -- same storm as the early returns. A short push
         * that ended with ENDPUSH has nothing left and the drain returns on
         * its first line. */
        if (!ok) {
            drain_push_payload();
        }
        printf("PUSH_ERR truncated\n");
        return;
    }
    if (esp_crc32_le(0, buf, got) != expect_crc) {
        free(buf);
        printf("PUSH_ERR crc_mismatch\n");
        return;
    }

    /* The payload is fully received and CRC-verified -- now, and only now,
     * touch the SD card. app_registry_write_app() does the write (temp file
     * + rename) and the registry rescan under the same lock that
     * app_registry_invalidate() takes to unmount the card, so a concurrent
     * Refresh cannot pull the filesystem out from under this write. The
     * lock is held only for the write/rescan, never across the multi-second
     * base64 receive above. */
    uint32_t sig_before = app_registry_signature();
    bool wrote = app_registry_write_app(name, buf, got);
    free(buf);

    if (!wrote) {
        printf("PUSH_ERR write_failed\n");
        return;
    }

    /* The registry now knows about the new app, but the home screen was built
     * from the old list and would keep showing it until someone tapped Refresh
     * on the panel -- a physical step in the middle of an otherwise hands-off
     * push/run loop.
     *
     * Only when the app SET actually changed, though. push.py sends one PUSH
     * per file, so a folder app (main.lua + icon.bin + assets) would otherwise
     * do a full build-load-delete of the whole screen tree once per file, and
     * re-pushing an existing app -- the common case in an edit/push/run loop --
     * would rebuild for no visible change while resetting the user's scroll
     * position. The signature covers ids, not contents; see its docs.
     *
     * The rebuild itself is asynchronous and deferred if the launcher is not on
     * screen; see launcher_refresh_ui(). */
    if (app_registry_signature() != sig_before) {
        launcher_refresh_ui();
    }

    ESP_LOGI(TAG, "received %s (%u bytes)", name, (unsigned)got);
    printf("PUSH_OK %s\n", name);
}

/* RUN <basename.lua> -- launch an app exactly as tapping its row does.
 * Useful when nobody is at the board to touch the screen. */
static void handle_run(const char *header)
{
    char name[NAME_MAX + 1];

    if (sscanf(header, "RUN %" STR(NAME_MAX) "s", name) != 1) {
        printf("RUN_ERR bad_name\n");
        return;
    }
    if (!id_is_safe(name)) {
        printf("RUN_ERR bad_name\n");
        return;
    }
    if (!registry_has_id(name)) {
        printf("RUN_ERR not_found\n");
        return;
    }
    if (!launcher_run_app_by_name(name)) {
        printf("RUN_ERR already_running\n");
        return;
    }

    ESP_LOGI(TAG, "RUN %s", name);
    printf("RUN_OK %s\n", name);
}

/* LIST -- print every app the registry knows, one basename per line,
 * then a LIST_OK trailer with the count. Copies each entry out of the
 * registry (never holds the lock across printf). */
static void handle_list(void)
{
    size_t n = 0;

    for (size_t i = 0; i < APP_MAX_COUNT; i++) {
        app_entry_t app;
        if (!app_registry_get_copy(i, &app)) {
            break;
        }
        printf("APP %s\n", app.id);   /* the id RUN/DELETE accept, folder or flat */
        n++;
    }
    printf("LIST_OK %u\n", (unsigned)n);
}

/* DELETE <basename.lua> -- remove an app file from the card. Goes through
 * app_registry_delete_app(), which holds the registry lock across the
 * unlink and rescans before returning -- never a bare unlink, so a
 * concurrent Refresh cannot unmount the card mid-delete. */
static void handle_delete(const char *header)
{
    char name[NAME_MAX + 1];

    if (sscanf(header, "DELETE %" STR(NAME_MAX) "s", name) != 1 || !id_is_safe(name)) {
        printf("DELETE_ERR bad_name\n");
        return;
    }
    if (!registry_has_id(name)) {
        printf("DELETE_ERR not_found\n");
        return;
    }
    if (app_registry_is_builtin(name)) {
        /* Distinct from delete_failed: this one is never going to succeed, and
         * a tool should say "built in" rather than retry. */
        printf("DELETE_ERR builtin\n");
        return;
    }
    if (!app_registry_delete_app(name)) {
        printf("DELETE_ERR delete_failed\n");
        return;
    }

    launcher_refresh_ui();   /* same staleness as PUSH -- see handle_push() */

    ESP_LOGI(TAG, "DELETE %s", name);
    printf("DELETE_OK %s\n", name);
}

/* SHOT -- re-render the active screen into a PSRAM buffer with
 * lv_snapshot and stream it out base64, so an agent can SEE the UI
 * without a human photographing the panel. Rick's request after a day
 * of design tuning over photos.
 *
 * Format: "SHOT <w> <h> <stride>" then base64 lines then "ENDSHOT".
 * Pixel data is RGB565 little-endian, h*stride bytes. The snapshot runs
 * under the display lock; streaming happens after it is released, from
 * our own copy of nothing -- the draw buf stays valid because only this
 * task frees it, and LVGL never touches snapshot bufs it did not make. */
static void handle_shot(void)
{
    bsp_display_lock(0);
    lv_obj_t *act = lv_screen_active();
    lv_draw_buf_t *buf = act ? lv_snapshot_take(act, LV_COLOR_FORMAT_RGB565) : NULL;
    bsp_display_unlock();

    if (buf == NULL) {
        printf("SHOT_ERR failed\n");
        return;
    }

    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    size_t total = (size_t)h * stride;
    printf("SHOT %u %u %u\n", (unsigned)w, (unsigned)h, (unsigned)stride);

    /* 720 rather than the original 90: a full 368x448 frame is 329,728 bytes,
     * so this is 458 fputs/fputc pairs instead of 3,664. Worth ~6% (1.63 s ->
     * 1.54 s round trip, measured) and no more -- see the note on the real
     * bottleneck above serial_push_start(). Kept a multiple of 3 so every line
     * but the last encodes without base64 padding, and well within the host's
     * line reader, which has no length limit. */
    /* static, not a stack local: at SHOT_CHUNK=720 this is ~1 KB, and putting
     * it on serial_push's stack overflowed the task outright (verified --
     * "***ERROR*** A stack overflow in task serial_push"). Safe as a static
     * because handle_shot() is only ever called from serial_push_task, which
     * is the single task servicing this protocol. */
    const unsigned char *d = buf->data;
    static unsigned char line[SHOT_B64_MAX];
    for (size_t off = 0; off < total; off += SHOT_CHUNK) {
        size_t chunk = total - off < SHOT_CHUNK ? total - off : SHOT_CHUNK;
        size_t olen = 0;
        if (mbedtls_base64_encode(line, sizeof(line), &olen, d + off, chunk) != 0) {
            break;
        }
        line[olen] = '\0';
        fputs((const char *)line, stdout);
        fputc('\n', stdout);
    }
    printf("ENDSHOT\n");

    bsp_display_lock(0);
    lv_draw_buf_destroy(buf);
    bsp_display_unlock();
}

/* MEM -- report free heap without disturbing the running app, so a
 * long soak can watch for slow leaks. Largest-contiguous matters as
 * much as the total: fragmentation fails allocations while plenty is
 * nominally free. */
static void handle_mem(void)
{
    printf("MEM %u %u %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* ---- STATS: the performance snapshot MEM cannot give ------------------
 *
 * MEM above is deliberately left exactly as it was -- tools/soak.py and
 * tools/chaos.py parse its three fields positionally, and widening the
 * reply would break both. STATS is additive.
 *
 * Three things MEM cannot tell you, all of which perf work needs:
 *
 *   1. The heap LOW-WATER mark. MEM samples the instant you ask, so a run
 *      that came within a few KB of the floor and recovered reads exactly
 *      like one that never got close. heap_caps_get_minimum_free_size()
 *      is the number that actually bounds headroom.
 *   2. Per-task CPU. Reported as permille of the interval SINCE THE LAST
 *      STATS CALL, not since boot -- a since-boot average buries exactly
 *      the spike you are hunting. Call it twice around the thing you want
 *      to measure.
 *   3. Per-task stack headroom, which is the only evidence for whether
 *      APP_TASK_STACK (32 KB) is right rather than merely untested.
 *
 * One field per line so a parser never counts columns:
 *
 *   STATS_BEGIN
 *   STAT uptime_ms <n>
 *   STAT psram free <n> min <n>
 *   STAT internal free <n> min <n> largest <n>
 *   STAT task <name> cpu_permille <n> stack_free <n>
 *   STATS_END
 */

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && \
    defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)
#define STATS_HAVE_CPU 1
#else
#define STATS_HAVE_CPU 0
#endif

#if STATS_HAVE_CPU
/* Bounded so the snapshot never allocates unboundedly from a serial
 * command. The launcher runs well under 30 tasks; extra ones are dropped
 * from the report rather than growing the buffer. */
#define STATS_MAX_TASKS 40

typedef struct {
    TaskHandle_t handle;
    uint32_t     run_time;
} stats_prev_t;

static stats_prev_t s_stats_prev[STATS_MAX_TASKS];
static int          s_stats_prev_n;
static uint32_t     s_stats_prev_total;
#endif

static void handle_stats(void)
{
    printf("STATS_BEGIN\n");
    printf("STAT uptime_ms %llu\n",
           (unsigned long long)(esp_timer_get_time() / 1000));

    printf("STAT psram free %u min %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    printf("STAT internal free %u min %u largest %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

#if STATS_HAVE_CPU
    UBaseType_t want = uxTaskGetNumberOfTasks();
    if (want > STATS_MAX_TASKS) {
        want = STATS_MAX_TASKS;
    }
    TaskStatus_t *st = calloc(want, sizeof(TaskStatus_t));
    if (st == NULL) {
        printf("STAT cpu alloc_failed\n");
    } else {
        uint32_t total = 0;
        UBaseType_t n = uxTaskGetSystemState(st, want, &total);

        /* Both cores contribute to the total, so the permille column sums
         * to ~2000 across a dual-core snapshot (idle tasks included), not
         * 1000. Per-task values are still directly comparable. */
        uint32_t dtotal = total - s_stats_prev_total;

        for (UBaseType_t i = 0; i < n; i++) {
            uint32_t prev = 0;
            for (int j = 0; j < s_stats_prev_n; j++) {
                if (s_stats_prev[j].handle == st[i].xHandle) {
                    prev = s_stats_prev[j].run_time;
                    break;
                }
            }
            uint32_t delta = st[i].ulRunTimeCounter - prev;
            unsigned permille = dtotal ?
                (unsigned)((uint64_t)delta * 1000u / dtotal) : 0u;
            /* usStackHighWaterMark is in StackType_t units, and ESP-IDF
             * defines StackType_t as uint8_t -- so this is bytes, not
             * words as on stock FreeRTOS. */
            printf("STAT task %s cpu_permille %u stack_free %u\n",
                   st[i].pcTaskName, permille,
                   (unsigned)st[i].usStackHighWaterMark);
        }

        s_stats_prev_n = (int)n;
        for (int i = 0; i < s_stats_prev_n; i++) {
            s_stats_prev[i].handle   = st[i].xHandle;
            s_stats_prev[i].run_time = st[i].ulRunTimeCounter;
        }
        s_stats_prev_total = total;
        free(st);
    }
#else
    printf("STAT cpu unavailable "
           "(rebuild with CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)\n");
#endif

    printf("STATS_END\n");
}

/* PWR -- inject a synthetic PWR press+release through the same edge
 * recorder the poller uses, so app button flows are drivable from the
 * harness (the physical button obviously is not). */
static void handle_pwr(void)
{
    app_button_record_edge(true);
    vTaskDelay(pdMS_TO_TICKS(60));
    app_button_record_edge(false);
    printf("PWR_OK\n");
}

/* BOOT -- inject a synthetic BOOT press through the same handler the poller
 * calls, so the shell's only navigation control is drivable from the harness.
 * The physical button obviously is not, and that gap is why the three-way
 * toggle went unverified for so long. */
static void handle_boot(void)
{
    launcher_boot_press();
    printf("BOOT_OK\n");
}

/* TAP <x> <y> / SWIPE <x0> <y0> <x1> <y1> [ms] -- synthetic touch through
 * launcher_input_inject(). With SHOT this closes the agent's loop: look,
 * tap, look again, no human at the panel. */
static void handle_tap(const char *header)
{
    int x = 0, y = 0;

    if (sscanf(header, "TAP %d %d", &x, &y) != 2) {
        printf("TAP_ERR bad_args\n");
        return;
    }
    launcher_input_inject(x, y, x, y, 80);
    printf("TAP_OK\n");
}

static void handle_swipe(const char *header)
{
    int x0, y0, x1, y1, ms = 250;

    if (sscanf(header, "SWIPE %d %d %d %d %d", &x0, &y0, &x1, &y1, &ms) < 4) {
        printf("SWIPE_ERR bad_args\n");
        return;
    }
    launcher_input_inject(x0, y0, x1, y1, ms);
    printf("SWIPE_OK\n");
}

/* STOP -- ask the running app to stop, exactly as pressing BOOT does. */
/* BRIGHT <pct> -- set panel brightness directly, 0-100.
 *
 * Exists because the screen timeout is otherwise miserable to test: the blank
 * step is two minutes away, and SHOT cannot see brightness at all (it renders
 * the LVGL framebuffer, not the panel). This makes the whole ladder reachable
 * in seconds, and it is four lines.
 *
 * bsp_display_brightness_set() validates 0-100 itself and returns
 * ESP_ERR_INVALID_ARG outside it, so the reply carries that rather than
 * duplicating the check here. */
static void handle_bright(const char *header)
{
    int pct = -1;

    if (sscanf(header, "BRIGHT %d", &pct) != 1) {
        printf("BRIGHT_ERR bad_args\n");
        return;
    }
    esp_err_t err = bsp_display_brightness_set(pct);
    printf("BRIGHT_OK %d err=%s\n", pct, esp_err_to_name(err));
}

static void handle_stop(void)
{
    if (!launcher_stop_app()) {
        printf("STOP_ERR not_running\n");
        return;
    }

    ESP_LOGI(TAG, "STOP");
    printf("STOP_OK\n");
}

static void serial_push_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];

    for (;;) {
        if (!read_line(line, sizeof(line))) {
            continue;
        }
        if (strncmp(line, "PUSH ", 5) == 0) {
            handle_push(line);
        } else if (strncmp(line, "RUN ", 4) == 0) {
            handle_run(line);
        } else if (strcmp(line, "STOP") == 0) {
            handle_stop();
        } else if (strcmp(line, "LIST") == 0) {
            handle_list();
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            handle_delete(line);
        } else if (strcmp(line, "SHOT") == 0) {
            handle_shot();
        } else if (strncmp(line, "TAP ", 4) == 0) {
            handle_tap(line);
        } else if (strncmp(line, "SWIPE ", 6) == 0) {
            handle_swipe(line);
        } else if (strcmp(line, "PWR") == 0) {
            handle_pwr();
        } else if (strcmp(line, "BOOT") == 0) {
            handle_boot();
        } else if (strcmp(line, "MEM") == 0) {
            handle_mem();
        } else if (strncmp(line, "BRIGHT ", 7) == 0) {
            handle_bright(line);
        } else if (strcmp(line, "STATS") == 0) {
            handle_stats();
        } else if (strcmp(line, "PING") == 0) {
            /* Identifies this port as the launcher. tools/ pick the first
             * /dev/cu.usbmodem* and hope; with this they can confirm what
             * answered, and the version tells a host tool whether the
             * firmware predates a command it wants to use. */
            printf("PONG launcher %s lvgl %d.%d.%d\n", LAUNCHER_PROTOCOL_VERSION,
                   LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
        } else if (line[0] != '\0' && !looks_like_base64(line)) {
            /* Anything else got silently dropped, so a typo'd command left the
             * host waiting out its full timeout with no clue why. Echo back
             * the first whitespace-delimited token.
             *
             * Two things this deliberately does NOT do:
             *
             * - It does not echo the whole line. Note the token is only the
             *   first SPACE-delimited word, so a space-free argument (a path)
             *   still lands in it -- the truncation to 31 bytes is the real
             *   bound, not the tokenizer. Do not describe this as "never
             *   echoes a path"; it can.
             * - It does not answer a line that looks like base64. A rejected
             *   PUSH is drained now (see drain_push_payload), but a stream
             *   desynced some other way would otherwise turn every orphaned
             *   body line into a reply. Belt and braces, cheap. */
            char verb[32];
            size_t i = 0;
            while (i + 1 < sizeof(verb) && line[i] != '\0' && line[i] != ' ') {
                /* Printable ASCII only. The raw byte could be ESC, and the
                 * docs tell people to watch this console with idf.py monitor
                 * or screen -- both of which would act on an escape sequence
                 * echoed back at them. */
                unsigned char c = (unsigned char)line[i];
                verb[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
                i++;
            }
            verb[i] = '\0';
            printf("ERR unknown_command %s\n", verb);
        }
    }
}

/* Why SHOT still costs ~1.5 s, and what does NOT fix it.
 *
 * Measured breakdown of one 368x448 capture: 1.445 s to get the base64 to the
 * host, 0.016 s to build the host's decode table, 0.005 s to convert pixels.
 * The transfer is ~94% of it, at roughly 306 KB/s.
 *
 * That is not per-line overhead. With CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG and
 * no driver installed, usb_serial_jtag_write() (ESP-IDF
 * esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c) loops
 * `s_ctx.tx_func(fd, c)` one character at a time straight at the peripheral
 * registers. The cost is per BYTE, which is why raising the base64 chunk size
 * in handle_shot() bought only ~6%.
 *
 * Installing the usb_serial_jtag driver and calling
 * usb_serial_jtag_vfs_use_driver() looks like the obvious fix -- buffered,
 * USB-sized packets instead of a register poll per byte. It was tried on this
 * board and it is 4-5x WORSE: the same capture went from 1.5 s to 7-10 s. That
 * is a measured dead end, not an untried idea; do not re-attempt it without
 * measuring first.
 *
 * If SHOT throughput ever matters enough to spend on, the untried directions
 * are framing raw bytes with an escape scheme instead of base64 (-25% payload)
 * or compressing the frame on-device before encoding. */
void serial_push_start(void)
{
    /* 8192, not 6144. The SHOT stack overflow above showed the old margin was
     * under 832 bytes -- closer to the edge than anyone had measured, since
     * STATS samples the high-water mark and nothing had run STATS immediately
     * after a SHOT. Check it with `STATS` right after a SHOT (the deepest
     * path here) rather than trusting this number. */
    xTaskCreate(serial_push_task, "serial_push", 8192, NULL, 4, NULL);
}
