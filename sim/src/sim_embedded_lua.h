/* Shared loader for the sim's embedded-Lua modules (ui, keyboard, store).
 *
 * On device these ship as blobs baked into the firmware via EMBED_TXTFILES; the
 * host build instead loads the very same .lua source from the launcher
 * component directory, so the sim always runs the current source with no copy
 * to keep in sync. This is the one place that "read a .lua from disk, run it,
 * and return its module table" lives, so every sim module loader shares it.
 */
#pragma once

#include "lua.h"

/* Load <dir>/<filename>, execute it, and leave its single return value (the
 * module table, as a luaopen_* would) on the stack. Raises a Lua error if the
 * file cannot be read or fails to load/run. Returns 1 (values pushed). */
int sim_open_lua_module(lua_State *L, const char *dir, const char *filename,
                        const char *chunkname);
