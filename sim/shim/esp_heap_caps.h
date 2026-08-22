/* Host shim for ESP-IDF's esp_heap_caps.h -- maps to plain malloc/calloc/free.
 * The sim has no PSRAM/DRAM distinction, so the capability flags are ignored. */
#ifndef SIM_ESP_HEAP_CAPS_H
#define SIM_ESP_HEAP_CAPS_H

#include <stddef.h>

#define MALLOC_CAP_DEFAULT  (1 << 0)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)

void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t n, size_t size, unsigned caps);
void  heap_caps_free(void *ptr);

#endif /* SIM_ESP_HEAP_CAPS_H */
