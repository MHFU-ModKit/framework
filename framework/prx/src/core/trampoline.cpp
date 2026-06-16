/*
 * Event trampolines + code cave + self-modifying-code helpers.
 *
 * Per event anchor: build a wrapper in the cave that spills the caller's
 * GPRs into an mhfu_anchor_regs_t on the stack, calls the C dispatcher,
 * restores, runs the two displaced instructions, and jumps back. The
 * anchor PC is patched to `J cave; NOP`.
 *
 * Delay-slot caveat: the two displaced instructions are copied verbatim,
 * so an anchor PC must point at a plain (non-branch) instruction. The
 * EU quest_beginning / quest_entered anchors are both `sv.q` stores —
 * vetted. New anchors must be checked before use.
 */
#include <pspsdk.h>
#include <psputils.h>
#include <pspthreadman.h>

#include "mhfu/mips.h"
#include "internal.h"

#define WRAPPER_INSNS 54        /* 49 used + 5 NOP pad */

typedef struct {
    int      installed;
    uint32_t anchor_pc;
    uint32_t saved_insn0;
    uint32_t saved_insn1;
    uint32_t wrapper_addr;
} install_record_t;

static install_record_t g_install[MHFU_EVENT_COUNT_];

/* --- cache helpers ------------------------------------------------------ */

/* CWCheat-style per-write ranged invalidate: the only PPSSPP path that
 * drops a stale JIT block for the patched address (routes through
 * MIPSState::InvalidateICache -> JitBlockCache::DestroyBlock). Also the
 * order real hardware requires (dcache writeback, then icache). */
extern "C" void mhfu_smc_patch_word(uint32_t addr, uint32_t word)
{
    *(volatile uint32_t *)addr = word;
    sceKernelDcacheWritebackInvalidateRange((const void *)addr, 4);
    sceKernelIcacheInvalidateRange((const void *)addr, 4);
}

extern "C" void mhfu_flush_caches(void)
{
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
}

/* --- wrapper builder ---------------------------------------------------- */

static int install_trampoline_for(uint32_t anchor_pc,
                                   uint32_t dispatcher, install_record_t *rec)
{
    if (anchor_pc & 0x3) return -1;

    uint32_t insn0 = *(volatile uint32_t *)(anchor_pc + 0);
    uint32_t insn1 = *(volatile uint32_t *)(anchor_pc + 4);

    uint32_t *w = mhfu_cave_alloc(WRAPPER_INSNS);
    if (!w) return -2;
    int i = 0;

    /* Prologue — 0x80 frame. Save every caller-saved GPR before calling
     * the C dispatcher (it clobbers $at,$t0..$t9 and the game expects
     * them to survive the original sv.q). Stack matches mhfu_anchor_regs_t
     * for the first 9 slots:
     *   +0x00 a0  +0x14 v1  +0x28 at
     *   +0x04 a1  +0x18 ra  +0x2C t0 ... +0x50 t9
     *   +0x08 a2  +0x1C sp
     *   +0x0C a3  +0x20 pc
     *   +0x10 v0  +0x24 (pad) */
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x80);
    w[i++] = mips_sw(MIPS_REG_A0, 0x00, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A1, 0x04, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_V0, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_V1, 0x14, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_AT, 0x28, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T0, 0x2C, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T1, 0x30, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T2, 0x34, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T3, 0x38, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T4, 0x3C, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T5, 0x40, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T6, 0x44, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T7, 0x48, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T8, 0x4C, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_T9, 0x50, MIPS_REG_SP);

    w[i++] = mips_addiu(MIPS_REG_T0, MIPS_REG_SP, 0x80);
    w[i++] = mips_sw(MIPS_REG_T0, 0x1C, MIPS_REG_SP);
    w[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(anchor_pc >> 16));
    w[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(anchor_pc & 0xFFFF));
    w[i++] = mips_sw(MIPS_REG_T0, 0x20, MIPS_REG_SP);

    w[i++] = mips_move(MIPS_REG_A0, MIPS_REG_SP);
    w[i++] = mips_jal(dispatcher);
    w[i++] = MIPS_NOP;

    w[i++] = mips_lw(MIPS_REG_A0, 0x00, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A1, 0x04, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_V0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_V1, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_AT, 0x28, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T0, 0x2C, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T1, 0x30, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T2, 0x34, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T3, 0x38, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T4, 0x3C, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T5, 0x40, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T6, 0x44, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T7, 0x48, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T8, 0x4C, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_T9, 0x50, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x80);

    w[i++] = insn0;
    w[i++] = insn1;
    w[i++] = mips_j(anchor_pc + 8);
    w[i++] = MIPS_NOP;

    while (i < WRAPPER_INSNS) w[i++] = MIPS_NOP;

    /* Patch the anchor: write the delay-slot NOP first so an interrupted
     * install degrades to "insn1 became NOP" rather than a stale branch. */
    *(volatile uint32_t *)(anchor_pc + 4) = MIPS_NOP;
    *(volatile uint32_t *)(anchor_pc + 0) = mips_j((uint32_t)w);
    mhfu_flush_caches();

    if (rec) {
        rec->installed    = 1;
        rec->anchor_pc    = anchor_pc;
        rec->saved_insn0  = insn0;
        rec->saved_insn1  = insn1;
        rec->wrapper_addr = (uint32_t)w;
    }
    return 0;
}

/* Generic prefix-trampoline on an arbitrary anchor PC (non-event). Used by the
 * inject module to hook get_subresource. Same wrapper shape as the event ones;
 * the dispatcher receives a0=&mhfu_anchor_regs_t. Install while JIT-cold (boot /
 * pre-quest). Records are kept here only so the cave wrapper survives. */
#define MHFU_EXTRA_TRAMPS 4
static install_record_t g_extra[MHFU_EXTRA_TRAMPS];
static int              g_extra_n;

extern "C" int mhfu_install_trampoline(uint32_t anchor_pc, uint32_t dispatcher)
{
    for (int i = 0; i < g_extra_n; i++)
        if (g_extra[i].installed && g_extra[i].anchor_pc == anchor_pc) return 0; /* idempotent */
    if (g_extra_n >= MHFU_EXTRA_TRAMPS) return -3;
    int rc = install_trampoline_for(anchor_pc, dispatcher, &g_extra[g_extra_n]);
    if (rc == 0) g_extra_n++;
    return rc;
}

extern "C" int mhfu_install_event_trampolines(void)
{
    const mhfu_region_addrs_t *r = mhfu_region();
    if (!r) return -1;

    int rc = install_trampoline_for(r->pc_quest_beginning,
                                    (uint32_t)&mhfu_dispatch_quest_beginning,
                                    &g_install[MHFU_EVENT_QUEST_BEGINNING]);
    if (rc != 0) return rc;
    rc = install_trampoline_for(r->pc_quest_entered,
                                (uint32_t)&mhfu_dispatch_quest_entered,
                                &g_install[MHFU_EVENT_QUEST_ENTERED]);
    return rc;
}

extern "C" void mhfu_uninstall_event_trampolines(void)
{
    for (int i = 0; i < MHFU_EVENT_COUNT_; i++) {
        install_record_t *rec = &g_install[i];
        if (!rec->installed) continue;
        *(volatile uint32_t *)(rec->anchor_pc + 0) = rec->saved_insn0;
        *(volatile uint32_t *)(rec->anchor_pc + 4) = rec->saved_insn1;
        rec->installed = 0;
    }
    mhfu_flush_caches();
}

/* --- install worker ----------------------------------------------------- */

/* Wait until the EBOOT is resident at both anchor PCs (the expected
 * Allegrex sv.q opcode 0x3E), install, then keep watching: a savestate
 * restore or later game-state event can revert the bytes, so re-install
 * if the anchor stops showing our `J` (opcode 0x02). */
extern "C" int mhfu_install_worker_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    const mhfu_region_addrs_t *r = mhfu_region();
    if (!r) return -1;
    mhfu_sentinel_set(0x10, 0xC0DE0001);

    for (int attempt = 0; attempt < 600; attempt++) {
        uint32_t w0 = *(volatile uint32_t *)(r->pc_quest_beginning);
        uint32_t w1 = *(volatile uint32_t *)(r->pc_quest_entered);
        if (((w0 >> 26) & 0x3F) == 0x3E && ((w1 >> 26) & 0x3F) == 0x3E) {
            sceKernelDelayThread(500 * 1000);
            w0 = *(volatile uint32_t *)(r->pc_quest_beginning);
            w1 = *(volatile uint32_t *)(r->pc_quest_entered);
            if (((w0 >> 26) & 0x3F) == 0x3E && ((w1 >> 26) & 0x3F) == 0x3E) {
                mhfu_sentinel_set(0x10, 0xC0DE0002);
                int rc = mhfu_install_event_trampolines();
                mhfu_sentinel_set(0x04, (uint32_t)rc);
                mhfu_sentinel_set(0x08, g_install[MHFU_EVENT_QUEST_BEGINNING].wrapper_addr);
                mhfu_sentinel_set(0x0C, *(volatile uint32_t *)(r->pc_quest_beginning));
                mhfu_sentinel_set(0x10, (rc == 0) ? 0xC0DE0003u : 0xC0DEDEAD);
                for (int re = 0; re < 1200; re++) {
                    sceKernelDelayThread(500 * 1000);
                    uint32_t a0 = *(volatile uint32_t *)(r->pc_quest_beginning);
                    uint32_t a1 = *(volatile uint32_t *)(r->pc_quest_entered);
                    if (((a0 >> 26) & 0x3F) != 0x02 || ((a1 >> 26) & 0x3F) != 0x02) {
                        mhfu_sentinel_set(0x10, 0xC0DE0004);
                        for (int i = 0; i < MHFU_EVENT_COUNT_; i++)
                            g_install[i].installed = 0;
                        mhfu_install_event_trampolines();
                        mhfu_sentinel_set(0x10, 0xC0DE0005);
                    }
                }
                return 0;
            }
        }
        sceKernelDelayThread(100 * 1000);
    }
    mhfu_sentinel_set(0x10, 0xC0DE0E0E);
    return 0;
}
