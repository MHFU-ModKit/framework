/*
 * lua_host — embeds a sandboxed Lua 5.4 VM in the framework PRX and runs
 * Lua "script mods" against a curated mhfu.* API. Phase 1 of the Lua
 * modding platform (docs/LUA_MODDING_PLATFORM_PLAN.md).
 *
 * Phase 1 scope: one embedded script that reads live game memory (walks
 * the Popo entities and logs their HP). Heap comes from a dedicated slab
 * (sceKernelAllocPartitionMemory) with a self-contained allocator, so Lua
 * never competes with the game or the framework's newlib heap. Sandbox
 * opens only base/table/string/math (io/os/debug dead-stripped at link —
 * verified Phase 0). A poll thread calls the script's mhfu_tick() at ~2 Hz.
 *
 * Later phases: load .lua files from the memstick (Phase 3), hot reload
 * (Phase 4), AI override bindings (Phase 2).
 */
#include <pspthreadman.h>
#include <pspsysmem.h>

#include "mhfu/mhfu.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

/* ------------------------------------------------------------------ slab
 * A self-contained implicit-free-list allocator over a fixed slab. Lua's
 * realloc-heavy pattern is fully supported (malloc/free/realloc with
 * forward coalescing). ~12 KB live for a typical mod (Phase 0 measured),
 * so 256 KB is generous headroom for many script mods + GC churn.
 */
#define LUA_SLAB_BYTES (512 * 1024)

typedef struct blk_hdr { unsigned size; unsigned free; } blk_hdr; /* 8 bytes */

static char     *g_slab;          /* slab base (8-byte aligned)      */
static unsigned  g_slab_bytes;
static SceUID    g_slab_uid = -1;
static unsigned  g_peak_used;     /* high-water, for the log         */

static unsigned align8(unsigned n) { return (n + 7u) & ~7u; }

static void slab_init(void *base, unsigned bytes)
{
    g_slab = (char *)base;
    g_slab_bytes = bytes;
    blk_hdr *h = (blk_hdr *)g_slab;
    h->size = bytes;          /* whole slab = one free block (incl hdr) */
    h->free = 1;
}

static unsigned slab_live(void)
{
    unsigned used = 0, off = 0;
    while (off + sizeof(blk_hdr) <= g_slab_bytes) {
        blk_hdr *h = (blk_hdr *)(g_slab + off);
        if (h->size == 0) break;
        if (!h->free) used += h->size;
        off += h->size;
    }
    return used;
}

static void slab_coalesce(void)
{
    unsigned off = 0;
    while (off + sizeof(blk_hdr) <= g_slab_bytes) {
        blk_hdr *h = (blk_hdr *)(g_slab + off);
        if (h->size == 0) break;
        if (h->free) {
            unsigned noff = off + h->size;
            while (noff + sizeof(blk_hdr) <= g_slab_bytes) {
                blk_hdr *n = (blk_hdr *)(g_slab + noff);
                if (n->size == 0 || !n->free) break;
                h->size += n->size;            /* merge */
                noff += n->size;
            }
        }
        off += h->size;
    }
}

static void *slab_malloc(unsigned n)
{
    unsigned need = align8(n) + sizeof(blk_hdr);
    if (need < sizeof(blk_hdr) + 8) need = sizeof(blk_hdr) + 8;
    unsigned off = 0;
    while (off + sizeof(blk_hdr) <= g_slab_bytes) {
        blk_hdr *h = (blk_hdr *)(g_slab + off);
        if (h->size == 0) break;
        if (h->free && h->size >= need) {
            unsigned rem = h->size - need;
            if (rem >= sizeof(blk_hdr) + 8) {     /* split */
                h->size = need;
                blk_hdr *s = (blk_hdr *)(g_slab + off + need);
                s->size = rem;
                s->free = 1;
            }
            h->free = 0;
            unsigned live = slab_live();
            if (live > g_peak_used) g_peak_used = live;
            return (char *)h + sizeof(blk_hdr);
        }
        off += h->size;
    }
    return 0; /* OOM */
}

static void slab_free(void *p)
{
    if (!p) return;
    blk_hdr *h = (blk_hdr *)((char *)p - sizeof(blk_hdr));
    h->free = 1;
    slab_coalesce();
}

static unsigned slab_payload(void *p)
{
    blk_hdr *h = (blk_hdr *)((char *)p - sizeof(blk_hdr));
    return h->size - sizeof(blk_hdr);
}

static void *slab_realloc(void *p, unsigned n)
{
    if (!p) return slab_malloc(n);
    unsigned cur = slab_payload(p);
    if (n <= cur) return p;                 /* shrink/keep in place */
    void *np = slab_malloc(n);
    if (!np) return 0;
    unsigned copy = cur < n ? cur : n;
    for (unsigned i = 0; i < copy; i++) ((char *)np)[i] = ((char *)p)[i];
    slab_free(p);
    return np;
}

static void *lua_slab_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) { slab_free(ptr); return 0; }
    if (!ptr)       return slab_malloc((unsigned)nsize);
    return slab_realloc(ptr, (unsigned)nsize);
}

/* ------------------------------------------------------------ bindings */

static int lb_log(lua_State *L)
{
    mhfu_log("%s", luaL_checkstring(L, 1));
    return 0;
}
static int lb_read_u8 (lua_State *L) { lua_pushinteger(L, mhfu_read_u8 ((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_read_u16(lua_State *L) { lua_pushinteger(L, mhfu_read_u16((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_read_u32(lua_State *L) { lua_pushinteger(L, mhfu_read_u32((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_mem_valid(lua_State *L){ lua_pushboolean(L, mhfu_mem_valid((uint32_t)luaL_checkinteger(L,1))); return 1; }

static int lb_screen_state(lua_State *L){ lua_pushinteger(L, mhfu_get_screen_state()); return 1; }
static int lb_area_index (lua_State *L){ lua_pushinteger(L, mhfu_get_area_index());  return 1; }
static int lb_quest_timer(lua_State *L){ lua_pushinteger(L, mhfu_get_quest_timer()); return 1; }

static int lb_entity_at  (lua_State *L){ lua_pushinteger(L, mhfu_entity_at((int)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_type(lua_State *L){ lua_pushinteger(L, mhfu_entity_type((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_hp  (lua_State *L){ lua_pushinteger(L, mhfu_entity_hp  ((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_size(lua_State *L){ lua_pushnumber (L, mhfu_entity_size((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_alive(lua_State *L){ lua_pushboolean(L, mhfu_entity_is_alive((uint32_t)luaL_checkinteger(L,1))); return 1; }

/* mhfu.entities_of_type(type) -> { ptr, ptr, ... } */
static int lb_entities_of_type(lua_State *L)
{
    int type = (int)luaL_checkinteger(L, 1);
    uint32_t buf[16];
    int n = mhfu_entities_of_type((mhfu_monster_id_t)type, buf, 16);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static const luaL_Reg k_mhfu_api[] = {
    { "log",              lb_log },
    { "read_u8",          lb_read_u8 },
    { "read_u16",         lb_read_u16 },
    { "read_u32",         lb_read_u32 },
    { "mem_valid",        lb_mem_valid },
    { "get_screen_state", lb_screen_state },
    { "get_area_index",   lb_area_index },
    { "get_quest_timer",  lb_quest_timer },
    { "entity_at",        lb_entity_at },
    { "entity_type",      lb_entity_type },
    { "entity_hp",        lb_entity_hp },
    { "entity_size",      lb_entity_size },
    { "entity_alive",     lb_entity_alive },
    { "entities_of_type", lb_entities_of_type },
    { 0, 0 },
};

static void register_mhfu_api(lua_State *L)
{
    lua_newtable(L);                       /* the mhfu table */
    luaL_setfuncs(L, k_mhfu_api, 0);
    /* species id constants */
    lua_pushinteger(L, 0x45); lua_setfield(L, -2, "MON_ANTEKA");
    lua_pushinteger(L, 0x46); lua_setfield(L, -2, "MON_POPO");
    lua_pushinteger(L, 0x4B); lua_setfield(L, -2, "MON_TIGREX");
    lua_pushinteger(L, 0x4D); lua_setfield(L, -2, "MON_GIADROME");
    lua_setglobal(L, "mhfu");
}

static void remove_unsafe_globals(lua_State *L)
{
    static const char *unsafe[] = {
        "dofile", "loadfile", "load", "loadstring", "require",
        "collectgarbage", "rawget", "rawset", "rawequal", "rawlen", 0
    };
    for (int i = 0; unsafe[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe[i]);
    }
}

/* ----------------------------------------------------- embedded mod */
/* Phase 1 demo mod: log every Popo's HP while in a quest area. */
static const char *k_demo_mod =
    "local n = 0\n"
    "function mhfu_tick()\n"
    "  n = n + 1\n"
    /* Only the single fixed screen-state byte is read until we are in a
     * STABLE in-area frame (scr==17). Walking the entity registry while a
     * section is mid-load (scr 1/2) reads slot pointers that PPSSPP may not
     * have mapped yet -> hard fault -> sceKernelExitGame. Gate on scr==17. */
    "  local scr = mhfu.read_u8(0x08A8CA48)\n"
    "  if scr ~= 17 then\n"
    "    if n % 8 == 0 then mhfu.log('[lua_mod] idle scr='..scr) end\n"
    "    return\n"
    "  end\n"
    "  local area  = mhfu.read_u16(0x08B0C7DC)\n"   /* visible map section */
    "  local popos = mhfu.entities_of_type(mhfu.MON_POPO)\n"
    "  if #popos == 0 then\n"
    "    if n % 4 == 0 then\n"  /* heartbeat ~ every 2s so we can see ticking */
    "      mhfu.log('[lua_mod] tick='..n..' scr='..scr..' area='..area..' popos=0')\n"
    "    end\n"
    "    return\n"
    "  end\n"
    "  local parts = {}\n"
    "  for i, ent in ipairs(popos) do\n"
    "    parts[#parts+1] = string.format('#%d@%08X hp=%d sz=%.2f',\n"
    "        i, ent, mhfu.entity_hp(ent), mhfu.entity_size(ent))\n"
    "  end\n"
    "  mhfu.log('[lua_mod] tick='..n..' scr='..scr..' area='..area\n"
    "      ..' popos='..#popos..' | '..table.concat(parts, '  '))\n"
    "end\n";

/* ------------------------------------------------------------ host */

static lua_State    *g_L;
static volatile int  g_have_tick;   /* mhfu_tick defined? */
static volatile int  g_paniced;     /* set by panic handler — stop ticking */

/* Lua's DEFAULT panic handler calls abort() -> on PSP that ends the game
 * (sceKernelExitGame). We must NOT take the game down: log the message and
 * mark the VM dead so the poll thread parks instead of aborting. */
static int lua_host_panic(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);
    mhfu_log("[lua_host] PANIC (VM disabled, game kept alive): %s",
             msg ? msg : "?");
    g_paniced = 1;
    /* Returning from a panic handler makes Lua abort(); instead, never
     * return — park this thread so the game lives. The poll thread is the
     * only caller, so parking it is safe. */
    for (;;) sceKernelDelayThread(1000 * 1000);
    return 0; /* unreachable */
}

static int lua_host_setup(void)
{
    g_L = lua_newstate(lua_slab_alloc, 0);
    if (!g_L) { mhfu_log("[lua_host] lua_newstate FAILED (slab OOM?)"); return -1; }
    lua_atpanic(g_L, lua_host_panic);

    /* sandbox: open ONLY the safe subset */
    luaL_requiref(g_L, "_G",     luaopen_base,   1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "table",  luaopen_table,  1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "string", luaopen_string, 1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "math",   luaopen_math,   1); lua_pop(g_L, 1);
    remove_unsafe_globals(g_L);
    register_mhfu_api(g_L);

    /* load + run the embedded mod (defines mhfu_tick) */
    if (luaL_loadstring(g_L, k_demo_mod) != LUA_OK) {
        mhfu_log("[lua_host] load demo FAILED: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return -1;
    }
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] run demo FAILED: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return -1;
    }
    lua_getglobal(g_L, "mhfu_tick");
    g_have_tick = lua_isfunction(g_L, -1);
    lua_pop(g_L, 1);

    mhfu_log("[lua_host] VM ready: lua_Number=%dB live=%uB peak=%uB tick=%d",
             (int)sizeof(lua_Number), slab_live(), g_peak_used, g_have_tick);
    return 0;
}

static void call_tick(void)
{
    if (!g_have_tick) return;
    lua_getglobal(g_L, "mhfu_tick");
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] mhfu_tick error: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
    }
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* let the framework + game settle before first tick */
    sceKernelDelayThread(3 * 1000 * 1000);
    for (;;) {
        sceKernelDelayThread(500 * 1000);   /* 2 Hz */
        if (g_L && !g_paniced) call_tick();
    }
    return 0;
}

static int lua_host_init(void)
{
    /* reserve a dedicated slab so Lua never fights the game's allocator */
    g_slab_uid = sceKernelAllocPartitionMemory(
        2 /* PSP_MEMORY_PARTITION_USER */, "mhfu_lua_slab",
        PSP_SMEM_Low, LUA_SLAB_BYTES + 64, 0);
    if (g_slab_uid < 0) {
        mhfu_log("[lua_host] AllocPartitionMemory FAILED rc=0x%08X", g_slab_uid);
        return -1;
    }
    void *base = sceKernelGetBlockHeadAddr(g_slab_uid);
    /* 8-byte align */
    unsigned a = ((unsigned)base + 7u) & ~7u;
    slab_init((void *)a, LUA_SLAB_BYTES);

    if (lua_host_setup() != 0) return -1;

    SceUID th = sceKernelCreateThread("mhfu_lua_host",
                                      (SceKernelThreadEntry)worker,
                                      0x18, 0x2000, 0, 0);
    if (th < 0) { mhfu_log("[lua_host] CreateThread FAILED"); return -1; }
    sceKernelStartThread(th, 0, 0);
    mhfu_log("[lua_host] init OK, poll thread started");
    return 0;
}

static void lua_host_shutdown(void)
{
    if (g_L) { lua_close(g_L); g_L = 0; }
    if (g_slab_uid >= 0) { sceKernelFreePartitionMemory(g_slab_uid); g_slab_uid = -1; }
}

MHFU_MOD(.id = "lua_host", .version = "0.1",
         .needs = 0, .conflicts = 0,
         .init = lua_host_init, .shutdown = lua_host_shutdown);
