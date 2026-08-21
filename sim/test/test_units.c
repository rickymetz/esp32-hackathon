/* Unit tests for the simulator's own hand-written C modules.
 *
 * These are the pieces with no third-party coverage: the dependency-free PNG
 * writer and the synthetic-input gesture queue. Everything else (the bindings,
 * LVGL, Lua) is exercised by the app render tests in sim/test.sh.
 *
 * Run via the `sim_tests` target; exits non-zero on any failure.
 */
#include "png_write.h"
#include "sim_input.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); g_failures++; } \
} while (0)

/* --- minimal PNG reader (matches png_write's stored-zlib output) ---------
 * png_write emits uncompressed ("stored") deflate blocks, so we can decode
 * without a real inflate. We still validate every chunk CRC and the zlib
 * adler32, so the test covers png_write's checksum math, not just its layout.
 */

static uint32_t crc_tab[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[n] = c;
    }
}
static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32_of(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

/* Decode a stored-block PNG into a freshly-malloc'd RGB buffer. Returns NULL
 * (and sets *why) on any structural or checksum mismatch. */
static uint8_t *png_decode(const uint8_t *d, size_t len, int *w, int *h, const char **why) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (len < 8 || memcmp(d, sig, 8)) { *why = "bad signature"; return NULL; }
    size_t i = 8;
    int iw = 0, ih = 0;
    uint8_t *idat = NULL; size_t idat_len = 0;
    while (i + 8 <= len) {
        uint32_t clen = rd32(d + i);
        const uint8_t *type = d + i + 4;
        const uint8_t *data = d + i + 8;
        if (i + 12 + clen > len) { *why = "chunk overruns"; free(idat); return NULL; }
        uint32_t want = rd32(d + i + 8 + clen);
        /* CRC covers type+data. */
        uint8_t *tmp = malloc(4 + clen);
        memcpy(tmp, type, 4); memcpy(tmp + 4, data, clen);
        uint32_t got = crc32_of(tmp, 4 + clen);
        free(tmp);
        if (got != want) { *why = "chunk CRC mismatch"; free(idat); return NULL; }
        if (!memcmp(type, "IHDR", 4)) {
            iw = (int)rd32(data); ih = (int)rd32(data + 4);
            if (data[8] != 8 || data[9] != 2) { *why = "not 8-bit RGB"; free(idat); return NULL; }
        } else if (!memcmp(type, "IDAT", 4)) {
            idat = realloc(idat, idat_len + clen);
            memcpy(idat + idat_len, data, clen); idat_len += clen;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        i += 12 + clen;
    }
    if (!idat || idat_len < 6) { *why = "no IDAT"; free(idat); return NULL; }

    /* zlib: 2-byte header, stored blocks, 4-byte adler32. */
    size_t p = 2;
    size_t raw_cap = (size_t)ih * (1 + (size_t)iw * 3);
    uint8_t *raw = malloc(raw_cap ? raw_cap : 1);
    size_t rl = 0;
    int final = 0;
    while (!final && p + 5 <= idat_len) {
        uint8_t hdr = idat[p++];
        final = hdr & 1;
        if ((hdr >> 1 & 3) != 0) { *why = "not a stored block"; free(idat); free(raw); return NULL; }
        uint16_t blk = idat[p] | (idat[p + 1] << 8);
        p += 4; /* LEN + NLEN */
        if (p + blk > idat_len || rl + blk > raw_cap) { *why = "block overruns"; free(idat); free(raw); return NULL; }
        memcpy(raw + rl, idat + p, blk); rl += blk; p += blk;
    }
    if (rl != raw_cap) { *why = "raw size mismatch"; free(idat); free(raw); return NULL; }
    uint32_t adler_want = rd32(idat + idat_len - 4);
    if (adler32_of(raw, rl) != adler_want) { *why = "adler32 mismatch"; free(idat); free(raw); return NULL; }
    free(idat);

    /* Strip the per-row filter byte (must be 0 == none). */
    uint8_t *rgb = malloc((size_t)iw * ih * 3);
    for (int y = 0; y < ih; y++) {
        if (raw[y * (1 + iw * 3)] != 0) { *why = "unexpected filter"; free(raw); free(rgb); return NULL; }
        memcpy(rgb + (size_t)y * iw * 3, raw + y * (1 + iw * 3) + 1, (size_t)iw * 3);
    }
    free(raw);
    *w = iw; *h = ih; *why = NULL;
    return rgb;
}

static void test_png_roundtrip(void) {
    const int W = 5, H = 4;
    uint8_t src[5 * 4 * 3];
    for (int i = 0; i < W * H; i++) {
        src[i * 3 + 0] = (uint8_t)(i * 7);
        src[i * 3 + 1] = (uint8_t)(i * 13 + 1);
        src[i * 3 + 2] = (uint8_t)(255 - i * 5);
    }
    const char *path = "sim_tests_tmp.png";
    CHECK(png_write_rgb(path, src, W, H) == 0, "png_write_rgb returned error");

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "cannot reopen written png");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    size_t rd = fread(buf, 1, n, f); fclose(f); remove(path);
    CHECK(rd == (size_t)n, "short read of png");

    int w = 0, h = 0; const char *why = NULL;
    uint8_t *out = png_decode(buf, n, &w, &h, &why);
    free(buf);
    CHECK(out != NULL, why ? why : "png_decode failed");
    if (!out) return;
    CHECK(w == W && h == H, "decoded dimensions wrong");
    CHECK(memcmp(out, src, sizeof(src)) == 0, "decoded pixels differ from source");
    free(out);
}

static void test_sim_input(void) {
    /* Fresh: nothing queued. */
    CHECK(sim_input_idle(), "input not idle at start");

    lv_indev_data_t data;
    sim_input_inject(100, 200, 100, 200, 60);   /* a tap */
    CHECK(!sim_input_idle(), "idle right after inject");

    sim_input_read_cb(NULL, &data);
    CHECK(data.state == LV_INDEV_STATE_PRESSED, "first read not PRESSED");
    CHECK(data.point.x == 100 && data.point.y == 200, "pressed point wrong");

    /* Advance past the 60ms tap + the 90ms release gap. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 220L * 1000 * 1000 };
    nanosleep(&ts, NULL);
    sim_input_read_cb(NULL, &data);
    CHECK(data.state == LV_INDEV_STATE_RELEASED, "read after duration not RELEASED");
    /* Another read after the gap: back to idle. */
    nanosleep(&ts, NULL);
    sim_input_read_cb(NULL, &data);
    CHECK(sim_input_idle(), "not idle after gesture drained");

    /* Queue cap: flooding must not crash or wedge. */
    for (int i = 0; i < 40; i++) sim_input_inject(i, i, i, i, 60);
    CHECK(!sim_input_idle(), "idle after flooding queue");
    for (int i = 0; i < 400 && !sim_input_idle(); i++) {
        sim_input_read_cb(NULL, &data);
        struct timespec s = { .tv_sec = 0, .tv_nsec = 5L * 1000 * 1000 };
        nanosleep(&s, NULL);
    }
    CHECK(sim_input_idle(), "flooded queue never drained");
}

int main(void) {
    crc_init();
    lv_init();          /* sim_input uses LVGL indev data types */
    test_png_roundtrip();
    test_sim_input();
    if (g_failures == 0) {
        printf("all unit tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d unit test(s) failed\n", g_failures);
    return 1;
}
