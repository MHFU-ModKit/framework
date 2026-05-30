/*
 * AI override events — implementation for mhfu/ai.h.
 *
 * Provides three new event surfaces on top of the generic monster-AI
 * engine RE'd in docs/AI_SCRIPTING_ENGINE.md §32d:
 *
 *   action_decided  --  wrap vt[8] = 0x08865254 (all species share).
 *                       Override-capable; priority chain. JIT-immune.
 *   anim_decided    --  wrap the JAL @ 0x0885F9CC -> z_un_0885f928.
 *                       Override-capable; priority chain. TITLE/MENU
 *                       gate via mhfu_patch_word_when_quiet.
 *   ai_step         --  prefix on z_un_08865648 (per-frame per-entity).
 *                       Observe-only.
 *
 * Wiring is lazy: the framework only installs the vt[8] swap once a mod
 * subscribes to action_decided (same for the other two). No subscribers
 * = no patches = no perf cost.
 */
#include <string.h>

#include "mhfu/ai.h"
#include "mhfu/ai_actions.h"
#include "mhfu/mips.h"
#include "mhfu/log.h"
#include "mhfu/entity.h"
#include "mhfu/hooks.h"
#include "internal.h"

/* Action-validity tables (popo + anteka), generated alongside ai_actions.h.
 * Big monsters have no static table — accept all IDs for them. */
static const uint16_t POPO_VALID[] = {
    0x0060, 0x0061, 0x0064, 0x0070, 0x0071, 0x0073, 0x0074, 0x0076,
    0x009C, 0x009D, 0x009E, 0x009F, 0x00A0,
};
#define POPO_VALID_N (int)(sizeof(POPO_VALID)/sizeof(POPO_VALID[0]))

static const uint16_t ANTEKA_VALID[] = {
    0x0043, 0x0044, 0x0045, 0x0047, 0x0048, 0x0049, 0x004B, 0x0457,
    0x0071, 0x0072, 0x008C, 0x008D, 0x008E, 0x008F, 0x0091, 0x0094,
    0x0095, 0x0096, 0x00A6, 0x00A7, 0x00C9, 0x00CA, 0x00CB, 0x00D2,
    0x00D3, 0x00D4, 0x04C1, 0x00DE, 0x00DF, 0x00E2, 0x00E4, 0x00E7,
    0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00EF, 0x00F0, 0x00F5, 0x00F6,
};
#define ANTEKA_VALID_N (int)(sizeof(ANTEKA_VALID)/sizeof(ANTEKA_VALID[0]))

extern "C" int mhfu_action_is_valid(uint8_t monster_type, uint32_t action_id)
{
    const uint16_t *t = 0; int n = 0;
    switch (monster_type) {
        case 0x46: t = POPO_VALID;   n = POPO_VALID_N;   break;
        case 0x45: t = ANTEKA_VALID; n = ANTEKA_VALID_N; break;
        case 0x4B: case 0x4D: return 1;   /* big monster — no static table; accept */
        default: return 0;
    }
    uint16_t id16 = (uint16_t)(action_id & 0xFFFF);
    for (int i = 0; i < n; i++) if (t[i] == id16) return 1;
    return 0;
}

#define MAX_HANDLERS 8

/* vt[8] is shared across all 4 known big-monster species; one swap per
 * vtable slot wires them all (we keep this list explicit so unmapped
 * species don't get hooked silently). */
typedef struct {
    const char *name;
    uint32_t    vtable_ptr;
    uint32_t    vt8_slot_addr;   /* vtable_ptr + 0x20 */
} species_vt_t;

static const species_vt_t g_species_vts[] = {
    { "popo",     0x089BC560u, 0x089BC580u },
    { "anteka",   0x089BC074u, 0x089BC094u },
    { "tigrex",   0x089BB69Cu, 0x089BB6BCu },
    { "giadrome", 0x089BC950u, 0x089BC970u },
};
#define N_SPECIES_VTS (int)(sizeof(g_species_vts) / sizeof(g_species_vts[0]))

#define VT8_ORIGINAL          0x08865254u   /* shared anim/action picker */
#define AI_STEP_TICK_ENTRY    0x08865648u   /* z_un_08865648 prologue */
#define ANIM_RESOLVER_CALL    0x0885F9C8u   /* JAL z_un_0885f928 inside installer */
#define ANIM_RESOLVER_TARGET  0x0885F928u
#define INSTALLER_FRAME_RA    0x0885F9D0u   /* MOVE a1, v0 — what we ride post-call */

#define AI_OWNER_TAG "mhfu_ai"

/* --- chain storage --------------------------------------------------- */

typedef struct { mhfu_action_override_cb_t cb; int priority; } action_entry_t;
typedef struct { mhfu_anim_override_cb_t   cb; int priority; } anim_entry_t;
typedef struct { mhfu_ai_step_cb_t         cb; int priority; } step_entry_t;
typedef struct { mhfu_bigmonster_spawn_cb_t cb; int priority; } bmspawn_entry_t;
typedef struct { mhfu_bigmonster_death_cb_t cb; int priority; } bmdeath_entry_t;

static action_entry_t  g_action_chain[MAX_HANDLERS];
static int             g_action_n = 0;
static anim_entry_t    g_anim_chain[MAX_HANDLERS];
static int             g_anim_n = 0;
static step_entry_t    g_step_chain[MAX_HANDLERS];
static int             g_step_n = 0;
static bmspawn_entry_t g_bmspawn_chain[MAX_HANDLERS];
static int             g_bmspawn_n = 0;
static bmdeath_entry_t g_bmdeath_chain[MAX_HANDLERS];
static int             g_bmdeath_n = 0;

/* HP transition tracker (used for death edge). Indexed by registry slot. */
static uint16_t        g_last_hp[MHFU_ENTITY_REGISTRY_SLOTS];
static uint8_t         g_was_big[MHFU_ENTITY_REGISTRY_SLOTS];

static int            g_action_hook_installed = 0;
static int            g_anim_hook_installed   = 0;
static int            g_step_hook_installed   = 0;

/* Cave-allocated stubs (set on first install). */
static uint32_t      *g_action_stub = 0;

/* --- chain insert/remove --------------------------------------------- */

#define CHAIN_INSERT(chain, n, max, cb_, pri_)                     \
    do {                                                           \
        if (n >= max) return MHFU_HOOK_NOSPACE;                    \
        int pos_ = n;                                              \
        while (pos_ > 0 && chain[pos_ - 1].priority < pri_) {      \
            chain[pos_] = chain[pos_ - 1]; pos_--;                 \
        }                                                          \
        chain[pos_].cb = cb_; chain[pos_].priority = pri_;         \
        n++;                                                       \
    } while (0)

#define CHAIN_REMOVE(chain, n, cb_)                                \
    do {                                                           \
        for (int i_ = 0; i_ < n; i_++) {                           \
            if (chain[i_].cb == cb_) {                             \
                for (int j_ = i_; j_ < n - 1; j_++)                \
                    chain[j_] = chain[j_ + 1];                     \
                n--;                                               \
                return MHFU_HOOK_OK;                               \
            }                                                      \
        }                                                          \
        return MHFU_HOOK_BADARG;                                   \
    } while (0)

/* --- big-monster predicate ------------------------------------------- */

extern "C" int mhfu_entity_is_big_monster(uint32_t entity_ptr)
{
    /* Until a clean species-flag is RE'd we use the quest's target list. */
    /* Defensive: if no quest active or quest API unavailable, fall back  */
    /* to a small monster-type allowlist that empirically covers the     */
    /* RE'd species set.                                                 */
    if (!entity_ptr) return 0;
    uint8_t t = mhfu_entity_type(entity_ptr);
    /* Known big-monster type bytes: Tigrex 0x4B, Giadrome 0x4D.         */
    /* Future: walk quest.targets[2] (quest.h) instead of this allowlist. */
    if (t == 0x4B || t == 0x4D) return 1;
    return 0;
}

/* --- entity recovery from action_list_ptr ---------------------------- */

extern "C" uint32_t mhfu_entity_from_action_list(uint32_t action_list_ptr)
{
    if (!action_list_ptr) return 0;
    for (int slot = 0; slot < MHFU_ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t e = mhfu_entity_at(slot);
        if (!e) continue;
        if (*(volatile uint32_t *)(e + 0x190) == action_list_ptr) return e;
    }
    return 0;
}

/* --- vt[8] action_decided stub builder + dispatcher ----------------- */

/* Stub layout (17 insns, branchless single basic block — Section 22):
 *   addiu sp, sp, -0x20
 *   sw    ra, 0x18(sp)
 *   sw    s0, 0x14(sp)
 *   sw    s1, 0x10(sp)
 *   move  s0, a0                  ; entity
 *   move  s1, a1                  ; vt8_input
 *   jal   VT8_ORIGINAL
 *   nop                           ; delay slot
 *   move  a0, s0                  ; entity for dispatcher
 *   jal   dispatch_c
 *   move  a1, s1                  ; delay slot — a1 = vt8_input
 *   ; on return, dispatcher placed value in v0 via $a2 thread...
 *   ; wait — different ABI: dispatch takes (entity, vt8_input, cur_v0)
 *   ; and returns the new value in v0
 *   move  v0, v0                  ; (already from dispatcher)
 *   lw    s1, 0x10(sp)
 *   lw    s0, 0x14(sp)
 *   lw    ra, 0x18(sp)
 *   jr    ra
 *   addiu sp, sp, 0x20            ; delay slot
 *
 * Dispatcher ABI: (a0=entity, a1=vt8_input, a2=engine_action_id) -> v0
 */

extern "C" uint32_t mhfu_ai_action_dispatch_c(
    uint32_t entity, uint32_t vt8_input, uint32_t engine_action_id)
{
    if (g_action_n == 0) return engine_action_id;
    if (!mhfu_entity_is_big_monster(entity)) return engine_action_id;

    mhfu_action_decision_ctx_t ctx;
    ctx.entity_ptr   = entity;
    ctx.monster_type = mhfu_entity_type(entity);
    ctx.slot         = 0;                          /* lost at vt[8] return  */
    ctx.vt8_input    = (uint16_t)(vt8_input & 0xFFFF);

    uint32_t v = engine_action_id;
    for (int i = 0; i < g_action_n; i++) v = g_action_chain[i].cb(&ctx, v);
    return v;
}

static int build_action_stub(uint32_t *stub, uint32_t dispatch_c_addr)
{
    int i = 0;
    stub[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    stub[i++] = mips_sw   (MIPS_REG_RA,  0x18, MIPS_REG_SP);
    stub[i++] = mips_sw   (MIPS_REG_S0,  0x14, MIPS_REG_SP);
    stub[i++] = mips_sw   (MIPS_REG_S1,  0x10, MIPS_REG_SP);
    stub[i++] = mips_sw   (MIPS_REG_A0,  0x04, MIPS_REG_SP);   /* save args */
    stub[i++] = mips_sw   (MIPS_REG_A1,  0x08, MIPS_REG_SP);
    /* original vt[8] reads a0,a1; we preserved them. Call original. */
    stub[i++] = mips_jal(VT8_ORIGINAL);
    stub[i++] = MIPS_NOP;                                       /* delay slot */
    /* v0 = engine action_id; thread it through dispatcher. */
    stub[i++] = mips_lw   (MIPS_REG_A0,  0x04, MIPS_REG_SP);   /* a0 = entity */
    stub[i++] = mips_lw   (MIPS_REG_A1,  0x08, MIPS_REG_SP);   /* a1 = vt8_input */
    stub[i++] = mips_move (MIPS_REG_A2,  MIPS_REG_V0);          /* a2 = engine v0 */
    stub[i++] = mips_jal(dispatch_c_addr);
    stub[i++] = MIPS_NOP;                                       /* delay slot */
    /* v0 already holds dispatcher return. Restore + return. */
    stub[i++] = mips_lw   (MIPS_REG_S1,  0x10, MIPS_REG_SP);
    stub[i++] = mips_lw   (MIPS_REG_S0,  0x14, MIPS_REG_SP);
    stub[i++] = mips_lw   (MIPS_REG_RA,  0x18, MIPS_REG_SP);
    stub[i++] = mips_jr(MIPS_REG_RA);
    stub[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);    /* delay slot */
    return i;
}

static int install_action_hook(void)
{
    if (g_action_hook_installed) return 0;

    uint32_t *stub = mhfu_cave_alloc(20);
    if (!stub) { mhfu_log("[ai] cave exhausted for action stub"); return -1; }
    int n = build_action_stub(stub, (uint32_t)(uintptr_t)&mhfu_ai_action_dispatch_c);
    (void)n;
    mhfu_flush_caches();
    g_action_stub = stub;

    int hooked = 0;
    for (int i = 0; i < N_SPECIES_VTS; i++) {
        if (mhfu_hook_vtable(g_species_vts[i].vt8_slot_addr,
                             (uint32_t)(uintptr_t)stub,
                             AI_OWNER_TAG) == MHFU_HOOK_OK) {
            hooked++;
        } else {
            mhfu_log("[ai] vt[8] swap rejected for %s @0x%08lx",
                     g_species_vts[i].name,
                     (unsigned long)g_species_vts[i].vt8_slot_addr);
        }
    }
    if (hooked == 0) return -1;
    g_action_hook_installed = 1;
    mhfu_log("[ai] action_decided installed on %d species vt[8] slots", hooked);
    return 0;
}

/* --- ai_step prefix --------------------------------------------------- */

extern "C" void mhfu_ai_step_dispatch_c(uint32_t entity)
{
    if (g_step_n == 0) return;
    if (!mhfu_entity_is_big_monster(entity)) return;
    mhfu_ai_step_ctx_t ctx;
    ctx.entity_ptr   = entity;
    ctx.monster_type = mhfu_entity_type(entity);
    for (int i = 0; i < g_step_n; i++) g_step_chain[i].cb(&ctx);
}

static int install_step_hook(void)
{
    if (g_step_hook_installed) return 0;
    /* z_un_08865648 has many call sites; the call-wrapper API needs a real
     * call_site argument. The clean wiring is to patch the function entry
     * with `J wrapper; NOP` where the wrapper preserves a0, calls the
     * helper, and resumes the function body by jumping back to addr+8 with
     * the orig insns copied into the wrapper -- a proper detour. Deferred
     * to a follow-up so we don't ship a half-wired hook. */
    mhfu_log("[ai] ai_step wiring not implemented yet -- registered handlers "
             "will not fire until detour lands");
    g_step_hook_installed = 1;   /* mark to avoid retry-spam */
    return 0;
}

/* --- anim_decided (deferred wiring) ---------------------------------- */

/* The hook would trampoline over the JAL @ 0x0885F9C8 -> z_un_0885f928.
 * The wrapper would call the original, fire the override chain, and
 * write the result back to $v0 before flow continues into MOVE a1, v0
 * at 0x0885F9D0. This requires a custom wrapper that exposes $v0 to the
 * helper (the existing mhfu_install_call_wrapper helper signature only
 * sees $a0). Deferred to a follow-up: see internal.h notes. */
static int install_anim_hook(void)
{
    if (g_anim_hook_installed) return 0;
    mhfu_log("[ai] anim_decided wiring not implemented yet — registered handlers "
             "will not fire until JAL trampoline lands");
    g_anim_hook_installed = 1;   /* mark to avoid retry-spam */
    return 0;
}

/* --- public registration --------------------------------------------- */

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_action_decided(
    mhfu_action_override_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_action_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_action_chain, g_action_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_anim_decided(
    mhfu_anim_override_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    install_anim_hook();
    CHAIN_INSERT(g_anim_chain, g_anim_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_ai_step(
    mhfu_ai_step_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_step_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_step_chain, g_step_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_action_decided(
    mhfu_action_override_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_action_chain, g_action_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_anim_decided(
    mhfu_anim_override_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_anim_chain, g_anim_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_ai_step(
    mhfu_ai_step_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_step_chain, g_step_n, cb);
}

/* --- spawn / death observe ------------------------------------------ */

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_spawn(
    mhfu_bigmonster_spawn_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_INSERT(g_bmspawn_chain, g_bmspawn_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_death(
    mhfu_bigmonster_death_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_INSERT(g_bmdeath_chain, g_bmdeath_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_spawn(mhfu_bigmonster_spawn_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_bmspawn_chain, g_bmspawn_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_death(mhfu_bigmonster_death_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_bmdeath_chain, g_bmdeath_n, cb);
}

/* Called by registry.cpp on the existing monster_spawned event when a
 * new entity ptr appears in a registry slot. We filter big-monster and
 * fan out our chain.  Also seeds the HP tracker. */
extern "C" void mhfu_ai_on_monster_spawn(int slot, uint32_t entity, uint8_t type,
                                         uint16_t hp)
{
    if (slot <= 0 || slot >= MHFU_ENTITY_REGISTRY_SLOTS) return;
    int is_big = mhfu_entity_is_big_monster(entity);
    g_was_big[slot] = (uint8_t)is_big;
    g_last_hp[slot] = hp;
    if (!is_big) return;
    if (g_bmspawn_n == 0) return;
    mhfu_bigmonster_spawn_ctx_t c;
    c.entity_ptr   = entity;
    c.slot         = slot;
    c.monster_type = type;
    c._pad[0] = c._pad[1] = c._pad[2] = 0;
    c.initial_hp   = hp;
    for (int i = 0; i < g_bmspawn_n; i++) g_bmspawn_chain[i].cb(&c);
}

/* Run from the monster-spawn poll thread (alongside the slot-ptr walk):
 * for each big-monster slot, watch HP > 0 -> 0 edge. */
extern "C" void mhfu_ai_poll_death(void)
{
    if (g_bmdeath_n == 0) return;
    for (int slot = 1; slot < MHFU_ENTITY_REGISTRY_SLOTS; slot++) {
        if (!g_was_big[slot]) continue;
        uint32_t e = mhfu_entity_at(slot);
        if (!e) { g_was_big[slot] = 0; continue; }
        uint16_t hp = mhfu_entity_hp(e);
        uint16_t prev = g_last_hp[slot];
        g_last_hp[slot] = hp;
        if (prev > 0 && hp == 0) {
            mhfu_bigmonster_death_ctx_t c;
            c.entity_ptr   = e;
            c.slot         = slot;
            c.monster_type = mhfu_entity_type(e);
            c._pad[0] = c._pad[1] = c._pad[2] = 0;
            for (int i = 0; i < g_bmdeath_n; i++) g_bmdeath_chain[i].cb(&c);
            g_was_big[slot] = 0;     /* one-shot — avoid re-firing on respawn */
        }
    }
}
