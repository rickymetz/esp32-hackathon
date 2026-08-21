/* Slice 2: prove the embedded Lua 5.5 core runs on the host. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>

int main(void)
{
    lua_State *L = luaL_newstate();
    if (!L) { fprintf(stderr, "luaL_newstate failed\n"); return 1; }
    luaL_openlibs(L);

    const char *script =
        "local v = _VERSION\n"
        "assert(3 // 2 == 1, 'integer div')\n"          /* 5.5 // */
        "assert(math.type(3/2) == 'float', 'float div')\n"
        "return v .. ' | 7*6=' .. tostring(7*6)\n";

    if (luaL_dostring(L, script) != LUA_OK) {
        fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    printf("lua ok: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return 0;
}
