/*
 * M2 — load a 2nd big-monster AI overlay (em*.ovl) into a fresh relocated slot.
 *
 * The engine loads every em*.ovl to ONE fixed slot (0x09d15100), so a 2nd
 * different family has nowhere to go. This reads the .ovl from memstick, allocates
 * a 64KB-aligned region, relocates a copy there (mhfu_ovl_relocate), zeroes its
 * BSS and runs its static-initializers. M3 then builds a manager that drives it.
 *
 * See docs/BIG_MONSTER_OVERLAY_RELOCATION.md.
 */
#ifndef MHFU_BIGMON_OVERLAY_H
#define MHFU_BIGMON_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      uid;          /* partition-mem block id (sceKernelAllocPartitionMemory) */
    uint32_t region_base;  /* relocated footprint base (= 0x09d15100 + delta) */
    int32_t  delta;        /* 64KB-aligned relocation delta */
    uint32_t new_load;     /* relocated header.load_address (file image VA) */
    uint32_t img_size;     /* .ovl file size */
    uint32_t foot_size;    /* total footprint bytes (incl lower/upper BSS) */
    uint32_t si_start;     /* relocated static-initializer list start (VA) */
    uint32_t si_end;
} mhfu_ovl_region_t;

/* Load+relocate+place the em overlay at `path` (e.g.
 * "ms0:/PSP/PLUGINS/mhfu_framework/overlays/em75.ovl"). Fills *out on success.
 * Returns 0 on success, negative on error. Does NOT wire it to the engine yet (M3). */
int mhfu_bigmon_load_relocated(const char *path, mhfu_ovl_region_t *out);

/* Run the relocated overlay's static-initializers (call once after placement,
 * before first use). */
void mhfu_bigmon_run_static_inits(const mhfu_ovl_region_t *r);

#ifdef __cplusplus
}
#endif

#endif /* MHFU_BIGMON_OVERLAY_H */
