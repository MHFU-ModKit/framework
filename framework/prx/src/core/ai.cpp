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

/* Action-validity walker — reads the species data table at runtime so the
 * predicate auto-updates if the game's species data changes. Eliminates the
 * need for hand-maintained tables in lockstep with ai_actions.h.
 *
 * Popo / anteka / giadrome all use the same (u16 id, u16 dur) record format
 * at species_entry+0x0C (8-byte header skipped). Tigrex's +0x0C is hitzone
 * data instead — its action selection is pointer-based via the probability
 * table at entity+0x1AC (Section 32h, 2026-05-30) — so id validation can't
 * apply; we accept all for tigrex.
 */
#define SPECIES_TABLE_BASE   0x09BB87C0u
#define SPECIES_STRIDE       0x1D0u
#define ACTION_LIST_PTR_OFF  0x0Cu
#define ACTION_LIST_HEADER   0x08u
#define ACTION_REC_SIZE      0x04u

extern "C" int mhfu_action_is_valid(uint8_t monster_type, uint32_t action_id)
{
    /* True big monster — engine returns pointers, not IDs. */
    if (monster_type == 0x4B) return 1;
    /* Unknown / unsupported species. */
    if (monster_type != 0x45 && monster_type != 0x46 && monster_type != 0x4D)
        return 0;

    uint32_t entry = SPECIES_TABLE_BASE + (uint32_t)monster_type * SPECIES_STRIDE;
    uint32_t list_ptr = *(volatile uint32_t *)(entry + ACTION_LIST_PTR_OFF);
    if (list_ptr < 0x08000000u || list_ptr >= 0x0A000000u) return 0;

    uint16_t id16 = (uint16_t)(action_id & 0xFFFFu);
    uint32_t rec  = list_ptr + ACTION_LIST_HEADER;
    for (int i = 0; i < 128; i++) {
        uint16_t aid = *(volatile uint16_t *)(rec + 0);
        if (aid == 0xFFFFu || aid == 0) return 0;
        if (aid == id16) return 1;
        rec += ACTION_REC_SIZE;
    }
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

/* Slot-loop top inside z_un_08865648 — see RE in docs/AI_SCRIPTING_ENGINE.md.
 * Layout:
 *   0x088656B0  sll   $v0, $s1, 6        ; <-- patch site (2 insns)
 *   0x088656B4  addu  $v0, $s2, $v0
 *   0x088656B8  lb    $v0, 0xbe($v0)     ; skip-gate #1
 *   ...                                  ; body uses $s1=slot, $s0=ent+2*slot
 *   0x08865728  addiu $s0, $s0, 2        ; (delay slot of bnez loop tail)
 *
 * $s1 holds slot index, $s0 = $s2 + 2*slot is the input cursor, $s2 is
 * the entity ptr. Our wrapper runs at loop top, calls a C dispatcher
 * that returns the slot to actually process, rewrites $s1 + $s0, then
 * replays the two patched insns and jumps into the body. */
#define SLOT_LOOP_TOP         0x088656B0u
#define SLOT_LOOP_BODY_RESUME 0x088656B8u   /* first un-displaced insn */

/* AI overlay loader call site (Section 2026-05-31 RE). The actual write
 * to overlay memory is done by memcpy from inside a per-segment loader
 * function at 0x0884EA1C; that function is invoked by a `jal` at
 * 0x0884E9F8 from inside its parent (0x0884E998). We wrap THAT outer
 * jal instead of memcpy directly because memcpy is also called for many
 * unrelated boot copies — wedging the boot timing if we wrap there.
 * One fire per load-orchestrator call → on the order of a few dozen
 * fires per map transition vs tens of thousands at the memcpy site.
 *
 * The wrapped jal's $a0 is the loader-context ptr, not the overlay dest,
 * so the helper detects AI-overlay residency by checking the
 * dispatcher's prologue signature at 0x09AC5228
 * (`addiu $sp, $sp, -0x40` = 0x27BDFFC0). One-shot per session. */
#define OVERLAY_LOADER_JAL    0x0884E9F8u
#define OVERLAY_LOADER_TARGET 0x0884EA1Cu
#define OVERLAY_AI_BASE       0x09ABF200u
#define OVERLAY_AI_SIZE       0x00100000u
#define OVERLAY_AI_END        (OVERLAY_AI_BASE + OVERLAY_AI_SIZE)
#define OVERLAY_AI_PROBE      0x09AC5228u   /* dispatcher prologue location */
#define OVERLAY_AI_PROBE_SIG  0x27BDFFC0u   /* addiu $sp, $sp, -0x40        */

/* Overlay-side slot loop (RE'd 2026-05-31 against the live overlay).
 * Counterpart to the EBOOT slot loop in z_un_08865648 — but this is the
 * one that actually executes for big-monster AI ticks in combat
 * (`entity+0x288` bit 17 gates the EBOOT path off for tigrex et al).
 *
 *   0x09AC52C4  move $s3, $zero          ; slot counter init
 *   0x09AC52DC  lw   $v1, 0x640($s5)     ; <-- patch site (2 insns)
 *   0x09AC52E0  addiu $a0, $zero, 2
 *   ...                                  ; body uses $s3 (slot),
 *                                        ; $s0 = entity + slot*2,
 *                                        ; $s1 = slot * 0xC8,
 *                                        ; $s6 = entity + slot*0x40,
 *                                        ; $s5 = entity (constant)
 *   0x09AC54C0  addiu $s3, $s3, 1
 *   0x09AC54C4  addiu $s1, $s1, 0xC8
 *   0x09AC54C8  sltu  $v1, $s3, $v1
 *   0x09AC54CC  addiu $s0, $s0, 2
 *   0x09AC54D0  bnez  $v1, 0x09AC52DC    ; back to body resume
 *   0x09AC54D4  addiu $s6, $s6, 0x40     ; delay slot
 *
 * Our wrapper replays the two displaced insns + recomputes $s0/$s1/$s6
 * from the (possibly redirected) slot, then resumes at 0x09AC52E4. */
#define OVL_SLOT_LOOP_TOP         0x09AC52DCu
#define OVL_SLOT_LOOP_BODY_RESUME 0x09AC52E4u
#define OVL_SLOT_LOOP_DISPLACED0  0x8EA30640u   /* lw $v1, 0x640($s5)   */
#define OVL_SLOT_LOOP_DISPLACED1  0x24040002u   /* addiu $a0, $zero, 2 */

#define AI_OWNER_TAG "mhfu_ai"

/* --- chain storage --------------------------------------------------- */

typedef struct { mhfu_ai_overlay_loaded_cb_t cb; int priority; } overlay_entry_t;
typedef struct { mhfu_slot_picked_override_cb_t cb; int priority; } slot_entry_t;
typedef struct { mhfu_action_input_override_cb_t cb; int priority; } input_entry_t;
typedef struct { mhfu_action_override_cb_t cb; int priority; } action_entry_t;
typedef struct { mhfu_ai_step_cb_t         cb; int priority; } step_entry_t;
typedef struct { mhfu_bigmonster_spawn_cb_t cb; int priority; } bmspawn_entry_t;
typedef struct { mhfu_bigmonster_death_cb_t cb; int priority; } bmdeath_entry_t;

static overlay_entry_t g_overlay_chain[MAX_HANDLERS];
static int             g_overlay_n = 0;
static slot_entry_t    g_slot_chain[MAX_HANDLERS];
static int             g_slot_n = 0;
static input_entry_t   g_input_chain[MAX_HANDLERS];
static int             g_input_n = 0;
static action_entry_t  g_action_chain[MAX_HANDLERS];
static int             g_action_n = 0;
static step_entry_t    g_step_chain[MAX_HANDLERS];
static int             g_step_n = 0;
static bmspawn_entry_t g_bmspawn_chain[MAX_HANDLERS];
static int             g_bmspawn_n = 0;
static bmdeath_entry_t g_bmdeath_chain[MAX_HANDLERS];
static int             g_bmdeath_n = 0;

/* Current slot index seen by the loop wrapper this iteration, for
 * action_input/decided ctx.slot enrichment. Valid only between loop
 * wrapper entry and next iteration; outside that window, treat as
 * stale.  Updated unconditionally by the wrapper (if installed). */
static volatile uint8_t g_current_slot = 0;

/* HP transition tracker (used for death edge). Indexed by registry slot. */
static uint16_t        g_last_hp[MHFU_ENTITY_REGISTRY_SLOTS];
static uint8_t         g_was_big[MHFU_ENTITY_REGISTRY_SLOTS];

static int            g_action_hook_installed  = 0;
static int            g_step_hook_installed    = 0;
static int            g_slot_hook_installed    = 0;
static int            g_overlay_hook_installed = 0;

/* Cave-allocated stubs (set on first install). */
static uint32_t      *g_action_stub = 0;
static uint32_t      *g_slot_wrapper = 0;
static uint32_t      *g_overlay_slot_wrapper = 0;
static int            g_overlay_slot_hook_installed = 0;

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

/* --- per-species (vt8_input -> ptr) cache --------------------------------
 * Big-monster (tigrex-style) vt[8] returns a per-run pointer into a
 * per-entity probability sub-table; the pointer varies across runs but the
 * input is stable. We snoop every observed pair and expose the cache via
 * mhfu_action_ptr_for() so mods can pin "give me the ptr the engine
 * resolved for THIS input last time we saw it" — the only safe way to
 * force a specific tigrex action without re-running vt[8] (which has
 * VFPU side effects and a small RNG path that we don't want to perturb).
 *
 * 4 species slots (popo / anteka / tigrex / giadrome); 96 entries each is
 * comfortably above the 54 unique inputs observed live for tigrex.
 */
#define ACT_CACHE_PER_SPECIES 96

typedef struct { uint16_t input; uint16_t _pad; uint32_t ptr; } act_cache_entry_t;

static act_cache_entry_t g_act_cache[4][ACT_CACHE_PER_SPECIES];
static int               g_act_cache_n[4];

static int species_cache_index(uint8_t type)
{
    switch (type) {
        case 0x46: return 0;   /* POPO     */
        case 0x45: return 1;   /* ANTEKA   */
        case 0x4B: return 2;   /* TIGREX   */
        case 0x4D: return 3;   /* GIADROME */
        default:   return -1;
    }
}

static void ai_cache_remember(uint8_t type, uint16_t input, uint32_t ptr)
{
    if (ptr == 0) return;          /* probe miss — don't cache "no action" */
    int sp = species_cache_index(type);
    if (sp < 0) return;
    /* Refresh existing entry if present. */
    for (int i = 0; i < g_act_cache_n[sp]; i++) {
        if (g_act_cache[sp][i].input == input) {
            g_act_cache[sp][i].ptr = ptr;
            return;
        }
    }
    if (g_act_cache_n[sp] >= ACT_CACHE_PER_SPECIES) return;   /* full — drop */
    g_act_cache[sp][g_act_cache_n[sp]].input = input;
    g_act_cache[sp][g_act_cache_n[sp]].ptr   = ptr;
    g_act_cache_n[sp]++;
}

extern "C" uint32_t mhfu_action_ptr_for(uint8_t monster_type, uint16_t input)
{
    int sp = species_cache_index(monster_type);
    if (sp < 0) return 0;
    for (int i = 0; i < g_act_cache_n[sp]; i++) {
        if (g_act_cache[sp][i].input == input) return g_act_cache[sp][i].ptr;
    }
    return 0;
}

extern "C" int mhfu_action_cache_size(uint8_t monster_type)
{
    int sp = species_cache_index(monster_type);
    return (sp < 0) ? 0 : g_act_cache_n[sp];
}

extern "C" int mhfu_action_cache_entry(uint8_t monster_type, int i,
                                       uint16_t *out_input, uint32_t *out_ptr)
{
    int sp = species_cache_index(monster_type);
    if (sp < 0 || i < 0 || i >= g_act_cache_n[sp]) return 0;
    if (out_input) *out_input = g_act_cache[sp][i].input;
    if (out_ptr)   *out_ptr   = g_act_cache[sp][i].ptr;
    return 1;
}

/* Slot-loop wrapper dispatcher: runs at the top of every AI slot
 * iteration. Returns the slot index the loop should actually process.
 * Also stashes that slot in g_current_slot for ctx.slot enrichment in
 * the action_input/action_decided chains that fire downstream. */
extern "C" uint32_t mhfu_ai_slot_picked_dispatch_c(
    uint32_t entity, uint32_t slot)
{
    uint32_t s = slot & 0xFFu;
    /* Always stash; downstream dispatchers read it. */
    g_current_slot = (uint8_t)s;
    if (g_slot_n == 0) return s;
    if (!mhfu_entity_is_big_monster(entity)) return s;

    mhfu_slot_picked_ctx_t ctx;
    ctx.entity_ptr    = entity;
    ctx.monster_type  = mhfu_entity_type(entity);
    ctx.original_slot = (uint8_t)s;
    ctx.action_count  = *(volatile uint16_t *)(entity + 0x1A2u);

    uint8_t v = (uint8_t)s;
    for (int i = 0; i < g_slot_n; i++) v = g_slot_chain[i].cb(&ctx, v);

    /* Clamp to declared bound to keep the loop safe from a mod
     * returning garbage. action_count == 0 is impossible here (loop
     * wouldn't have entered), but guard anyway. */
    if (ctx.action_count == 0) return s;
    if (v >= (uint8_t)ctx.action_count) v = (uint8_t)(ctx.action_count - 1);

    g_current_slot = v;
    return (uint32_t)v;
}

/* PRE-call dispatcher: runs BEFORE the engine's vt[8] body. Returns the
 * (possibly mutated) input value to forward into vt[8]. Lower 16 bits
 * are meaningful; upper bits are zero. */
extern "C" uint32_t mhfu_ai_action_input_dispatch_c(
    uint32_t entity, uint32_t vt8_input)
{
    uint32_t in = vt8_input & 0xFFFFu;
    if (g_input_n == 0) return in;
    if (!mhfu_entity_is_big_monster(entity)) return in;

    mhfu_action_input_ctx_t ctx;
    ctx.entity_ptr   = entity;
    ctx.monster_type = mhfu_entity_type(entity);
    ctx.slot         = g_slot_hook_installed ? g_current_slot : 0;
    ctx.vt8_input    = (uint16_t)in;

    uint16_t v = (uint16_t)in;
    for (int i = 0; i < g_input_n; i++) {
        v = g_input_chain[i].cb(&ctx, v);
        ctx.vt8_input = v;
    }
    return (uint32_t)v & 0xFFFFu;
}

extern "C" uint32_t mhfu_ai_action_dispatch_c(
    uint32_t entity, uint32_t vt8_input, uint32_t engine_action_id)
{
    if (!mhfu_entity_is_big_monster(entity)) return engine_action_id;

    /* Snoop the (input, ptr) pair into the per-species cache regardless of
     * whether any mod has subscribed — so resolution works the moment a
     * mod calls mhfu_action_ptr_for(). */
    uint8_t type = mhfu_entity_type(entity);
    ai_cache_remember(type, (uint16_t)(vt8_input & 0xFFFF), engine_action_id);

    if (g_action_n == 0) return engine_action_id;

    mhfu_action_decision_ctx_t ctx;
    ctx.entity_ptr   = entity;
    ctx.monster_type = type;
    ctx.slot         = g_slot_hook_installed ? g_current_slot : 0;
    ctx.vt8_input    = (uint16_t)(vt8_input & 0xFFFF);

    uint32_t v = engine_action_id;
    for (int i = 0; i < g_action_n; i++) v = g_action_chain[i].cb(&ctx, v);
    return v;
}

/* Stub layout (21 insns, single-basic-block per Section 22 JIT rules):
 *
 *   addiu sp, sp, -0x20
 *   sw    ra, 0x18(sp)
 *   sw    a0, 0x04(sp)                ; save entity
 *   sw    a1, 0x08(sp)                ; save engine input
 *   jal   pre_dispatch_c              ; (a0=entity, a1=input) -> v0=new input
 *   nop
 *   sw    v0, 0x08(sp)                ; persist mutated input
 *   lw    a0, 0x04(sp)
 *   move  a1, v0                      ; a1 = mutated input
 *   jal   VT8_ORIGINAL                ; engine picker runs with our input
 *   nop
 *   lw    a0, 0x04(sp)
 *   lw    a1, 0x08(sp)                ; mutated input (post sees same value)
 *   move  a2, v0                      ; engine outcome ptr / id
 *   jal   post_dispatch_c             ; (a0,a1,a2) -> v0 final
 *   nop
 *   lw    ra, 0x18(sp)
 *   jr    ra
 *   addiu sp, sp, 0x20                ; delay slot
 *
 * Both dispatcher C functions early-out when their chain is empty, so a
 * mod that only subscribes to one side pays only the call/return cost
 * for the other.
 */
static int build_action_stub(uint32_t *stub,
                             uint32_t pre_dispatch_addr,
                             uint32_t post_dispatch_addr)
{
    int i = 0;
    stub[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    stub[i++] = mips_sw   (MIPS_REG_RA,  0x18, MIPS_REG_SP);
    stub[i++] = mips_sw   (MIPS_REG_A0,  0x04, MIPS_REG_SP);   /* save entity */
    stub[i++] = mips_sw   (MIPS_REG_A1,  0x08, MIPS_REG_SP);   /* save input  */
    /* PRE-call: (a0=entity, a1=input) -> v0 = mutated input. */
    stub[i++] = mips_jal(pre_dispatch_addr);
    stub[i++] = MIPS_NOP;
    stub[i++] = mips_sw   (MIPS_REG_V0,  0x08, MIPS_REG_SP);   /* persist new input */
    /* Call original vt[8] with the (possibly mutated) input. */
    stub[i++] = mips_lw   (MIPS_REG_A0,  0x04, MIPS_REG_SP);
    stub[i++] = mips_move (MIPS_REG_A1,  MIPS_REG_V0);
    stub[i++] = mips_jal(VT8_ORIGINAL);
    stub[i++] = MIPS_NOP;
    /* POST-call: thread v0 through the action_decided chain. */
    stub[i++] = mips_lw   (MIPS_REG_A0,  0x04, MIPS_REG_SP);   /* a0 = entity */
    stub[i++] = mips_lw   (MIPS_REG_A1,  0x08, MIPS_REG_SP);   /* a1 = mutated input */
    stub[i++] = mips_move (MIPS_REG_A2,  MIPS_REG_V0);          /* a2 = engine v0 */
    stub[i++] = mips_jal(post_dispatch_addr);
    stub[i++] = MIPS_NOP;
    /* v0 holds final dispatcher return. Restore + return. */
    stub[i++] = mips_lw   (MIPS_REG_RA,  0x18, MIPS_REG_SP);
    stub[i++] = mips_jr(MIPS_REG_RA);
    stub[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);    /* delay slot */
    return i;
}

static int install_action_hook(void)
{
    if (g_action_hook_installed) return 0;

    uint32_t *stub = mhfu_cave_alloc(24);
    if (!stub) { mhfu_log("[ai] cave exhausted for action stub"); return -1; }
    int n = build_action_stub(
        stub,
        (uint32_t)(uintptr_t)&mhfu_ai_action_input_dispatch_c,
        (uint32_t)(uintptr_t)&mhfu_ai_action_dispatch_c);
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

/* --- ai overlay loader hook -----------------------------------------
 *
 * Wraps the jal at EBOOT 0x0884EAA8 (postfix). When memcpy returns, the
 * helper sees $a0 = original dest. If dest lies in the AI overlay range
 * (0x09abf200..0x09bbf200), we fan out the chain — mods have a clean
 * window to patch overlay code (e.g., the dispatcher at 0x09AC5228)
 * before any AI tick runs and PPSSPP's JIT pre-caches the bytes. */
extern "C" void mhfu_ai_overlay_loaded_helper(uint32_t /*ctx_a0*/)
{
    /* Postfix on the loader's jal: $a0 is the caller's loader-context
     * (we ignore it). Detect by signature at OVERLAY_AI_PROBE, fire
     * once per session — on the next quest map-enter we want the
     * subscriber to re-apply if needed, but for now one-shot is safe
     * because cold boot is the only path that resets engine memory. */
    static volatile int s_fired = 0;
    if (s_fired) return;
    uint32_t w = *(volatile uint32_t *)OVERLAY_AI_PROBE;
    if (w != OVERLAY_AI_PROBE_SIG) return;     /* not our overlay */
    s_fired = 1;

    /* Now that the AI overlay bytes are in RAM but the engine hasn't
     * ticked AI yet (= no JIT translation of the overlay AI code), this
     * is the window to install the overlay-side slot-loop wrapper that
     * mirrors the EBOOT one. No-op if no slot_picked subscribers. */
    extern int install_overlay_slot_hook_fwd(void);  /* see install fn below */
    install_overlay_slot_hook_fwd();

    if (g_overlay_n == 0) {
        mhfu_log("[ai] AI overlay loaded (probe@0x%08X = 0x%08X) — no subscribers",
                 OVERLAY_AI_PROBE, (unsigned)w);
        return;
    }
    mhfu_ai_overlay_ctx_t ctx;
    ctx.dest = OVERLAY_AI_BASE;
    ctx.src  = 0;
    ctx.size = OVERLAY_AI_SIZE;
    for (int i = 0; i < g_overlay_n; i++) g_overlay_chain[i].cb(&ctx);
}

static int install_overlay_hook(void)
{
    if (g_overlay_hook_installed) return 0;
    mhfu_hook_rc_t r = mhfu_install_call_wrapper(
        OVERLAY_LOADER_JAL,
        OVERLAY_LOADER_TARGET,
        (void (*)(uint32_t))&mhfu_ai_overlay_loaded_helper,
        MHFU_WRAP_POSTFIX,
        AI_OWNER_TAG);
    if (r != MHFU_HOOK_OK) {
        mhfu_log("[ai] overlay loader wrap failed (r=%d)", (int)r);
        return -1;
    }
    g_overlay_hook_installed = 1;
    mhfu_log("[ai] overlay loader wrap queued @ 0x%08X (postfix on jal -> 0x%08X)",
             OVERLAY_LOADER_JAL, OVERLAY_LOADER_TARGET);
    return 0;
}

/* --- slot-loop wrapper (top of z_un_08865648's AI slot loop) ---------
 *
 * Patches the first two insns of the loop top with `J wrapper; nop`.
 * The wrapper calls our C dispatcher with (entity=$s2, slot=$s1),
 * receives the (possibly redirected) slot in $v0, rewrites $s1 (slot)
 * and $s0 (entity + 2*slot — the +0x1B8 input cursor) to match, then
 * replays the two displaced insns and falls through to the body.
 *
 * Wrapper layout (16 insns + nop padding):
 *   addiu sp, sp, -0x10
 *   sw    ra, 0x0c(sp)
 *   move  a0, s2                 ; entity
 *   jal   dispatch_c
 *   move  a1, s1                 ; (delay) current slot
 *   lw    ra, 0x0c(sp)
 *   addiu sp, sp,  0x10
 *   andi  v0, v0, 0xff           ; clamp to u8
 *   move  s1, v0                 ; new slot
 *   sll   v0, s1, 1              ; v0 = slot*2
 *   addu  s0, s2, v0             ; $s0 = entity + 2*slot
 *   sll   v0, s1, 6              ; replay displaced #1
 *   addu  v0, s2, v0             ; replay displaced #2
 *   j     SLOT_LOOP_BODY_RESUME  ; into 0x088656B8
 *   nop                          ; (delay slot of J)
 *
 * z_un_08865648 saved its $ra to (its own) sp+0x0c in the prologue, so
 * trashing $ra during the inner jal is harmless: the function reloads
 * $ra from its frame before its final jr.  We use our own 0x10-byte
 * frame purely so the inner jal has a clean stack window. */
static int build_slot_wrapper(uint32_t *w, uint32_t dispatch_addr)
{
    int i = 0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x10);
    w[i++] = mips_sw   (MIPS_REG_RA,  0x0c, MIPS_REG_SP);
    w[i++] = mips_move (MIPS_REG_A0,  MIPS_REG_S2);              /* entity */
    w[i++] = mips_jal  (dispatch_addr);
    w[i++] = mips_move (MIPS_REG_A1,  MIPS_REG_S1);              /* delay: slot */
    w[i++] = mips_lw   (MIPS_REG_RA,  0x0c, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP,  0x10);
    /* andi v0, v0, 0xff */
    w[i++] = (0x0Cu << 26) | (MIPS_REG_V0 << 21) | (MIPS_REG_V0 << 16) | 0x00FFu;
    w[i++] = mips_move (MIPS_REG_S1,  MIPS_REG_V0);              /* $s1 = new slot */
    /* sll v0, s1, 1  (R-type: 0, 0, rd=v0, rt=s1, sa=1, sll func=0) */
    w[i++] = (MIPS_REG_S1 << 16) | (MIPS_REG_V0 << 11) | (1 << 6) | 0x00u;
    /* addu s0, s2, v0  (R-type: 0, rs=s2, rt=v0, rd=s0, sa=0, addu=0x21) */
    w[i++] = (MIPS_REG_S2 << 21) | (MIPS_REG_V0 << 16) | (MIPS_REG_S0 << 11) | 0x21u;
    /* Replay displaced insn #1: sll v0, s1, 6 */
    w[i++] = (MIPS_REG_S1 << 16) | (MIPS_REG_V0 << 11) | (6 << 6) | 0x00u;
    /* Replay displaced insn #2: addu v0, s2, v0 */
    w[i++] = (MIPS_REG_S2 << 21) | (MIPS_REG_V0 << 16) | (MIPS_REG_V0 << 11) | 0x21u;
    w[i++] = mips_j(SLOT_LOOP_BODY_RESUME);
    w[i++] = MIPS_NOP;                                            /* delay slot */
    while (i < 20) w[i++] = MIPS_NOP;
    return i;
}

/* --- overlay-side slot-loop wrapper --------------------------------------
 *
 * Same dispatch C function as the EBOOT wrapper (`mhfu_ai_slot_picked_dispatch_c`)
 * so any mod registered for on_bigmonster_slot_picked sees redirects fire
 * from both paths.  Installed lazily from mhfu_ai_overlay_loaded_helper —
 * by then the overlay bytes are in RAM but JIT hasn't yet translated them,
 * so we patch with a direct write + cache flush (no quiet-gate needed).
 *
 * Wrapper layout (18 insns; cave alloc 20):
 *   addiu sp, sp, -0x10
 *   sw    ra, 0x0c(sp)
 *   move  a0, s5                       ; entity
 *   jal   dispatch_c
 *   move  a1, s3                       ; (delay) current slot
 *   move  s3, v0                       ; new slot
 *   lw    ra, 0x0c(sp)
 *   addiu sp, sp,  0x10
 *   ori   t0, zero, 0xC8
 *   mult  s3, t0                       ; HI:LO = slot * 0xC8
 *   mflo  s1                           ; s1 = slot * 0xC8
 *   sll   t0, s3, 1                    ; t0 = slot * 2
 *   addu  s0, s5, t0                   ; s0 = entity + slot*2
 *   sll   t0, s3, 6                    ; t0 = slot * 0x40
 *   addu  s6, s5, t0                   ; s6 = entity + slot*0x40
 *   lw    v1, 0x640(s5)                ; replayed insn #1
 *   j     OVL_SLOT_LOOP_BODY_RESUME
 *   addiu a0, zero, 2                  ; replayed insn #2 (J's delay slot)
 */
static int build_overlay_slot_wrapper(uint32_t *w, uint32_t dispatch_addr)
{
    int i = 0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x10);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x0c, MIPS_REG_SP);
    w[i++] = mips_move (MIPS_REG_A0, MIPS_REG_S5);
    w[i++] = mips_jal  (dispatch_addr);
    w[i++] = mips_move (MIPS_REG_A1, MIPS_REG_S3);                /* delay */
    w[i++] = mips_move (MIPS_REG_S3, MIPS_REG_V0);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x0c, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP,  0x10);
    /* ori t0, zero, 0xC8 */
    w[i++] = (0x0Du << 26) | (MIPS_REG_ZERO << 21) | (MIPS_REG_T0 << 16) | 0x00C8u;
    /* mult s3, t0 */
    w[i++] = (MIPS_REG_S3 << 21) | (MIPS_REG_T0 << 16) | 0x18u;
    /* mflo s1 */
    w[i++] = (MIPS_REG_S1 << 11) | 0x12u;
    /* sll t0, s3, 1 */
    w[i++] = (MIPS_REG_S3 << 16) | (MIPS_REG_T0 << 11) | (1 << 6);
    /* addu s0, s5, t0 */
    w[i++] = (MIPS_REG_S5 << 21) | (MIPS_REG_T0 << 16) | (MIPS_REG_S0 << 11) | 0x21u;
    /* sll t0, s3, 6 */
    w[i++] = (MIPS_REG_S3 << 16) | (MIPS_REG_T0 << 11) | (6 << 6);
    /* addu s6, s5, t0 */
    w[i++] = (MIPS_REG_S5 << 21) | (MIPS_REG_T0 << 16) | (MIPS_REG_S6 << 11) | 0x21u;
    /* replay #1: lw v1, 0x640(s5) */
    w[i++] = mips_lw(MIPS_REG_V1, 0x640, MIPS_REG_S5);
    w[i++] = mips_j(OVL_SLOT_LOOP_BODY_RESUME);
    /* replay #2 (J delay slot): addiu a0, zero, 2 */
    w[i++] = mips_addiu(MIPS_REG_A0, MIPS_REG_ZERO, 2);
    while (i < 20) w[i++] = MIPS_NOP;
    return i;
}

/* External alias so the helper above can forward-call without a header. */
extern "C" int install_overlay_slot_hook_fwd(void);

static int install_overlay_slot_hook(void)
{
    if (g_overlay_slot_hook_installed) return 0;
    /* Skip if nobody subscribed — saves cave words for unused capability. */
    if (g_slot_n == 0) return 0;

    uint32_t *w = mhfu_cave_alloc(20);
    if (!w) { mhfu_log("[ai] cave exhausted for ovl slot wrapper"); return -1; }
    build_overlay_slot_wrapper(w,
        (uint32_t)(uintptr_t)&mhfu_ai_slot_picked_dispatch_c);
    mhfu_flush_caches();
    g_overlay_slot_wrapper = w;

    uint32_t cur0 = *(volatile uint32_t *)OVL_SLOT_LOOP_TOP;
    uint32_t cur1 = *(volatile uint32_t *)(OVL_SLOT_LOOP_TOP + 4);
    if (cur0 != OVL_SLOT_LOOP_DISPLACED0 || cur1 != OVL_SLOT_LOOP_DISPLACED1) {
        mhfu_log("[ai] ovl slot loop unexpected: [0x%08X]=0x%08lx [0x%08X]=0x%08lx",
                 OVL_SLOT_LOOP_TOP, (unsigned long)cur0,
                 OVL_SLOT_LOOP_TOP + 4, (unsigned long)cur1);
        return -1;
    }
    *(volatile uint32_t *)OVL_SLOT_LOOP_TOP       = mips_j((uint32_t)(uintptr_t)w);
    *(volatile uint32_t *)(OVL_SLOT_LOOP_TOP + 4) = MIPS_NOP;
    mhfu_flush_caches();

    g_overlay_slot_hook_installed = 1;
    mhfu_log("[ai] ovl slot loop wrapped @ 0x%08X (wrapper 0x%08X, resumes 0x%08X)",
             OVL_SLOT_LOOP_TOP, (unsigned)(uintptr_t)w, OVL_SLOT_LOOP_BODY_RESUME);
    return 0;
}

extern "C" int install_overlay_slot_hook_fwd(void) { return install_overlay_slot_hook(); }

static int install_slot_hook(void)
{
    if (g_slot_hook_installed) return 0;

    uint32_t *w = mhfu_cave_alloc(20);
    if (!w) { mhfu_log("[ai] cave exhausted for slot wrapper"); return -1; }
    build_slot_wrapper(w, (uint32_t)(uintptr_t)&mhfu_ai_slot_picked_dispatch_c);
    mhfu_flush_caches();
    g_slot_wrapper = w;

    /* Displaced insns:
     *   0x088656B0  sll  $v0, $s1, 6   (encoded as 0x00111180)
     *   0x088656B4  addu $v0, $s2, $v0 (encoded as 0x02421021)
     * Replace with: J wrapper ; NOP */
    uint32_t orig0 = (MIPS_REG_S1 << 16) | (MIPS_REG_V0 << 11) | (6 << 6) | 0x00u;
    uint32_t orig1 = (MIPS_REG_S2 << 21) | (MIPS_REG_V0 << 16) | (MIPS_REG_V0 << 11) | 0x21u;
    uint32_t new0  = mips_j((uint32_t)(uintptr_t)w);
    uint32_t new1  = MIPS_NOP;

    mhfu_hook_rc_t r0 = mhfu_patch_word_when_quiet(
        SLOT_LOOP_TOP + 0x00, orig0, new0, AI_OWNER_TAG);
    mhfu_hook_rc_t r1 = mhfu_patch_word_when_quiet(
        SLOT_LOOP_TOP + 0x04, orig1, new1, AI_OWNER_TAG);
    if (r0 != MHFU_HOOK_OK || r1 != MHFU_HOOK_OK) {
        mhfu_log("[ai] slot loop queue failed (r0=%d r1=%d)", (int)r0, (int)r1);
        return -1;
    }

    g_slot_hook_installed = 1;
    mhfu_log("[ai] slot_picked patch queued @ 0x%08X (wrapper 0x%08X)",
             SLOT_LOOP_TOP, (unsigned)(uintptr_t)w);
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

/* Entry-detour wrapper for z_un_08865648 (the per-frame per-entity AI tick).
 *
 * Patches:
 *    addr+0x00:  ADDIU sp,sp,-0x20  -> J wrapper        (0x27BDFFE0 -> J)
 *    addr+0x04:  SW    ra,0xC(sp)   -> NOP              (0xAFBF000C -> 0)
 *
 * Wrapper executes (12 insns):
 *    its own frame -> save ra/a0 -> jal helper(a0=entity) -> restore ra/a0
 *    -> displaced ADDIU sp,sp,-0x20 -> displaced SW ra,0xC(sp)
 *    -> J  addr+0x08  (resume function body where it expects post-prologue
 *                      state — sp decremented, ra saved).
 *
 * Quiet-gate the two patches at TITLE/MENU (Section 26 pattern): the EBOOT
 * is loaded but z_un_08865648 hasn't yet executed, so JIT pre-cache is
 * dodged — first translation of the block picks up our redirect.
 */
static int install_step_hook(void)
{
    if (g_step_hook_installed) return 0;

    uint32_t *w = mhfu_cave_alloc(16);
    if (!w) { mhfu_log("[ai] cave exhausted for ai_step wrapper"); return -1; }

    uint32_t helper = (uint32_t)(uintptr_t)&mhfu_ai_step_dispatch_c;
    int i = 0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);     /* our frame */
    w[i++] = mips_sw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_jal  (helper);
    w[i++] = MIPS_NOP;                                         /* delay slot */
    w[i++] = mips_lw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);       /* pop our frame */
    /* Replay displaced prologue (2 insns we overwrote). */
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x0C, MIPS_REG_SP);
    /* Resume function at the next un-displaced insn. */
    w[i++] = mips_j    (AI_STEP_TICK_ENTRY + 0x08);
    w[i++] = MIPS_NOP;                                         /* J delay slot */
    while (i < 16) w[i++] = MIPS_NOP;
    mhfu_flush_caches();

    uint32_t orig0 = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    uint32_t orig1 = mips_sw(MIPS_REG_RA, 0x0C, MIPS_REG_SP);
    uint32_t new0  = mips_j((uint32_t)(uintptr_t)w);
    uint32_t new1  = MIPS_NOP;

    mhfu_hook_rc_t r0 = mhfu_patch_word_when_quiet(
        AI_STEP_TICK_ENTRY + 0x00, orig0, new0, AI_OWNER_TAG);
    mhfu_hook_rc_t r1 = mhfu_patch_word_when_quiet(
        AI_STEP_TICK_ENTRY + 0x04, orig1, new1, AI_OWNER_TAG);
    if (r0 != MHFU_HOOK_OK || r1 != MHFU_HOOK_OK) {
        mhfu_log("[ai] ai_step queue failed (r0=%d r1=%d)", (int)r0, (int)r1);
        return -1;
    }

    g_step_hook_installed = 1;
    mhfu_log("[ai] ai_step entry detour queued @ 0x%08X (wrapper 0x%08X)",
             AI_STEP_TICK_ENTRY, (unsigned)(uintptr_t)w);
    return 0;
}

/* --- public registration --------------------------------------------- */

extern "C" mhfu_hook_rc_t mhfu_on_ai_overlay_loaded(
    mhfu_ai_overlay_loaded_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_overlay_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_overlay_chain, g_overlay_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_off_ai_overlay_loaded(
    mhfu_ai_overlay_loaded_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_overlay_chain, g_overlay_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_slot_picked(
    mhfu_slot_picked_override_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_slot_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_slot_chain, g_slot_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_action_input(
    mhfu_action_input_override_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_action_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_input_chain, g_input_n, MAX_HANDLERS, cb, priority);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_on_bigmonster_action_decided(
    mhfu_action_override_cb_t cb, int priority)
{
    if (!cb) return MHFU_HOOK_BADARG;
    if (install_action_hook() != 0) return MHFU_HOOK_CONFLICT;
    CHAIN_INSERT(g_action_chain, g_action_n, MAX_HANDLERS, cb, priority);
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

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_slot_picked(
    mhfu_slot_picked_override_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_slot_chain, g_slot_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_action_input(
    mhfu_action_input_override_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_input_chain, g_input_n, cb);
}

extern "C" mhfu_hook_rc_t mhfu_off_bigmonster_action_decided(
    mhfu_action_override_cb_t cb)
{
    if (!cb) return MHFU_HOOK_BADARG;
    CHAIN_REMOVE(g_action_chain, g_action_n, cb);
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
