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
 *   on_bigmonster_action_decided
 *     - Hook = vt[8] swap on shared 0x08865254 (all 4 species).
 *     - vt[8] is the species probability-table lookup; engine returns
 *       an OUTCOME POINTER into the per-species table at entity+0x1AC
 *       (or sentinels 0x3E8 = "no table" / 0 = "no entry").
 *     - Mod returns a replacement outcome ptr (or the same one).
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

typedef void (*mhfu_ai_step_cb_t)(const mhfu_ai_step_ctx_t *ctx);

/* --- registration ---------------------------------------------------- */

/* Higher priority runs first.  Ties keep registration order. */
mhfu_hook_rc_t mhfu_on_bigmonster_action_decided(
    mhfu_action_override_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_ai_step(
    mhfu_ai_step_cb_t cb, int priority);

mhfu_hook_rc_t mhfu_on_bigmonster_spawn(
    mhfu_bigmonster_spawn_cb_t cb, int priority);
mhfu_hook_rc_t mhfu_on_bigmonster_death(
    mhfu_bigmonster_death_cb_t cb, int priority);

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
