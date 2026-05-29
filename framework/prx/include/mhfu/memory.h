/*
 * Game-memory access. The PRX runs ON the emulated PSP, so these are
 * plain volatile loads/stores — no debugger round-trip.
 *
 * Prefer the named getters over raw addresses: when multi-region support
 * lands, a mod built against the getters keeps working without a rebuild.
 */
#ifndef MHFU_MEMORY_H
#define MHFU_MEMORY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Named cells (resolved through the active region table). */
uint8_t  mhfu_get_screen_state(void);     /* 0x08A8CA48; 17 = in quest area */
uint16_t mhfu_get_area_index(void);        /* 0x08B0C7DC; visible map section */
uint32_t mhfu_get_quest_timer(void);       /* 0x09A05DD0; frames @ 30 Hz */
uint32_t mhfu_get_player_hp(void);
uint16_t mhfu_get_sharpness_current(void);

/* Lower-level escape hatches. */
uint8_t  mhfu_read_u8 (uint32_t addr);
uint16_t mhfu_read_u16(uint32_t addr);
uint32_t mhfu_read_u32(uint32_t addr);
void     mhfu_write_u8 (uint32_t addr, uint8_t  v);
void     mhfu_write_u16(uint32_t addr, uint16_t v);
void     mhfu_write_u32(uint32_t addr, uint32_t v);

/* Float convenience (type-punned, no strict-aliasing UB). */
float    mhfu_read_f32 (uint32_t addr);
void     mhfu_write_f32(uint32_t addr, float v);

/* 1 if addr is in PSP user/EBOOT RAM (0x08000000..0x0A000000), else 0.
 * Use before dereferencing a value read from game memory. */
int      mhfu_mem_valid(uint32_t addr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_MEMORY_H */
