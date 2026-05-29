/*
 * diag — built-in smoke-test mod. Logs each framework event once so a
 * fresh build can be verified end to end. Pure observer; no hooks.
 */
#include "mhfu/mhfu.h"

static void on_quest_beginning(const mhfu_event_ctx_t *ctx)
{
    mhfu_log("[diag] QUEST BEGINNING  cell=%u a0=0x%08lx",
             (unsigned)ctx->cell_value, (unsigned long)ctx->a0);
}

static void on_quest_entered(const mhfu_event_ctx_t *ctx)
{
    (void)ctx;
    unsigned t = (unsigned)mhfu_get_quest_timer();
    mhfu_log("[diag] QUEST ENTERED  area=%u scr=0x%02x timer=%u (%u.%us)",
             (unsigned)mhfu_get_area_index(), (unsigned)mhfu_get_screen_state(),
             t, t / 30, (t % 30) * 10 / 3);
}

static void on_map_section(const mhfu_map_section_ctx_t *ctx)
{
    mhfu_log("[diag] MAP SECTION  %u -> %u (qtimer=%u)",
             (unsigned)ctx->prev_section_id, (unsigned)ctx->section_id,
             (unsigned)ctx->quest_timer);
}

static void on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    union { float f; uint32_t u; } sz; sz.f = ctx->size_scale;
    mhfu_log("[diag] MONSTER SPAWNED  slot=%d ptr=0x%08lx type=0x%02x hp=%u size_raw=0x%08lx",
             ctx->slot, (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->monster_type, (unsigned)ctx->hp, (unsigned long)sz.u);
}

static int diag_init(void)
{
    mhfu_on_quest_beginning(on_quest_beginning);
    mhfu_on_quest_entered(on_quest_entered);
    mhfu_on_map_section_entered(on_map_section);
    mhfu_on_monster_spawned(on_monster);
    return 0;
}

MHFU_MOD(.id = "diag", .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = diag_init, .shutdown = 0);
