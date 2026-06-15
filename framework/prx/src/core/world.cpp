/* World / player / cheat helpers — see include/mhfu/world.h. */
#include "mhfu/world.h"
#include "mhfu/memory.h"

extern "C" {

mhfu_vec3_t mhfu_player_pos(void)
{
    mhfu_vec3_t p;
    p.x = mhfu_read_f32(MHFU_PLAYER_POS + 0);
    p.y = mhfu_read_f32(MHFU_PLAYER_POS + 4);
    p.z = mhfu_read_f32(MHFU_PLAYER_POS + 8);
    return p;
}

void mhfu_paint_map(void)
{
    mhfu_write_u8(MHFU_PAINT_MAP_CELL, 0xFF);
}

} /* extern "C" */
