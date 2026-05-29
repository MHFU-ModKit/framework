/* Species-global monster ops — see include/mhfu/monster.h. */
#include "mhfu/monster.h"
#include "mhfu/memory.h"

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
