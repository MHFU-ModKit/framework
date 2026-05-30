/* Species-global monster ops — see include/mhfu/monster.h. */
#include "mhfu/monster.h"
#include "mhfu/memory.h"
#include "mhfu/ids.h"
#include <stdio.h>

extern "C" const char *mhfu_monster_name(unsigned int t)
{
    switch ((mhfu_monster_id_t)(t & 0xFF)) {
        case MON_ANTEKA:   return "ANTEKA";
        case MON_POPO:     return "POPO";
        case MON_TIGREX:   return "TIGREX";
        case MON_GIADROME: return "GIADROME";
        default: break;
    }
    static char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", t & 0xFF);
    return buf;
}

/* Verified species sight/detection-radius cells. Only Tigrex is confirmed
 * (0x09BC1030); add entries as other species' offsets are decoded. */
static uint32_t species_detection_addr(mhfu_monster_id_t id)
{
    switch (id) {
        case MON_TIGREX: return 0x09BC1030u;
        default:         return 0;
    }
}

extern "C" int mhfu_species_set_detection(mhfu_monster_id_t id, float radius)
{
    uint32_t a = species_detection_addr(id);
    if (!a) return -1;          /* offset not known for this species */
    mhfu_write_f32(a, radius);
    return 0;
}
