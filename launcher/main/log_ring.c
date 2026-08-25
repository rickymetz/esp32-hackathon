#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#include "log_ring.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

/* 32 KB in PSRAM. Internal DRAM is the scarce resource here (largest
 * contiguous block measures ~73 KB), and this is a debugging convenience --
 * it has no business competing for that. 32 KB is a few hundred log lines,
 * which comfortably spans an app launch, its failure and the exit path. */
#define RING_CAP (32 * 1024)

/* One line's formatted length. Longer lines are truncated in the RING only;
 * the console still gets the whole thing, because the real vprintf is handed
 * the untouched format and args. */
#define LINE_MAX_LOCAL 256

static char   *s_buf;
static size_t  s_head;      /* next write offset */
static bool    s_wrapped;   /* true once s_head has lapped the buffer */
static bool    s_dumping;   /* suppress capture while printing the ring */
static vprintf_like_t s_prev;

/* A spinlock rather than a mutex: ESP_LOG is called from many tasks and the
 * critical section here is a bounded memcpy of at most LINE_MAX_LOCAL bytes,
 * with no allocation and no blocking call inside it. Formatting happens
 * OUTSIDE the lock, which is the part that must not be held long. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void ring_append(const char *src, size_t len)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head++] = src[i];
        if (s_head == RING_CAP) {
            s_head = 0;
            s_wrapped = true;
        }
    }
    portEXIT_CRITICAL(&s_mux);
}

static int log_hook(const char *fmt, va_list ap)
{
    /* va_copy because the real vprintf below consumes `ap`, and a va_list may
     * not be walked twice. */
    if (s_buf != NULL && !s_dumping) {
        char line[LINE_MAX_LOCAL];
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(line, sizeof(line), fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            size_t len = (n < (int)sizeof(line)) ? (size_t)n : sizeof(line) - 1;
            ring_append(line, len);
        }
    }
    return s_prev ? s_prev(fmt, ap) : 0;
}

void log_ring_puts(const char *data, size_t len)
{
    if (s_buf == NULL || s_dumping || data == NULL || len == 0) {
        return;
    }
    ring_append(data, len);
}

void log_ring_init(void)
{
    if (s_buf != NULL) {
        return;
    }
    s_buf = heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        /* Deliberately not fatal and not even an error: a board that cannot
         * spare 32 KB of PSRAM should still boot and run apps. */
        ESP_LOGW("log_ring", "no PSRAM for the log ring -- capture disabled");
        return;
    }
    s_prev = esp_log_set_vprintf(log_hook);
}

size_t log_ring_used(void)
{
    if (s_buf == NULL) {
        return 0;
    }
    return s_wrapped ? RING_CAP : s_head;
}

void log_ring_dump(void)
{
    if (s_buf == NULL) {
        printf("LOG_ERR no_buffer\n");
        return;
    }

    /* Snapshot the bounds under the lock, then print outside it: printing is
     * slow (it goes out of the USB CDC) and must not be done with a spinlock
     * held. Lines logged during the dump are lost from the ring rather than
     * interleaved, which is the right trade for a debugging dump. */
    portENTER_CRITICAL(&s_mux);
    s_dumping = true;
    size_t head = s_head;
    bool wrapped = s_wrapped;
    portEXIT_CRITICAL(&s_mux);

    size_t total = wrapped ? RING_CAP : head;
    printf("LOG_BEGIN %u\n", (unsigned)total);

    if (wrapped) {
        /* Oldest data sits just past the head. */
        fwrite(s_buf + head, 1, RING_CAP - head, stdout);
    }
    fwrite(s_buf, 1, head, stdout);
    fflush(stdout);

    printf("\nLOG_END\n");
    s_dumping = false;
}
