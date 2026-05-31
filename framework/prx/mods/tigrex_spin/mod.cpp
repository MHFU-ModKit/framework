/*
 * tigrex_spin — replace the Giadrome quest's headline monster with a
 * Tigrex (0.5x size), then once that Tigrex naturally picks
 * TIGREX_ANGRY_SPIN, redirect every subsequent AI slot iteration on
 * that entity to the SPIN slot until it takes damage. Once HP drops
 * the pin releases and the engine resumes normal slot iteration.
 *
 * Redirection uses the slot-loop wrapper (mhfu_on_bigmonster_slot_picked):
 * the wrapper intercepts the AI slot loop INSIDE z_un_08865648 BEFORE
 * each iteration's skip-gates / vt[8] / installer run. By returning a
 * fixed slot, the entire same-slot path (input read at +0x1B8+slot*2,
 * vt[8] outcome, applier writing slot's container at +0x110+slot*0x40)
 * stays consistent — no descriptor / container shape mismatch.
 *
 * The earlier vt[8] post/pre-hook variants crashed because they could
 * only change the outcome ptr or input value but not the slot identity
 * the applier wrote into.
 */
#include "mhfu/mhfu.h"

#define MOD_ID      "tigrex_spin"
#define TIGREX_SIZE 0.5f

static volatile int      g_replaced       = 0;
static volatile int      g_pin_active     = 0;
static volatile uint32_t g_pin_entity     = 0;
static volatile uint8_t  g_pin_slot       = 0xFF;     /* SPIN's slot in the loop */
static volatile uint16_t g_pin_initial_hp = 0;

static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (mhfu_quest_has(ctx->quest, MON_GIADROME) &&
        mhfu_quest_replace_monster(ctx->quest, MON_GIADROME, MON_TIGREX)
            == MHFU_HOOK_OK) {
        g_replaced = 1;
        mhfu_log("[%s] giadrome -> tigrex (native coords)", MOD_ID);
    }
}

static void on_spawn(const mhfu_bigmonster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != MON_TIGREX) return;
    mhfu_entity_set_size(ctx->entity_ptr, TIGREX_SIZE);
    g_pin_active     = 0;
    g_pin_entity     = 0;
    g_pin_slot       = 0xFF;
    g_pin_initial_hp = 0;
    mhfu_log("[%s] tigrex spawn slot=%d ent=0x%08lx hp=%u (resized 0.5x)",
             MOD_ID, ctx->slot,
             (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->initial_hp);
}

static void on_death(const mhfu_bigmonster_death_ctx_t *ctx)
{
    if (ctx->monster_type != MON_TIGREX) return;
    g_pin_active     = 0;
    g_pin_entity     = 0;
    g_pin_slot       = 0xFF;
    g_pin_initial_hp = 0;
}

/* Smoke-test for the new event: log the moment the AI overlay lands.
 * Later this is where the slot-redirect patch on 0x09AC5228 will go. */
static void on_overlay_loaded(const mhfu_ai_overlay_ctx_t *ctx)
{
    mhfu_log("[%s] AI overlay loaded -> dest=0x%08lx size=0x%lX",
             MOD_ID,
             (unsigned long)ctx->dest,
             (unsigned long)ctx->size);
}

/* POST-call observer: detect natural SPIN. ctx.slot is now accurate
 * because the slot-loop wrapper stashes it before vt[8] fires. */
static uint32_t observe_natural_spin(const mhfu_action_decision_ctx_t *ctx,
                                     uint32_t engine_value)
{
    if (ctx->monster_type != MON_TIGREX) return engine_value;
    if (g_pin_active) return engine_value;
    if (ctx->vt8_input == TIGREX_ANGRY_SPIN) {
        g_pin_active     = 1;
        g_pin_entity     = ctx->entity_ptr;
        g_pin_slot       = ctx->slot;
        g_pin_initial_hp = mhfu_entity_hp(ctx->entity_ptr);
        mhfu_log("[%s] natural SPIN observed -> pin armed "
                 "(ent=0x%08lx slot=%u input=0x%04x hp=%u)",
                 MOD_ID,
                 (unsigned long)ctx->entity_ptr,
                 (unsigned)ctx->slot,
                 (unsigned)ctx->vt8_input,
                 (unsigned)g_pin_initial_hp);
    }
    return engine_value;
}

/* Slot redirect: once armed, every iteration on the pinned entity
 * gets redirected to the SPIN slot. Other entities (and other species)
 * untouched. */
static uint8_t pin_slot(const mhfu_slot_picked_ctx_t *ctx,
                        uint8_t current_slot)
{
    if (ctx->monster_type != MON_TIGREX) return current_slot;
    if (!g_pin_active) return current_slot;
    if (ctx->entity_ptr != g_pin_entity) return current_slot;

    uint16_t cur = mhfu_entity_hp(ctx->entity_ptr);
    if (cur < g_pin_initial_hp) {
        mhfu_log("[%s] hp %u -> %u, releasing pin",
                 MOD_ID,
                 (unsigned)g_pin_initial_hp, (unsigned)cur);
        g_pin_active     = 0;
        g_pin_entity     = 0;
        g_pin_slot       = 0xFF;
        g_pin_initial_hp = 0;
        return current_slot;
    }

    /* Defensive: only redirect to a slot in range. */
    if (g_pin_slot >= ctx->action_count) return current_slot;
    return g_pin_slot;
}

static int mod_init(void)
{
    mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                    (mhfu_event_cb_t)(void *)on_targets_building, 0);
    mhfu_on_bigmonster_spawn(on_spawn, 0);
    mhfu_on_bigmonster_death(on_death, 0);
    mhfu_on_ai_overlay_loaded(on_overlay_loaded, 0);
    mhfu_on_bigmonster_slot_picked(pin_slot, 100);
    mhfu_on_bigmonster_action_decided(observe_natural_spin, 0);
    mhfu_log("[%s] registered (slot-redirect variant)", MOD_ID);
    return 0;
}

static void mod_shutdown(void)
{
    mhfu_off_bigmonster_action_decided(observe_natural_spin);
    mhfu_off_bigmonster_slot_picked(pin_slot);
    mhfu_off_ai_overlay_loaded(on_overlay_loaded);
    mhfu_off_bigmonster_spawn(on_spawn);
    mhfu_off_bigmonster_death(on_death);
}

MHFU_MOD(.id = MOD_ID, .version = "0.3",
         .needs = 0,
         .conflicts = "tigrex_inject popo_heading_hook",
         .init = mod_init, .shutdown = mod_shutdown);
