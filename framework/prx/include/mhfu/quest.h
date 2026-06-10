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

/* ADD `id` as a 2ND big monster (Section 38 cap = 2 groups). Fabricates a
 * record + mon-header in the rec_base tail, appends a list-A node (native
 * model load), and the framework's buildTargets postfix wires it into
 * target[1] + raises Quest+0x67C to 2. MUST be called from a
 * MHFU_EVENT_QUEST_TARGETS_BUILDING subscriber. x/z = spawn coords (0 = clone
 * the source monster's section, then RELOCATE in-area for section 1). Only
 * species with a known record layout (Tigrex) are supported. */
mhfu_hook_rc_t mhfu_quest_add_monster(mhfu_quest_t q, mhfu_monster_id_t id,
                                      float x, float z);

/* DUPLICATE the quest's existing big monster `id` as a 2ND instance of the SAME
 * family (Section 48). buildTargets is uncapped, so two same-species list-A nodes
 * land in group 0 (count 2) sharing the one resident overlay — no 2nd group, no
 * forge, no relocation. Clones the source record, offsets its spawn coords by
 * dx/dz so the pair don't stack, appends a list-A node. MUST be called from a
 * MHFU_EVENT_QUEST_TARGETS_BUILDING subscriber. Returns MHFU_HOOK_OK, or BADARG
 * if `id` isn't in the quest. */
mhfu_hook_rc_t mhfu_quest_clone_monster(mhfu_quest_t q, mhfu_monster_id_t id,
                                        float dx, float dz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_QUEST_H */
