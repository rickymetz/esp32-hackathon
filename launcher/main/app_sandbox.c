#include "app_sandbox.h"
#include "cap_lua.h"
#include "esp_log.h"
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

static void nil_field(lua_State *L, const char *table, const char *field)
{
    lua_getglobal(L, table);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, field);
    }
    lua_pop(L, 1);
}

void app_sandbox_apply(lua_State *L)
{
    /* debug.sethook would let an app remove our interrupt hook in one line. */
    lua_pushnil(L);
    lua_setglobal(L, "debug");

    /* package.loadlib is an escape hatch to C. */
    lua_pushnil(L);
    lua_setglobal(L, "package");

    nil_field(L, "os", "exit");
    nil_field(L, "os", "execute");

    ESP_LOGD(TAG, "sandbox applied");
}
