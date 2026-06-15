/* Game-memory helpers — see include/mhfu/memory.h. */
#include "mhfu/memory.h"
#include "internal.h"

extern "C" {

uint8_t  mhfu_read_u8 (uint32_t a) { return *(volatile uint8_t  *)a; }
uint16_t mhfu_read_u16(uint32_t a) { return *(volatile uint16_t *)a; }
uint32_t mhfu_read_u32(uint32_t a) { return *(volatile uint32_t *)a; }
void     mhfu_write_u8 (uint32_t a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
void     mhfu_write_u16(uint32_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
void     mhfu_write_u32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

float mhfu_read_f32(uint32_t a)
{
    union { uint32_t u; float f; } cvt;
    cvt.u = *(volatile uint32_t *)a;
    return cvt.f;
}
void mhfu_write_f32(uint32_t a, float v)
{
    union { uint32_t u; float f; } cvt;
    cvt.f = v;
    *(volatile uint32_t *)a = cvt.u;
}

int mhfu_mem_valid(uint32_t a)
{
    /* Main managed RAM only. The extra memory=64 region [0x0A000000,0x0C000000)
     * (clones / relocated overlays) is validated with explicit range checks at the
     * call sites that need it — NOT here — so this stays a conservative gate for
     * the quest/recbase/EBOOT-pointer checks that rely on it. */
    return a >= 0x08000000u && a < 0x0A000000u;
}

uint8_t mhfu_get_screen_state(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    return r ? *(volatile uint8_t *)r->cell_screen_state : 0;
}
uint16_t mhfu_get_area_index(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    return r ? *(volatile uint16_t *)r->cell_area_index : 0;
}
uint32_t mhfu_get_quest_timer(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    return r ? *(volatile uint32_t *)r->cell_quest_timer : 0;
}
uint32_t mhfu_get_player_hp(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    return r ? *(volatile uint32_t *)r->cell_player_hp : 0;
}
uint16_t mhfu_get_sharpness_current(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    return r ? *(volatile uint16_t *)r->cell_sharpness_current : 0;
}

} /* extern "C" */
