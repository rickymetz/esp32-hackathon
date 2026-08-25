/*
 * An in-memory ring of the most recent console output, dumpable over serial.
 *
 * WHY: the console cannot be read while the board is doing something. It
 * exists only on the USB-Serial/JTAG CDC (CONFIG_ESP_CONSOLE_SECONDARY_NONE),
 * one host process can hold that port, and the harness needs it -- so a
 * logger and tools/drive.py cannot both attach. The documented workaround
 * (tools/read_serial.py) pulses RTS, which RESETS the board: it shows you the
 * boot log of a fresh boot, not the error you were trying to catch.
 *
 * That cost real debugging time. An app was exiting intermittently (~1 in 8)
 * and the error text was simply unreadable, so the cause had to be guessed at
 * from uptime and heap readings -- and three single-trial "bisects" against a
 * 12% failure each produced a confident wrong answer.
 *
 * So: tee every ESP_LOG line into a PSRAM ring as it is printed, and add a
 * `LOG` serial verb that dumps it. You trigger the fault, then ask what
 * happened, over the same port the harness already owns. No reset, no second
 * channel, no sharing.
 *
 * Captures BOTH: ESP_LOGx via the vprintf hook -- including the launcher's
 * "app '<name>' failed:" and its Lua traceback -- and an app's own print(),
 * which Lua writes straight to stdout and the hook therefore never sees. The
 * sandbox installs a print() that tees through log_ring_puts() for exactly
 * that reason; print() is what an app author actually wants to read back.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start capturing console output. Call once, early in app_main().
 *
 * Allocates the ring in PSRAM. If that fails, capture stays off and logging
 * is unaffected -- a debugging aid must never be the reason a board does not
 * boot.
 */
void log_ring_init(void);

/**
 * @brief Print the buffered output, oldest first, to the console.
 *
 * Capture is suppressed for the duration, or the dump would feed itself.
 */
void log_ring_dump(void);

/**
 * @brief Append raw bytes to the ring, for output that is not an ESP_LOG.
 *
 * An app's print() goes to stdout via fwrite and never reaches the vprintf
 * hook, so the sandbox routes it here instead -- print() is what an author
 * actually wants to read back.
 */
void log_ring_puts(const char *data, size_t len);

/** @brief Bytes currently buffered. */
size_t log_ring_used(void);

#ifdef __cplusplus
}
#endif
