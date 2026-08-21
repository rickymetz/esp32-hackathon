/*
 * Receive .lua files over the USB console so app authors do not have to
 * shuttle the SD card to a laptop for every edit. Also accepts RUN/STOP so
 * an app can be launched and stopped without touching the screen.
 *
 * NOT a recovery path: a crashed app takes the native USB device with it, so
 * push dies with the board. Use flash.sh for that.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Start the receiver task. Call once at boot, after the SD card is mounted. */
void serial_push_start(void);

#ifdef __cplusplus
}
#endif
