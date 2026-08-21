#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- CRC32 (PNG chunks) ------------------------------------------------- */

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!crc_ready) crc_init();
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* --- Adler32 (zlib) ----------------------------------------------------- */

static uint32_t adler32(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

/* --- helpers ------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

/* Write one PNG chunk: length, type, data, CRC(type+data). */
static int write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put_u32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;

    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    if (len) crc = crc32_update(crc, data, len);
    uint8_t crcb[4];
    put_u32(crcb, crc);
    return (fwrite(crcb, 1, 4, f) == 4) ? 0 : -1;
}

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    if (w <= 0 || h <= 0) return -1;

    /* Raw scanlines: each row prefixed with filter byte 0. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return -1;
    {
        size_t o = 0;
        for (int y = 0; y < h; y++) {
            raw[o++] = 0; /* filter: none */
            memcpy(raw + o, rgb + (size_t)y * w * 3, (size_t)w * 3);
            o += (size_t)w * 3;
        }
    }

    /* zlib stream: 2-byte header + stored deflate blocks + adler32.
     * Stored blocks cap each block's payload at 65535 bytes. */
    size_t nblocks = (raw_len + 65534) / 65535;
    if (nblocks == 0) nblocks = 1;
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return -1; }

    size_t zo = 0;
    z[zo++] = 0x78; /* CMF: deflate, 32K window */
    z[zo++] = 0x01; /* FLG: check bits, no dict, fastest */

    size_t remaining = raw_len, src = 0;
    while (remaining > 0 || raw_len == 0) {
        size_t block = remaining > 65535 ? 65535 : remaining;
        int final = (remaining - block == 0);
        z[zo++] = (uint8_t)(final ? 1 : 0); /* BFINAL, BTYPE=00 (stored) */
        z[zo++] = (uint8_t)(block & 0xFF);
        z[zo++] = (uint8_t)((block >> 8) & 0xFF);
        z[zo++] = (uint8_t)(~block & 0xFF);
        z[zo++] = (uint8_t)((~block >> 8) & 0xFF);
        if (block) { memcpy(z + zo, raw + src, block); zo += block; src += block; }
        remaining -= block;
        if (raw_len == 0) break;
    }
    put_u32(z + zo, adler32(raw, raw_len));
    zo += 4;

    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    int rc = (fwrite(sig, 1, 8, f) == 8) ? 0 : -1;

    if (rc == 0) {
        uint8_t ihdr[13];
        put_u32(ihdr + 0, (uint32_t)w);
        put_u32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 2;  /* color type: truecolor RGB */
        ihdr[10] = 0; /* compression */
        ihdr[11] = 0; /* filter */
        ihdr[12] = 0; /* interlace */
        rc = write_chunk(f, "IHDR", ihdr, 13);
    }
    if (rc == 0) rc = write_chunk(f, "IDAT", z, (uint32_t)zo);
    if (rc == 0) rc = write_chunk(f, "IEND", NULL, 0);

    free(z);
    fclose(f);
    return rc;
}
