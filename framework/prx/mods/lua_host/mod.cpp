/*
 * lua_host — embeds a sandboxed Lua 5.4 VM in the framework PRX and runs
 * Lua "script mods" against a curated mhfu.* API.
 *
 * Phase 1 (DONE): read live game memory from Lua (see git history / the
 * demo reader). Phase 2 (THIS): bind the AI override-event API so a Lua
 * script can intercept and override the engine's per-frame big-monster AI
 * decisions, with the script's return value threaded back into the engine.
 * The acceptance script is scripts/tigrex_spin.lua — a 1:1 Lua port of the
 * C mod framework/prx/mods/tigrex_spin/mod.cpp.
 *
 * Threading model: the AI override callbacks (action_decided / slot_picked /
 * action_input / overlay_loaded / quest_targets_building) fire on the engine's
 * GAME thread (the AI-tick thread). That thread CORRUPTS Lua heap allocation
 * done in its context (bisected exhaustively — see the marshal block below), so
 * those callbacks do NOT run Lua directly: they marshal the request to a
 * dedicated exec thread (a known-good context) and block for the result.
 * spawn/death run on the framework's 5 Hz poll thread and mhfu_tick() on our
 * own 2 Hz worker — both good contexts — so they call the VM directly. The Lua
 * VM is not reentrant, so every entry into g_L (exec / poll / worker) is
 * serialised by a binary semaphore (lua_enter / lua_leave). The panic handler
 * releases the lock before parking so a dead VM never hangs the game thread.
 *
 * Heap: a dedicated 64 KB slab (sceKernelAllocPartitionMemory) with a
 * self-contained allocator, so Lua never competes with the game or the
 * framework newlib heap. MHFU leaves <512 KB contiguous free at runtime and
 * each section load pulls its own PAC — a big slab starves that load and the
 * game bails with sceKernelExitGame (Phase 1 root-cause). Keep the slab small.
 *
 * Sandbox: opens only base/table/string/math; io/os/debug dead-stripped at
 * link. Phase 3: load .lua from the memstick. Phase 4 (THIS): hot reload —
 * the worker thread polls each .lua's (size, mtime) and re-execs changed files
 * in the same VM, so a live edit takes effect with no cold boot.
 */
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mhfu/mhfu.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

/* Phase 3: mods load as plain .lua files off the memstick at boot — no
 * recompile to add/edit a mod. Drop a .lua here and it loads next cold boot:
 *
 *   ms0:/PSP/PLUGINS/mhfu_framework/mods/<name>.lua
 *
 * The embedded tigrex_spin.lua.h below is kept only as a SAFETY FALLBACK for
 * when that directory is empty/absent (so the PRX is never bricked). When any
 * .lua is present on the memstick the embedded copy is NOT used. */
#include "scripts/tigrex_spin.lua.h"

#define LUA_MODS_DIR "ms0:/PSP/PLUGINS/mhfu_framework/mods"
static char g_filebuf[48 * 1024];   /* per-file read scratch (BSS, reused) */

/* ------------------------------------------------------------------ slab
 * A self-contained implicit-free-list allocator over a fixed slab. Lua's
 * realloc-heavy pattern is fully supported (malloc/free/realloc + forward
 * coalescing). Keep the slab SMALL (see header comment). */
#define LUA_SLAB_BYTES (64 * 1024)

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

/* Phase-D diagnostic toggle: route Lua through the framework newlib heap
 * instead of the slab. Proved the allocator is NOT the corruption source (both
 * corrupt on the game thread) — kept off; the slab is the production choice
 * (Phase 1: a large heap starves MHFU's per-section asset load). */
#define LUA_USE_LIBC_ALLOC 0
#if LUA_USE_LIBC_ALLOC
static void *lua_libc_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) { free(ptr); return 0; }
    return realloc(ptr, nsize);
}
#endif

/* ------------------------------------------------------------------ VM */

static lua_State    *g_L;
static volatile int  g_have_tick;   /* mhfu_tick defined?            */
static volatile int  g_paniced;     /* set by panic handler          */
static volatile int  g_ready;       /* setup complete -> events live */
static SceUID        g_lua_sema = -1;

/* Serialise every entry into g_L (game thread / poll thread / worker). */
static int lua_enter(void)
{
    if (!g_L || g_paniced || !g_ready) return 0;
    sceKernelWaitSema(g_lua_sema, 1, 0);
    if (g_paniced) { sceKernelSignalSema(g_lua_sema, 1); return 0; }
    return 1;
}
static void lua_leave(void) { sceKernelSignalSema(g_lua_sema, 1); }

/* Lua's default panic calls abort() -> sceKernelExitGame on PSP. Keep the
 * game alive: release the lock so waiting threads don't hang, then park. */
static int lua_host_panic(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);
    mhfu_log("[lua_host] PANIC (VM disabled, game kept alive): %s",
             msg ? msg : "?");
    g_paniced = 1;
    if (g_lua_sema >= 0) sceKernelSignalSema(g_lua_sema, 1);
    for (;;) sceKernelDelayThread(1000 * 1000);
    return 0; /* unreachable */
}

/* ------------------------------------------------- mhfu.* read bindings */

static int lb_log(lua_State *L) { mhfu_log("%s", luaL_checkstring(L, 1)); return 0; }
static int lb_read_u8 (lua_State *L) { lua_pushinteger(L, mhfu_read_u8 ((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_read_u16(lua_State *L) { lua_pushinteger(L, mhfu_read_u16((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_read_u32(lua_State *L) { lua_pushinteger(L, (lua_Integer)mhfu_read_u32((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_mem_valid(lua_State *L){ lua_pushboolean(L, mhfu_mem_valid((uint32_t)luaL_checkinteger(L,1))); return 1; }

static int lb_write_u8 (lua_State *L){ mhfu_write_u8 ((uint32_t)luaL_checkinteger(L,1), (uint8_t )luaL_checkinteger(L,2)); return 0; }
static int lb_write_u16(lua_State *L){ mhfu_write_u16((uint32_t)luaL_checkinteger(L,1), (uint16_t)luaL_checkinteger(L,2)); return 0; }
static int lb_write_u32(lua_State *L){ mhfu_write_u32((uint32_t)luaL_checkinteger(L,1), (uint32_t)luaL_checkinteger(L,2)); return 0; }

static int lb_screen_state(lua_State *L){ lua_pushinteger(L, mhfu_get_screen_state()); return 1; }
static int lb_area_index (lua_State *L){ lua_pushinteger(L, mhfu_get_area_index());  return 1; }
static int lb_quest_timer(lua_State *L){ lua_pushinteger(L, (lua_Integer)mhfu_get_quest_timer()); return 1; }

static int lb_entity_at  (lua_State *L){ lua_pushinteger(L, (lua_Integer)mhfu_entity_at((int)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_type(lua_State *L){ lua_pushinteger(L, mhfu_entity_type((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_hp  (lua_State *L){ lua_pushinteger(L, mhfu_entity_hp  ((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_size(lua_State *L){ lua_pushnumber (L, mhfu_entity_size((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_size(lua_State *L){ mhfu_entity_set_size((uint32_t)luaL_checkinteger(L,1), (float)luaL_checknumber(L,2)); return 0; }
static int lb_entity_alive(lua_State *L){ lua_pushboolean(L, mhfu_entity_is_alive((uint32_t)luaL_checkinteger(L,1))); return 1; }

/* mhfu.entities_of_type(type) -> { ptr, ptr, ... } */
static int lb_entities_of_type(lua_State *L)
{
    int type = (int)luaL_checkinteger(L, 1);
    uint32_t buf[16];
    int n = mhfu_entities_of_type((mhfu_monster_id_t)type, buf, 16);
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        lua_pushinteger(L, (lua_Integer)buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ------------------------------------------------ mhfu.* quest / ai helpers */

static int lb_quest_has(lua_State *L)
{
    lua_pushboolean(L, mhfu_quest_has((mhfu_quest_t)luaL_checkinteger(L,1),
                                      (mhfu_monster_id_t)luaL_checkinteger(L,2)));
    return 1;
}
static int lb_quest_replace_monster(lua_State *L)
{
    mhfu_hook_rc_t rc = mhfu_quest_replace_monster(
        (mhfu_quest_t)luaL_checkinteger(L,1),
        (mhfu_monster_id_t)luaL_checkinteger(L,2),
        (mhfu_monster_id_t)luaL_checkinteger(L,3));
    lua_pushboolean(L, rc == MHFU_HOOK_OK);
    return 1;
}
static int lb_action_ptr_for(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_action_ptr_for(
        (uint8_t)luaL_checkinteger(L,1), (uint16_t)luaL_checkinteger(L,2)));
    return 1;
}

/* ------------------------------------------------- event bridge (Lua refs) */

static int g_inst_quest   = 0;          /* C trampoline installed?         */
static int g_inst_spawn   = 0;
static int g_inst_death   = 0;
static int g_inst_overlay = 0;
static int g_inst_slot    = 0;
static int g_inst_input   = 0;
static int g_inst_decided = 0;
static int g_inst_action  = 0;

static int r_quest   = LUA_NOREF;       /* registry ref to the Lua handler */
static int r_spawn   = LUA_NOREF;
static int r_death   = LUA_NOREF;
static int r_overlay = LUA_NOREF;
static int r_slot    = LUA_NOREF;
static int r_input   = LUA_NOREF;
static int r_decided = LUA_NOREF;
static int r_action  = LUA_NOREF;

/* Replace the stored ref with the function at stack index 1. */
static void store_ref(lua_State *L, int *slot)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (*slot != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, *slot);
    lua_pushvalue(L, 1);
    *slot = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Push the action ctx table { entity, type, slot, input }. */
static void push_action_ctx(lua_State *L, uint32_t ent, uint8_t type,
                            uint8_t slot, uint16_t input)
{
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)ent);   lua_setfield(L, -2, "entity");
    lua_pushinteger(L, type);               lua_setfield(L, -2, "type");
    lua_pushinteger(L, slot);               lua_setfield(L, -2, "slot");
    lua_pushinteger(L, input);              lua_setfield(L, -2, "input");
}

/* ------------------------------------------------------------ marshal
 * The engine's game thread (the AI-tick thread that invokes the override
 * callbacks) corrupts Lua HEAP ALLOCATION performed in its context: a fresh
 * table's node array collapses so every key reads back as the last value.
 * Bisected exhaustively (docs / session log): independent of allocator (slab
 * AND newlib malloc), GC (corrupts with GC stopped), FPU, $gp, and C-stack
 * headroom; writing a pre-allocated table on the game thread is fine, and ALL
 * of it works perfectly on the framework's own threads. It is a per-thread
 * emulation quirk we cannot fix at the Lua source.
 *
 * Fix: game-thread override callbacks NEVER touch the VM directly. They pack
 * the request, wake a dedicated exec thread (a known-good context), and block
 * for the result. spawn/death (poll thread) and mhfu_tick (worker) already run
 * on good threads, so they call the VM directly. */
enum { REQ_NONE = 0, REQ_INPUT, REQ_DECIDED, REQ_SLOT, REQ_OVERLAY, REQ_QUEST,
       REQ_ACTIONSEL };

typedef struct {
    int      kind;
    uint32_t entity;     /* entity_ptr, or quest ptr for REQ_QUEST            */
    uint8_t  mtype;
    uint8_t  slot;
    uint16_t aux;        /* action_count (slot) / vt8_input (decided ctx)     */
    uint32_t in;         /* engine_value / cur                                */
    uint32_t out;        /* result, defaults to `in` (passthrough)            */
} lua_req_t;

static volatile lua_req_t g_req;
static SceUID g_req_sema  = -1;   /* exec thread blocks here for work          */
static SceUID g_resp_sema = -1;   /* the game-thread caller blocks here        */
static volatile int g_exec_ready;

/* ---- Lua-side dispatch — ALWAYS runs on the exec thread. ---- */

/* action_input handler: function(ctx) -> new_input, where ctx is
 * { entity, type, slot, input }. PRE-call hook: mutate ctx.input by returning
 * a new vt8_input. (Ctx tables are safe now that all game-thread Lua runs on
 * the exec thread — see the marshal block.) */
static uint32_t dispatch_input(lua_State *L, const volatile lua_req_t *q)
{
    uint32_t ret = q->in;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_input);
    push_action_ctx(L, q->entity, q->mtype, q->slot, (uint16_t)q->in);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
        if (lua_isnumber(L, -1)) ret = (uint16_t)lua_tointeger(L, -1);
    } else {
        mhfu_log("[lua_host] action_input err: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return ret;
}

static uint32_t dispatch_decided(lua_State *L, const volatile lua_req_t *q)
{
    uint32_t ret = q->in;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_decided);
    push_action_ctx(L, q->entity, q->mtype, q->slot, q->aux);
    lua_pushinteger(L, (lua_Integer)q->in);
    if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
        if (lua_isnumber(L, -1)) ret = (uint32_t)lua_tointeger(L, -1);
    } else {
        mhfu_log("[lua_host] action_decided err: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return ret;
}

/* on_bigmonster_action handler: function(ctx) -> new_action_id, where ctx is
 * { entity, type, action_id }. The COHERENT force seam (executor 0x09AC5228):
 * the returned id is fanned to all body slots by the engine itself. */
static uint32_t dispatch_actionsel(lua_State *L, const volatile lua_req_t *q)
{
    uint32_t ret = q->in;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_action);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)q->entity); lua_setfield(L, -2, "entity");
    lua_pushinteger(L, q->mtype);               lua_setfield(L, -2, "type");
    lua_pushinteger(L, (lua_Integer)q->in);     lua_setfield(L, -2, "action_id");
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
        if (lua_isnumber(L, -1)) ret = (uint32_t)lua_tointeger(L, -1);
    } else {
        mhfu_log("[lua_host] action(sel) err: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return ret;
}

static uint32_t dispatch_slot(lua_State *L, const volatile lua_req_t *q)
{
    uint32_t ret = q->in;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_slot);
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)q->entity); lua_setfield(L, -2, "entity");
    lua_pushinteger(L, q->mtype);               lua_setfield(L, -2, "type");
    lua_pushinteger(L, q->slot);                lua_setfield(L, -2, "slot");
    lua_pushinteger(L, q->aux);                 lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)q->in);
    if (lua_pcall(L, 2, 1, 0) == LUA_OK) {
        if (lua_isnumber(L, -1)) {
            int s = (int)lua_tointeger(L, -1);
            /* ai.h: an OOB slot crashes the engine — clamp to valid range. */
            if (s >= 0 && s < (int)q->aux) ret = (uint8_t)s;
        }
    } else {
        mhfu_log("[lua_host] slot_picked err: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return ret;
}

static void dispatch_overlay(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_overlay);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] overlay_loaded err: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

static void dispatch_quest(lua_State *L, const volatile lua_req_t *q)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_quest);
    lua_pushinteger(L, (lua_Integer)q->entity);   /* quest ptr stored in entity */
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] quest_targets_building err: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

/* The exec thread: serve one marshalled request per loop. Always signals the
 * response sema (even on VM-down) so the blocked game thread never hangs. */
static int exec_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelWaitSema(g_req_sema, 1, 0);
        uint32_t out = g_req.in;          /* default = passthrough */
        if (lua_enter()) {
            lua_State *L = g_L;
            switch (g_req.kind) {
            case REQ_INPUT:   out = dispatch_input(L, &g_req);   break;
            case REQ_DECIDED: out = dispatch_decided(L, &g_req); break;
            case REQ_SLOT:    out = dispatch_slot(L, &g_req);    break;
            case REQ_OVERLAY: dispatch_overlay(L);               break;
            case REQ_QUEST:   dispatch_quest(L, &g_req);         break;
            case REQ_ACTIONSEL: out = dispatch_actionsel(L, &g_req); break;
            default: break;
            }
            lua_leave();
        }
        g_req.out = out;
        sceKernelSignalSema(g_resp_sema, 1);
    }
    return 0;
}

/* Game-thread marshal: package the request, wake the exec thread, block for
 * the result. Single producer (the engine's AI-tick thread is blocked between
 * the signal and the wait), so no request lock is needed. */
static uint32_t marshal(int kind, uint32_t entity, uint8_t mtype,
                        uint8_t slot, uint16_t aux, uint32_t in)
{
    if (!g_exec_ready) return in;
    g_req.kind   = kind;
    g_req.entity = entity;
    g_req.mtype  = mtype;
    g_req.slot   = slot;
    g_req.aux    = aux;
    g_req.in     = in;
    g_req.out    = in;
    sceKernelSignalSema(g_req_sema, 1);
    sceKernelWaitSema(g_resp_sema, 1, 0);
    return g_req.out;
}

/* --- C trampolines: the framework calls these on the GAME thread; they
 * marshal to the exec thread (above), never touching the VM here. --- */
static uint32_t tramp_decided(const mhfu_action_decision_ctx_t *ctx,
                              uint32_t engine_value)
{
    if (r_decided == LUA_NOREF) return engine_value;
    return marshal(REQ_DECIDED, ctx->entity_ptr, ctx->monster_type, ctx->slot,
                   ctx->vt8_input, engine_value);
}

static uint16_t tramp_input(const mhfu_action_input_ctx_t *ctx, uint16_t cur)
{
    if (r_input == LUA_NOREF) return cur;
    return (uint16_t)marshal(REQ_INPUT, ctx->entity_ptr, ctx->monster_type,
                             ctx->slot, 0, cur);
}

/* on_bigmonster_action: fires on the game thread at executor entry. */
static uint32_t tramp_action(const mhfu_action_sel_ctx_t *ctx, uint32_t a1)
{
    if (r_action == LUA_NOREF) return a1;
    return marshal(REQ_ACTIONSEL, ctx->entity_ptr, ctx->monster_type, 0, 0, a1);
}

static uint8_t tramp_slot(const mhfu_slot_picked_ctx_t *ctx, uint8_t cur)
{
    if (r_slot == LUA_NOREF) return cur;
    return (uint8_t)marshal(REQ_SLOT, ctx->entity_ptr, ctx->monster_type,
                            ctx->original_slot, ctx->action_count, cur);
}

static void tramp_overlay(const mhfu_ai_overlay_ctx_t *ctx)
{
    (void)ctx;
    if (r_overlay == LUA_NOREF) return;
    marshal(REQ_OVERLAY, 0, 0, 0, 0, 0);
}

static void tramp_spawn(const mhfu_bigmonster_spawn_ctx_t *ctx)
{
    if (r_spawn == LUA_NOREF || !lua_enter()) return;   /* poll thread = direct */
    lua_State *L = g_L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_spawn);
    lua_pushinteger(L, (lua_Integer)ctx->entity_ptr);
    lua_pushinteger(L, ctx->monster_type);
    lua_pushinteger(L, ctx->slot);
    lua_pushinteger(L, ctx->initial_hp);
    if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] spawn err: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_leave();
}

static void tramp_death(const mhfu_bigmonster_death_ctx_t *ctx)
{
    if (r_death == LUA_NOREF || !lua_enter()) return;
    lua_State *L = g_L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_death);
    lua_pushinteger(L, (lua_Integer)ctx->entity_ptr);
    lua_pushinteger(L, ctx->monster_type);
    lua_pushinteger(L, ctx->slot);
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] death err: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_leave();
}

/* quest_targets_building fires on the engine's quest-load (game) thread, so it
 * marshals to the exec thread like the other game-thread callbacks. */
static void tramp_quest(const void *vctx)
{
    const mhfu_quest_ctx_t *ctx = (const mhfu_quest_ctx_t *)vctx;
    if (r_quest == LUA_NOREF) return;
    marshal(REQ_QUEST, (uint32_t)ctx->quest, 0, 0, 0, 0);
}

/* --- Lua registration bindings: mhfu.on_<event>(fn [, priority]). --- */

static int lb_on_quest(lua_State *L)
{
    store_ref(L, &r_quest);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_quest) {
        mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                        (mhfu_event_cb_t)tramp_quest, prio);
        g_inst_quest = 1;
    }
    return 0;
}
static int lb_on_spawn(lua_State *L)
{
    store_ref(L, &r_spawn);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_spawn) { mhfu_on_bigmonster_spawn(tramp_spawn, prio); g_inst_spawn = 1; }
    return 0;
}
static int lb_on_death(lua_State *L)
{
    store_ref(L, &r_death);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_death) { mhfu_on_bigmonster_death(tramp_death, prio); g_inst_death = 1; }
    return 0;
}
static int lb_on_overlay(lua_State *L)
{
    store_ref(L, &r_overlay);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_overlay) { mhfu_on_ai_overlay_loaded(tramp_overlay, prio); g_inst_overlay = 1; }
    return 0;
}
static int lb_on_slot(lua_State *L)
{
    store_ref(L, &r_slot);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_slot) { mhfu_on_bigmonster_slot_picked(tramp_slot, prio); g_inst_slot = 1; }
    return 0;
}
static int lb_on_input(lua_State *L)
{
    store_ref(L, &r_input);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_input) { mhfu_on_bigmonster_action_input(tramp_input, prio); g_inst_input = 1; }
    return 0;
}
static int lb_on_decided(lua_State *L)
{
    store_ref(L, &r_decided);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_decided) { mhfu_on_bigmonster_action_decided(tramp_decided, prio); g_inst_decided = 1; }
    return 0;
}
static int lb_on_action(lua_State *L)
{
    store_ref(L, &r_action);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_action) { mhfu_on_bigmonster_action(tramp_action, prio); g_inst_action = 1; }
    return 0;
}

/* ------------------------------------------------------------ api table */

static const luaL_Reg k_mhfu_api[] = {
    { "log",              lb_log },
    { "read_u8",          lb_read_u8 },
    { "read_u16",         lb_read_u16 },
    { "read_u32",         lb_read_u32 },
    { "write_u8",         lb_write_u8 },
    { "write_u16",        lb_write_u16 },
    { "write_u32",        lb_write_u32 },
    { "mem_valid",        lb_mem_valid },
    { "get_screen_state", lb_screen_state },
    { "get_area_index",   lb_area_index },
    { "get_quest_timer",  lb_quest_timer },
    { "entity_at",        lb_entity_at },
    { "entity_type",      lb_entity_type },
    { "entity_hp",        lb_entity_hp },
    { "entity_size",      lb_entity_size },
    { "entity_set_size",  lb_entity_set_size },
    { "entity_alive",     lb_entity_alive },
    { "entities_of_type", lb_entities_of_type },
    { "quest_has",            lb_quest_has },
    { "quest_replace_monster", lb_quest_replace_monster },
    { "action_ptr_for",   lb_action_ptr_for },
    /* AI override-event registration */
    { "on_quest_targets_building",  lb_on_quest },
    { "on_bigmonster_spawn",        lb_on_spawn },
    { "on_bigmonster_death",        lb_on_death },
    { "on_ai_overlay_loaded",       lb_on_overlay },
    { "on_bigmonster_slot_picked",  lb_on_slot },
    { "on_bigmonster_action_input", lb_on_input },
    { "on_bigmonster_action_decided", lb_on_decided },
    { "on_bigmonster_action",       lb_on_action },
    { 0, 0 },
};

static void register_mhfu_api(lua_State *L)
{
    lua_newtable(L);                       /* the mhfu table */
    luaL_setfuncs(L, k_mhfu_api, 0);
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

/* --------------------------------------------------- memstick .lua loading */

static int ends_with_lua(const char *s)
{
    int n = (int)strlen(s);
    if (n < 5) return 0;                 /* need at least "x.lua" */
    return s[n-4] == '.'
        && (s[n-3]|0x20) == 'l'
        && (s[n-2]|0x20) == 'u'
        && (s[n-1]|0x20) == 'a';
}

/* Read one .lua file (by basename), compile, run it (registers its event
 * handlers). The CALLER owns VM serialisation: at setup it's single-threaded;
 * at hot-reload the worker holds lua_enter(). 0 ok / -1 err. */
static int load_lua_file(lua_State *L, const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", LUA_MODS_DIR, name);
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { mhfu_log("[lua_host] open FAILED %s rc=0x%08X", path, (unsigned)fd); return -1; }
    int n = sceIoRead(fd, g_filebuf, sizeof(g_filebuf) - 1);
    sceIoClose(fd);
    if (n < 0) { mhfu_log("[lua_host] read FAILED %s rc=0x%08X", name, (unsigned)n); return -1; }
    if (n >= (int)sizeof(g_filebuf) - 1) {
        mhfu_log("[lua_host] %s too big (>%uB) — skipped", name, (unsigned)sizeof(g_filebuf) - 1);
        return -1;
    }
    g_filebuf[n] = 0;

    char chunk[72];
    snprintf(chunk, sizeof(chunk), "@%s", name);   /* '@' => Lua treats as file */
    if (luaL_loadbuffer(L, g_filebuf, (size_t)n, chunk) != LUA_OK) {
        mhfu_log("[lua_host] compile FAILED %s: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] run FAILED %s: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    mhfu_log("[lua_host] loaded mod %s (%dB)", name, n);
    return 0;
}

/* Scan LUA_MODS_DIR, load every *.lua. Returns count successfully loaded. */
static int load_lua_dir(lua_State *L)
{
    SceUID d = sceIoDopen(LUA_MODS_DIR);
    if (d < 0) {
        mhfu_log("[lua_host] mods dir absent (%s) rc=0x%08X", LUA_MODS_DIR, (unsigned)d);
        return 0;
    }
    int loaded = 0;
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        int r = sceIoDread(d, &ent);
        if (r <= 0) break;                              /* end of dir / error */
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) continue;  /* skip subdirs */
        if (!ends_with_lua(ent.d_name)) continue;
        if (load_lua_file(L, ent.d_name) == 0) loaded++;
    }
    sceIoDclose(d);
    return loaded;
}

/* --------------------------------------------------------- hot reload (P4)
 * Watch each memstick .lua's (size, mtime); when one changes, re-exec it in
 * the same VM. The mod's mhfu.on_*(fn) calls run again and store_ref() unrefs
 * the old handler + stores the new one — the framework's C event hooks are
 * already installed and keep dispatching, now into the fresh closures. So a
 * live edit takes effect with NO hook reinstall and NO cold boot.
 *
 * Scope/limits (Phase 5): one shared global env + one lua_State, so module-
 * level `local` state resets on reload (intended) and two mods still can't own
 * the same event (last wins). Deleting a .lua does NOT unregister its handlers
 * (refs persist) — needs per-mod ref tracking. */
#define MAX_TRACKED 16
typedef struct {
    char           name[64];
    SceOff         size;
    ScePspDateTime mtime;
} tracked_t;
static tracked_t g_tracked[MAX_TRACKED];
static int       g_ntracked;

static int find_tracked(const char *name)
{
    for (int i = 0; i < g_ntracked; i++)
        if (strcmp(g_tracked[i].name, name) == 0) return i;
    return -1;
}

/* Record the current (size, mtime) for `name`. Returns 0 if recorded, -1 if
 * the file can't be stat'd or the table is full. */
static int track_set(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", LUA_MODS_DIR, name);
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(path, &st) < 0) return -1;
    int idx = find_tracked(name);
    if (idx < 0) {
        if (g_ntracked >= MAX_TRACKED) return -1;
        idx = g_ntracked++;
        snprintf(g_tracked[idx].name, sizeof(g_tracked[idx].name), "%s", name);
    }
    g_tracked[idx].size  = st.st_size;
    g_tracked[idx].mtime = st.sce_st_mtime;
    return 0;
}

/* Record current stats for every loaded .lua so the first poll doesn't think
 * everything changed. Called once after the setup load. */
static void prime_tracked(void)
{
    SceUID d = sceIoDopen(LUA_MODS_DIR);
    if (d < 0) return;
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) continue;
        if (!ends_with_lua(ent.d_name)) continue;
        track_set(ent.d_name);
    }
    sceIoDclose(d);
}

/* Poll the mods dir; re-exec any .lua whose (size, mtime) changed or that is
 * new. Runs on the worker thread. Updates the tracked stat BEFORE the reload
 * attempt so a file with a compile error doesn't reload-spin every poll. */
static void hot_reload_scan(void)
{
    SceUID d = sceIoDopen(LUA_MODS_DIR);
    if (d < 0) return;
    int reloaded = 0;
    SceIoDirent ent;
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        if (sceIoDread(d, &ent) <= 0) break;
        if (FIO_S_ISDIR(ent.d_stat.st_mode)) continue;
        if (!ends_with_lua(ent.d_name)) continue;

        char path[160];
        snprintf(path, sizeof(path), "%s/%s", LUA_MODS_DIR, ent.d_name);
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        if (sceIoGetstat(path, &st) < 0) continue;

        int idx = find_tracked(ent.d_name);
        int changed = (idx < 0)
                   || g_tracked[idx].size != st.st_size
                   || memcmp(&g_tracked[idx].mtime, &st.sce_st_mtime,
                             sizeof(ScePspDateTime)) != 0;
        if (!changed) continue;

        track_set(ent.d_name);                 /* update first (avoid spin) */

        if (!lua_enter()) break;               /* VM busy/dead — try later */
        mhfu_log("[lua_host] hot-reload %s ...", ent.d_name);
        if (load_lua_file(g_L, ent.d_name) == 0) {
            lua_getglobal(g_L, "mhfu_tick");
            g_have_tick = lua_isfunction(g_L, -1);
            lua_pop(g_L, 1);
            lua_gc(g_L, LUA_GCCOLLECT, 0);      /* reclaim old closures */
            reloaded++;
        }
        lua_leave();
    }
    sceIoDclose(d);
    if (reloaded)
        mhfu_log("[lua_host] hot-reloaded %d mod(s), live=%uB",
                 reloaded, slab_live());
}

/* ------------------------------------------------------------ host setup */

static int lua_host_setup(void)
{
#if LUA_USE_LIBC_ALLOC
    g_L = lua_newstate(lua_libc_alloc, 0);
#else
    g_L = lua_newstate(lua_slab_alloc, 0);
#endif
    if (!g_L) { mhfu_log("[lua_host] lua_newstate FAILED (slab OOM?)"); return -1; }
    lua_atpanic(g_L, lua_host_panic);

    luaL_requiref(g_L, "_G",     luaopen_base,   1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "table",  luaopen_table,  1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "string", luaopen_string, 1); lua_pop(g_L, 1);
    luaL_requiref(g_L, "math",   luaopen_math,   1); lua_pop(g_L, 1);
    remove_unsafe_globals(g_L);
    register_mhfu_api(g_L);

    /* Phase 3: load mods from memstick .lua files. Fall back to the embedded
     * tigrex_spin only if the mods dir is empty/absent (never brick the PRX). */
    int loaded = load_lua_dir(g_L);
    if (loaded == 0) {
        mhfu_log("[lua_host] no memstick mods — using embedded tigrex_spin fallback");
        if (luaL_loadstring(g_L, k_lua_tigrex_spin) != LUA_OK) {
            mhfu_log("[lua_host] load embedded FAILED: %s", lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
            return -1;
        }
        if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
            mhfu_log("[lua_host] run embedded FAILED: %s", lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
            return -1;
        }
        loaded = 1;
    }
    mhfu_log("[lua_host] %d mod(s) loaded", loaded);
    prime_tracked();   /* record current file stats for hot-reload polling */

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
    if (!lua_enter()) return;
    lua_getglobal(g_L, "mhfu_tick");
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] mhfu_tick error: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
    }
    lua_leave();
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sceKernelDelayThread(3 * 1000 * 1000);   /* let game + framework settle */
    for (;;) {
        sceKernelDelayThread(500 * 1000);    /* 2 Hz */
        call_tick();
        hot_reload_scan();                   /* Phase 4: live .lua reload */
    }
    return 0;
}

static int lua_host_init(void)
{
    mhfu_log("[lua_host] free mem before slab: max=%u total=%u",
             (unsigned)sceKernelMaxFreeMemSize(),
             (unsigned)sceKernelTotalFreeMemSize());

    g_lua_sema = sceKernelCreateSema("mhfu_lua_lock", 0, 1, 1, 0);
    if (g_lua_sema < 0) { mhfu_log("[lua_host] CreateSema FAILED rc=0x%08X", g_lua_sema); return -1; }

    g_slab_uid = sceKernelAllocPartitionMemory(
        2 /* PSP_MEMORY_PARTITION_USER */, "mhfu_lua_slab",
        PSP_SMEM_Low, LUA_SLAB_BYTES + 64, 0);
    if (g_slab_uid < 0) {
        mhfu_log("[lua_host] AllocPartitionMemory FAILED rc=0x%08X", g_slab_uid);
        return -1;
    }
    mhfu_log("[lua_host] free mem after %uKB slab: max=%u total=%u",
             (unsigned)(LUA_SLAB_BYTES / 1024),
             (unsigned)sceKernelMaxFreeMemSize(),
             (unsigned)sceKernelTotalFreeMemSize());
    void *base = sceKernelGetBlockHeadAddr(g_slab_uid);
    unsigned a = ((unsigned)base + 7u) & ~7u;
    slab_init((void *)a, LUA_SLAB_BYTES);

    if (lua_host_setup() != 0) return -1;

    /* Only now allow the event trampolines to enter the VM. The setup above
     * may have already installed framework hooks (spawn poll thread); g_ready
     * gates them until the VM + refs are fully built. */
    g_ready = 1;

    /* Exec thread: runs ALL game-thread override callbacks' Lua work in a
     * known-good context (the engine's AI-tick thread corrupts Lua heap
     * allocation — see the marshal block). Higher priority than the worker so
     * it preempts to answer the blocked game thread quickly. */
    g_req_sema  = sceKernelCreateSema("mhfu_lua_req",  0, 0, 1, 0);
    g_resp_sema = sceKernelCreateSema("mhfu_lua_resp", 0, 0, 1, 0);
    if (g_req_sema < 0 || g_resp_sema < 0) {
        mhfu_log("[lua_host] marshal sema create FAILED"); return -1;
    }
    SceUID ex = sceKernelCreateThread("mhfu_lua_exec",
                                      (SceKernelThreadEntry)exec_thread,
                                      0x11, 0x10000, 0, 0);
    if (ex < 0) { mhfu_log("[lua_host] exec CreateThread FAILED"); return -1; }
    sceKernelStartThread(ex, 0, 0);
    g_exec_ready = 1;

    SceUID th = sceKernelCreateThread("mhfu_lua_host",
                                      (SceKernelThreadEntry)worker,
                                      0x18, 0x10000, 0, 0);
    if (th < 0) { mhfu_log("[lua_host] CreateThread FAILED"); return -1; }
    sceKernelStartThread(th, 0, 0);
    mhfu_log("[lua_host] init OK, exec + poll threads started");
    return 0;
}

static void lua_host_shutdown(void)
{
    g_ready = 0;
    g_exec_ready = 0;
    if (g_L) { lua_close(g_L); g_L = 0; }
    if (g_slab_uid >= 0) { sceKernelFreePartitionMemory(g_slab_uid); g_slab_uid = -1; }
    if (g_lua_sema >= 0) { sceKernelDeleteSema(g_lua_sema); g_lua_sema = -1; }
    if (g_req_sema >= 0) { sceKernelDeleteSema(g_req_sema); g_req_sema = -1; }
    if (g_resp_sema >= 0) { sceKernelDeleteSema(g_resp_sema); g_resp_sema = -1; }
}

MHFU_MOD(.id = "lua_host", .version = "0.4",
         .needs = 0, .conflicts = 0,
         .init = lua_host_init, .shutdown = lua_host_shutdown);
