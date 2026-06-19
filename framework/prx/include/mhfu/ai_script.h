/*
 * ai_script.h — v1 Lua AI scripting layer for ported big monsters.
 *
 * Purpose: let a Lua script define the behaviour of a ported monster (e.g.
 * Brute Tigrex) that runs on a resident MHFU species as its engine host.
 * The engine handles rendering, physics, and slot management; the script
 * picks WHICH actions to play and WHEN by wiring into the existing event API.
 *
 * v1 SCOPE (intentionally minimal):
 *   Events:   on_spawn, on_tick  (poll/worker threads — direct Lua OK)
 *   Override: trigger_action(id) wrapping mhfu.on_bigmonster_action
 *   Helpers:  apply_freeze_gate_fix, apply_render_fix, set_aggro
 *   Baseline: resident species (Tigrex) acts + animates even without a script
 *
 * DEFERRED to v2: on_action_end (needs per-slot timer polling — not cheap),
 *   damage-per-part, part-break, wall-hit events.
 *
 * ---- Lua API (mhfu.* bindings, v1) ----------------------------------------
 *
 * Registration:
 *
 *   mhfu.on_bigmonster_spawn(fn)           -- fn(ent, mtype, slot, hp)
 *   mhfu.on_bigmonster_action(fn, prio)    -- fn(ctx) -> action_id
 *                                          -- ctx = { entity, type, action_id }
 *
 *   These are the same events used by tigrex_spin.lua.  spawn fires on the
 *   poll thread; action fires on the exec thread (game thread marshalled).
 *
 * Action force seam (coherent, no desync/crash):
 *
 *   From an on_bigmonster_action callback:
 *     return mhfu.TIGREX_ANGRY_SPIN         -- force this action this frame
 *     return ctx.action_id                  -- abstain (engine's own pick)
 *
 *   action_id to executor-a1 relation:
 *     executor_a1 = vt8_input(slot2) - 0x578 = vt8_input(slot0) - 0x3E8
 *     e.g. ANGRY_SPIN vt8_input=0x05A3 -> a1=0x2B
 *
 *   Named a1 constants (defined below):
 *     MHFU_AI_A1_IDLE_STAND      0x20
 *     MHFU_AI_A1_IDLE_WALK       0x03
 *     MHFU_AI_A1_ANGRY_SPIN      0x2B
 *     MHFU_AI_A1_ANGRY_CHARGE    0x11
 *     (add more by observing: a1 = captured vt8_input(slot2) - 0x578)
 *
 * Maintenance helpers (call from mhfu_tick or on_spawn):
 *
 *   mhfu.write_u32(ent + 0x4B8, 0)
 *     -- Clear the freeze gate.  Engine sets bits 0x100|0x10000 during forced
 *     -- repeat-fire and halts the AI tick.  Zero each action tick or the
 *     -- monster freezes after ~3 forced repeats.  ALSO zero from the 2 Hz
 *     -- worker (mhfu_tick) as the halted AI tick can't clear it itself.
 *
 *   mhfu.entity_make_visible(ent, area_index)
 *     -- Write entity+0x29A = area_index and set entity+0x638 bit 0x8000.
 *     -- Required for swap-spawned monsters that the engine culls until their
 *     -- first natural roam sets the section tracker.
 *
 *   mhfu.entity_force_aggro(ent, x, y, z)
 *     -- Write the pursuit vector (entity+0x5D0/+0x5DC=1.0) to force aggro.
 *
 * Per-frame tick (2 Hz, worker thread):
 *
 *   function mhfu_tick()
 *     -- called by the framework at ~2 Hz on the worker thread.
 *     -- Good for maintenance writes (freeze gate, map paint, etc.).
 *     -- NOT called on the game thread; do NOT call engine action functions
 *     -- here except through the mhfu.* helper wrappers.
 *   end
 *
 * ---- C companion (for PRX mods that want the same surface in C) -----------
 *
 * Tigrex executor-a1 constants (a1 = vt8_input(slot2) - 0x578):
 */
#ifndef MHFU_AI_SCRIPT_H
#define MHFU_AI_SCRIPT_H

#include <stdint.h>
#include "ai.h"
#include "entity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Executor a1 action constants (Tigrex, resident species) --------------
 *
 * These are the raw `a1` values passed to the big-mon executor 0x09AC5228.
 * Derivation: a1 = (slot2_vt8_input - 0x578) = (slot0_vt8_input - 0x3E8).
 * Names come from hand-observation of forced actions.  Regenerate unknowns
 * by forcing a value and watching the in-game animation.
 *
 * These constants are what a Lua on_bigmonster_action callback should return
 * to force a coherent full-body action (all 3 body-part slots get the same
 * action because the executor fans the single a1 to each).
 */
#define MHFU_AI_A1_IDLE_WALK         0x03u  /* TIGREX_IDLE_WALKSTRAIGHT (0x057B) */
#define MHFU_AI_A1_IDLE_STAND        0x20u  /* TIGREX_IDLE_STAND        (0x0598) */
#define MHFU_AI_A1_ANGRY_SPIN        0x2Bu  /* TIGREX_ANGRY_SPIN        (0x05A3) */
#define MHFU_AI_A1_ANGRY_CHARGE      0x11u  /* TIGREX_ANGRY_CHARGE      (0x0589) */
#define MHFU_AI_A1_ANGRY_BITE_FWD    0x29u  /* TIGREX_ANGRY_BITE_FORWARD(0x05A1) */
#define MHFU_AI_A1_ANGRY_JUMP_FWD    0x2Fu  /* TIGREX_ANGRY_JUMP_FORWARD(0x05A7) */
#define MHFU_AI_A1_ANGRY_THROW_ROCKS 0x2Du  /* TIGREX_ANGRY_THROWROCKS  (0x05A5) */
#define MHFU_AI_A1_ANGRY_TURN_LEFT   0x08u  /* TIGREX_ANGRY_TURN_LEFT   (0x0580) */
#define MHFU_AI_A1_ANGRY_TURN_RIGHT  0x07u  /* TIGREX_ANGRY_TURN_RIGHT  (0x057F) */
#define MHFU_AI_A1_IDLE_SUSPICIOUS   0x50u  /* TIGREX_IDLE_SUSPICIOUS   (0x05C8) */

/* Freeze-gate cell and bits (entity-relative offsets). */
#define MHFU_AI_OFF_FREEZE_GATE      0x4B8u  /* u32; bits 0x100|0x10000 halt AI */
#define MHFU_AI_FREEZE_BITS          (0x100u | 0x10000u)

/* Section-tracker cells for the render-visibility fix. */
#define MHFU_AI_OFF_SECTION          0x29Au  /* u16: monster's tracked section   */
#define MHFU_AI_OFF_FLAGS638         0x638u  /* u32: bit 0x8000 = render gate B  */
#define MHFU_AI_FLAGS638_RENDER_BIT  0x8000u

/* ---- Inline C helpers ------------------------------------------------------
 *
 * These wrap raw memory writes with the same semantics as the Lua helpers.
 * Mods that write C rather than Lua can call these directly.
 */

/* Clear the AI freeze gate on `entity`.  Call every action tick AND from the
 * 5 Hz poll or a worker thread as a background net (the halted AI tick cannot
 * run the per-tick clear itself once frozen). */
static inline void mhfu_aiscript_clear_freeze_gate(uint32_t entity)
{
    volatile uint32_t *gate =
        (volatile uint32_t *)(uintptr_t)(entity + MHFU_AI_OFF_FREEZE_GATE);
    *gate &= ~(uint32_t)MHFU_AI_FREEZE_BITS;
}

/* Apply the section-tracker render fix for a swap-spawned monster.
 * `area_index` should be the player's current area from mhfu_get_area_index().
 * Call from mhfu_tick while the player is co-located with the monster (XZ
 * distance small) — once the engine's own roam sets the tracker, stop calling. */
static inline void mhfu_aiscript_fix_render(uint32_t entity, uint16_t area_index)
{
    volatile uint16_t *sec =
        (volatile uint16_t *)(uintptr_t)(entity + MHFU_AI_OFF_SECTION);
    volatile uint32_t *flags =
        (volatile uint32_t *)(uintptr_t)(entity + MHFU_AI_OFF_FLAGS638);
    *sec    = area_index;
    *flags |= MHFU_AI_FLAGS638_RENDER_BIT;
}

/* ---- Per-monster behavior descriptor (v1) ----------------------------------
 *
 * A behavior is a lightweight object a C mod can populate and hand to the
 * framework.  In v1 the framework does not own the behavior lifecycle — the
 * mod owns the struct and registers the callbacks it wants.  This struct is
 * purely documentary; C mods may inline the same logic directly.
 *
 * Lua mods do NOT use this struct; they wire directly to mhfu.on_* and
 * mhfu.on_bigmonster_action as shown in brute_tigrex.lua.
 */
typedef struct {
    /* Monster species the behavior should run for (e.g. MON_TIGREX 0x4B). */
    uint8_t host_species;

    /* on_spawn: fires when a monster of host_species appears in the registry.
     * `hp` is the monster's initial HP.  Good place to apply render fixes and
     * set the initial state machine state.
     * Runs on the 5 Hz poll thread — direct Lua / C call is safe. */
    void (*on_spawn)(uint32_t entity, uint8_t mtype, int slot, uint16_t hp,
                     void *userdata);

    /* on_tick: fires every 2 Hz worker tick.
     * Good for maintenance writes (freeze gate, map paint, aggro).
     * Runs on the worker thread — NOT the game thread; safe for memory reads
     * and writes but do NOT call engine action functions directly. */
    void (*on_tick)(uint32_t entity, uint8_t mtype, void *userdata);

    /* on_bigmonster_action override: fired on the exec thread (marshalled from
     * the game thread) for each executor call on an entity of host_species.
     * Return a MHFU_AI_A1_* constant to force that action, or `action_id`
     * unchanged to abstain.
     * This is the coherent action-force seam (executor 0x09AC5228): the engine
     * fans the returned id to all body-part slots itself — no desync/crash. */
    uint32_t (*on_action)(uint32_t entity, uint8_t mtype, uint32_t action_id,
                          void *userdata);

    /* Arbitrary state the behavior needs across calls. */
    void *userdata;
} mhfu_aiscript_behavior_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_AI_SCRIPT_H */
