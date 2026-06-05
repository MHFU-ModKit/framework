/* Monster-entity access — see include/mhfu/entity.h. */
#include "mhfu/entity.h"
#include "mhfu/memory.h"

static const uint32_t SIZE_MIRRORS[5] = { 0x024, 0x220, 0x224, 0x228, 0x270 };

static int in_ram(uint32_t a) { return a >= 0x08000000u && a < 0x0A000000u; }

extern "C" {

uint32_t mhfu_entity_at(int slot)
{
    if (slot < 0 || slot >= MHFU_ENTITY_REGISTRY_SLOTS) return 0;
    return *(volatile uint32_t *)(MHFU_ENTITY_REGISTRY + slot * 4);
}

int mhfu_entity_slot_of(uint32_t ent)
{
    for (int s = 1; s < MHFU_ENTITY_REGISTRY_SLOTS; s++)
        if (mhfu_entity_at(s) == ent) return s;
    return -1;
}

int mhfu_entity_is_alive(uint32_t ent)
{
    return ent && mhfu_entity_slot_of(ent) >= 0;
}

uint8_t mhfu_entity_type(uint32_t ent)
{
    return in_ram(ent) ? *(volatile uint8_t *)(ent + MHFU_ENT_MONSTER_TYPE) : 0;
}

uint16_t mhfu_entity_hp(uint32_t ent)
{
    return in_ram(ent) ? *(volatile uint16_t *)(ent + MHFU_ENT_HP) : 0;
}

float mhfu_entity_size(uint32_t ent)
{
    return in_ram(ent) ? mhfu_read_f32(ent + MHFU_ENT_SIZE_SCALE) : 0.0f;
}

void mhfu_entity_set_size(uint32_t ent, float v)
{
    if (!in_ram(ent)) return;
    for (int i = 0; i < 5; i++) mhfu_write_f32(ent + SIZE_MIRRORS[i], v);
}

mhfu_vec3_t mhfu_entity_pos(uint32_t ent)
{
    mhfu_vec3_t p = { 0.0f, 0.0f, 0.0f };
    if (!in_ram(ent)) return p;
    p.x = mhfu_read_f32(ent + MHFU_ENT_POSITION + 0);
    p.y = mhfu_read_f32(ent + MHFU_ENT_POSITION + 4);
    p.z = mhfu_read_f32(ent + MHFU_ENT_POSITION + 8);
    return p;
}

void mhfu_entity_set_pos(uint32_t ent, mhfu_vec3_t p)
{
    if (!in_ram(ent)) return;
    mhfu_write_f32(ent + MHFU_ENT_POSITION + 0, p.x);
    mhfu_write_f32(ent + MHFU_ENT_POSITION + 4, p.y);
    mhfu_write_f32(ent + MHFU_ENT_POSITION + 8, p.z);
    /* keep the transform translation row in sync */
    mhfu_write_f32(ent + MHFU_ENT_TRANSLATION + 0, p.x);
    mhfu_write_f32(ent + MHFU_ENT_TRANSLATION + 4, p.y);
    mhfu_write_f32(ent + MHFU_ENT_TRANSLATION + 8, p.z);
}

int mhfu_entities_of_type(mhfu_monster_id_t type, uint32_t *out, int max)
{
    int n = 0;
    for (int s = 1; s < MHFU_ENTITY_REGISTRY_SLOTS && n < max; s++) {
        uint32_t p = mhfu_entity_at(s);
        if (p && mhfu_entity_type(p) == (uint8_t)type) out[n++] = p;
    }
    return n;
}

uint16_t mhfu_entity_yaw(uint32_t ent)
{
    return in_ram(ent) ? *(volatile uint16_t *)(ent + MHFU_ENT_YAW) : 0;
}
void mhfu_entity_set_yaw(uint32_t ent, uint16_t yaw)
{
    if (in_ram(ent)) *(volatile uint16_t *)(ent + MHFU_ENT_YAW) = yaw;
}

uint8_t mhfu_entity_ai_state(uint32_t ent)
{
    return in_ram(ent) ? *(volatile uint8_t *)(ent + MHFU_ENT_AI_STATE) : 0;
}
void mhfu_entity_set_ai_state(uint32_t ent, uint8_t s)
{
    if (in_ram(ent)) *(volatile uint8_t *)(ent + MHFU_ENT_AI_STATE) = s;
}

int mhfu_entity_engaged(uint32_t ent)
{
    return in_ram(ent) && mhfu_read_f32(ent + MHFU_ENT_ENGAGE_FLAG) >= 0.5f;
}
void mhfu_entity_set_engaged(uint32_t ent, int engaged)
{
    if (in_ram(ent)) mhfu_write_f32(ent + MHFU_ENT_ENGAGE_FLAG, engaged ? 1.0f : 0.0f);
}

uint16_t mhfu_entity_section(uint32_t ent)
{
    return in_ram(ent) ? *(volatile uint16_t *)(ent + MHFU_ENT_SECTION) : 0;
}
void mhfu_entity_set_section(uint32_t ent, uint16_t section)
{
    if (in_ram(ent)) *(volatile uint16_t *)(ent + MHFU_ENT_SECTION) = section;
}

void mhfu_entity_make_visible(uint32_t ent, uint16_t section)
{
    if (!in_ram(ent)) return;
    if (*(volatile uint16_t *)(ent + MHFU_ENT_SECTION) != section)
        *(volatile uint16_t *)(ent + MHFU_ENT_SECTION) = section;
    uint32_t fl = mhfu_read_u32(ent + MHFU_ENT_FLAGS638);
    if ((fl & 0x8000u) == 0) mhfu_write_u32(ent + MHFU_ENT_FLAGS638, fl | 0x8000u);
}

void mhfu_entity_force_aggro(uint32_t ent, mhfu_vec3_t target)
{
    if (!in_ram(ent)) return;
    mhfu_vec3_t p = mhfu_entity_pos(ent);
    mhfu_write_f32(ent + MHFU_ENT_PURSUIT + 0, target.x - p.x);
    mhfu_write_f32(ent + MHFU_ENT_PURSUIT + 4, target.y - p.y);
    mhfu_write_f32(ent + MHFU_ENT_PURSUIT + 8, target.z - p.z);
    mhfu_write_f32(ent + MHFU_ENT_ENGAGE_FLAG, 1.0f);
    *(volatile uint8_t *)(ent + MHFU_ENT_AI_STATE)   = 2;
    *(volatile uint8_t *)(ent + MHFU_ENT_TARGET_ACQ) = 1;
}

void mhfu_entity_calm(uint32_t ent)
{
    if (!in_ram(ent)) return;
    /* per-entity detection ranges (read-on-evaluate, the engine never
     * writes them, so they stick) + clear the current engage flag */
    static const uint32_t det_offs[6] = {0x064C, 0x0650, 0x0654, 0x067C, 0x0680, 0x0684};
    for (int k = 0; k < 6; k++) mhfu_write_f32(ent + det_offs[k], 0.0f);
    mhfu_write_f32(ent + MHFU_ENT_ENGAGE_FLAG, 0.0f);
}

} /* extern "C" */
