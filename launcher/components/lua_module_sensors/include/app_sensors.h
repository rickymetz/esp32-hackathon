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

#ifdef __cplusplus
extern "C" {
#endif

/** Probe the three chips and register the modules. One-time, from app_main(). */
esp_err_t app_sensors_register(void);

#ifdef __cplusplus
}
#endif
