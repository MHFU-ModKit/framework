/*
 * tigrex_spin — Replace the Giadrome quest's headline monster with a
 * 0.5x Tigrex; lock that Tigrex into TIGREX_ANGRY_SPIN after his first
 * natural spin; release the lock on first HP drop.
 *
 * Mechanism (RE'd in docs/AI_SCRIPTING_ENGINE.md §32j):
 *   - Slots = body parts. SPIN's full-body animation is 3 per-slot
 *     descriptors playing in concert. SPIN's natural per-slot vt[8]
 *     inputs are (0x0413, 0x04DB, 0x05A3).
 *   - vt[8] is deterministic (no RNG) — same input → same return ptr
 *     for a given entity. So we can cache the engine's natural SPIN-
 *     part ptrs the first time each slot picks SPIN, then replay them.
 *   - vt[8] is called TWICE per slot per tick (gate + descriptor).
 *     Both calls go through the action_decided post-hook. Preserve the
 *     engine's natural gate by only overriding when engine_value != 0.
 *   - +0x4B8 is the engine's "exhaustion" bitfield. Each forced spin
 *     OR's bits 0x100 / 0x10000; when set, the AI tick halts entirely
 *     (frame counter +0x092 stops advancing). Zeroing this cell each
 *     pin call keeps the engine fresh, preventing freeze.
 */
#include "mhfu/mhfu.h"

#define MOD_ID      "tigrex_spin"
#define TIGREX_SIZE 0.5f

/* Per-slot natural vt[8] inputs for SPIN's body-part descriptors.
 * Stride 200 between slots is the engine's per-slot input offset. */
static const uint16_t SPIN_INPUT[3] = { 0x0413, 0x04DB, 0x05A3 };

static volatile int      g_pin_active     = 0;
static volatile uint32_t g_pin_entity     = 0;
static volatile uint16_t g_pin_initial_hp = 0;
static volatile uint32_t g_spin_ptr[3]    = { 0, 0, 0 };

static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (mhfu_quest_has(ctx->quest, MON_GIADROME) &&
        mhfu_quest_replace_monster(ctx->quest, MON_GIADROME, MON_TIGREX)
            == MHFU_HOOK_OK) {
        mhfu_log("[%s] giadrome -> tigrex (native coords)", MOD_ID);
    }
}

static void reset_pin(void)
{
    g_pin_active     = 0;
    g_pin_entity     = 0;
    g_pin_initial_hp = 0;
    g_spin_ptr[0] = g_spin_ptr[1] = g_spin_ptr[2] = 0;
}

static void on_spawn(const mhfu_bigmonster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != MON_TIGREX) return;
    mhfu_entity_set_size(ctx->entity_ptr, TIGREX_SIZE);
    reset_pin();
    mhfu_log("[%s] tigrex spawn slot=%d ent=0x%08lx hp=%u (resized 0.5x)",
             MOD_ID, ctx->slot,
             (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->initial_hp);
}

static void on_death(const mhfu_bigmonster_death_ctx_t *ctx)
{
    if (ctx->monster_type != MON_TIGREX) return;
    reset_pin();
}

/* Slot wrapper subscription — abstain. Kept registered because the
 * framework only installs the per-tick slot wrapper when at least one
 * mod subscribes to slot_picked, and that wrapper is what enriches
 * ctx.slot in the action_decided event we depend on. */
static uint8_t pin_slot(const mhfu_slot_picked_ctx_t *ctx, uint8_t slot)
{
    (void)ctx;
    return slot;
}

/* Overlay-loaded subscription — abstain. Load-bearing: the framework
 * only installs the overlay-loader postfix (which in turn arms the
 * BIG-mon slot wrapper at 0x09AC52DC) when at least one mod subscribes
 * to ai_overlay_loaded. Without it, Tigrex's overlay slot loop has
 * no ctx.slot enrichment and our snoop misses slot 0 / slot 2. */
static void pin_overlay(const mhfu_ai_overlay_ctx_t *ctx)
{
    (void)ctx;
}

/* Post-call vt[8] override.
 *   1. SNOOP — cache each slot's natural SPIN-part vt[8] return when
 *      it's first picked. Slot 0 fires with input 0x0413, slot 1 with
 *      0x04DB, slot 2 with 0x05A3 during a natural SPIN; the engine
 *      iterates 0→1→2 so we get all 3 ptrs from the same arming tick.
 *   2. ARM — slot 2's input 0x05A3 is the SPIN attack trigger.
 *   3. OVERRIDE — once armed, return cached SPIN-part ptr per slot,
 *      gated on engine_value != 0 (preserves engine's natural skip
 *      gate so we don't thrash the install path).
 *   4. UNFREEZE — write +0x4B8 = 0 every fire to clear the engine's
 *      "exhaustion" bitfield before it accumulates to a halt.
 *   5. RELEASE — first HP drop from the snapshot at arm time. */
static uint32_t pin_spin(const mhfu_action_decision_ctx_t *ctx,
                         uint32_t engine_value)
{
    if (ctx->monster_type != MON_TIGREX) return engine_value;

    if (engine_value != 0 && ctx->slot < 3 &&
        ctx->vt8_input == SPIN_INPUT[ctx->slot] &&
        g_spin_ptr[ctx->slot] != engine_value) {
        g_spin_ptr[ctx->slot] = engine_value;
        mhfu_log("[%s] SPIN slot %u cached: input=0x%04x ptr=0x%08lx",
                 MOD_ID,
                 (unsigned)ctx->slot,
                 (unsigned)ctx->vt8_input,
                 (unsigned long)engine_value);
    }

    if (!g_pin_active) {
        if (ctx->slot == 2 && ctx->vt8_input == TIGREX_ANGRY_SPIN) {
            g_pin_active     = 1;
            g_pin_entity     = ctx->entity_ptr;
            g_pin_initial_hp = mhfu_entity_hp(ctx->entity_ptr);
            mhfu_log("[%s] natural SPIN observed -> pin armed "
                     "(ent=0x%08lx hp=%u)",
                     MOD_ID,
                     (unsigned long)ctx->entity_ptr,
                     (unsigned)g_pin_initial_hp);
        }
        return engine_value;
    }

    if (ctx->entity_ptr != g_pin_entity) return engine_value;

    uint16_t cur = mhfu_entity_hp(ctx->entity_ptr);
    if (cur < g_pin_initial_hp) {
        mhfu_log("[%s] hp %u -> %u, releasing pin",
                 MOD_ID,
                 (unsigned)g_pin_initial_hp, (unsigned)cur);
        reset_pin();
        return engine_value;
    }

    /* Maintenance writes — v0.12 empirical-working set restored.
     * Cleanup attempts (v1.0–v1.2) tried to drop individual writes
     * based on RE theory, but each drop reintroduced freeze / crash.
     * Keep the full set; tagging origin per cell:
     *   +0x4B8 = 0       — bisect-proven freeze gate (v0.12 RE).
     *   +0x1A8..+0x1AA=1 — per-slot enable bytes.
     *   +0x1B8..+0x1BC   — SPIN input triple (keeps engine's vt[8]
     *                       in sync with our forced ptr substitution).
     *   +0x414 = 0       — bitfield engine ORs during forced repeat.
     *                       Bisect tagged it red herring but dropping
     *                       it reintroduces 3-spin freeze. Keep.
     *   +0x29C = 0       — stress mode byte; v0.12 empirical.
     *   +0x32C = 0       — AI param scratch; v0.12 empirical.
     *   +0x550 = 9000    — stamina-like counter (decays ~10/333ms).
     *   +0x54E = 1225    — fast decrementer; v0.11 thought false-
     *                       positive but dropping reintroduces freeze. */
    mhfu_write_u32(ctx->entity_ptr + 0x4B8, 0);
    mhfu_write_u8 (ctx->entity_ptr + 0x1A8, 1);
    mhfu_write_u8 (ctx->entity_ptr + 0x1A9, 1);
    mhfu_write_u8 (ctx->entity_ptr + 0x1AA, 1);
    mhfu_write_u16(ctx->entity_ptr + 0x1B8, SPIN_INPUT[0]);
    mhfu_write_u16(ctx->entity_ptr + 0x1BA, SPIN_INPUT[1]);
    mhfu_write_u16(ctx->entity_ptr + 0x1BC, SPIN_INPUT[2]);
    mhfu_write_u32(ctx->entity_ptr + 0x414, 0);
    mhfu_write_u32(ctx->entity_ptr + 0x29C, 0);
    mhfu_write_u16(ctx->entity_ptr + 0x32C, 0);
    mhfu_write_u16(ctx->entity_ptr + 0x54E, 1225);
    mhfu_write_u16(ctx->entity_ptr + 0x550, 9000);

    if (ctx->slot < 3 && g_spin_ptr[ctx->slot] && engine_value != 0) {
        return g_spin_ptr[ctx->slot];
    }
    return engine_value;
}

static int mod_init(void)
{
    mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                    (mhfu_event_cb_t)(void *)on_targets_building, 0);
    mhfu_on_bigmonster_spawn(on_spawn, 0);
    mhfu_on_bigmonster_death(on_death, 0);
    mhfu_on_ai_overlay_loaded(pin_overlay, 0);
    mhfu_on_bigmonster_slot_picked(pin_slot, 100);
    mhfu_on_bigmonster_action_decided(pin_spin, 100);
    mhfu_log("[%s] registered", MOD_ID);
    return 0;
}

static void mod_shutdown(void)
{
    mhfu_off_bigmonster_action_decided(pin_spin);
    mhfu_off_bigmonster_slot_picked(pin_slot);
    mhfu_off_ai_overlay_loaded(pin_overlay);
    mhfu_off_bigmonster_spawn(on_spawn);
    mhfu_off_bigmonster_death(on_death);
}

MHFU_MOD(.id = MOD_ID, .version = "1.3",
         .needs = 0,
         .conflicts = "tigrex_inject popo_heading_hook",
         .init = mod_init, .shutdown = mod_shutdown);
