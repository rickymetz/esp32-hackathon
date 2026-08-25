#include "app_sandbox.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "log_ring.h"
#include "lauxlib.h"
#include "lualib.h"

static const char *TAG = "app_sandbox";

/* Fires every HOOK_COUNT VM instructions. Small enough to feel instant,
 * large enough that the check is not measurable in normal use. */
#define HOOK_COUNT 10000

static void interrupt_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    if (!cap_lua_runtime_stop_requested(L)) {
        return;
    }
    /* Latch: re-arm at every single instruction so that even if the app
     * catches this error in a pcall, the next instruction raises again and
     * forward progress is impossible. */
    lua_sethook(L, interrupt_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "app stopped by launcher");
}

void app_sandbox_install_hook(lua_State *L)
{
    lua_sethook(L, interrupt_hook, LUA_MASKCOUNT, HOOK_COUNT);
}

/* coroutine.create, wrapped: lua_newthread starts with hookmask == 0, so
 * a tight loop inside a coroutine body was invisible to the interrupt
 * hook and fell through to a ~10s watchdog reboot of the whole board
 * (open issue 1 from the handoff). This calls the original create (kept
 * as an upvalue) and installs the hook on the new thread before handing
 * it back. */
static int sandbox_co_create(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, lua_upvalueindex(1));   /* original coroutine.create */
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    lua_State *co = lua_tothread(L, -1);
    if (co != NULL) {
        lua_sethook(co, interrupt_hook, LUA_MASKCOUNT, HOOK_COUNT);
    }
    return 1;
}

/* Replace coroutine.create in place (the cached module table is the same
 * object, so require("coroutine") sees the wrapper too), and rebuild
 * coroutine.wrap on top of it in Lua -- the original wrap hides its
 * thread, so there is no C-side handle to hook. */
static void sandbox_wrap_coroutines(lua_State *L)
{
    static const char wrap_src[] =
        "local create, resume, unpack = ...\n"
        "return function(f)\n"
        "  local co = create(f)\n"
        "  return function(...)\n"
        "    local r = { resume(co, ...) }\n"
        "    if r[1] then return unpack(r, 2) end\n"
        "    error(r[2], 0)\n"
        "  end\n"
        "end\n";

    lua_getglobal(L, "coroutine");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    /* coroutine.create = C wrapper closing over the original */
    lua_getfield(L, -1, "create");
    lua_pushcclosure(L, sandbox_co_create, 1);
    lua_setfield(L, -2, "create");

    /* coroutine.wrap = Lua chunk(create_wrapper, resume, table.unpack) */
    if (luaL_loadstring(L, wrap_src) == LUA_OK) {
        lua_getfield(L, -2, "create");   /* the wrapper installed above */
        lua_getfield(L, -3, "resume");
        lua_getglobal(L, "table");
        lua_getfield(L, -1, "unpack");
        lua_remove(L, -2);               /* table */
        lua_call(L, 3, 1);               /* -> wrap function */
        lua_setfield(L, -2, "wrap");
    } else {
        ESP_LOGE(TAG, "coroutine.wrap shim failed to load: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   /* coroutine table */
}

static void nil_field(lua_State *L, const char *table, const char *field)
{
    lua_getglobal(L, table);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
    }
    lua_pop(L, 1);
}

/* An app's print() replaced, so its output reaches the LOG ring.
 *
 * Lua's own print writes with fwrite() straight to stdout, which the ring's
 * esp_log_set_vprintf() hook never sees -- so an app author's print(), the
 * single most useful thing to read when an app misbehaves, was the one thing
 * `LOG` could not hand back. This routes it through the same path as
 * everything else and still prints it, so nothing changes for a human
 * watching the console.
 *
 * Semantics kept identical to Lua's print: tostring() on every argument (so
 * __tostring metamethods work), tabs between, newline at the end. */
static int sandboxed_print(lua_State *L)
{
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *piece = luaL_tolstring(L, i, &len);   /* honours __tostring */
        log_ring_puts(piece, len);
        fwrite(piece, 1, len, stdout);
        if (i < n) {
            log_ring_puts("\t", 1);
            fputc('\t', stdout);
        }
        lua_pop(L, 1);   /* the string luaL_tolstring pushed */
    }
    log_ring_puts("\n", 1);
    fputc('\n', stdout);
    /* Lua's print ends with lua_writeline(), which is a newline AND an
     * fflush (luaconf.h). Without this the claim of identical semantics above
     * was false, and a buffered stdout would delay an author's print() off the
     * console -- the very latency this is meant to avoid. */
    fflush(stdout);
    return 0;
}

void app_sandbox_apply(lua_State *L)
{
    /* debug.sethook would let an app remove our interrupt hook in one line.
     * Nil the fields on the table object itself -- not just the global --
     * so the change is visible through every reference to that table,
     * including one obtained later via require("debug"). Field-nils must
     * happen before we drop the registry cache entry below, since that is
     * our last handle on the live table. */
    nil_field(L, "debug", "sethook");
    nil_field(L, "debug", "gethook");

    lua_pushnil(L);
    lua_setglobal(L, "debug");

    /* package.loadlib is an escape hatch to C. */
    lua_pushnil(L);
    lua_setglobal(L, "package");

    nil_field(L, "os", "exit");
    nil_field(L, "os", "execute");

    /* luaL_openlibs() caches every module table in the registry's
     * LUA_LOADED_TABLE (via luaL_requiref), separately from the global.
     * require() reads from that cache, not from _G, so nilling the globals
     * above does not stop require("debug")/require("package") from handing
     * back the live table. Clear the cache entries too. */
    lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "debug");
        lua_pushnil(L);
        lua_setfield(L, -2, "package");
    }
    lua_pop(L, 1);

    /* Not a restriction like the rest of this function -- print() stays fully
     * usable. It is replaced only so its output is capturable; see
     * sandboxed_print(). */
    lua_pushcfunction(L, sandboxed_print);
    lua_setglobal(L, "print");

    sandbox_wrap_coroutines(L);

    ESP_LOGD(TAG, "sandbox applied");
}
