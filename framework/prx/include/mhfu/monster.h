/*
 * Species-global monster operations (data shared by all entities of a
 * type, as opposed to the per-entity accessors in entity.h).
 */
#ifndef MHFU_MONSTER_H
#define MHFU_MONSTER_H

#include "ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set a species' sight/detection radius. 0 = blind = won't acquire the
 * player = stops NEW aggro (verified recipe, memory tigrex-aggro-disable).
 * Persists — the engine reads this on evaluation, never writes it.
 *
 * Scope honesty: only Tigrex's address is verified (0x09BC1030). Other
 * species return non-zero (unsupported) until their offset is decoded. */
int mhfu_species_set_detection(mhfu_monster_id_t id, float radius);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_MONSTER_H */
