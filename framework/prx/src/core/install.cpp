/*
 * Ergonomic install helpers: a deferred "patch when the screen is
 * JIT-quiet" queue, and a call-wrapper builder that emits the stub into
 * the code cave. These move the two scariest hand-rolled patterns
 * (TITLE/MENU gate + hand-encoded MIPS wrapper stubs) out of mods.
 */
#include <pspthreadman.h>

#include "mhfu/hooks.h"
#include "mhfu/mips.h"
#include "mhfu/memory.h"
#include "mhfu/log.h"
#include "internal.h"

#define MAX_DEFERRED 16

typedef struct {
    int         pending;
    uint32_t    addr;
    uint32_t    expect;
    uint32_t    word;
    const char *owner;
} deferred_t;

static deferred_t g_deferred[MAX_DEFERRED];

extern "C" mhfu_hook_rc_t mhfu_patch_word_when_quiet(uint32_t addr, uint32_t expect,
                                                     uint32_t word, const char *owner)
{
    if (!owner) return MHFU_HOOK_BADARG;
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (g_deferred[i].pending && g_deferred[i].addr == addr) return MHFU_HOOK_OK;
    }
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (g_deferred[i].pending) continue;
        g_deferred[i].addr    = addr;
        g_deferred[i].expect  = expect;
        g_deferred[i].word    = word;
        g_deferred[i].owner   = owner;
        g_deferred[i].pending = 1;
        return MHFU_HOOK_OK;
    }
    return MHFU_HOOK_NOSPACE;
}

extern "C" int mhfu_deferred_poll_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(100 * 1000);   /* 10 Hz */
        uint8_t scr = mhfu_get_screen_state();

        /* Real-HW volatile activity is DEFERRED until we've reached the village at least
         * once (scr=22). Before the village, the savedata utility borrows the 4 MB
         * volatile partition at character-select; touching it there froze the load. */
        static int g_village_seen = 0;
        if (scr == 22) g_village_seen = 1;
        if (g_village_seen) {
            /* OBSERVE hook: repoint the game's volatile Lock/Unlock import stubs (once,
             * here at the village = post-savedata, before any quest depart) and drain the
             * pass-through call log (ms0-gated). Real-HW only; PPSSPP no-op. */
            mhfu_vobs_install();
            mhfu_vobs_flush();
            /* lock/stage driver — no-op in the recon build (lock path disabled): prelock
             * early-returns on !g_prelock_armed, release on g_vol_locked<=0. release on
             * the quest-exit edge. */
            mhfu_inject_xram_prelock();
            {
                static uint8_t s_prev_scr = 0xFF;
                if (s_prev_scr == 17 && scr != 17) mhfu_inject_xram_release();
                s_prev_scr = scr;
            }
            /* Volatile lock-HOLD recon (real HW): flushes the quest-depart acquire log,
             * probes village free/busy, heartbeats while held, releases save-safely.
             * acquire itself fires from mhfu_dispatch_quest_beginning. PPSSPP no-op. */
            mhfu_inject_xram_recon_tick(scr);
            /* NOTE: the read-only volatile USAGE probes (mhfu_inject_xram_usage_baseline /
             * _scan) are REMOVED — they raw-READ the 4 MB volatile partition (0x08400000)
             * WITHOUT holding its lock, which faults during the savedata/character-select
             * window and froze the boot (HW bisect 2026-06-27). They were diagnostic-only
             * (squat-feasibility measurement) and are not on the Brute-load path. */
        }

        if (scr != 0x01 && scr != 0x04) continue;   /* MENU / TITLE only */
        for (int i = 0; i < MAX_DEFERRED; i++) {
            deferred_t *d = &g_deferred[i];
            if (!d->pending) continue;
            if (*(volatile uint32_t *)d->addr != d->expect) continue;  /* not yet / changed */
            if (mhfu_patch_word(d->addr, d->word, d->owner) == MHFU_HOOK_OK) {
                d->pending = 0;
                mhfu_log("[install] quiet-patched 0x%08lx for '%s'",
                         (unsigned long)d->addr, d->owner);
            }
        }
    }
    return 0;
}

extern "C" mhfu_hook_rc_t mhfu_install_call_wrapper(uint32_t call_site, uint32_t orig_target,
                                                    void (*helper)(uint32_t), int mode,
                                                    const char *owner)
{
    if (!helper || !owner) return MHFU_HOOK_BADARG;
    uint32_t *s = mhfu_cave_alloc(16);
    if (!s) return MHFU_HOOK_NOSPACE;
    uint32_t h = (uint32_t)(uintptr_t)helper;
    int i = 0;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    if (mode == MHFU_WRAP_POSTFIX) {
        s[i++] = mips_jal(orig_target); s[i++] = MIPS_NOP;
        s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
        s[i++] = mips_jal(h);           s[i++] = MIPS_NOP;
    } else { /* PREFIX */
        s[i++] = mips_jal(h);           s[i++] = MIPS_NOP;
        s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
        s[i++] = mips_jal(orig_target); s[i++] = MIPS_NOP;
    }
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
    while (i < 16) s[i++] = MIPS_NOP;

    mhfu_flush_caches();
    return mhfu_patch_word_when_quiet(call_site, mips_jal(orig_target),
                                      mips_jal((uint32_t)(uintptr_t)s), owner);
}
