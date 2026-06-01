/*
 * Phase-0 spike part B: prove embedded Lua links into a PRX under the
 * framework's exact light-C++ regime (-fno-exceptions -fno-rtti
 * -fno-threadsafe-statics) with NO libstdc++. If this links clean without
 * -lstdc++, Lua drags in no C++ runtime and slots into mhfu_framework.prx.
 *
 * Built as a PRX (BUILD_PRX=1); crt0_prx supplies module_start, we supply
 * main(). Result is written to a log so a live run also confirms behavior.
 */
extern "C" {
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdlib.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

PSP_MODULE_INFO("lua_prx_test", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

// Mimic the framework: a tiny C++ object with a method, no STL, no globals
// needing constructors.
namespace {
size_t g_live = 0, g_peak = 0;

void *track_alloc(void *, void *ptr, size_t osize, size_t nsize) {
    if (nsize == 0) { if (ptr) { g_live -= osize; free(ptr); } return nullptr; }
    void *np = realloc(ptr, nsize);
    if (np) { g_live += nsize - (ptr ? osize : 0); if (g_live > g_peak) g_peak = g_live; }
    return np;
}

int l_probe(lua_State *L) { lua_pushinteger(L, 0xB00B); return 1; }

struct LuaHost {
    lua_State *L = nullptr;
    bool start() {
        L = lua_newstate(track_alloc, nullptr);
        if (!L) return false;
        luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
        luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
        luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
        luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
        lua_pushcfunction(L, l_probe);
        lua_setglobal(L, "mhfu_probe");
        return true;
    }
    long run(const char *src) {
        if (luaL_loadstring(L, src) != LUA_OK) return -1;
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) return -2;
        long r = (long)lua_tointeger(L, -1);
        lua_pop(L, 1);
        return r;
    }
};
}

int main(void) {
    FILE *f = fopen("ms0:/lua_prx_test.log", "w");
    LuaHost host;
    if (!host.start()) {
        if (f) { fputs("FATAL newstate\n", f); fclose(f); }
        sceKernelExitDeleteThread(0);
        return 0;
    }
    long r = host.run("local s=0 for i=1,123 do s=s+mhfu_probe() end return s");
    if (f) {
        fprintf(f, "PRX-link OK: run=%ld (expect 123*0xB00B=%ld) live=%u peak=%u\n",
                r, (long)(123 * 0xB00B), (unsigned)g_live, (unsigned)g_peak);
        fclose(f);
    }
    lua_close(host.L);
    // Don't return (crt0 would unload). Spin like a real plugin.
    for (;;) sceKernelDelayThread(1000000);
    return 0;
}
