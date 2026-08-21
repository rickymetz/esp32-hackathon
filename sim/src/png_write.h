/* Minimal, dependency-free PNG writer (RGB888, 8-bit, no compression).
 *
 * Emits a single IDAT using zlib "stored" (uncompressed) deflate blocks, so we
 * pull in neither zlib nor libpng. Output is a valid PNG that any decoder reads;
 * it is just larger than a compressed one, which is fine for a 368x448 sim frame.
 */
#ifndef SIM_PNG_WRITE_H
#define SIM_PNG_WRITE_H

#include <stdint.h>

/* Write `w`*`h` RGB888 pixels (3 bytes/pixel, row-major, top-to-bottom) to
 * `path`. Returns 0 on success, non-zero on I/O failure. */
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);

#endif /* SIM_PNG_WRITE_H */
