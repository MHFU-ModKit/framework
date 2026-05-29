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
#define MHFU_ENT_AI_STATE    0x334  /* u8  */
#define MHFU_ENT_ENGAGE_FLAG 0x05DC /* f32 — 1.0 = engaged */

typedef struct { float x, y, z; } mhfu_vec3_t;

/* Registry access. */
uint32_t mhfu_entity_at(int slot);             /* ptr in slot, or 0 */
int      mhfu_entity_slot_of(uint32_t ent);    /* slot of ptr, or -1 */
int      mhfu_entity_is_alive(uint32_t ent);   /* still in the registry? */

/* Typed field accessors (return 0 / no-op on a NULL/out-of-RAM ptr). */
uint8_t     mhfu_entity_type(uint32_t ent);
uint16_t    mhfu_entity_hp(uint32_t ent);
float       mhfu_entity_size(uint32_t ent);
void        mhfu_entity_set_size(uint32_t ent, float v);  /* writes all 5 mirrors */
mhfu_vec3_t mhfu_entity_pos(uint32_t ent);
void        mhfu_entity_set_pos(uint32_t ent, mhfu_vec3_t p); /* + translation row */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_ENTITY_H */
