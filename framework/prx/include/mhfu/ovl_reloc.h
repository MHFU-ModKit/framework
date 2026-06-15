/*
 * MWo3 monster-overlay relocator (C port of tools/ovl_reloc.py).
 *
 * Big-monster AI overlays (em*.ovl) are position-dependent blobs that the engine
 * loads raw to one fixed slot (em -> 0x09d15100). To run a 2nd different family we
 * relocate a copy to a fresh VA. This applies the validated relocation in place.
 *
 * See docs/BIG_MONSTER_OVERLAY_RELOCATION.md.
 */
#ifndef MHFU_OVL_RELOC_H
#define MHFU_OVL_RELOC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* em-overlay class slot base (foot_lo). Overlay code references its own lower BSS
 * below the file image, so the relocatable footprint starts here, not at the MWo3
 * header.load_address. */
#define MHFU_OVL_SLOT_EM   0x09d15100u

typedef struct {
    char     magic[4];          /* "MWo3" */
    uint32_t overlay_id;
    uint32_t load_address;       /* VA of file offset 0 */
    uint32_t text_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t static_init_start;
    uint32_t static_init_end;
    char     name[32];
} mhfu_mwo3_header_t;

typedef struct {
    int      ok;                 /* 1 = success */
    uint32_t n_jump;             /* MIPS_26 relocated */
    uint32_t n_hilo;             /* HI16/LO16 lo-sites relocated */
    uint32_t n_data;             /* MIPS_32 data pointers relocated */
    uint32_t new_base;           /* load_address + delta */
} mhfu_ovl_reloc_stats_t;

/* Relocate the MWo3 overlay `image` (of `image_size` bytes, == 64+text+data) IN
 * PLACE by `delta` (newbase = header.load_address + delta). `slot_base` is the
 * class footprint base (MHFU_OVL_SLOT_EM for em*). `delta` MUST be 64KB-aligned
 * (low 16 bits zero). Returns stats; stats.ok==0 on malformed header / bad delta. */
mhfu_ovl_reloc_stats_t mhfu_ovl_relocate(void *image, uint32_t image_size,
                                         uint32_t slot_base, int32_t delta);

#ifdef __cplusplus
}
#endif

#endif /* MHFU_OVL_RELOC_H */
