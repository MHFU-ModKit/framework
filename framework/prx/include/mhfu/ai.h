/*
 * AI override events — synchronous priority-chain hooks that let mods
 * intercept and override the engine's per-frame AI decisions for big
 * monsters (Popo / Anteka / Tigrex / Giadrome and any other species
 * that share the generic AI engine).
 *
 * Distinct from the fan-out events in events.h: each AI override event
 * runs a chain of (priority, handler) tuples in priority-descending
 * order; each handler sees the previous handler's output as its input,
 * and returns the value to forward. Return the input unchanged to
 * abstain. The framework writes the final chain value into the
 * register the engine reads next.
 *
 * Hook points (verified Section 32d/g, 2026-05-30):
 *
 *   on_ai_overlay_loaded              (postfix, observe / install-window)
 *     - Fires the moment the engine's big-monster AI overlay region
 *       (0x09Axxxxx) has just been memcpy'd into place from DATA.BIN
 *       (~1 MB at dest 0x09abf200). The wrapper sits on the `jal` at
 *       EBOOT 0x0884EAA8 (call into a local memcpy-variant
 *       0x0884ECAC) and runs POSTFIX, so when the chain fires the
 *       bytes are already in RAM and no JIT translation has happened
 *       yet — the right window to patch overlay code (e.g., the AI
 *       dispatcher at 0x09AC5228) without racing PPSSPP's JIT
 *       pre-cache (Section 26 pattern, applied one layer above the
 *       function instead of "wait until TITLE/MENU").
 *     - Observe-only by signature: no return value; mods read ctx and
 *       perform their own writes against the freshly-mapped region.
 *
 *   on_bigmonster_slot_picked         (PRE-iteration, override-capable)
 *     - Hook = patch on the AI slot-loop entry inside z_un_08865648
 *       (loop top @ 0x088656B0). The loop iterates entity slots
 *       0..action_count-1; for each it derives the slot's input @
 *       entity+0x1B8+slot*2, calls vt[8], and applies the outcome to
 *       that slot's container @ entity+0x110+slot*0x40.
 *     - Runs once per iteration BEFORE the slot's skip gates +
 *       vt[8] + applier. Mods can REDIRECT this iteration to a
 *       different slot by returning a new slot index. Slot identity
 *       then flows through vt[8] (input read) AND the installer
 *       (container written), so the engine exercises its own
 *       same-slot path — no container-shape mismatch.
 *     - Quiet-gated install (Section 26): the patch lands at TITLE/
 *       MENU before PPSSPP's JIT caches the function. Affects all
 *       big-monster species sharing z_un_08865648.
 *
 *   on_bigmonster_action_input        (PRE-call, override-capable)
 *     - Same vt[8] swap stub as action_decided, but the chain runs
 *       BEFORE the engine's vt[8] body. Lets mods mutate the
 *       `vt8_input` ($a1) the picker reads — and because the picker
 *       reads input + per-input engine state in one pass, mutating
 *       here keeps the engine's downstream state (slot bookkeeping,
 *       applier descriptor walk) consistent with the chosen action.
 *       This is the durable way to FORCE a specific action. Use it
 *       instead of action_decided whenever you want to redirect the
 *       picker; reserve action_decided for OBSERVE or same-context
 *       sanitisation.
 *     - JIT-immune (vtable lookup).
 *
 *   on_bigmonster_action_decided      (POST-call, override-capable)
 *     - Hook = vt[8] swap on shared 0x08865254 (all 4 species).
 *     - vt[8] is the species probability-table lookup; engine returns
 *       an OUTCOME POINTER into the per-species table at entity+0x1AC
 *       (or sentinels 0x3E8 = "no table" / 0 = "no entry").
 *     - Mod returns a replacement outcome ptr (or the same one).
 *     - WARNING: forcing a ptr here that doesn't match the engine's
 *       just-completed pick can desync the applier (the picker has
 *       already written per-slot bookkeeping for the engine's choice).
 *       Prefer `on_bigmonster_action_input` for forcing.
 *     - JIT-immune (vtable lookup).
 *
 *   on_bigmonster_ai_step
 *     - Hook = prefix on z_un_08865648 (per-frame per-entity tick).
 *     - Observe-only (no override).
 *
 *   spawn / death — fan-out observe events (poll-driven).
 *
 * NOTE: there is no `anim_decided` event. An earlier design proposed
 * one hooking z_un_0885f928's return, but RE showed that function is a
 * SLOT-INDEX tree-search returning the per-slot state container — not
 * a transform of the vt[8] outcome. Behavioral override is fully
 * captured by action_decided. See docs/AI_SCRIPTING_ENGINE.md §32g.
 *
 * Python mods can observe these events through the Python event bridge
 * but cannot override -- the override chain runs in PRX inside the
 * engine's call stack, microsecond budget.
 */
#ifndef MHFU_AI_H
#define MHFU_AI_H

#include <stdint.h>
#include "hooks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- contexts --------------------------------------------------------- */

typedef struct {
    uint32_t entity_ptr;     /* the entity whose AI is deciding         */
    uint8_t  monster_type;   /* entity+0x1E8 species byte               */
    uint8_t  slot;           /* AI slot index (0..entity+0x1A2 - 1)     */
    uint16_t vt8_input;      /* a1 to vt[8] = u16 at +0x1B8 + slot*2    */
} mhfu_action_decision_ctx_t;

/* Context for on_bigmonster_action_input (PRE-call). vt8_input is the
 * input the engine is about to pass to vt[8] (already mutated by any
 * earlier higher-priority handler in the chain). */
typedef struct {
    uint32_t entity_ptr;
    uint8_t  monster_type;
    uint8_t  slot;
    uint16_t vt8_input;
} mhfu_action_input_ctx_t;

/* Context for on_ai_overlay_loaded. dest is the address memcpy was
 * called with ($a0 of the wrapped jal); src + size are the well-known
 * constants for this load (1 MB starting at 0x09abf200 from DATA.BIN
 * offset 0x0bb37800). Reported for callers that want to early-out on
 * unrelated copies though the wrapper already gates dest to the AI
 * overlay range. */
typedef struct {
    uint32_t dest;
    uint32_t src;     /* may be 0 if not introspectable */
    uint32_t size;    /* may be 0 if not introspectable */
} mhfu_ai_overlay_ctx_t;

/* Context for on_bigmonster_slot_picked (PRE-iteration). action_count
 * is u16 at entity+0x1A2 (the AI loop's upper bound). original_slot is
 * the slot the engine reached this iteration; the chain's return value
 * is what the iteration will actually process. */
typedef struct {
    uint32_t entity_ptr;
    uint8_t  monster_type;
    uint8_t  original_slot;
    uint16_t action_count;
} mhfu_slot_picked_ctx_t;

typedef struct {
    uint32_t entity_ptr;
    uint8_t  monster_type;
    uint8_t  _pad[3];
} mhfu_ai_step_ctx_t;

/* Spawn / death observe events.  Big-monster filtered. */
typedef struct {
    uint32_t entity_ptr;
    int      slot;          /* entity-registry slot 1..20 */
    uint8_t  monster_type;
    uint8_t  _pad[3];
    uint16_t initial_hp;
} mhfu_bigmonster_spawn_ctx_t;

typedef struct {
    uint32_t entity_ptr;
    int      slot;
    uint8_t  monster_type;
    uint8_t  _pad[3];
} mhfu_bigmonster_death_ctx_t;

typedef void (*mhfu_bigmonster_spawn_cb_t)(const mhfu_bigmonster_spawn_ctx_t *ctx);
typedef void (*mhfu_bigmonster_death_cb_t)(const mhfu_bigmonster_death_ctx_t *ctx);

/* --- handler signatures ---------------------------------------------- */

/* Override handlers: receive the engine's current decision (already
 * threaded through any higher-priority mods in the chain) plus context;
 * return the value to forward.  Return `engine_value` unchanged to
 * abstain.
 *
 * IMPORTANT — the meaning of `engine_action_id` differs by species class:
 *   * Small-monster-shape (POPO, ANTEKA, GIADROME): a u16 action ID,
 *     zero-extended into u32. Return a value from the matching
 *     `<SPECIES>_ACTION_*` macros in `ai_actions.h`.
 *   * True-big-monster-shape (TIGREX): a POINTER into the per-entity
 *     probability table at entity+0x1AC. Pointer values vary per run, so
 *     mods must CACHE a ptr they see for a given `ctx->vt8_input` and
 *     re-return that ptr later. Use `TIGREX_VT8_INPUT_*` macros to match
 *     on the (stable) input value; never return a small numeric literal
 *     as a "tigrex action ID" — it will be dereferenced as a pointer.
 */
typedef uint32_t (*mhfu_action_override_cb_t)(
    const mhfu_action_decision_ctx_t *ctx, uint32_t engine_action_id);

/* PRE-call handler: receives the current `vt8_input` (already threaded
 * through any higher-priority pre-handler) and returns the value the
 * engine's vt[8] will see. Return `current_input` unchanged to abstain.
 * Only the low 16 bits are used by the engine. */
typedef uint16_t (*mhfu_action_input_override_cb_t)(
    const mhfu_action_input_ctx_t *ctx, uint16_t current_input);

/* PRE-iteration handler: receives the slot index the loop reached;
 * return the slot index to actually process this iteration. Return
 * `current_slot` unchanged to abstain. Return a slot index outside
 * 0..action_count-1 to crash the engine — only return valid slot
 * indices. */
typedef uint8_t (*mhfu_slot_picked_override_cb_t)(
    const mhfu_slot_picked_ctx_t *ctx, uint8_t current_slot);

typedef void (*mhfu_ai_step_cb_t)(const mhfu_ai_step_ctx_t *ctx);

/* AI-overlay-loaded handler. Observe-only — no override semantics. */
typedef void (*mhfu_ai_overlay_loaded_cb_t)(const mhfu_ai_overlay_ctx_t *ctx);

/* --- registration ---------------------------------------------------- */

/* Higher priority runs first.  Ties keep registration order. */
mhfu_hook_rc_t mhfu_on_ai_overlay_loaded(
    mhfu_ai_overlay_loaded_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_off_ai_overlay_loaded(
    mhfu_ai_overlay_loaded_cb_t cb);

mhfu_hook_rc_t mhfu_on_bigmonster_slot_picked(
    mhfu_slot_picked_override_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_action_input(
    mhfu_action_input_override_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_action_decided(
    mhfu_action_override_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_ai_step(
    mhfu_ai_step_cb_t cb, int priority);

mhfu_hook_rc_t mhfu_on_bigmonster_spawn(
    mhfu_bigmonster_spawn_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_death(
    mhfu_bigmonster_death_cb_t cb, int priority);

mhfu_hook_rc_t mhfu_off_bigmonster_slot_picked   (mhfu_slot_picked_override_cb_t cb);
mhfu_hook_rc_t mhfu_off_bigmonster_action_input  (mhfu_action_input_override_cb_t cb);
mhfu_hook_rc_t mhfu_off_bigmonster_action_decided(mhfu_action_override_cb_t cb);
mhfu_hook_rc_t mhfu_off_bigmonster_ai_step      (mhfu_ai_step_cb_t        cb);
mhfu_hook_rc_t mhfu_off_bigmonster_spawn        (mhfu_bigmonster_spawn_cb_t cb);
mhfu_hook_rc_t mhfu_off_bigmonster_death        (mhfu_bigmonster_death_cb_t cb);

/* ---- action-ptr cache (tigrex-style pointer resolution) ----------------
 *
 * For big monsters whose vt[8] returns a POINTER (currently: tigrex), the
 * framework snoops every (vt8_input -> engine_id) pair seen on action_decided
 * and caches it per species. mhfu_action_ptr_for() returns the cached ptr
 * for an input — 0 if not yet observed.
 *
 * This is the convenience surface for forcing a specific tigrex action:
 *
 *   static uint32_t pin_to_charge(const mhfu_action_decision_ctx_t *ctx,
 *                                  uint32_t engine_value) {
 *       if (ctx->monster_type == MON_TIGREX) {
 *           uint32_t p = mhfu_action_ptr_for(MON_TIGREX,
 *                                            TIGREX_VT8_INPUT_0x04B1);
 *           if (p) return p;       // force this action whenever cached
 *       }
 *       return engine_value;       // abstain if not seen yet
 *   }
 *
 * Returns 0 if the input hasn't been observed yet — the engine must have
 * picked that action at least once this run. (A future helper would call
 * the original vt[8] directly to populate the cache eagerly; deferred
 * because vt[8] has VFPU + RNG side effects that we don't want to
 * perturb on speculative calls.)
 *
 * Cache size: 96 entries per species (popo/anteka/tigrex/giadrome).
 */
uint32_t mhfu_action_ptr_for(uint8_t monster_type, uint16_t vt8_input);
int      mhfu_action_cache_size(uint8_t monster_type);
int      mhfu_action_cache_entry(uint8_t monster_type, int index,
                                 uint16_t *out_input, uint32_t *out_ptr);

/* --- helpers --------------------------------------------------------- */

/* Returns the entity ptr whose +0x190 == action_list_ptr by scanning the
 * registry, or 0 if none.  Used internally by the anim_decided dispatcher
 * to populate ctx->entity_ptr; mods can call it too. */
uint32_t mhfu_entity_from_action_list(uint32_t action_list_ptr);

/* Predicate: is this entity a "big monster" per the current quest's
 * targets[2] list?  Returns 1 if yes, 0 otherwise.  Mod handlers should
 * call this to early-out on minor monsters. */
int mhfu_entity_is_big_monster(uint32_t entity_ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_AI_H */
