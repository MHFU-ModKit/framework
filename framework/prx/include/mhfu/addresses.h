/*
 * MHFU per-region address tables.
 *
 * C-side mirror of src/mhfu_bot/events/addresses.py and
 * docs/agent_memory_map.md. The framework resolves the active table at
 * load time from the running game's UMD id. Mods must never hard-code
 * region addresses — read them through this table or the memory helpers.
 */
#ifndef MHFU_ADDRESSES_H
#define MHFU_ADDRESSES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *game_id;
    /* anchor PCs (writer instructions the event trampolines patch) */
    uint32_t pc_quest_beginning;
    uint32_t pc_quest_entered;
    /* observable cells */
    uint32_t cell_screen_state;
    uint32_t cell_quest_timer;
    uint32_t cell_area_index;
    uint32_t cell_player_hp;
    uint32_t cell_sharpness_current;
    uint32_t cell_sharpness_max;
    uint32_t cell_sharpness_tier;
} mhfu_region_addrs_t;

/* MHFU EU — ULES01213 — verified 2026-05-25 */
static const mhfu_region_addrs_t MHFU_REGION_EU = {
    /* .game_id                = */ "ULES01213",
    /* .pc_quest_beginning     = */ 0x088655E4,
    /* .pc_quest_entered       = */ 0x0884CDFC,
    /* .cell_screen_state      = */ 0x08A8CA48,
    /* .cell_quest_timer       = */ 0x09A05DD0,
    /* .cell_area_index        = */ 0x08B0C7DC,
    /* .cell_player_hp         = */ 0x090B3724,
    /* .cell_sharpness_current = */ 0x090B4532,
    /* .cell_sharpness_max     = */ 0x090B3A4C,
    /* .cell_sharpness_tier    = */ 0x090B3A32,
};

/* MHFU NA — ULUS10391 — addresses NOT yet verified (discover via
 * scripts/re_quest_events.py against an NA save). */
static const mhfu_region_addrs_t MHFU_REGION_NA = {
    /* .game_id = */ "ULUS10391",
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_ADDRESSES_H */
