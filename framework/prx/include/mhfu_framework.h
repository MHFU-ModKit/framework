/*
 * MHFU Modding Framework — PRX SDK header.
 *
 * This is the C API mod authors target when building a `.prx` plugin
 * that ships at native game speed. The Python-side live framework
 * (src/mhfu_bot/events/) and this C SDK expose the SAME named events.
 * A mod proven out interactively in the live framework can be ported
 * to the PRX path with minimal changes: replace Python async callbacks
 * with C functions of identical signature, recompile.
 *
 * Build with the pspdev toolchain (docker pull pspdev/pspdev). See
 * framework/prx/README.md and framework/prx/mods/hello_world_prx/
 * for a working example.
 *
 * Region note:
 *   Anchor PCs in mhfu_framework_addresses.h are MHFU EU (ULES01213)
 *   only. The framework's address table is the single source of truth
 *   for region-specific addresses; mods should never hard-code them.
 *
 * (c) 2026 — MIT, same as the live framework.
 */

#ifndef MHFU_FRAMEWORK_H
#define MHFU_FRAMEWORK_H

#include <pspkerneltypes.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ *
 * Event identifiers.
 * Add new events to mhfu_framework_addresses.h; the framework resolves the
 * anchor PC at load time using the running game's id.
 * ------------------------------------------------------------------------ */

typedef enum {
    MHFU_EVENT_QUEST_BEGINNING = 0, /* player commits to a quest         */
    MHFU_EVENT_QUEST_ENTERED   = 1, /* player spawns into the quest map  */

    MHFU_EVENT_COUNT_
} mhfu_event_id_t;

/* ------------------------------------------------------------------------ *
 * Callback context.
 *
 * Mod callbacks receive a snapshot of the CPU registers at the moment
 * the anchor instruction was about to execute. Reading game memory
 * via mhfu_read_* is safe here; calling game functions is permitted
 * but the mod author is responsible for argument hygiene.
 *
 * `event_id` lets one callback service multiple events.
 * `cell_value` is the post-write value of the event's anchor cell;
 *   for quest_beginning this is the new quest_timer value, for
 *   quest_entered this is the new area_index (98 at map entry).
 * ------------------------------------------------------------------------ */

typedef struct {
    mhfu_event_id_t event_id;
    uint32_t        cell_value;
    /* CPU register snapshot (a0..a3 are the most useful) */
    uint32_t a0, a1, a2, a3;
    uint32_t v0, v1;
    uint32_t ra, sp;
    uint32_t pc;
} mhfu_event_ctx_t;

typedef void (*mhfu_event_cb_t)(const mhfu_event_ctx_t *ctx);

/* ------------------------------------------------------------------------ *
 * Registration API.
 *
 * Call from your mod's module_start (or any init function): for each
 * event you care about, register one callback. Multiple mods can
 * register against the same event — they all run, in registration
 * order. Priorities to come.
 *
 * mhfu_on_quest_beginning() and friends are thin wrappers around
 * mhfu_register_event(); use whichever feels cleaner.
 *
 * Returns 0 on success, negative on error (e.g. unknown event id,
 * framework not initialized).
 * ------------------------------------------------------------------------ */

int mhfu_register_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb);
int mhfu_unregister_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb);

/* Sugar — same as mhfu_register_event(MHFU_EVENT_QUEST_BEGINNING, cb). */
static inline int mhfu_on_quest_beginning(mhfu_event_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_QUEST_BEGINNING, cb);
}
static inline int mhfu_on_quest_entered(mhfu_event_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_QUEST_ENTERED, cb);
}

/* ------------------------------------------------------------------------ *
 * Game memory helpers.
 *
 * Same addresses as the live framework's docs/agent_memory_map.md.
 * Read these instead of poking absolute addresses by hand — when the
 * framework adds multi-region support, your mod keeps working without
 * a rebuild.
 * ------------------------------------------------------------------------ */

uint8_t  mhfu_get_screen_state(void);
uint16_t mhfu_get_area_index(void);
uint32_t mhfu_get_quest_timer(void);
uint32_t mhfu_get_player_hp(void);
uint16_t mhfu_get_sharpness_current(void);

/* Lower-level escape hatches. */
uint8_t  mhfu_read_u8 (uint32_t addr);
uint16_t mhfu_read_u16(uint32_t addr);
uint32_t mhfu_read_u32(uint32_t addr);
void     mhfu_write_u8 (uint32_t addr, uint8_t v);
void     mhfu_write_u16(uint32_t addr, uint16_t v);
void     mhfu_write_u32(uint32_t addr, uint32_t v);

/* ------------------------------------------------------------------------ *
 * Logging.
 *
 * Mod logs go to PSP/SEPLUGINS/mhfu_framework/<mod-id>.log on the
 * memstick. On PPSSPP this lands in the configured memstick dir; on
 * real PSP, on the actual stick.
 * ------------------------------------------------------------------------ */

void mhfu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_FRAMEWORK_H */
