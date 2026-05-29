/*
 * Framework events. The framework installs the trampolines / poll
 * threads behind these; a mod just registers callbacks. Multiple mods
 * may register the same event — all fire (see hooks.h for ordering).
 *
 * The Python live framework (src/mhfu_bot/events/) exposes the SAME
 * named events: a mod proven there ports to this C SDK with matching
 * signatures.
 */
#ifndef MHFU_EVENTS_H
#define MHFU_EVENTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MHFU_EVENT_QUEST_BEGINNING     = 0, /* player commits to a quest        */
    MHFU_EVENT_QUEST_ENTERED       = 1, /* player spawns into the quest map */
    MHFU_EVENT_MAP_SECTION_ENTERED = 2, /* area_index changed (any section) */
    MHFU_EVENT_MONSTER_SPAWNED     = 3, /* new entity in registry slot 1+   */

    MHFU_EVENT_COUNT_
} mhfu_event_id_t;

/* Quest events carry the CPU register snapshot at hook entry. */
typedef struct {
    mhfu_event_id_t event_id;
    uint32_t        cell_value;
    uint32_t a0, a1, a2, a3;
    uint32_t v0, v1;
    uint32_t ra, sp;
    uint32_t pc;
} mhfu_event_ctx_t;

typedef struct {
    mhfu_event_id_t event_id;
    uint16_t section_id;          /* new area_index */
    uint16_t prev_section_id;     /* previous area_index */
    uint32_t quest_timer;
    uint8_t  screen_state;
    uint8_t  is_in_quest_area;    /* screen_state == 17 */
    uint16_t _pad;
} mhfu_map_section_ctx_t;

typedef struct {
    mhfu_event_id_t event_id;
    int      slot;                /* entity-registry slot index (1..20) */
    uint32_t entity_ptr;
    uint8_t  monster_type;        /* u8 at entity_ptr+0x1E8 */
    uint8_t  entity_id;           /* u8 at entity_ptr+0x1E4 */
    uint16_t hp;                  /* u16 at entity_ptr+0x2E4 */
    float    size_scale;          /* f32 at entity_ptr+0x024 */
} mhfu_monster_spawn_ctx_t;

/* Generic registry storage; the typed sugar below casts into this. */
typedef void (*mhfu_event_cb_t)(const void *ctx);

/* Typed aliases — caller-side signature checking. */
typedef void (*mhfu_quest_cb_t)        (const mhfu_event_ctx_t *ctx);
typedef void (*mhfu_map_section_cb_t)  (const mhfu_map_section_ctx_t *ctx);
typedef void (*mhfu_monster_spawn_cb_t)(const mhfu_monster_spawn_ctx_t *ctx);

/* Core registration. Returns 0 on success, negative on error. Prefer
 * the mhfu_on_* sugar (typed) or mhfu_hook_event (priority, in hooks.h). */
int mhfu_register_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb);
int mhfu_unregister_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb);

static inline int mhfu_on_quest_beginning(mhfu_quest_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_QUEST_BEGINNING,
                               (mhfu_event_cb_t)(void *)cb);
}
static inline int mhfu_on_quest_entered(mhfu_quest_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_QUEST_ENTERED,
                               (mhfu_event_cb_t)(void *)cb);
}
static inline int mhfu_on_map_section_entered(mhfu_map_section_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_MAP_SECTION_ENTERED,
                               (mhfu_event_cb_t)(void *)cb);
}
static inline int mhfu_on_monster_spawned(mhfu_monster_spawn_cb_t cb) {
    return mhfu_register_event(MHFU_EVENT_MONSTER_SPAWNED,
                               (mhfu_event_cb_t)(void *)cb);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_EVENTS_H */
