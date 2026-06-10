/*
 * Monster-entity access.
 *
 * The entity registry at 0x09C1213C is a u32[] of entity pointers; slot
 * 0 is reserved, slots 1..20 are monsters (0 = empty). Offsets below are
 * the verified-stable subset from docs/agent_memory_map.md; for anything
 * else read the entity with mhfu_read_* (memory.h).
 */
#ifndef MHFU_ENTITY_H
#define MHFU_ENTITY_H

#include <stdint.h>
#include "ids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MHFU_ENTITY_REGISTRY      0x09C1213Cu
#define MHFU_ENTITY_REGISTRY_SLOTS 21

/* Entity-relative offsets (verified). */
#define MHFU_ENT_SIZE_SCALE  0x024  /* f32 (mirrors: 0x220/0x224/0x228/0x270) */
#define MHFU_ENT_HEADING     0x010  /* vec3 — engine OUTPUT (render fwd row)   */
#define MHFU_ENT_TRANSLATION 0x040  /* transform translation row (x,y,z,1)     */
#define MHFU_ENT_YAW         0x1F4  /* u16 packed yaw — THE rotation control   */
#define MHFU_ENT_ENTITY_ID   0x1E4  /* u8  */
#define MHFU_ENT_MONSTER_TYPE 0x1E8 /* u8  — 0x46 Popo, 0x45 Anteka, 0x4B Tigrex */
#define MHFU_ENT_POSITION    0x200  /* vec3 world pos */
#define MHFU_ENT_HP          0x2E4  /* u16 */
#define MHFU_ENT_SECTION     0x29A  /* u16 — map section (visibility gate A)   */
#define MHFU_ENT_TARGET_ACQ  0x2A4  /* u8  — target-acquired                   */
#define MHFU_ENT_AI_STATE    0x334  /* u8  — 2 = engaged                       */
#define MHFU_ENT_PURSUIT     0x05D0 /* vec3 — pursuit/target vec (w/ engage)   */
#define MHFU_ENT_ENGAGE_FLAG 0x05DC /* f32 — 1.0 = engaged */
#define MHFU_ENT_FLAGS638    0x638  /* u32 — bit 0x8000 = visibility gate B    */
#define MHFU_ENT_NEXTOBJ     0x1C4  /* ObjBase nextObj — engine update/tick chain */
#define MHFU_ENT_PREVOBJ     0x1C8  /* ObjBase prevObj                            */

typedef struct { float x, y, z; } mhfu_vec3_t;

/* Registry access. */
uint32_t mhfu_entity_at(int slot);             /* ptr in slot, or 0 */
int      mhfu_entity_slot_of(uint32_t ent);    /* slot of ptr, or -1 */
int      mhfu_entity_is_alive(uint32_t ent);   /* still in the registry? */

/* Collect up to `max` live entity pointers of a given type into `out`.
 * Returns the count written. */
int mhfu_entities_of_type(mhfu_monster_id_t type, uint32_t *out, int max);

/* Typed field accessors (return 0 / no-op on a NULL/out-of-RAM ptr). */
uint8_t     mhfu_entity_type(uint32_t ent);
uint16_t    mhfu_entity_hp(uint32_t ent);
float       mhfu_entity_size(uint32_t ent);
void        mhfu_entity_set_size(uint32_t ent, float v);  /* writes all 5 mirrors */
mhfu_vec3_t mhfu_entity_pos(uint32_t ent);
void        mhfu_entity_set_pos(uint32_t ent, mhfu_vec3_t p); /* + translation row */

uint16_t    mhfu_entity_yaw(uint32_t ent);              /* +0x1F4 packed u16 */
void        mhfu_entity_set_yaw(uint32_t ent, uint16_t yaw);
uint8_t     mhfu_entity_ai_state(uint32_t ent);         /* +0x334 */
void        mhfu_entity_set_ai_state(uint32_t ent, uint8_t s);
int         mhfu_entity_engaged(uint32_t ent);          /* +0x05DC f32 >= 0.5 */
void        mhfu_entity_set_engaged(uint32_t ent, int engaged);

uint16_t    mhfu_entity_section(uint32_t ent);          /* +0x29A */
void        mhfu_entity_set_section(uint32_t ent, uint16_t section);

/* Make a (swapped/relocated) monster render in `section`: set the section
 * tracker +0x29A and OR the +0x638 visibility bit, so the per-frame gate
 * 0x09AC4960 stops culling it. Pass the player's current area_index as
 * `section` (memory tigrex-section1 / giadrome-tigrex-render-bug). */
void        mhfu_entity_make_visible(uint32_t ent, uint16_t section);

/* Force an entity to engage `target` (world pos): write the pursuit vec
 * (target - pos) at +0x5D0, engage flag +0x5DC = 1.0, ai_state +0x334 = 2,
 * and target-acquired +0x2A4 = 1 — together, the way the engine's own
 * per-frame sv.q writes them (Section 33g). Use to force aggro where the
 * engine's target resolver returns null (e.g. basecamp). */
void        mhfu_entity_force_aggro(uint32_t ent, mhfu_vec3_t target);

/* Clear an entity's aggression: zero the engage flag + the per-entity
 * detection ranges so it won't re-acquire the player. Pair with
 * mhfu_species_set_detection() to also block NEW aggro (monster.h). */
void        mhfu_entity_calm(uint32_t ent);

/* Clone a live monster entity (SAME species) into fresh scratch RAM and
 * make it a real, ticking, rendered monster. Deep-copies the per-species
 * struct, rebases every internal self-pointer, splices the copy onto the
 * engine update chain (+0x1C4) + a free registry slot, and spawns it CALM.
 * The clone SHARES the source's read-only model / skeleton / overlay /
 * species buffers, so duplicating is cheap (only the entity struct is
 * copied) — but the species MUST be resident (clone a Tigrex only in a
 * quest where a Tigrex is loaded). Returns the clone's pointer, or 0 on
 * failure. Recipe: memory tigrex-clone-recipe / objbase-linked-list-spawn. */
uint32_t    mhfu_entity_clone(uint32_t src);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_ENTITY_H */
