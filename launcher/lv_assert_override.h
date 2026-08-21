/*
 * I3: force-included ahead of every translation unit (see CMakeLists.txt)
 * so LV_ASSERT_HANDLER is already defined by the time
 * managed_components/lvgl__lvgl/src/lv_conf_internal.h processes its
 * `#ifndef LV_ASSERT_HANDLER` fallback to `while(1);`.
 *
 * This can't be done through sdkconfig.defaults: CONFIG_LV_ASSERT_HANDLER_INCLUDE
 * has a real Kconfig symbol and works from sdkconfig.defaults, but the
 * handler body itself (CONFIG_LV_ASSERT_HANDLER) has no backing Kconfig
 * entry -- setting it in sdkconfig.defaults is silently dropped ("unknown
 * kconfig symbol"). A global `-D` compile definition was tried next, but
 * embedding the literal ';' this macro's body needs (LV_ASSERT_HANDLER is
 * used bare, with no trailing ';' at its call sites, so the macro's own
 * expansion must supply one) does not survive ESP-IDF's global
 * COMPILE_DEFINITIONS property -- confirmed on hardware: both `\;` escaping
 * and the `$<SEMICOLON>` generator expression came out the other end with
 * the semicolon silently missing. A force-included header sidesteps that
 * escaping entirely: the semicolon lives in plain C text, never passed
 * through a CMake list.
 *
 * Without this, an lv_malloc() failure (CONFIG_LV_USE_ASSERT_MALLOC) spins
 * the calling task in `while(1);` forever, and CONFIG_ESP_TASK_WDT_PANIC
 * only reboots the board 10s later via the watchdog. Rebooting immediately
 * here gets back to a working launcher without that 10s stall.
 */
#pragma once

#ifndef LV_ASSERT_HANDLER
#include "esp_system.h"
#define LV_ASSERT_HANDLER esp_restart();
#endif
