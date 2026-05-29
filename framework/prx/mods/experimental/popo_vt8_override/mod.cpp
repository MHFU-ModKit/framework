/*
 * popo_vt8_override (EXPERIMENTAL) — swap popo vtable[8] (anim
 * probability picker, 0x08865254) for a stub that calls the original
 * then forces $a1 = a chosen anim, so the dispatcher applies our anim.
 *
 * STATUS: the postfix stub below FROZE PPSSPP (PC=0) on section-1 load
 * in testing (Section 22 v2/v3). The working fix was a branchless
 * single-basic-block stub (MOVN, no beq) — see docs/SECTION_22_VT8_HOOK.md
 * and scripts/test_vt8_branchless.py. Kept as a structural reference for
 * the vtable-swap-via-hookmgr pattern. Off by default; do not ship.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"

#define MOD_ID          "popo_vt8_override"
#define VT8_SLOT        0x089BC580u   /* popo vtable[8] cell */
#define VT8_ORIGINAL    0x08865254u
#define DEFAULT_ANIM    1011u
#define STUB_INSNS      32
#define TARGET_AREA     99

static volatile uint32_t g_stub[STUB_INSNS];
static volatile uint32_t g_override_anim   = DEFAULT_ANIM;
static volatile uint32_t g_override_enable = 0;
static volatile uint32_t g_hit             = 0;
static volatile int      g_installed       = 0;

static void build_stub(void)
{
    uint32_t *s = (uint32_t *)g_stub;
    int i = 0;
    uint32_t hit = (uint32_t)(uintptr_t)&g_hit;
    uint32_t en  = (uint32_t)(uintptr_t)&g_override_enable;
    uint32_t an  = (uint32_t)(uintptr_t)&g_override_anim;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -32);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(hit >> 16));
    s[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(hit & 0xFFFF));
    s[i++] = mips_lw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_T1, 1);
    s[i++] = mips_sw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_jal(VT8_ORIGINAL);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(en >> 16));
    s[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(en & 0xFFFF));
    s[i++] = mips_lw(MIPS_REG_T2, 0, MIPS_REG_T0);
    s[i++] = mips_beq(MIPS_REG_T2, MIPS_REG_ZERO, 7);
    s[i++] = MIPS_NOP;
    s[i++] = mips_beq(MIPS_REG_A1, MIPS_REG_ZERO, 4);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(an >> 16));
    s[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(an & 0xFFFF));
    s[i++] = mips_lw(MIPS_REG_A1, 0, MIPS_REG_T0);
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 32);
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
            if (mhfu_hook_vtable(VT8_SLOT, (uint32_t)(uintptr_t)g_stub, MOD_ID) == MHFU_HOOK_OK)
                g_installed = 1;
        }
        g_override_enable = should ? 1u : 0u;
    }
    return 0;
}

static int vt8_init(void)
{
    build_stub();
    mhfu_flush_caches();
    SceUID th = sceKernelCreateThread(MOD_ID, (SceKernelThreadEntry)worker,
                                      0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

static void vt8_shutdown(void) { g_override_enable = 0; }  /* vtable restored by framework */

MHFU_MOD(.id = MOD_ID, .version = "0.3-experimental",
         .needs = 0, .conflicts = "popo_vt5_freeze popo_heading_hook",
         .init = vt8_init, .shutdown = vt8_shutdown);
