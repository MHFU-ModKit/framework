/*
 * Internal contract between the framework's core TUs. NOT part of the
 * public mod SDK under include/mhfu — mod authors never include this.
 */
#ifndef MHFU_CORE_INTERNAL_H
#define MHFU_CORE_INTERNAL_H

#include <stdint.h>
#include <pspkerneltypes.h>
#include "mhfu/events.h"
#include "mhfu/addresses.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register snapshot the event trampolines build on the stack. The first
 * 9 u32s match the on-stack layout the wrapper writes, offset by offset. */
typedef struct mhfu_anchor_regs {
    uint32_t a0, a1, a2, a3;
    uint32_t v0, v1;
    uint32_t ra, sp, pc;
} mhfu_anchor_regs_t;

/* --- region (region.cpp) --- */
int                         mhfu_region_detect(void);
const mhfu_region_addrs_t  *mhfu_region(void);

/* --- registry (registry.cpp) --- */
int  mhfu_registry_count(mhfu_event_id_t id);
void mhfu_registry_fire(mhfu_event_id_t id, const void *ctx);
/* trampoline entry points (addresses taken by the cave; bodies fan out) */
void mhfu_dispatch_quest_beginning(const mhfu_anchor_regs_t *regs);
void mhfu_dispatch_quest_entered  (const mhfu_anchor_regs_t *regs);
int  mhfu_monster_spawn_poll_thread(SceSize args, void *argp);

/* --- cache / self-modifying-code (trampoline.cpp) --- */
void mhfu_smc_patch_word(uint32_t addr, uint32_t word); /* ranged invalidate */
void mhfu_flush_caches(void);                           /* dcache+icache, all */

/* --- event trampolines (trampoline.cpp) --- */
int  mhfu_install_event_trampolines(void);
void mhfu_uninstall_event_trampolines(void);
int  mhfu_install_worker_thread(SceSize args, void *argp);

/* --- hook arbitration (hooks.cpp / hookmgr) --- */
void mhfu_hookmgr_init(void);

/* --- mod table (modtable.cpp) --- */
void mhfu_mods_init_all(void);
void mhfu_mods_shutdown_all(void);

/* --- bootstrap (bootstrap.cpp) --- */
void mhfu_sentinel_set(uint32_t offset, uint32_t value);
#define MHFU_SENTINEL_BASE 0x08AEFFE0u

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_CORE_INTERNAL_H */
