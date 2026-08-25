/* Host shim for ESP-IDF's esp_log.h -- routes ESP_LOGx to stderr. */
#ifndef SIM_ESP_LOG_H
#define SIM_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { (void)(tag); } while (0)

/* The device routes every ESP_LOG through esp_log_set_vprintf() so log_ring.c
 * can tee it. The sim's ESP_LOGx go straight to stderr, so there is nothing to
 * intercept -- this exists so log_ring.c compiles and its ring still captures
 * what the sandbox pushes in explicitly (an app's print()). Returns NULL: no
 * previous handler to chain to. */
typedef int (*vprintf_like_t)(const char *, va_list);
static inline vprintf_like_t esp_log_set_vprintf(vprintf_like_t f)
{
    (void)f;
    return NULL;
}

#endif /* SIM_ESP_LOG_H */
