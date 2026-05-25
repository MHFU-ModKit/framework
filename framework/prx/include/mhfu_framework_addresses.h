/*
 * MHFU per-region address tables.
 *
 * This file is the C-side mirror of src/mhfu_bot/events/addresses.py
 * and docs/agent_memory_map.md. When you update one, update the
 * others. The framework resolves these at PRX-load time based on
 * the running game's UMD id.
 *
 * Currently EU only. NA/JP entries will be filled in via re-running
 * the discovery script (scripts/re_quest_events.py) on saves from
 * those regions.
 */

#ifndef MHFU_FRAMEWORK_ADDRESSES_H
#define MHFU_FRAMEWORK_ADDRESSES_H

#include <stdint.h>

typedef struct {
    const char *game_id;
    /* anchor PCs (writer instructions) */
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
    .game_id                = "ULES01213",
    .pc_quest_beginning     = 0x088655E4,
    .pc_quest_entered       = 0x0884CDFC,
    .cell_screen_state      = 0x08A8CA48,
    .cell_quest_timer       = 0x09A05DD0,
    .cell_area_index        = 0x08B0C7DC,
    .cell_player_hp         = 0x090B3724,
    .cell_sharpness_current = 0x090B4532,
    .cell_sharpness_max     = 0x090B3A4C,
    .cell_sharpness_tier    = 0x090B3A32,
};

/* MHFU NA — ULUS10391 — addresses NOT yet verified */
static const mhfu_region_addrs_t MHFU_REGION_NA = {
    .game_id                = "ULUS10391",
    /* TODO: discover via PPSSPP + scripts/re_quest_events.py with NA save */
    .pc_quest_beginning     = 0,
    .pc_quest_entered       = 0,
    .cell_screen_state      = 0,
    .cell_quest_timer       = 0,
    .cell_area_index        = 0,
    .cell_player_hp         = 0,
    .cell_sharpness_current = 0,
    .cell_sharpness_max     = 0,
    .cell_sharpness_tier    = 0,
};

#endif /* MHFU_FRAMEWORK_ADDRESSES_H */
