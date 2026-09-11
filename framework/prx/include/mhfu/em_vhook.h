/*
 * em_vhook — the big-monster vtable seams, as a public surface.
 *
 * The mod under framework/prx/mods/em_vhook latches onto the species vtable of
 * the big monster the engine spawns and wraps two slots (docs/EM_OVERLAY_ABI.md
 * §9–§15):
 *
 *   abi slot 32 (vt+0x88)  ENTER-ACTION  enter(entity, main, id, mode) — the
 *                          one call that PROVISIONS a behaviour pair: the
 *                          species' per-main translator sets the charge's run
 *                          budget, then act_set writes entity+0x298/+0x299.
 *   abi slot 29 (vt+0x7C)  the per-frame AI step, ~30.8 dispatches/s.
 *
 * Three capabilities sit on them, all data-driven so the stubs never rebuild:
 *
 *   SUBSTITUTION (issue #15, slot 32 pre)   the engine's own choice of
 *       (main, id) is rewritten to ours BEFORE the species enter-action runs,
 *       so our pair is provisioned exactly like a native one and ends the way
 *       the chain says (the charge hands to the skid on budget, not to a
 *       38-second park). Up to EM_VHOOK_SUBS entries, first match wins.
 *
 *   REQUEST (issue #15/#16, slot 29 pre)    a pair a script wants entered NOW.
 *       Written from Lua, issued by the stub on the game thread inside the very
 *       next AI frame through the engine's own dispatcher 0x09AC89F0 — the
 *       provisioned replacement for writing the two cells by hand.
 *
 *   RULES (issue #16, slot 29 pre)          a native 30 Hz brain: up to
 *       EM_VHOOK_RULES triggers of the form "in pair P for >= N frames, player
 *       distance in [lo, hi), receding/closing -> enter (main, sub, mode)",
 *       with a cooldown and a fire budget. Evaluated every frame without Lua.
 *
 *   BUDGET (v2, slot 32 post + slot 29 one-shot)  entity+0x414 override for
 *       the timer-gated actions — unchanged from v2.
 *
 * Every function here is safe to call from any thread: they only write the
 * config block the stubs read on their next dispatch. All of them are no-ops
 * that return 0/false while no big monster's vtable is latched.
 */
#ifndef MHFU_EM_VHOOK_H
#define MHFU_EM_VHOOK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EM_VHOOK_SUBS   4    /* substitution entries */
#define EM_VHOOK_RULES  4    /* native brain rules */
#define EM_VHOOK_RING   8    /* last enter-action dispatches kept */

/* "any sub state" for from_sub. 0xFF is the never-match idle value. */
#define EM_VHOOK_SUB_ANY   0xFEu
/* a fire budget that never runs out (136 years at 30 Hz) */
#define EM_VHOOK_UNLIMITED 0xFFFFFFFFu

/* rule.flags */
#define EM_VHOOK_RULE_RECEDING 0x01   /* only while the distance is GROWING */
#define EM_VHOOK_RULE_CLOSING  0x02   /* only while the distance is SHRINKING */

typedef struct {
    uint8_t  from_mask;    /* bit k set = the rule may fire from main state k */
    uint8_t  from_sub;     /* exact sub, or EM_VHOOK_SUB_ANY */
    uint8_t  to_main, to_sub, mode;
    uint8_t  flags;        /* EM_VHOOK_RULE_* */
    uint32_t min_frames;   /* the pair must have stood this many frames */
    float    dist_lo, dist_hi;   /* player XZ distance window [lo, hi), units */
    uint32_t cooldown;     /* frames between two fires of this rule */
    uint32_t count;        /* fires allowed; EM_VHOOK_UNLIMITED for a standing rule */
} em_vhook_rule_t;

typedef struct {
    uint32_t installed;
    uint32_t ai_ticks, act_enters, last_pair, frames;
    float    dist;               /* player XZ distance as the stub last measured it */
    uint32_t sub_hits, sub_landed, sub_last_in;
    uint32_t brain_fires;
    uint32_t req_pending, req_done, req_result;
    uint32_t ring_idx;
    uint32_t ring[EM_VHOOK_RING];   /* (subst<<24)|(mode<<16)|(main<<8)|sub, oldest at ring_idx+1 */
    uint32_t rule_fired[EM_VHOOK_RULES];
    uint32_t rule_left[EM_VHOOK_RULES];
    uint32_t sub_left[EM_VHOOK_SUBS];
} em_vhook_status_t;

/* 1 while a big monster's vtable is wrapped (from its spawn to quest_beginning). */
int  em_vhook_installed(void);

/* Substitution entry `slot` (0..EM_VHOOK_SUBS-1): an incoming enter-action whose
 * main is in `from_mask` and whose id equals `from_sub` (or any, with
 * EM_VHOOK_SUB_ANY) is entered as (to_main, to_sub) instead, `count` times
 * (0 clears the entry, EM_VHOOK_UNLIMITED makes it standing). */
void em_vhook_substitute(int slot, uint8_t from_mask, uint8_t from_sub,
                         uint8_t to_main, uint8_t to_sub, uint32_t count);

/* Ask the slot-29 stub to enter (main, sub, mode) on its next frame. Returns 0
 * when nothing is latched (the caller falls back to whatever it did before). */
int  em_vhook_request(uint8_t main_state, uint8_t sub_state, uint8_t mode);

/* Rule `slot` (0..EM_VHOOK_RULES-1); NULL clears it. */
void em_vhook_rule(int slot, const em_vhook_rule_t *r);

/* Drop every substitution, rule and pending request. */
void em_vhook_clear(void);

void em_vhook_status(em_vhook_status_t *out);

/* v2 surface, unchanged: the +0x414 action-budget override. */
void em_vhook_arm(uint8_t main_state, uint8_t sub_state, uint32_t off, uint32_t val);
void em_vhook_seam29(uint8_t on);
void em_vhook_stats(uint32_t *ai, uint32_t *acts, uint32_t *last);

#ifdef __cplusplus
}
#endif
#endif /* MHFU_EM_VHOOK_H */
