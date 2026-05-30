/*
 * ai_demo (EXPERIMENTAL) — reference mod for the AI override-event API
 * added in Section 32d. Demonstrates priority-chain override semantics
 * with two action_decided handlers and one observe-only ai_step.
 *
 * Logs the engine's pick + each chain step. Does NOT actually change
 * any monster's behavior (both override handlers return the value they
 * received). Drop this in to verify the wiring fires; replace returns
 * with custom action_ids to script real AI.
 *
 * Mutually exclusive with other popo-AI experiments because they all
 * claim popo vt[8].
 */
#include "mhfu/mhfu.h"

#define MOD_ID "ai_demo"

/* High-priority handler (priority 100): runs first, sees the engine's
 * raw pick.  Logs and forwards unchanged. */
static uint32_t high_pri_action(const mhfu_action_decision_ctx_t *ctx,
                                uint32_t engine_action_id)
{
    mhfu_log("[ai_demo] action HI  entity=0x%08lx type=0x%02x slot=%u "
             "vt8_in=0x%04x engine_id=0x%08lx",
             (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->monster_type,
             (unsigned)ctx->slot,
             (unsigned)ctx->vt8_input,
             (unsigned long)engine_action_id);
    return engine_action_id;     /* abstain */
}

/* Low-priority handler (priority 0): runs after, sees whatever the
 * HI handler forwarded.  Also abstains. */
static uint32_t low_pri_action(const mhfu_action_decision_ctx_t *ctx,
                               uint32_t chain_value)
{
    (void)ctx;
    mhfu_log("[ai_demo] action LO  chain_value=0x%08lx",
             (unsigned long)chain_value);
    return chain_value;
}

/* Anim override (chain of 1).  Stub for when the JAL trampoline lands. */
static uint32_t demo_anim(const mhfu_anim_decision_ctx_t *ctx,
                          uint32_t engine_anim_node_ptr)
{
    mhfu_log("[ai_demo] anim     entity=0x%08lx action=0x%08lx node_ptr=0x%08lx",
             (unsigned long)ctx->entity_ptr,
             (unsigned long)ctx->action_id,
             (unsigned long)engine_anim_node_ptr);
    return engine_anim_node_ptr;
}

static void demo_spawn(const mhfu_bigmonster_spawn_ctx_t *ctx)
{
    mhfu_log("[ai_demo] SPAWN  slot=%d entity=0x%08lx type=0x%02x hp=%u",
             ctx->slot, (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->monster_type, (unsigned)ctx->initial_hp);
}

static void demo_death(const mhfu_bigmonster_death_ctx_t *ctx)
{
    mhfu_log("[ai_demo] DEATH  slot=%d entity=0x%08lx type=0x%02x",
             ctx->slot, (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->monster_type);
}

/* Observe-only AI step.  Fires every frame for each big monster. */
static void demo_step(const mhfu_ai_step_ctx_t *ctx)
{
    static uint32_t s_tick = 0;
    if ((s_tick++ & 0x3F) == 0) {       /* throttle log spam */
        mhfu_log("[ai_demo] step     entity=0x%08lx type=0x%02x",
                 (unsigned long)ctx->entity_ptr,
                 (unsigned)ctx->monster_type);
    }
}

static int ai_demo_init(void)
{
    mhfu_on_bigmonster_action_decided(high_pri_action, /* priority */ 100);
    mhfu_on_bigmonster_action_decided(low_pri_action,  /* priority */ 0);
    mhfu_on_bigmonster_anim_decided  (demo_anim,       /* priority */ 0);
    mhfu_on_bigmonster_ai_step       (demo_step,       /* priority */ 0);
    mhfu_on_bigmonster_spawn         (demo_spawn,      /* priority */ 0);
    mhfu_on_bigmonster_death         (demo_death,      /* priority */ 0);
    mhfu_log("[ai_demo] registered");
    return 0;
}

static void ai_demo_shutdown(void)
{
    mhfu_off_bigmonster_action_decided(high_pri_action);
    mhfu_off_bigmonster_action_decided(low_pri_action);
    mhfu_off_bigmonster_anim_decided  (demo_anim);
    mhfu_off_bigmonster_ai_step       (demo_step);
    mhfu_off_bigmonster_spawn         (demo_spawn);
    mhfu_off_bigmonster_death         (demo_death);
}

MHFU_MOD(.id = MOD_ID, .version = "0.1-experimental",
         .needs = 0,
         .conflicts = "popo_vt8_override popo_aggression popo_growth",
         .init = ai_demo_init, .shutdown = ai_demo_shutdown);
