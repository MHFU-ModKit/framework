/*
 * popo_vt5_freeze (EXPERIMENTAL) — swap popo vtable[5] (per-frame tick,
 * ObjBase::vtable_0x14 base 0x08864634) for a save/restore stub that
 * snapshots the heading vec before the original runs and restores it
 * after, cancelling the engine's per-frame heading drift.
 *
 * STATUS: byte-level correct (heading freeze verified) but HALTS popos —
 * the 60 Hz per-frame write rate trips the AI's halt regime (Section 23).
 * Reference for the vtable-wrap save/restore pattern. Off by default.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"

#define MOD_ID       "popo_vt5_freeze"
#define VT5_SLOT     0x089BC574u   /* popo vtable[5] cell */
#define VT5_ORIGINAL 0x08864634u
#define STUB_INSNS   48
#define TARGET_AREA  99

static volatile uint32_t g_stub[STUB_INSNS];
static volatile uint32_t g_freeze_enable = 0;
static volatile uint32_t g_hit           = 0;
static volatile int      g_installed     = 0;

static void build_stub(void)
{
    uint32_t *s = (uint32_t *)g_stub;
    int i = 0;
    uint32_t hit = (uint32_t)(uintptr_t)&g_hit;
    uint32_t en  = (uint32_t)(uintptr_t)&g_freeze_enable;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -64);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S0, 0x1C, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S1, 0x20, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S2, 0x24, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S3, 0x28, MIPS_REG_SP);
    s[i++] = mips_move(MIPS_REG_S0, MIPS_REG_A0);
    s[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(hit >> 16));
    s[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(hit & 0xFFFF));
    s[i++] = mips_lw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_T1, 1);
    s[i++] = mips_sw(MIPS_REG_T1, 0, MIPS_REG_T0);
    /* snapshot heading +0x10/+0x14/+0x18 */
    s[i++] = mips_lw(MIPS_REG_S1, 0x10, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_S2, 0x14, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_S3, 0x18, MIPS_REG_S0);
    s[i++] = mips_jal(VT5_ORIGINAL);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(en >> 16));
    s[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(en & 0xFFFF));
    s[i++] = mips_lw(MIPS_REG_T8, 0, MIPS_REG_T0);
    /* branchless restore: if enable != 0, write saved back */
    s[i++] = mips_lw(MIPS_REG_T1, 0x10, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S1, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x10, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_T1, 0x14, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S2, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x14, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_T1, 0x18, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S3, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x18, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_S3, 0x28, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S2, 0x24, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S1, 0x20, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S0, 0x1C, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 64);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
    while (i < STUB_INSNS) s[i++] = MIPS_NOP;
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(333 * 1000);
        int should = (mhfu_get_area_index() == TARGET_AREA);
        if (should && !g_installed) {
            if (mhfu_hook_vtable(VT5_SLOT, (uint32_t)(uintptr_t)g_stub, MOD_ID) == MHFU_HOOK_OK)
                g_installed = 1;
        }
        g_freeze_enable = should ? 1u : 0u;
    }
    return 0;
}

static int vt5_init(void)
{
    build_stub();
    mhfu_flush_caches();
    SceUID th = sceKernelCreateThread(MOD_ID, (SceKernelThreadEntry)worker,
                                      0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

static void vt5_shutdown(void) { g_freeze_enable = 0; }

MHFU_MOD(.id = MOD_ID, .version = "0.1-experimental",
         .needs = 0, .conflicts = "popo_vt8_override popo_heading_hook",
         .init = vt5_init, .shutdown = vt5_shutdown);
