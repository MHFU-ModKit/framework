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

/* --- code cave (cave.cpp) --- */
uint32_t *mhfu_cave_alloc(int n_insns);   /* bump allocator; 0 if exhausted */

/* --- cache / self-modifying-code (trampoline.cpp) --- */
void mhfu_smc_patch_word(uint32_t addr, uint32_t word); /* ranged invalidate */
void mhfu_flush_caches(void);                           /* dcache+icache, all */

/* --- deferred / quiet-screen patch queue (install.cpp) --- */
int  mhfu_deferred_poll_thread(SceSize args, void *argp);

/* --- event trampolines (trampoline.cpp) --- */
int  mhfu_install_event_trampolines(void);
void mhfu_uninstall_event_trampolines(void);
int  mhfu_install_worker_thread(SceSize args, void *argp);
/* Generic prefix-trampoline on an arbitrary (non-event) anchor PC; dispatcher
 * receives a0=&mhfu_anchor_regs_t. Install while JIT-cold. Idempotent per addr. */
int  mhfu_install_trampoline(uint32_t anchor_pc, uint32_t dispatcher);

/* --- hook arbitration (hooks.cpp / hookmgr) --- */
void mhfu_hookmgr_init(void);

/* --- mod table (modtable.cpp) --- */
void mhfu_mods_init_all(void);
void mhfu_mods_shutdown_all(void);

/* --- AI events (ai.cpp) --- */
void mhfu_ai_on_monster_spawn(int slot, uint32_t entity, uint8_t type, uint16_t hp);
void mhfu_ai_poll_death(void);

/* --- quest domain (quest.cpp) --- */
/* Installs the buildTargets wrapper IFF a mod subscribed to
 * MHFU_EVENT_QUEST_TARGETS_BUILDING. Call after mhfu_mods_init_all(). */
void mhfu_quest_init(void);

/* --- inject (inject.cpp) --- */
/* Picks the inject scratch region once (emulator raw window vs real-HW 4 MB
 * volatile). Fault-safe; call early from bootstrap. Idempotent. */
void mhfu_xram_platform_init(void);

/* --- bootstrap (bootstrap.cpp) --- */
void mhfu_sentinel_set(uint32_t offset, uint32_t value);
#define MHFU_SENTINEL_BASE 0x08AEFFE0u

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_CORE_INTERNAL_H */
