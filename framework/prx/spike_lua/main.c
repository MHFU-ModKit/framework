/*
 * Phase-0 spike: prove embedded Lua runs on PSP/Allegrex and measure its
 * real heap demand. Standalone EBOOT (runnable on PPSSPP) — separate from
 * the framework PRX link test (see prx_link_test.cpp).
 *
 * Heap measurement uses the standard lua_Alloc delta trick: track live and
 * peak bytes from (osize,nsize) regardless of the backing allocator. No
 * custom slab needed to get the number.
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

PSP_MODULE_INFO("lua_spike", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define printf pspDebugScreenPrintf

static size_t g_live = 0, g_peak = 0;
static unsigned g_allocs = 0, g_frees = 0;

static void *track_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        if (ptr) { g_live -= osize; free(ptr); g_frees++; }
        return NULL;
    }
    void *np = realloc(ptr, nsize);
    if (np) {
        g_live += nsize - (ptr ? osize : 0);
        if (g_live > g_peak) g_peak = g_live;
        g_allocs++;
    }
    return np;
}

static FILE *g_log;
static void logline(const char *s) {
    printf("%s\n", s);
    if (g_log) { fputs(s, g_log); fputs("\n", g_log); fflush(g_log); }
}
static char buf[256];

/* a curated C binding, to prove C->Lua exposure works */
static int l_mhfu_probe(lua_State *L) {
    lua_pushinteger(L, 0xDEAD);
    return 1;
}

int main(void) {
    pspDebugScreenInit();
    g_log = fopen("ms0:/lua_spike.log", "w");
    if (!g_log) g_log = fopen("host0:/lua_spike.log", "w");

    logline("=== MHFU Lua Phase-0 spike ===");
    snprintf(buf, sizeof buf, "lua_Number size=%u  lua_Integer size=%u",
             (unsigned)sizeof(lua_Number), (unsigned)sizeof(lua_Integer));
    logline(buf);

    lua_State *L = lua_newstate(track_alloc, NULL);
    if (!L) { logline("FATAL: lua_newstate returned NULL"); goto done; }
    snprintf(buf, sizeof buf, "after newstate:    live=%6u  peak=%6u",
             (unsigned)g_live, (unsigned)g_peak);
    logline(buf);

    /* sandbox: open ONLY the safe subset */
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    snprintf(buf, sizeof buf, "after 4 safe libs: live=%6u  peak=%6u",
             (unsigned)g_live, (unsigned)g_peak);
    logline(buf);

    /* register a curated binding */
    lua_pushcfunction(L, l_mhfu_probe);
    lua_setglobal(L, "mhfu_probe");

    /* run a representative mod-ish script: loop + table + string + our C fn */
    const char *script =
        "local sum = 0\n"
        "for i = 1, 1000 do sum = sum + i end\n"
        "local t = {}\n"
        "for i = 1, 50 do t[i] = 'popo' .. tostring(i) end\n"
        "local probe = mhfu_probe()\n"
        "return sum, #t, probe\n";
    int rc = luaL_loadstring(L, script);
    if (rc != LUA_OK) {
        snprintf(buf, sizeof buf, "loadstring FAILED rc=%d: %s", rc, lua_tostring(L, -1));
        logline(buf);
    } else {
        rc = lua_pcall(L, 0, 3, 0);
        if (rc != LUA_OK) {
            snprintf(buf, sizeof buf, "pcall FAILED rc=%d: %s", rc, lua_tostring(L, -1));
            logline(buf);
        } else {
            lua_Integer sum   = lua_tointeger(L, -3);
            lua_Integer count = lua_tointeger(L, -2);
            lua_Integer probe = lua_tointeger(L, -1);
            snprintf(buf, sizeof buf, "script OK: sum=%ld tcount=%ld probe=0x%lX",
                     (long)sum, (long)count, (long)probe);
            logline(buf);
            lua_pop(L, 3);
        }
    }
    snprintf(buf, sizeof buf, "after script run:  live=%6u  peak=%6u",
             (unsigned)g_live, (unsigned)g_peak);
    logline(buf);

    /* force a full GC and re-measure to see steady-state floor */
    lua_gc(L, LUA_GCCOLLECT, 0);
    snprintf(buf, sizeof buf, "after full GC:     live=%6u  peak=%6u",
             (unsigned)g_live, (unsigned)g_peak);
    logline(buf);
    snprintf(buf, sizeof buf, "lua_gc KB count=%d  allocs=%u frees=%u",
             lua_gc(L, LUA_GCCOUNT, 0), g_allocs, g_frees);
    logline(buf);

    lua_close(L);
    snprintf(buf, sizeof buf, "after close:       live=%6u  (should be 0)",
             (unsigned)g_live);
    logline(buf);

    logline("=== SPIKE COMPLETE — Lua runs on Allegrex ===");
done:
    if (g_log) fclose(g_log);
    /* keep screen up a moment, then exit so headless run terminates */
    int i;
    for (i = 0; i < 60 * 8; i++) sceDisplayWaitVblankStart();
    sceKernelExitGame();
    return 0;
}
