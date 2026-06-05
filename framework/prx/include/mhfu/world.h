/*
 * World / player / cheat helpers — global game state that isn't tied to a
 * single monster entity. The per-monster accessors live in entity.h; the
 * named cells (screen_state, area_index, quest_timer, player HP) in
 * memory.h. This header adds the player WORLD POSITION and a couple of the
 * verified CWCheat-style cheats so mods don't carry raw addresses.
 */
#ifndef MHFU_WORLD_H
#define MHFU_WORLD_H

#include <stdint.h>
#include "entity.h"   /* mhfu_vec3_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Player world position. EU: the camera-target cell 0x09998D50 tracks the
 * player's world pos (vec3 f32) — used as the aggro/relocate anchor in
 * every mod. */
#define MHFU_PLAYER_POS 0x09998D50u
mhfu_vec3_t mhfu_player_pos(void);

/* Paint all big monsters on the in-game minimap (the EU "Auto-Paint Bosses"
 * cheat — real addr 0x090B3A6A, u8 0xFF). Call ≥2 Hz; the marker shows the
 * monster's LOGICAL position even when it's culled / in another section. */
#define MHFU_PAINT_MAP_CELL 0x090B3A6Au
void mhfu_paint_map(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_WORLD_H */
