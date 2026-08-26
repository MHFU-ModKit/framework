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
 * Heap: a dedicated 256 KB slab (sceKernelAllocPartitionMemory) with a
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
#include <pspctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "mhfu/mhfu.h"
#include "mhfu/mips.h"
#include "mhfu/bigmon_overlay.h"
#include "mhfu/inject.h"

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
/* Ergonomic OO layer (mhfu.world / mhfu.entity / mhfu.mem) — run before any
 * user mod so scripts can use handles instead of raw addresses. */
#include "scripts/_prelude.lua.h"

#define LUA_MODS_DIR "ms0:/PSP/PLUGINS/mhfu_framework/mods"
/* Per-file .lua read scratch. Allocated dynamically at init (NOT a .bss array) so
 * it doesn't inflate the PRX's contiguous load image — that 48 KB mattered for
 * fitting MHFU's bare 24 MB user partition on real hardware. */
#define G_FILEBUF_SZ (48 * 1024)
static char  *g_filebuf;
static SceUID g_filebuf_uid = -1;

/* ------------------------------------------------------------------ slab
 * A self-contained implicit-free-list allocator over a fixed slab. Lua's
 * realloc-heavy pattern is fully supported (malloc/free/realloc + forward
 * coalescing).
 *
 * 🔴 SIZED 2026-08-26, and the old 64 KB was a real ceiling, not a margin. The
 * VM plus the prelude plus ONE ~10 KB mod already sat at live=63776B of 65536 —
 * a second mod (the ported-monster runtime) could not compile at all, and the
 * failure was invisible because mhfu_log drops everything before the game
 * reaches an area, so boot-time compile errors never reached framework.log. A
 * mod that never loaded looked exactly like a mod that did nothing.
 *
 * 256 KB is ~1% of the 24 MB user partition on real hardware, which is the
 * constraint that made this small in the first place. `[lua_host] VM ready`
 * logs live and peak — size it off that rather than off a guess. */
#define LUA_SLAB_BYTES (256 * 1024)

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

/* Native float access — replaces the hand-rolled IEEE-754 packing in Lua. */
static int lb_read_f32 (lua_State *L){ lua_pushnumber(L, mhfu_read_f32((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_write_f32(lua_State *L){ mhfu_write_f32((uint32_t)luaL_checkinteger(L,1), (float)luaL_checknumber(L,2)); return 0; }

static int lb_screen_state(lua_State *L){ lua_pushinteger(L, mhfu_get_screen_state()); return 1; }
static int lb_area_index (lua_State *L){ lua_pushinteger(L, mhfu_get_area_index());  return 1; }
static int lb_quest_timer(lua_State *L){ lua_pushinteger(L, (lua_Integer)mhfu_get_quest_timer()); return 1; }

static int lb_entity_at  (lua_State *L){ lua_pushinteger(L, (lua_Integer)mhfu_entity_at((int)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_type(lua_State *L){ lua_pushinteger(L, mhfu_entity_type((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_hp  (lua_State *L){ lua_pushinteger(L, mhfu_entity_hp  ((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_size(lua_State *L){ lua_pushnumber (L, mhfu_entity_size((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_size(lua_State *L){ mhfu_entity_set_size((uint32_t)luaL_checkinteger(L,1), (float)luaL_checknumber(L,2)); return 0; }
static int lb_entity_alive(lua_State *L){ lua_pushboolean(L, mhfu_entity_is_alive((uint32_t)luaL_checkinteger(L,1))); return 1; }

/* Typed entity accessors (Tier 1 — wrap the C SDK in include/mhfu/entity.h). */
static int lb_entity_pos(lua_State *L)
{
    mhfu_vec3_t p = mhfu_entity_pos((uint32_t)luaL_checkinteger(L,1));
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}
static int lb_entity_set_pos(lua_State *L)
{
    mhfu_vec3_t p = { (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3),
                      (float)luaL_checknumber(L,4) };
    mhfu_entity_set_pos((uint32_t)luaL_checkinteger(L,1), p);
    return 0;
}
static int lb_entity_yaw(lua_State *L){ lua_pushinteger(L, mhfu_entity_yaw((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_yaw(lua_State *L){ mhfu_entity_set_yaw((uint32_t)luaL_checkinteger(L,1), (uint16_t)luaL_checkinteger(L,2)); return 0; }
static int lb_entity_ai_state(lua_State *L){ lua_pushinteger(L, mhfu_entity_ai_state((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_ai_state(lua_State *L){ mhfu_entity_set_ai_state((uint32_t)luaL_checkinteger(L,1), (uint8_t)luaL_checkinteger(L,2)); return 0; }
static int lb_entity_engaged(lua_State *L){ lua_pushboolean(L, mhfu_entity_engaged((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_engaged(lua_State *L){ mhfu_entity_set_engaged((uint32_t)luaL_checkinteger(L,1), lua_toboolean(L,2)); return 0; }
static int lb_entity_calm(lua_State *L){ mhfu_entity_calm((uint32_t)luaL_checkinteger(L,1)); return 0; }
static int lb_entity_section(lua_State *L){ lua_pushinteger(L, mhfu_entity_section((uint32_t)luaL_checkinteger(L,1))); return 1; }
static int lb_entity_set_section(lua_State *L){ mhfu_entity_set_section((uint32_t)luaL_checkinteger(L,1), (uint16_t)luaL_checkinteger(L,2)); return 0; }

/* Tier 2 — high-level intents lifted out of mods. */
static int lb_entity_make_visible(lua_State *L)
{
    mhfu_entity_make_visible((uint32_t)luaL_checkinteger(L,1), (uint16_t)luaL_checkinteger(L,2));
    return 0;
}
static int lb_entity_force_aggro(lua_State *L)
{
    mhfu_vec3_t t = { (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3),
                      (float)luaL_checknumber(L,4) };
    mhfu_entity_force_aggro((uint32_t)luaL_checkinteger(L,1), t);
    return 0;
}
static int lb_player_pos(lua_State *L)
{
    mhfu_vec3_t p = mhfu_player_pos();
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}
static int lb_player_hp(lua_State *L){ lua_pushinteger(L, (lua_Integer)mhfu_get_player_hp()); return 1; }
static int lb_paint_map(lua_State *L){ (void)L; mhfu_paint_map(); return 0; }

/* mhfu.resolve_attack(entity) — run the engine's OWN per-monster attack resolver
 * for `entity`. This is z_un_08865934(entity): it advances the attack-state
 * timers and (if entity+0x33C==0) calls entity->vt[0x3C](entity) = the full
 * attack resolution (hitboxes, per-body-part, damage formula -> player applier
 * 0x088D6594). The engine's combat enumeration only resolves the ~2 manager-
 * registered combatants, so an injected CLONE never gets here and deals 0 damage
 * (RE this session). Calling this for a clone makes the engine compute + apply
 * its damage exactly like a native — no reimplementation, no manager.
 *
 * THREADING: this is a heavy engine call. It MUST run game-thread-synced — i.e.
 * from inside an override callback (which executes on the exec thread WHILE the
 * game thread is blocked == engine quiescent). Do NOT call it from mhfu_tick()
 * (the free-running 2 Hz worker) — that races the engine. */
typedef void (*mhfu_attack_resolver_fn)(uint32_t entity);
#define MHFU_ATTACK_RESOLVER 0x08865934u
static int lb_resolve_attack(lua_State *L)
{
    uint32_t e = (uint32_t)luaL_checkinteger(L, 1);
    if (e >= 0x08000000u && e < 0x0C000000u) ((mhfu_attack_resolver_fn)MHFU_ATTACK_RESOLVER)(e);
    return 0;
}

/* Per-frame, game-thread CLONE COMBAT DRIVER.
 * Registered as an ai_step prefix (mhfu_on_bigmonster_ai_step) — fires per big
 * monster, per frame, ON THE GAME THREAD (prefix on z_un_08865648, the per-entity
 * AI tick). The engine's combat enumeration only resolves the ~2 manager-
 * registered combatants, so a CLONE gets its AI/movement tick but never its
 * attack-resolve tick -> it roams but its attack state never advances and it
 * deals 0 damage. Here, for each extra-RAM clone, we run the engine attack
 * resolver z_un_08865934(clone) every frame — same cadence the engine gives the
 * native — so the clone advances its attack state AND resolves hits. No marshal
 * (this is a C cb, runs inline on the game thread); no manager. */
typedef void (*mhfu_entity_tick_fn)(uint32_t entity);
#define MHFU_AI_TICK 0x08865648u   /* z_un_08865648 = per-entity AI tick (movement/pick) */
typedef void (*mhfu_executor_fn)(uint32_t entity, uint32_t a1, uint32_t a2, uint32_t a3);
#define MHFU_EXECUTOR 0x09AC5228u  /* big-mon action executor f(entity, a1=action_id,..) */
#define CLONE_SPIN_A1 0x2Bu        /* ANGRY_SPIN (tigrex): a1=0x2B (memory big-mon-action-seam) */
#define FREEZE_GATE_OFF 0x4B8u     /* clear bits 0x100|0x10000 each force or AI tick halts */

/* Clone list the driver ticks. The engine enumerates AI via the REGISTRY, which
 * holds only the native — clones (extra RAM, not registry-listed, not chained)
 * never get z_un_08865648, so they never run their own AI and never attack. Lua
 * pushes the live clone pointers here each tick (mhfu.clones_set). */
static uint32_t      g_clones[12];
static volatile int  g_clone_count  = 0;
static volatile int  g_clone_resolve = 0;

/* ai_step prefix: fires per big monster per frame on the game thread. The NATIVE
 * is registry-enumerated so it fires every frame; we hang the swarm off it. For
 * each clone we run BOTH the AI tick (so it picks + drives attacks like a real
 * monster) and the attack resolver (so the active attack's hitbox lands damage).
 * Both run inline on the game thread, mid-frame — the same context the native
 * gets. We drive only when fired for the native (low RAM) so the nested clone
 * ticks (which re-enter this prefix with a clone ptr) don't recurse. */
static void clone_combat_step(const mhfu_ai_step_ctx_t *ctx)
{
    if (!g_clone_resolve) return;
    if (ctx->entity_ptr >= 0x0A000000u) return;   /* clone re-entry: bail (no recursion) */
    int n = g_clone_count;
    static int dbg = 0;
    int log_now = ((dbg++ % 180) == 0);   /* ~ every 3 s @60fps */
    if (log_now)
        mhfu_log("[clonecmb] fire nat=0x%08X resolve=%d count=%d c0=0x%08X",
                 ctx->entity_ptr, g_clone_resolve, n, n > 0 ? g_clones[0] : 0);
    for (int i = 0; i < n; i++) {
        uint32_t c = g_clones[i];
        if (c >= 0x0A000000u && c < 0x0C000000u) {
            uint8_t ai0 = mhfu_read_u8(c + 0x334);
            /* Natural behaviour: drive the clone's own AI tick (it aggros on sight +
             * runs its own attack patterns, like a real Tigrex) then run the attack
             * resolver so its active-attack hitbox can land on the player. The AI tick
             * is what makes them attack — do NOT puppet/force a fixed action. */
            ((mhfu_entity_tick_fn)MHFU_AI_TICK)(c);              /* AI: aggro + attack patterns */
            ((mhfu_attack_resolver_fn)MHFU_ATTACK_RESOLVER)(c); /* resolve active attack -> damage */
            if (log_now && i == 0)
                mhfu_log("[clonecmb]  c0=0x%08X AISTATE %d->%d 0x33C=%d eng=%d",
                         c, ai0, mhfu_read_u8(c + 0x334), mhfu_read_u8(c + 0x33C),
                         (int)mhfu_read_u32(c + 0x5DC));
        }
    }
}
/* mhfu.clone_combat(enable) — turn the per-frame clone driver on/off. The ai_step
 * detour is installed LAZILY on first enable (not at init) so a default-off config
 * never patches z_un_08865648 — keeping quest entry on the known-stable path.
 * NOTE: the driver re-enters z_un_08865648 from inside the ai_step prefix, which
 * can misalign the stack for the engine's nested VFPU-quad transform code (observed
 * crash: alignment at 088652dc). EXPERIMENTAL — leave off unless testing. */
static int lb_clone_combat(lua_State *L)
{
    int en = lua_toboolean(L, 1);
    g_clone_resolve = en;
    static int s_installed = 0;
    if (en && !s_installed) { mhfu_on_bigmonster_ai_step(clone_combat_step, 50); s_installed = 1; }
    return 0;
}
/* mhfu.clones_set({ptr,ptr,...}) — set the live clone pointers the driver ticks. */
static int lb_clones_set(lua_State *L)
{
    int n = 0;
    if (lua_istable(L, 1)) {
        int len = (int)lua_rawlen(L, 1);
        for (int i = 1; i <= len && n < 12; i++) {
            lua_rawgeti(L, 1, i);
            uint32_t v = (uint32_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (v) g_clones[n++] = v;
        }
    }
    g_clone_count = n;
    return 0;
}

/* ---- Path 1b: NATIVE combat-node registration (real clone damage) ----------
 * RE 2026-06-14 (memory combat-registration-node-gate): a monster damages the
 * player only if it owns a node in the per-frame collision list. The engine
 * builds that node, for each managed combatant EVERY FRAME, via:
 *   0x09B661EC(mgr_this, entity, idx):
 *      if 0x8865bac(entity) != 1 return        ; GATE: entity+0x29a == cur section
 *      node = 0x8859b44(g+0x5000, 0xe0, 0x10)  ; per-frame pool alloc
 *      0x09D4B318(node, entity, idx)           ; init fields + splice into list
 * A clone passes the section gate (the shepherd keeps clone+0x29a == cur section),
 * so calling 0x09B661EC(mgr, clone, idx) ourselves each frame makes the engine
 * build a REAL node for the clone -> it becomes a genuine combatant that frame ->
 * native hitbox/per-part/formula damage. The node is a per-FRAME transient (pool
 * reset+rebuilt each frame), so teardown is uniform (no dangling-node crash that
 * a STATIC injected node caused). a0 = manager `this` (must be nonzero — gates the
 * alloc branch). Integer + single-float only (no VFPU quads) -> lower call risk. */
/* Native per-frame node builder: jal 0x9b661e8 (from 0x09D383A0 &c) with
 * a0 = g = [0x09C18FD0] (collision global), a1 = entity, a2 = idx byte.
 * (Entry is 0x09B661E8 — the +4 mistake skipped `addiu sp,-0x10` -> stack corrupt
 * -> NULL node -> Write@0x10. And a0 is g, NOT the manager.) */
typedef void (*mhfu_combat_build_fn)(uint32_t g, uint32_t entity, uint32_t idx);
#define MHFU_COMBAT_BUILD    0x09B661E8u
#define MHFU_COLL_GLOBAL_PTR 0x09C18FD0u
#define MHFU_COMBAT_IDX      0x0Eu          /* the idx the native uses (0x09D383A4) */
static volatile int g_combat_nodes = 0;

/* IN-CONTEXT via JIT-IMMUNE FIELD SWAP (Path 1b v6). The overlay detours (v4/v5)
 * were install-timing-blocked (the builder/call-site overlay regions aren't
 * covered by the framework's cold-window helper). But the per-frame drive calls
 * the per-monster PROCESSOR through a function-pointer FIELD: manager+0xC
 * (0x08B0C7CC) = 0x09A60198, dispatched via JALR. A pointer field is a DATA word
 * → swapping it is JIT-IMMUNE (no cold window needed), the production override
 * mechanism. We point it at a cave stub: call the original processor (native node
 * built) THEN build clone nodes — in-context (pool live), in the same per-frame
 * drive, before the consumer. Re-applied each tick (survives quest reload). */
extern "C" uint32_t *mhfu_cave_alloc(int n_insns);
#define MGR_PROC_SLOT 0x08B0C7CCu   /* manager+0xC: fn-ptr the per-frame drive JALRs */
#define MGR_PROC_ORIG 0x09A60198u   /* per-monster processor it normally points to    */
static uint32_t g_combat_stub = 0;

/* Called by the stub AFTER the engine's per-monster processor (native node built),
 * on the game thread, in-context. Build a node for each clone. */
#define MGR_FRAME_CTR 0x08B0C7F0u   /* manager+0x30: ++ once per drive (frame) */
/* Does any node in the collision list have +0x10 == entity? (the engine NEVER
 * removes our injected nodes — building unconditionally floods the pool, count 75
 * — so we add a clone's node only when it has none, self-healing.) */
static int node_in_list(uint32_t g, uint32_t entity)
{
    uint32_t n = mhfu_read_u32(g + 0x502c);
    for (int i = 0; i < 96 && n >= 0x08000000u && n < 0x0C000000u; i++) {
        if (mhfu_read_u32(n + 0x10) == entity) return 1;
        n = mhfu_read_u32(n + 0x04);
    }
    return 0;
}
static uint32_t find_node(uint32_t g, uint32_t entity)
{
    uint32_t n = mhfu_read_u32(g + 0x502c);
    for (int i = 0; i < 96 && n >= 0x08000000u && n < 0x0C000000u; i++) {
        if (mhfu_read_u32(n + 0x10) == entity) return n;
        n = mhfu_read_u32(n + 0x04);
    }
    return 0;
}
/* The first node whose entity backref is a low-RAM (native) monster = the fully
 * engine-populated template. */
static uint32_t find_native_node(uint32_t g)
{
    uint32_t n = mhfu_read_u32(g + 0x502c);
    for (int i = 0; i < 96 && n >= 0x08000000u && n < 0x0C000000u; i++) {
        uint32_t e = mhfu_read_u32(n + 0x10);
        if (e >= 0x08000000u && e < 0x0A000000u && mhfu_read_u8(e + 0x1e8) == 0x4B) return n;
        n = mhfu_read_u32(n + 0x04);
    }
    return 0;
}
extern "C" void mhfu_combat_node_dispatch_c(void)
{
    if (!g_combat_nodes) return;
    static uint32_t last_frame = 0xFFFFFFFFu;
    uint32_t frame = mhfu_read_u32(MGR_FRAME_CTR);
    if (frame == last_frame) return;            /* once per frame */
    last_frame = frame;
    uint32_t g = mhfu_read_u32(MHFU_COLL_GLOBAL_PTR);
    if (g < 0x08000000u || g >= 0x0C000000u) return;
    uint32_t natn = find_native_node(g);
    if (!natn) return;                          /* native not engaged yet */
    int n = g_clone_count;
    static int dbg = 0;
    if ((dbg++ % 120) == 0)
        mhfu_log("[combatnode] dispatch frame=%u clones=%d count=%u natnode=0x%08X",
                 (unsigned)frame, n, (unsigned)mhfu_read_u32(g + 0x5034), natn);
    for (int i = 0; i < n; i++) {
        uint32_t c = g_clones[i];
        if (c < 0x0A000000u || c >= 0x0C000000u) continue;
        uint32_t cn = find_node(g, c);
        if (!cn) { ((mhfu_combat_build_fn)MHFU_COMBAT_BUILD)(g, c, MHFU_COMBAT_IDX); cn = find_node(g, c); }
        if (!cn || cn == natn) continue;
        /* Fully POPULATE the clone node from the native's (the bare builder leaves
         * it skeletal — missing the hitbox/combat fields the consumer needs). Keep
         * the clone node's list links + rebind entity/pos. */
        uint32_t nx = mhfu_read_u32(cn + 0x04);
        uint32_t bk = mhfu_read_u32(cn + 0x08);
        for (uint32_t o = 0; o < 0x110u; o += 4)
            mhfu_write_u32(cn + o, mhfu_read_u32(natn + o));
        mhfu_write_u32(cn + 0x04, nx);
        mhfu_write_u32(cn + 0x08, bk);
        mhfu_write_u32(cn + 0x10, c);            /* entity backref = clone */
        mhfu_vec3_t p = mhfu_entity_pos(c);
        mhfu_write_f32(cn + 0x40, p.x); mhfu_write_f32(cn + 0x44, p.y); mhfu_write_f32(cn + 0x48, p.z);
    }
}

/* mhfu.combat_swap() — install (idempotent) the field swap. Builds the cave stub
 * once; (re)points manager+0xC at it whenever the slot holds the original (quest
 * reload reverts it). Call each tick when combat_nodes(true). */
static int lb_combat_swap(lua_State *L)
{
    (void)L;
    if (!g_combat_nodes) return 0;
    if (!g_combat_stub) {
        uint32_t *w = mhfu_cave_alloc(12);
        if (!w) { mhfu_log("[combatnode] cave exhausted"); return 0; }
        int i = 0;
        w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x10);
        w[i++] = mips_sw(MIPS_REG_RA, 0x0C, MIPS_REG_SP);
        w[i++] = mips_jal(MGR_PROC_ORIG);                 /* native processor (a0-a3 intact) */
        w[i++] = MIPS_NOP;
        w[i++] = mips_sw(MIPS_REG_V0, 0x08, MIPS_REG_SP); /* preserve its return */
        w[i++] = mips_jal((uint32_t)(uintptr_t)&mhfu_combat_node_dispatch_c);
        w[i++] = MIPS_NOP;
        w[i++] = mips_lw(MIPS_REG_V0, 0x08, MIPS_REG_SP);
        w[i++] = mips_lw(MIPS_REG_RA, 0x0C, MIPS_REG_SP);
        w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x10);
        w[i++] = mips_jr(MIPS_REG_RA);
        w[i++] = MIPS_NOP;
        mhfu_flush_caches();
        g_combat_stub = (uint32_t)(uintptr_t)w;
    }
    volatile uint32_t *slot = (volatile uint32_t *)MGR_PROC_SLOT;
    if (*slot == MGR_PROC_ORIG) {
        *slot = g_combat_stub;
        mhfu_flush_caches();
        mhfu_log("[combatnode] field swap @0x%08X -> stub 0x%08X (orig 0x%08X)",
                 MGR_PROC_SLOT, g_combat_stub, MGR_PROC_ORIG);
    }
    return 0;
}

/* mhfu.combat_nodes(enable) — arm/disarm the clone combat-node builder. */
static int lb_combat_nodes(lua_State *L)
{
    g_combat_nodes = lua_toboolean(L, 1);
    return 0;
}
/* mhfu.combat_register_all() — retained no-op (the field swap drives it now). */
static int lb_combat_register_all(lua_State *L) { (void)L; return 0; }

/* mhfu.load_relocated_overlay(path[, run_inits]) -> region_base, new_load, delta
 * (or nil,errcode). M2: load+relocate+place a 2nd em*.ovl at a fresh VA. */
static int lb_load_relocated_overlay(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int run_inits = lua_isnoneornil(L, 2) ? 0 : lua_toboolean(L, 2);
    static mhfu_ovl_region_t reg;   /* keep alive (region stays mapped) */
    int rc = mhfu_bigmon_load_relocated(path, &reg);
    if (rc != 0) { lua_pushnil(L); lua_pushinteger(L, rc); return 2; }
    if (run_inits) mhfu_bigmon_run_static_inits(&reg);
    lua_pushinteger(L, (lua_Integer)reg.region_base);
    lua_pushinteger(L, (lua_Integer)reg.new_load);
    lua_pushinteger(L, (lua_Integer)reg.delta);
    return 3;
}

/* mhfu.inject_register(file_id, path) -> ok:bool
 * Phase 4: watch a Blender-edited big-monster PAC on the memstick and overwrite
 * the species' loaded buffer in place (anim live; skeleton/geom on next rebuild).
 * The worker thread drives mhfu_inject_tick() at 2 Hz. */
static int lb_inject_register(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    const char *path = luaL_checkstring(L, 2);
    lua_pushboolean(L, mhfu_inject_register(id, path) == 0);
    return 1;
}
/* mhfu.inject_relocate(file_id, grown_path, orig_path) -> ok:bool
 * Phase 5 topology-grow: deliver a BIGGER PAC than the engine's fixed raw buffer by
 * redirecting the load transform's source (get_subresource a0) to a grown PAC in
 * xram. orig_path recognizes the engine's raw buffer. */
static int lb_inject_relocate(lua_State *L)
{
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    const char *grown = luaL_checkstring(L, 2);
    const char *orig  = luaL_checkstring(L, 3);
    lua_pushboolean(L, mhfu_inject_register_relocate(id, grown, orig) == 0);
    return 1;
}
/* mhfu.inject_now(file_id) -> dst_addr (0 = not located) — force re-read+apply. */
static int lb_inject_now(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_inject_now((uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}
/* mhfu.inject_locate(file_id) -> live buffer addr (0 = not found). */
static int lb_inject_locate(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_inject_locate((uint32_t)luaL_checkinteger(L, 1)));
    return 1;
}

/* mhfu.buttons() -> buttons:u32, lx:0..255, ly:0..255
 * Reads the live controller. PSP_CTRL_* masks are exposed on the mhfu table.
 * Analog stick centred ~128; d-pad / face buttons are bits in `buttons`. */
static int lb_buttons(lua_State *L)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    lua_pushinteger(L, (lua_Integer)pad.Buttons);
    lua_pushinteger(L, (lua_Integer)pad.Lx);
    lua_pushinteger(L, (lua_Integer)pad.Ly);
    return 3;
}

/* --- USB screen capture (capture.cpp) ---
 * mhfu.capture(on [, scale [, interval_ms [, path]]]) -> running:bool
 *   on=true starts streaming the framebuffer to host0:/cap/stream.bin (psplink
 *   usbhostfs). scale 1=full 2=half(default), interval_ms ~grab period (66≈15fps).
 *   Capture runs on its own thread — an absent USB host never blocks the game.
 * mhfu.capture_status() -> active:bool, frames:int, kb:int, last_err:int */
extern "C" int  mhfu_capture_set(int on);
extern "C" int  mhfu_capture_status(int *frames, int *kb, int *err);
extern "C" void mhfu_capture_configure(int scale, int interval_ms, const char *path);

static int lb_capture(lua_State *L)
{
    int on = lua_toboolean(L, 1);
    if (on && !lua_isnoneornil(L, 2)) {
        int scale = (int)luaL_optinteger(L, 2, 2);
        int iv    = (int)luaL_optinteger(L, 3, 66);
        const char *path = luaL_optstring(L, 4, 0);
        mhfu_capture_configure(scale, iv, path);
    }
    lua_pushboolean(L, mhfu_capture_set(on));
    return 1;
}

static int lb_capture_status(lua_State *L)
{
    int f = 0, kb = 0, err = 0;
    int active = mhfu_capture_status(&f, &kb, &err);
    lua_pushboolean(L, active);
    lua_pushinteger(L, f);
    lua_pushinteger(L, kb);
    lua_pushinteger(L, err);
    return 4;
}

/* ===========================================================================
 * Freecam / cam-tick override engine  (orbit-input override)
 *
 * The EBOOT camera-update fn 0x08886B68 is an orbit-camera builder:
 *   eye = pivot(s4+0x50 = 0x09998ED0) + R(euler angles) * offset
 * RE'd live (re_cam*.py). The pivot is copied from the followed object each
 * frame; the 3 euler-angle floats are derived into stack temps sp+0x17C/180/
 * 184 (radians) and consumed at 0x088878D4 to build R. We cold-install
 * (TITLE/MENU quiet-gate, Section 26) a MID-FUNCTION detour at 0x088878BC —
 * AFTER pivot+angles are set, BEFORE they are consumed — that calls
 * cam_orbit_c(cam_sp). When freecam is active it overwrites pivot + the angle
 * temps with our values, so the engine builds the whole view (eye + matrix +
 * GE upload) from OUR orbit state. Feeding continuous angles also removes the
 * vertical pitch snap. Toggle: double-tap SELECT (detected here per-frame).
 *
 * NOTE: the view-matrix look-at target is read separately from s3+0x200 (the
 * player) later in the fn, so with injection #1 alone the camera flies but
 * still points at the player; injection #2 (look-at redirect) is the next step.
 * ========================================================================= */
/* framework-internal code-cave bump allocator (defined in src/core/cave.cpp;
 * same PRX, resolves at link — not part of the public SDK header set). */
extern "C" uint32_t *mhfu_cave_alloc(int n_insns);

/* ORBIT fly-cam (restored known-good). The engine builds eye = pivot(s4+0x50) +
 * R(euler angles)*offset and looks AT the focus. We override the orbit INPUTS
 * mid-fn so the engine flies its whole pipeline from OUR pivot+angles:
 *   - injection #1 @ 0x088878BC: overwrite pivot x/z (s4+0x50/58) + angle temps
 *     (sp+0x17C/180/184) + the double-tap SELECT toggle.
 *   - injection #3 @ 0x088879A0: rewrite the just-computed eye_y (engine ties it
 *     to player.Y) to OUR height, so vertical fly is camera-only. Gated → normal
 *     play 100% stock.
 * Look direction stays engine default (focus ≈ player). True free-LOOK via the
 * target cell 0x09998D50 is the next RE step. Toggle: double-tap SELECT. */
#define CAM_INJECT         0x088878BCu  /* addiu v1,sp,0x180 (orbit detour)   */
#define CAM_INJECT_RESUME  0x088878C4u
#define CAM_PIVOT          0x09998ED0u  /* s4+0x50 pivot (eye x/z anchor)     */
#define CAM_ANG_OFF        0x17C        /* angle temps sp+0x17C/180/184 (rad) */
#define CAM_INJECT3        0x088879A0u  /* swc1 f1,0x44(s4) (eye_y store)     */
#define CAM_INJECT3_RESUME 0x088879A8u
#define CAM_EYE            0x09998EC0u  /* eye vec3 (s4+0x40)                 */
#define CAM_OFF            0x09998E10u  /* offset = eye - target             */
#define CAM_TARGET         0x09998D50u  /* look-at point (view dir source)   */
#define ENC_SWC1_F1_S4_44  0xE6810044u
#define ENC_LWC1_F1_S4_58  0xC6810058u
#define ENC_SWC1_F0_SP_1C  0xE7A0001Cu
#define ENC_LWC1_F0_SP_1C  0xC7A0001Cu
/* injection #4: view-direction builder 0x08815D84 reads target @0x09998D50 to
 * compute the look vector. Entry-detour to set target = eye + aim (free-look). */
#define CAM_INJECT4        0x08815D84u  /* addiu sp,sp,-0x70 (fn entry)       */
#define CAM_INJECT4_RESUME 0x08815D8Cu  /* sw s0,0x8(sp)    (resume)         */

static volatile int   g_fc_active   = 0;
static volatile int   g_fc_snap_off = 0;   /* (future) remove snap when freecam off */
static volatile int   g_fc_village  = 0;   /* (future) unlock village camera         */
static volatile int   g_fc_freeze   = 1;   /* (future) player input lock             */
static volatile int   g_fc_inited   = 0;
static volatile int   g_fc_ang_cap  = 0;   /* quest orbit-angle seed captured        */
static volatile int   g_fc_yaw_slot   = 1;
static volatile int   g_fc_pitch_slot = 2;
static float    g_fc_pivot[3] = {0,0,0};   /* camera focus x/z (eye anchor)           */
static float    g_fc_height    = 0.0f;     /* camera focus Y (eye_y anchor)           */
static float    g_fc_ang_seed[3] = {0,0,0};/* frozen orbit angles (no eye-orbit)      */
static float    g_fc_yaw       = 0.0f;     /* AIM yaw   (free-look)                    */
static float    g_fc_pitch     = 0.0f;     /* AIM pitch (free-look)                    */
static float    g_fc_move_spd  = 25.0f;
static float    g_fc_rot_spd   = 0.035f;
static float    g_fc_look_dist = 300.0f;   /* (reserved for free-look)               */
static int      g_fc_prev_sel  = 0;
static int      g_fc_tap_age   = 99999;
static uint32_t *g_cam_w1 = 0;
static uint32_t *g_cam_w3 = 0;
static uint32_t *g_cam_w4 = 0;

/* injection #4 — the UNIVERSAL freecam driver. The view-direction builder
 * 0x08815D84 runs in BOTH the quest and the village (unlike the quest-only orbit
 * cam 0x08886B68), so the toggle + input + camera control live here:
 *   - double-tap SELECT toggle (works everywhere)
 *   - read pad: fly g_fc_pivot/height + aim g_fc_yaw/pitch
 *   - write the eye cell (drives the VILLAGE camera, whose eye is otherwise
 *     fixed/never written — verified it sticks) and the target cell = eye + aim
 *     (the look-at this fn is about to read -> free-look everywhere).
 * In the QUEST the eye/pivot position also flows through inj#1/#3 (the orbit
 * pipeline); the eye-cell write here is consistent with it. */
extern "C" void cam_target_c(void)
{
    SceCtrlData pad; sceCtrlPeekBufferPositive(&pad, 1);
    unsigned b = pad.Buttons;
    int sel = (b & PSP_CTRL_SELECT) ? 1 : 0;
    if (g_fc_tap_age < 1000000) g_fc_tap_age++;
    if (sel && !g_fc_prev_sel) {
        if (g_fc_tap_age < 20) { g_fc_active = !g_fc_active; g_fc_inited = 0; g_fc_ang_cap = 0; g_fc_tap_age = 1000000; }
        else g_fc_tap_age = 0;
    }
    g_fc_prev_sel = sel;
    if (!g_fc_active) return;

    float *eye = (float *)CAM_EYE;
    float *tgt = (float *)CAM_TARGET;
    float ox=*(float*)CAM_OFF, oy=*(float*)(CAM_OFF+4), oz=*(float*)(CAM_OFF+8);
    int village = (ox==0.0f && oy==0.0f && oz==0.0f);   /* quest uses the offset cell */

    if (!g_fc_inited) {
        /* seed position: village flies the eye cell; quest flies the orbit pivot */
        float *src = village ? eye : (float *)CAM_PIVOT;
        g_fc_pivot[0]=src[0]; g_fc_pivot[2]=src[2];
        g_fc_height = eye[1];
        if (village) {
            float dx=tgt[0]-eye[0], dy=tgt[1]-eye[1], dz=tgt[2]-eye[2];
            g_fc_yaw   = atan2f(dx, dz);
            g_fc_pitch = atan2f(dy, sqrtf(dx*dx+dz*dz));
        } else {
            g_fc_yaw   = atan2f(-ox, -oz);
            g_fc_pitch = atan2f(-oy, sqrtf(ox*ox+oz*oz));
        }
        g_fc_inited = 1;
    }

    if (b & PSP_CTRL_LEFT)  g_fc_yaw   -= g_fc_rot_spd;
    if (b & PSP_CTRL_RIGHT) g_fc_yaw   += g_fc_rot_spd;
    if (b & PSP_CTRL_UP)    g_fc_pitch += g_fc_rot_spd;
    if (b & PSP_CTRL_DOWN)  g_fc_pitch -= g_fc_rot_spd;
    if (g_fc_pitch >  1.4f) g_fc_pitch =  1.4f;
    if (g_fc_pitch < -1.4f) g_fc_pitch = -1.4f;

    float cp=cosf(g_fc_pitch), sp=sinf(g_fc_pitch);
    float cy=cosf(g_fc_yaw),   sy=sinf(g_fc_yaw);
    float mv=((int)pad.Ly - 128)/128.0f;
    float st=((int)pad.Lx - 128)/128.0f;
    if (mv>-0.12f && mv<0.12f) mv=0;
    if (st>-0.12f && st<0.12f) st=0;
    g_fc_pivot[0]+=(sy*mv + cy*st)*g_fc_move_spd;
    g_fc_pivot[2]+=(cy*mv - sy*st)*g_fc_move_spd;
    if (b & PSP_CTRL_RTRIGGER) g_fc_height+=g_fc_move_spd;
    if (b & PSP_CTRL_LTRIGGER) g_fc_height-=g_fc_move_spd;

    /* VILLAGE: nothing else writes the eye cell -> fly it directly. QUEST: the
     * orbit pipeline (inj#1/#3) owns the eye position; don't fight it. */
    if (village) { eye[0]=g_fc_pivot[0]; eye[1]=g_fc_height; eye[2]=g_fc_pivot[2]; }

    /* look-at = current eye + aim (read the live eye cell so it matches the
     * render eye in both modes) */
    float d=g_fc_look_dist;
    tgt[0]=eye[0]+cp*sy*d; tgt[1]=eye[1]+sp*d; tgt[2]=eye[2]+cp*cy*d;
}

/* injection #3: eye_y = OUR height + rotated.y (camera-only vertical; gated). */
extern "C" void cam_eyey_c(uint32_t s4base, uint32_t player)
{
    if (!g_fc_active) return;
    float *eye_y = (float *)(s4base + 0x44);
    float py = *(float *)(player + 0x204);
    *eye_y = g_fc_height + (*eye_y - py);
}

/* injection #1 (QUEST only — 0x08886B68 doesn't run in the village): apply the
 * fly position to the orbit pivot (x/z; eye_y via inj#3) so the quest's eye
 * pipeline lands at our position, and FREEZE the orbit angles so d-pad (= aim,
 * handled in inj#4) doesn't also orbit the eye. Input/toggle live in inj#4. */
extern "C" void cam_orbit_c(uint32_t cam_sp, uint32_t player)
{
    (void)player;
    if (!g_fc_active) return;
    float *ang = (float *)(cam_sp + CAM_ANG_OFF);
    float *piv = (float *)CAM_PIVOT;
    if (!g_fc_ang_cap) {                 /* capture orbit-angle seed once */
        g_fc_ang_seed[0]=ang[0]; g_fc_ang_seed[1]=ang[1]; g_fc_ang_seed[2]=ang[2];
        g_fc_ang_cap = 1;
    }
    piv[0]=g_fc_pivot[0]; piv[2]=g_fc_pivot[2];
    ang[0]=g_fc_ang_seed[0]; ang[1]=g_fc_ang_seed[1]; ang[2]=g_fc_ang_seed[2];
}

/* injection #1 stub (replay addiu v1,sp,0x180 ; addiu v0,sp,0x17C). */
static int build_cam_w1(uint32_t *w)
{
    int i=0;
    w[i++] = mips_move (MIPS_REG_T0, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, (int16_t)-0x20);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_T0, 0x14, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A1, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A0, 0x0C, MIPS_REG_SP);
    w[i++] = mips_move (MIPS_REG_A0, MIPS_REG_T0);
    w[i++] = mips_move (MIPS_REG_A1, MIPS_REG_S3);
    w[i++] = mips_jal  ((uint32_t)(uintptr_t)&cam_orbit_c);
    w[i++] = MIPS_NOP;
    w[i++] = mips_lw   (MIPS_REG_A0, 0x0C, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A1, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_T0, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    w[i++] = mips_addiu(MIPS_REG_V1, MIPS_REG_SP, 0x180);
    w[i++] = mips_j    (CAM_INJECT_RESUME);
    w[i++] = mips_addiu(MIPS_REG_V0, MIPS_REG_SP, 0x17C);
    while (i < 20) w[i++] = MIPS_NOP;
    return i;
}

/* injection #3 stub (eye_y). */
static int build_cam_w3(uint32_t *w)
{
    int i=0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, (int16_t)-0x20);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = ENC_SWC1_F0_SP_1C;
    w[i++] = ENC_SWC1_F1_S4_44;
    w[i++] = mips_move (MIPS_REG_A0, MIPS_REG_S4);
    w[i++] = mips_move (MIPS_REG_A1, MIPS_REG_S3);
    w[i++] = mips_jal  ((uint32_t)(uintptr_t)&cam_eyey_c);
    w[i++] = MIPS_NOP;
    w[i++] = ENC_LWC1_F0_SP_1C;
    w[i++] = mips_lw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    w[i++] = ENC_LWC1_F1_S4_58;
    w[i++] = mips_j    (CAM_INJECT3_RESUME);
    w[i++] = MIPS_NOP;
    while (i < 20) w[i++] = MIPS_NOP;
    return i;
}

/* injection #4 stub: prefix entry-detour on the view-direction builder. Call
 * cam_target_c (sets target=eye+aim), then replay the displaced prologue. */
static int build_cam_w4(uint32_t *w)
{
    int i=0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, (int16_t)-0x20);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A2, 0x0C, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A3, 0x08, MIPS_REG_SP);
    w[i++] = mips_jal  ((uint32_t)(uintptr_t)&cam_target_c);
    w[i++] = MIPS_NOP;
    w[i++] = mips_lw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A2, 0x0C, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A3, 0x08, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, (int16_t)-0x70);  /* replay disp #0 */
    w[i++] = mips_sw   (MIPS_REG_RA, 0x0C, MIPS_REG_SP);            /* replay disp #1 */
    w[i++] = mips_j    (CAM_INJECT4_RESUME);
    w[i++] = MIPS_NOP;
    while (i < 20) w[i++] = MIPS_NOP;
    return i;
}

static int freecam_install(void)
{
    if (g_cam_w1) return 0;
    uint32_t *w1 = mhfu_cave_alloc(20);
    if (!w1) { mhfu_log("[freecam] cave exhausted (1)"); return -1; }
    build_cam_w1(w1); mhfu_flush_caches(); g_cam_w1 = w1;
    uint32_t o0 = mips_addiu(MIPS_REG_V1, MIPS_REG_SP, 0x180);
    uint32_t o1 = mips_addiu(MIPS_REG_V0, MIPS_REG_SP, 0x17C);
    mhfu_hook_rc_t r0 = mhfu_patch_word_when_quiet(CAM_INJECT+0, o0, mips_j((uint32_t)(uintptr_t)w1), "freecam");
    mhfu_hook_rc_t r1 = mhfu_patch_word_when_quiet(CAM_INJECT+4, o1, MIPS_NOP, "freecam");

    uint32_t *w3 = mhfu_cave_alloc(20);
    if (!w3) { mhfu_log("[freecam] cave exhausted (3)"); return -1; }
    build_cam_w3(w3); mhfu_flush_caches(); g_cam_w3 = w3;
    mhfu_hook_rc_t r4 = mhfu_patch_word_when_quiet(CAM_INJECT3+0, ENC_SWC1_F1_S4_44, mips_j((uint32_t)(uintptr_t)w3), "freecam");
    mhfu_hook_rc_t r5 = mhfu_patch_word_when_quiet(CAM_INJECT3+4, ENC_LWC1_F1_S4_58, MIPS_NOP, "freecam");

    uint32_t *w4 = mhfu_cave_alloc(20);
    if (!w4) { mhfu_log("[freecam] cave exhausted (4)"); return -1; }
    build_cam_w4(w4); mhfu_flush_caches(); g_cam_w4 = w4;
    uint32_t t0w = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, (int16_t)-0x70);
    uint32_t t1w = mips_sw(MIPS_REG_RA, 0x0C, MIPS_REG_SP);
    mhfu_hook_rc_t r6 = mhfu_patch_word_when_quiet(CAM_INJECT4+0, t0w, mips_j((uint32_t)(uintptr_t)w4), "freecam");
    mhfu_hook_rc_t r7 = mhfu_patch_word_when_quiet(CAM_INJECT4+4, t1w, MIPS_NOP, "freecam");

    mhfu_log("[freecam] queued: orbit@0x%08X(%d,%d) eyeY@0x%08X(%d,%d) lookdir@0x%08X(%d,%d)",
             CAM_INJECT,(int)r0,(int)r1, CAM_INJECT3,(int)r4,(int)r5, CAM_INJECT4,(int)r6,(int)r7);
    return (r0==MHFU_HOOK_OK&&r1==MHFU_HOOK_OK&&r4==MHFU_HOOK_OK&&r5==MHFU_HOOK_OK
            &&r6==MHFU_HOOK_OK&&r7==MHFU_HOOK_OK) ? 0 : -1;
}

/* --- Lua bindings for the cam engine --- */
static int lb_freecam(lua_State *L){ g_fc_active = lua_toboolean(L,1); g_fc_inited = 0; return 0; }
static int lb_freecam_active(lua_State *L){ lua_pushboolean(L, g_fc_active); return 1; }
static int lb_cam_snap_disable(lua_State *L){ g_fc_snap_off = lua_toboolean(L,1); return 0; }
static int lb_cam_freeze_player(lua_State *L){ g_fc_freeze = lua_toboolean(L,1); return 0; }
static int lb_cam_village_unlock(lua_State *L){ g_fc_village = lua_toboolean(L,1); return 0; }
static int lb_cam_config(lua_State *L){
    if (!lua_isnoneornil(L,1)) g_fc_move_spd  = (float)luaL_checknumber(L,1);
    if (!lua_isnoneornil(L,2)) g_fc_rot_spd   = (float)luaL_checknumber(L,2);
    if (!lua_isnoneornil(L,3)) g_fc_look_dist = (float)luaL_checknumber(L,3);
    return 0;
}
static int lb_cam_eye(lua_State *L){
    lua_pushnumber(L, *(volatile float*)CAM_EYE);
    lua_pushnumber(L, *(volatile float*)(CAM_EYE+4));
    lua_pushnumber(L, *(volatile float*)(CAM_EYE+8));
    return 3;
}

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
static int lb_quest_add_monster(lua_State *L)
{
    /* quest_add_monster(quest, id [, x, z]) — ADD a 2nd big monster.
     * MUST be called from on_quest_targets_building. x/z 0 = clone source. */
    float x = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 0.0);
    mhfu_hook_rc_t rc = mhfu_quest_add_monster(
        (mhfu_quest_t)luaL_checkinteger(L,1),
        (mhfu_monster_id_t)luaL_checkinteger(L,2), x, z);
    lua_pushboolean(L, rc == MHFU_HOOK_OK);
    return 1;
}
static int lb_quest_monster_count(lua_State *L)
{
    lua_pushinteger(L, mhfu_quest_monster_count((mhfu_quest_t)luaL_checkinteger(L,1)));
    return 1;
}
static int lb_quest_first_monster(lua_State *L)
{
    lua_pushinteger(L, mhfu_quest_first_monster((mhfu_quest_t)luaL_checkinteger(L,1)));
    return 1;
}
/* mhfu.entity_clone(src_ptr) -> clone_ptr (0 on failure). Same-species deep
 * copy + self-ptr rebase + splice into the engine update chain + registry. */
static int lb_entity_clone(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_entity_clone((uint32_t)luaL_checkinteger(L,1)));
    return 1;
}
/* ---- Route A: collision-node clone (give a clone NATIVE player damage) ---- */
/* mhfu.node_clone(template_node, clone_ent, uid) -> node_ptr (0 on failure) */
static int lb_node_clone(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_node_clone(
        (uint32_t)luaL_checkinteger(L,1), (uint32_t)luaL_checkinteger(L,2),
        (uint16_t)luaL_checkinteger(L,3)));
    return 1;
}
/* mhfu.node_of(ent) -> node_ptr (entity+0x2EC), 0 if none */
static int lb_node_of(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)mhfu_node_of((uint32_t)luaL_checkinteger(L,1)));
    return 1;
}
/* mhfu.node_linked(node) -> bool */
static int lb_node_linked(lua_State *L)
{
    lua_pushboolean(L, mhfu_node_linked((uint32_t)luaL_checkinteger(L,1)));
    return 1;
}
/* mhfu.node_relink(node) -> bool (re-linked?) */
static int lb_node_relink(lua_State *L)
{
    lua_pushboolean(L, mhfu_node_relink((uint32_t)luaL_checkinteger(L,1)));
    return 1;
}
/* mhfu.node_sync(node, ent) */
static int lb_node_sync(lua_State *L)
{
    mhfu_node_sync((uint32_t)luaL_checkinteger(L,1), (uint32_t)luaL_checkinteger(L,2));
    return 0;
}
/* mhfu.node_detach(node) */
static int lb_node_detach(lua_State *L)
{
    mhfu_node_detach((uint32_t)luaL_checkinteger(L,1));
    return 0;
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
static int g_inst_damaged = 0;
static int g_inst_overlay = 0;
static int g_inst_slot    = 0;
static int g_inst_input   = 0;
static int g_inst_decided = 0;
static int g_inst_action  = 0;

static int r_quest   = LUA_NOREF;       /* registry ref to the Lua handler */
static int r_spawn   = LUA_NOREF;
static int r_death   = LUA_NOREF;
static int r_damaged = LUA_NOREF;
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

/* on_bigmonster_damaged fires from the 5 Hz monster poll thread (same context
 * as spawn/death), so it calls lua_enter() directly. Lua signature:
 *   function(entity_ptr, monster_type, amount, hp, slot) end           */
static void tramp_damaged(const mhfu_bigmonster_damage_ctx_t *ctx)
{
    if (r_damaged == LUA_NOREF || !lua_enter()) return;
    lua_State *L = g_L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r_damaged);
    lua_pushinteger(L, (lua_Integer)ctx->entity_ptr);
    lua_pushinteger(L, ctx->monster_type);
    lua_pushinteger(L, ctx->amount);
    lua_pushinteger(L, ctx->hp);
    lua_pushinteger(L, ctx->slot);
    if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
        mhfu_log("[lua_host] damaged err: %s", lua_tostring(L, -1));
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
static int lb_on_damaged(lua_State *L)
{
    store_ref(L, &r_damaged);
    int prio = (int)luaL_optinteger(L, 2, 0);
    if (!g_inst_damaged) { mhfu_on_bigmonster_damaged(tramp_damaged, prio); g_inst_damaged = 1; }
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
    { "read_f32",         lb_read_f32 },
    { "write_f32",        lb_write_f32 },
    { "mem_valid",        lb_mem_valid },
    { "get_screen_state", lb_screen_state },
    { "get_area_index",   lb_area_index },
    { "get_quest_timer",  lb_quest_timer },
    { "get_player_hp",    lb_player_hp },
    { "player_pos",       lb_player_pos },
    { "paint_map",        lb_paint_map },
    { "resolve_attack",   lb_resolve_attack },
    { "clone_combat",     lb_clone_combat },
    { "combat_nodes",         lb_combat_nodes },
    { "combat_swap",          lb_combat_swap },
    { "combat_register_all",  lb_combat_register_all },
    { "clones_set",       lb_clones_set },
    { "buttons",          lb_buttons },
    { "capture",          lb_capture },
    { "capture_status",   lb_capture_status },
    { "freecam",          lb_freecam },
    { "freecam_active",   lb_freecam_active },
    { "cam_snap_disable", lb_cam_snap_disable },
    { "cam_freeze_player", lb_cam_freeze_player },
    { "cam_village_unlock", lb_cam_village_unlock },
    { "cam_config",       lb_cam_config },
    { "cam_eye",          lb_cam_eye },
    { "entity_at",        lb_entity_at },
    { "entity_type",      lb_entity_type },
    { "entity_hp",        lb_entity_hp },
    { "entity_size",      lb_entity_size },
    { "entity_set_size",  lb_entity_set_size },
    { "entity_alive",     lb_entity_alive },
    { "entity_pos",       lb_entity_pos },
    { "entity_set_pos",   lb_entity_set_pos },
    { "entity_yaw",       lb_entity_yaw },
    { "entity_set_yaw",   lb_entity_set_yaw },
    { "entity_ai_state",  lb_entity_ai_state },
    { "entity_set_ai_state", lb_entity_set_ai_state },
    { "entity_engaged",   lb_entity_engaged },
    { "entity_set_engaged", lb_entity_set_engaged },
    { "entity_calm",      lb_entity_calm },
    { "entity_section",   lb_entity_section },
    { "entity_set_section", lb_entity_set_section },
    { "entity_make_visible", lb_entity_make_visible },
    { "entity_force_aggro", lb_entity_force_aggro },
    { "entity_clone",     lb_entity_clone },
    { "node_clone",       lb_node_clone },
    { "node_of",          lb_node_of },
    { "node_linked",      lb_node_linked },
    { "node_relink",      lb_node_relink },
    { "node_sync",        lb_node_sync },
    { "node_detach",      lb_node_detach },
    { "entities_of_type", lb_entities_of_type },
    { "quest_has",            lb_quest_has },
    { "quest_monster_count",  lb_quest_monster_count },
    { "quest_first_monster",  lb_quest_first_monster },
    { "quest_replace_monster", lb_quest_replace_monster },
    { "quest_add_monster",    lb_quest_add_monster },
    { "load_relocated_overlay", lb_load_relocated_overlay },
    { "inject_register",  lb_inject_register },
    { "inject_relocate",  lb_inject_relocate },
    { "inject_now",       lb_inject_now },
    { "inject_locate",    lb_inject_locate },
    { "action_ptr_for",   lb_action_ptr_for },
    /* AI override-event registration */
    { "on_quest_targets_building",  lb_on_quest },
    { "on_bigmonster_spawn",        lb_on_spawn },
    { "on_bigmonster_death",        lb_on_death },
    { "on_bigmonster_damaged",      lb_on_damaged },
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
    /* PSP_CTRL_* button masks (for mhfu.buttons()) */
    lua_pushinteger(L, PSP_CTRL_SELECT);   lua_setfield(L, -2, "CTRL_SELECT");
    lua_pushinteger(L, PSP_CTRL_START);    lua_setfield(L, -2, "CTRL_START");
    lua_pushinteger(L, PSP_CTRL_UP);       lua_setfield(L, -2, "CTRL_UP");
    lua_pushinteger(L, PSP_CTRL_RIGHT);    lua_setfield(L, -2, "CTRL_RIGHT");
    lua_pushinteger(L, PSP_CTRL_DOWN);     lua_setfield(L, -2, "CTRL_DOWN");
    lua_pushinteger(L, PSP_CTRL_LEFT);     lua_setfield(L, -2, "CTRL_LEFT");
    lua_pushinteger(L, PSP_CTRL_LTRIGGER); lua_setfield(L, -2, "CTRL_L");
    lua_pushinteger(L, PSP_CTRL_RTRIGGER); lua_setfield(L, -2, "CTRL_R");
    lua_pushinteger(L, PSP_CTRL_TRIANGLE); lua_setfield(L, -2, "CTRL_TRIANGLE");
    lua_pushinteger(L, PSP_CTRL_CIRCLE);   lua_setfield(L, -2, "CTRL_CIRCLE");
    lua_pushinteger(L, PSP_CTRL_CROSS);    lua_setfield(L, -2, "CTRL_CROSS");
    lua_pushinteger(L, PSP_CTRL_SQUARE);   lua_setfield(L, -2, "CTRL_SQUARE");
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
    if (s[0] == '_') return 0;           /* '_'-prefixed = private partial (e.g. _prelude) */
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
    if (!g_filebuf) { mhfu_log("[lua_host] no file scratch — skip %s", name); return -1; }
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { mhfu_log("[lua_host] open FAILED %s rc=0x%08X", path, (unsigned)fd); return -1; }
    int n = sceIoRead(fd, g_filebuf, G_FILEBUF_SZ - 1);
    sceIoClose(fd);
    if (n < 0) { mhfu_log("[lua_host] read FAILED %s rc=0x%08X", name, (unsigned)n); return -1; }
    if (n >= (int)(G_FILEBUF_SZ - 1)) {
        mhfu_log("[lua_host] %s too big (>%uB) — skipped", name, (unsigned)(G_FILEBUF_SZ - 1));
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
    /* CONTINUOUS ms0 I/O (Dopen/Dread/Getstat/Open). Skip entirely unless in active
     * gameplay (17/22): scanning the Memory Stick while the SAVEDATA utility loads
     * your save (boot character-select) or saves (mid-game) races the non-reentrant
     * MS driver and FREEZES the PSP — the intermittent character-select hang. */
    if (!mhfu_ms0_io_safe()) return;
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

    /* Tier 3: build the OO sugar (mhfu.world / mhfu.entity / mhfu.mem) on top
     * of the flat C bindings, before any user mod runs. Non-fatal on error —
     * the flat API still works without it. */
    if (luaL_loadstring(g_L, k_lua__prelude) == LUA_OK
        && lua_pcall(g_L, 0, 0, 0) == LUA_OK) {
        /* ok */
    } else {
        mhfu_log("[lua_host] prelude FAILED: %s", lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
    }

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
    /* mhfu_tick runs mod logic that READS and WRITES game memory (e.g. the paintball
     * cheat write). Only fire it during active gameplay (17/22) — running it at the
     * main-menu -> character-select transition, while the SAVEDATA utility initialises,
     * is what froze the save load. Mod ticks are meaningless outside gameplay anyway. */
    if (!mhfu_ms0_io_safe()) return;
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
    /* Inject scan runs at ~10 Hz (every 100 ms) to better catch the raw model
     * buffer BEFORE the overlay transform reads it; the heavier per-tick + hot-
     * reload duties stay at 2 Hz (every 5th iteration). */
    int sub = 0;
    for (;;) {
        sceKernelDelayThread(100 * 1000);    /* 10 Hz base */
        mhfu_inject_tick();                  /* Phase 4: live PAC injection (10 Hz) */
        if (++sub >= 5) {
            sub = 0;
            call_tick();
            hot_reload_scan();               /* Phase 4: live .lua reload */
        }
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

    /* Per-file read scratch (was a 48 KB .bss array; moved off the load image). */
    g_filebuf_uid = sceKernelAllocPartitionMemory(
        2 /* PSP_MEMORY_PARTITION_USER */, "mhfu_lua_filebuf",
        PSP_SMEM_Low, G_FILEBUF_SZ, 0);
    g_filebuf = (g_filebuf_uid >= 0) ? (char *)sceKernelGetBlockHeadAddr(g_filebuf_uid) : 0;
    if (!g_filebuf) mhfu_log("[lua_host] filebuf alloc FAILED rc=0x%08X — scripts won't load",
                             (unsigned)g_filebuf_uid);

    if (lua_host_setup() != 0) return -1;

    /* Queue the cam-tick postfix detour (applied at TITLE/MENU by the
     * framework's deferred quiet-poll — Section 26 JIT-cold window). */
    /* DISABLED 2026-06-08: freecam also toggles on double-tap SELECT, which
     * collides with monster_nameplates. Re-enable this call to restore freecam
     * (and pick a non-SELECT toggle for one of them). */
    /* freecam_install(); */

    /* Only now allow the event trampolines to enter the VM. The setup above
     * may have already installed framework hooks (spawn poll thread); g_ready
     * gates them until the VM + refs are fully built. */
    g_ready = 1;
    /* NB: the experimental clone combat driver's ai_step detour is installed lazily
     * by mhfu.clone_combat(true) (see lb_clone_combat), NOT here — so a default-off
     * config never patches z_un_08865648 and quest entry stays on the stable path. */

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
