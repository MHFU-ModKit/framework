/*
 * Quest domain — read/edit the active quest's monster list.
 *
 * Editing must happen before the loading-screen model-load reads the
 * list. Subscribe to MHFU_EVENT_QUEST_TARGETS_BUILDING (events.h) and do
 * the edit in that callback; the framework owns the buildTargets hook +
 * JIT-safe install timing so mods don't.
 *
 * See docs/QUEST_LOADING_LIFECYCLE.md for the underlying mechanism.
 */
#ifndef MHFU_QUEST_H
#define MHFU_QUEST_H

#include <stdint.h>
#include "ids.h"
#include "hooks.h"   /* mhfu_hook_rc_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t mhfu_quest_t;            /* opaque Quest-singleton handle; 0 = none */

/* Context for MHFU_EVENT_QUEST_TARGETS_BUILDING (events.h). */
typedef struct {
    mhfu_event_id_t event_id;
    mhfu_quest_t    quest;
} mhfu_quest_ctx_t;

mhfu_quest_t mhfu_quest_current(void);
int          mhfu_quest_monster_count(mhfu_quest_t q);
int          mhfu_quest_has(mhfu_quest_t q, mhfu_monster_id_t id);

/* Retag the quest's existing monster `from` -> `to` in place (count
 * stays the same, native spawn). The engine then loads `to`'s model
 * natively at the loading screen. Returns MHFU_HOOK_OK, or BADARG if the
 * quest/`from` isn't present or `to`'s record layout isn't known. */
mhfu_hook_rc_t mhfu_quest_replace_monster(mhfu_quest_t q,
                                          mhfu_monster_id_t from,
                                          mhfu_monster_id_t to);

/* Append `id` as an additional monster at spawn (x,z). NOTE: a 2nd big
 * monster currently hits a per-quest spawn cap (returns MHFU_HOOK_NOSPACE)
 * — replace_monster is the reliable path until the cap is understood. */
mhfu_hook_rc_t mhfu_quest_add_monster(mhfu_quest_t q, mhfu_monster_id_t id,
                                      float x, float z);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_QUEST_H */
