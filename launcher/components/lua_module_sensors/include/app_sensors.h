/*
 * The `rtc`, `imu` and `battery` Lua modules.
 *
 * Three chips on the BSP's existing I2C bus -- PCF85063A wall clock at
 * 0x51, QMI8658 6-axis IMU at 0x6B, AXP2101 PMU at 0x34 -- reached by
 * direct register access (no driver component covers them on this
 * board). Every read is synchronous and short; none of them blocks long
 * enough to need the stop-request polling that longer C calls do.
 *
 * All three degrade rather than raise: a chip that NAKs makes its
 * accessors return nil plus a message, so an app written for a board
 * whose sensor is dead still runs.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Probe the three chips and register the modules. One-time, from app_main(). */
esp_err_t app_sensors_register(void);

/**
 * @brief Set the RTC from a broken-down UTC time. Used by the NTP sync
 * so the wall clock survives reboots without anyone typing the date.
 */
esp_err_t app_sensors_rtc_set_tm(int year, int mon, int mday, int hour, int min, int sec, int wday);

/* ---- C-side reads, for the shell -------------------------------------
 * The shell draws the built-in watch face in C with no Lua VM resident, so it
 * cannot go through the `rtc`/`battery` Lua modules. These read the same
 * registers and apply the same honesty rules those modules do; the shell
 * composes the results into the face's data. Kept as primitives rather than a
 * fill-the-face helper so this component never has to know about main/. */

/**
 * @brief Read the wall clock.
 *
 * @return ESP_OK on a trustworthy reading; ESP_ERR_INVALID_STATE when the RTC
 *         reports lost integrity (fresh board, dead backup cell) -- callers
 *         must show "not set" rather than a plausible wrong time; or an I2C
 *         error if the chip does not respond. Outputs are untouched unless
 *         ESP_OK is returned. Any pointer may be NULL.
 */
esp_err_t app_sensors_rtc_get_tm(int *year, int *mon, int *mday,
                                 int *hour, int *min, int *sec, int *wday);

/**
 * @brief Read the battery gauge.
 *
 * @return ESP_OK with 0-100 in *percent; ESP_ERR_INVALID_STATE while the gauge
 *         is still settling (it reports 0xFF), or an I2C error. Outputs are
 *         untouched unless ESP_OK is returned. Any pointer may be NULL.
 */
esp_err_t app_sensors_battery_get(int *percent, bool *charging);

#ifdef __cplusplus
}
#endif
