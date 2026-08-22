/*
 * The `wifi` Lua module: station-mode networking and NTP time.
 *
 * Connecting takes seconds, so nothing here blocks: connect() starts
 * the attempt and returns, status() reports progress, and the launcher
 * pumps on regardless. That matches how `voice` behaves and keeps the
 * "an app builds its UI and returns" rule intact.
 *
 * Credentials live at /sdcard/wifi.txt (line 1 SSID, line 2 password).
 * They are entered on the device with apps/wifi_setup.lua rather than
 * typed into a host terminal, so they never travel over the serial link.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the module. One-time, from app_main(). */
esp_err_t app_wifi_register(void);

/**
 * @brief If credentials exist on the card, start connecting and, once
 * up, sync the RTC over NTP. Non-blocking; safe when there is no card,
 * no credentials, and no network.
 */
void app_wifi_autostart(void);

#ifdef __cplusplus
}
#endif
