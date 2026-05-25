/*
 * mhfu_framework.prx — runtime loader + dispatcher.
 *
 * This PRX is loaded by PPSSPP (or real PSP CFW) from
 * <memstick>/PSP/PLUGINS/mhfu_framework/. On module_start it:
 *
 *   1. Reads the running game's UMD ID, picks the matching
 *      mhfu_region_addrs_t from mhfu_framework_addresses.h, bails if
 *      the game isn't MHFU (TODO: region detection is stubbed; for
 *      now we default to EU).
 *
 *   2. Walks PSP/PLUGINS/mhfu_framework/mods/ and loads every *.prx
 *      (TODO — for now mod-PRXes load via PPSSPP's normal plugin
 *      mechanism and register against us at their module_start).
 *
 *   3. For each known event, installs a MIPS trampoline at the
 *      anchor PC, redirecting through a code cave that calls our
 *      C-side dispatcher. The dispatcher fans out to every callback
 *      registered against that event.
 *
 * The trampoline layout, built into the cave by install_trampoline_for():
 *
 *   wrapper:
 *     addiu  $sp, $sp, -0x40           ; reserve a 64-byte scratch frame
 *     sw     $a0,  0x00($sp)           ; spill a0..a3, v0, v1, ra, sp, pc
 *     sw     $a1,  0x04($sp)           ;   into 9 contiguous u32s — this
 *     sw     $a2,  0x08($sp)           ;   is the on-stack layout of
 *     sw     $a3,  0x0C($sp)           ;   mhfu_anchor_regs_t
 *     sw     $v0,  0x10($sp)
 *     sw     $v1,  0x14($sp)
 *     sw     $ra,  0x18($sp)           ; ra at hook moment
 *     addiu  $t0,  $sp, 0x40
 *     sw     $t0,  0x1C($sp)           ; original sp = current sp + frame
 *     lui    $t0,  HI16(anchor_pc)
 *     ori    $t0,  $t0, LO16(anchor_pc)
 *     sw     $t0,  0x20($sp)           ; pc
 *
 *     sw     $ra,  0x30($sp)           ; preserve the wrapper's $ra too
 *
 *     move   $a0,  $sp                 ; dispatcher arg: &anchor_regs
 *     jal    dispatcher                ; →  mhfu_dispatch_*
 *     nop                              ;   delay slot
 *
 *     lw     $a0,  0x00($sp)           ; restore caller regs
 *     lw     $a1,  0x04($sp)
 *     lw     $a2,  0x08($sp)
 *     lw     $a3,  0x0C($sp)
 *     lw     $v0,  0x10($sp)
 *     lw     $v1,  0x14($sp)
 *     lw     $ra,  0x30($sp)
 *     addiu  $sp,  $sp, 0x40           ; tear down scratch frame
 *
 *     <displaced insn0>                ; original anchor[0]
 *     <displaced insn1>                ; original anchor[1]
 *
 *     j      anchor_pc + 8             ; resume at the instruction after
 *     nop                              ;   the two we displaced
 *
 *   Plus padding to round up to a whole multiple of 4. Total: 30 insns,
 *   120 bytes per wrapper.
 *
 * Delay-slot caveat:
 *   The two displaced instructions are copied verbatim. If anchor[0] or
 *   anchor[1] is itself a control-transfer (j/jal/branch), the wrapper
 *   misbehaves — the displaced instruction's delay slot relationship is
 *   broken. For our quest_beginning + quest_entered anchors, both PCs
 *   point at `sw` instructions (plain stores), so this is fine. The
 *   framework documents which anchor PCs are vetted; new event anchors
 *   must be checked.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspsysmem.h>
#include <pspthreadman.h>
#include <pspmodulemgr.h>
#include <psputils.h>      /* sceKernelDcacheWritebackInvalidateAll, sceKernelIcacheInvalidateAll */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "mhfu_framework.h"
#include "mhfu_framework_addresses.h"
#include "mhfu_internal.h"
#include "mips_encoder.h"

#define MAX_CBS_PER_EVENT 16
#define MOD_NAME          "mhfu_framework"

/* Wrapper size in instructions; must match the layout in install_trampoline_for(). */
#define WRAPPER_INSNS     54        /* 49 used + 5 NOP pad */
#define WRAPPER_BYTES     (WRAPPER_INSNS * 4)
#define CAVE_SLOTS        8        /* room for up to 8 wrappers */

/* User-mode module (PSP_MODULE_USER = 0).  PPSSPP's plugin host is
 * user-mode only; the 0x1000 kernel-mode bit causes "unsupported
 * thread attributes 0x07" warnings and prevents main() from running.  */
PSP_MODULE_INFO(MOD_NAME, 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

/* Leave crt0_prx's default in place: _start (= module_start) spawns
 * a thread for main(), then returns. This is critical for PPSSPP
 * plugin compatibility — if main() runs synchronously in the
 * module_start thread, our idle loop blocks sceKernelStartModule
 * indefinitely and PPSSPP can't boot the game (black screen).
 *
 * The earlier "thread spawn hangs silently" symptom I observed was
 * actually due to the module being in kernel mode (attr 0x1007); once
 * we switched to user mode (attr 0), the default crt0 thread-spawn
 * works correctly and main() runs in its own thread. */

/* ------------------------------------------------------------------------ *
 * Per-event callback lists.
 * ------------------------------------------------------------------------ */

static mhfu_event_cb_t g_cbs[MHFU_EVENT_COUNT_][MAX_CBS_PER_EVENT];
static int             g_n_cbs[MHFU_EVENT_COUNT_];

static const mhfu_region_addrs_t *g_addrs = NULL;

/* ------------------------------------------------------------------------ *
 * Code cave.
 *
 * Allocating in BSS gives us zero-initialized writable memory at a stable
 * address (the linker assigns it). PSP user RAM is RWX so we can also
 * execute these bytes; the only ceremony required is cache flush after
 * each write (see install_trampoline_for).
 *
 * Aligned to 64 bytes so the I-cache line containing a wrapper start
 * is the only one we have to invalidate per install.
 * ------------------------------------------------------------------------ */

__attribute__((aligned(64)))
static uint32_t g_code_cave[CAVE_SLOTS * WRAPPER_INSNS];

/* Per-event installation bookkeeping (needed for uninstall). */
typedef struct {
    int      installed;
    uint32_t anchor_pc;
    uint32_t saved_insn0;
    uint32_t saved_insn1;
    uint32_t wrapper_addr;
} install_record_t;

static install_record_t g_install[MHFU_EVENT_COUNT_];

/* ------------------------------------------------------------------------ *
 * Registration.
 * ------------------------------------------------------------------------ */

int mhfu_register_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb)
{
    if (event_id < 0 || event_id >= MHFU_EVENT_COUNT_) return -1;
    if (!cb) return -2;
    int *n = &g_n_cbs[event_id];
    if (*n >= MAX_CBS_PER_EVENT) return -3;
    g_cbs[event_id][(*n)++] = cb;
    return 0;
}

int mhfu_unregister_event(mhfu_event_id_t event_id, mhfu_event_cb_t cb)
{
    if (event_id < 0 || event_id >= MHFU_EVENT_COUNT_) return -1;
    mhfu_event_cb_t *list = g_cbs[event_id];
    int *n = &g_n_cbs[event_id];
    for (int i = 0; i < *n; i++) {
        if (list[i] == cb) {
            for (int j = i; j < *n - 1; j++) list[j] = list[j + 1];
            (*n)--;
            return 0;
        }
    }
    return -4;
}

/* ------------------------------------------------------------------------ *
 * Dispatchers — these are what the wrapper trampolines call.
 *
 * Each takes a pointer to the on-stack anchor_regs the wrapper built;
 * builds a public mhfu_event_ctx_t; walks the callback list.
 * ------------------------------------------------------------------------ */

/* Per-event last-seen cell values, so the dispatchers only fire on
 * the meaningful transition rather than on every instruction
 * execution. The instruction at our anchor PC runs every frame
 * (writes the cell whether or not the value changes); we want
 * mhfu_on_quest_beginning to fire ONCE when the timer goes 0 → 90000,
 * not 60 times per second.
 *
 * BSS-zero-initialised — the first poll after game boot will see
 * "transition from 0 to <whatever>" which is wrong only on the very
 * first frame; for quest_timer that's fine because 0 means "no quest
 * active" so no event fires.
 */
static uint32_t g_last_quest_timer = 0;
static uint16_t g_last_area_index  = 0;

void mhfu_dispatch_quest_beginning(const mhfu_anchor_regs_t *regs)
{
    uint32_t cur  = mhfu_get_quest_timer();
    uint32_t prev = g_last_quest_timer;
    g_last_quest_timer = cur;
    /* Quest-start signal: timer transitions FROM zero TO non-zero. */
    if (prev != 0 || cur == 0) return;

    mhfu_event_ctx_t ctx;
    ctx.event_id   = MHFU_EVENT_QUEST_BEGINNING;
    ctx.cell_value = cur;
    ctx.a0 = regs->a0; ctx.a1 = regs->a1; ctx.a2 = regs->a2; ctx.a3 = regs->a3;
    ctx.v0 = regs->v0; ctx.v1 = regs->v1;
    ctx.ra = regs->ra; ctx.sp = regs->sp; ctx.pc = regs->pc;
    for (int i = 0; i < g_n_cbs[MHFU_EVENT_QUEST_BEGINNING]; i++)
        g_cbs[MHFU_EVENT_QUEST_BEGINNING][i](&ctx);
}

void mhfu_dispatch_quest_entered(const mhfu_anchor_regs_t *regs)
{
    uint16_t cur  = mhfu_get_area_index();
    uint16_t prev = g_last_area_index;
    g_last_area_index = cur;
    /* Quest-entered signal: area_index transitions TO 98 (in-area). */
    if (cur != 98 || prev == 98) return;

    mhfu_event_ctx_t ctx;
    ctx.event_id   = MHFU_EVENT_QUEST_ENTERED;
    ctx.cell_value = cur;
    ctx.a0 = regs->a0; ctx.a1 = regs->a1; ctx.a2 = regs->a2; ctx.a3 = regs->a3;
    ctx.v0 = regs->v0; ctx.v1 = regs->v1;
    ctx.ra = regs->ra; ctx.sp = regs->sp; ctx.pc = regs->pc;
    for (int i = 0; i < g_n_cbs[MHFU_EVENT_QUEST_ENTERED]; i++)
        g_cbs[MHFU_EVENT_QUEST_ENTERED][i](&ctx);
}

/* ------------------------------------------------------------------------ *
 * Memory helpers — pointer reads, no debugger needed (we ARE the game).
 * ------------------------------------------------------------------------ */

uint8_t  mhfu_get_screen_state(void) { return g_addrs ? *(volatile uint8_t  *)g_addrs->cell_screen_state      : 0; }
uint16_t mhfu_get_area_index  (void) { return g_addrs ? *(volatile uint16_t *)g_addrs->cell_area_index        : 0; }
uint32_t mhfu_get_quest_timer (void) { return g_addrs ? *(volatile uint32_t *)g_addrs->cell_quest_timer       : 0; }
uint32_t mhfu_get_player_hp   (void) { return g_addrs ? *(volatile uint32_t *)g_addrs->cell_player_hp         : 0; }
uint16_t mhfu_get_sharpness_current(void) { return g_addrs ? *(volatile uint16_t *)g_addrs->cell_sharpness_current : 0; }

uint8_t  mhfu_read_u8 (uint32_t a) { return *(volatile uint8_t  *)a; }
uint16_t mhfu_read_u16(uint32_t a) { return *(volatile uint16_t *)a; }
uint32_t mhfu_read_u32(uint32_t a) { return *(volatile uint32_t *)a; }
void     mhfu_write_u8 (uint32_t a, uint8_t  v) { *(volatile uint8_t  *)a = v; }
void     mhfu_write_u16(uint32_t a, uint16_t v) { *(volatile uint16_t *)a = v; }
void     mhfu_write_u32(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ------------------------------------------------------------------------ *
 * Logging.
 * ------------------------------------------------------------------------ */

static SceUID g_log_fd = -1;

void mhfu_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (g_log_fd < 0) {
        g_log_fd = sceIoOpen(
            "ms0:/PSP/PLUGINS/mhfu_framework/framework.log",
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    }
    if (g_log_fd >= 0) {
        sceIoWrite(g_log_fd, buf, (size_t)n);
        sceIoWrite(g_log_fd, "\n", 1);
    }
}

/* ------------------------------------------------------------------------ *
 * Trampoline installer — the work that makes hooks actually fire.
 *
 * Per-event: pick a cave slot, build the wrapper into it, patch the
 * anchor PC with `J cave; NOP`, flush the dcache + icache so the CPU
 * picks up the new bytes.
 * ------------------------------------------------------------------------ */

static uint32_t * cave_slot_addr(int slot_index)
{
    return &g_code_cave[slot_index * WRAPPER_INSNS];
}

static int install_trampoline_for(
    int             slot_index,
    uint32_t        anchor_pc,
    uint32_t        dispatcher_addr,
    install_record_t *rec)
{
    if (anchor_pc & 0x3) return -1;
    if (slot_index < 0 || slot_index >= CAVE_SLOTS) return -2;

    /* Snapshot the two instructions we're about to displace, so
     * uninstall can restore them. */
    uint32_t insn0 = *(volatile uint32_t *)(anchor_pc + 0);
    uint32_t insn1 = *(volatile uint32_t *)(anchor_pc + 4);

    uint32_t *w = cave_slot_addr(slot_index);
    int i = 0;

    /* Prologue — reserve 0x80 scratch frame. We must save every
     * caller-saved GPR before calling our C dispatcher, otherwise the
     * dispatcher's compiled code clobbers $at, $t0..$t9 and the game's
     * surrounding code (which expects them to survive the original
     * `sv.q` store) gets corrupted state. Earlier wrappers saved only
     * $a0..$a3 + $v0..$v1 + $ra; the resulting black-screen lockup was
     * the symptom of $t0..$t9 being garbage on return.
     *
     * Stack layout (matches mhfu_anchor_regs_t for the first 9 slots):
     *   +0x00 a0      +0x14 v1     +0x28 at
     *   +0x04 a1      +0x18 ra     +0x2C t0
     *   +0x08 a2      +0x1C sp     +0x30 t1   ...   +0x50 t9
     *   +0x0C a3      +0x20 pc
     *   +0x10 v0      +0x24 (pad)
     */
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x80);
    w[i++] = mips_sw(MIPS_REG_A0, 0x00, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A1, 0x04, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_V0, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_V1, 0x14, MIPS_REG_SP);
    w[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);

    /* Save $at and $t0..$t9 BEFORE we use any of them as scratch. */
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

    /* Now safe to use $t0 as scratch — its original value is on the stack. */
    w[i++] = mips_addiu(MIPS_REG_T0, MIPS_REG_SP, 0x80);
    w[i++] = mips_sw(MIPS_REG_T0, 0x1C, MIPS_REG_SP);

    w[i++] = mips_lui(MIPS_REG_T0, (uint16_t)(anchor_pc >> 16));
    w[i++] = mips_ori(MIPS_REG_T0, MIPS_REG_T0, (uint16_t)(anchor_pc & 0xFFFF));
    w[i++] = mips_sw(MIPS_REG_T0, 0x20, MIPS_REG_SP);

    /* Call dispatcher with $a0 = &anchor_regs (which is just $sp). */
    w[i++] = mips_move(MIPS_REG_A0, MIPS_REG_SP);
    w[i++] = mips_jal(dispatcher_addr);
    w[i++] = MIPS_NOP;   /* delay slot of jal */

    /* Restore everything we saved, in any order. */
    w[i++] = mips_lw(MIPS_REG_A0, 0x00, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A1, 0x04, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_V0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_V1, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);   /* GAME's ra */
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

    /* The two displaced instructions, then jump back. */
    w[i++] = insn0;
    w[i++] = insn1;
    w[i++] = mips_j(anchor_pc + 8);
    w[i++] = MIPS_NOP;   /* delay slot of j */

    /* Pad to WRAPPER_INSNS with NOPs (defensive — easier to read in a
     * disassembler if mod authors ever look). */
    while (i < WRAPPER_INSNS) w[i++] = MIPS_NOP;

    /* Patch the anchor: J cave; NOP. Write the delay-slot NOP first so
     * an interrupted install leaves the worst case as "insn1 became
     * NOP" rather than "J went to wherever, but the delay slot still
     * ran the old insn1". */
    *(volatile uint32_t *)(anchor_pc + 4) = MIPS_NOP;
    *(volatile uint32_t *)(anchor_pc + 0) = mips_j((uint32_t)w);

    /* Flush the dcache so the writes land in real RAM, then invalidate
     * the icache so the CPU sees the new code. The PSP exposes both;
     * PPSSPP intercepts both as the cue to re-translate. */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();

    if (rec) {
        rec->installed    = 1;
        rec->anchor_pc    = anchor_pc;
        rec->saved_insn0  = insn0;
        rec->saved_insn1  = insn1;
        rec->wrapper_addr = (uint32_t)w;
    }
    return 0;
}

static int uninstall_trampoline_for(install_record_t *rec)
{
    if (!rec || !rec->installed) return 0;
    *(volatile uint32_t *)(rec->anchor_pc + 0) = rec->saved_insn0;
    *(volatile uint32_t *)(rec->anchor_pc + 4) = rec->saved_insn1;
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    rec->installed = 0;
    return 0;
}

static int install_trampolines(void)
{
    if (!g_addrs) return -1;

    int rc;
    rc = install_trampoline_for(
        /*slot=*/0,
        g_addrs->pc_quest_beginning,
        (uint32_t)&mhfu_dispatch_quest_beginning,
        &g_install[MHFU_EVENT_QUEST_BEGINNING]);
    if (rc != 0) {
        mhfu_log("[framework] install_trampoline_for quest_beginning: %d", rc);
        return rc;
    }
    mhfu_log("[framework] hook installed: quest_beginning @ 0x%08lx (cave 0x%08lx)",
             (unsigned long)g_addrs->pc_quest_beginning,
             (unsigned long)g_install[MHFU_EVENT_QUEST_BEGINNING].wrapper_addr);

    rc = install_trampoline_for(
        /*slot=*/1,
        g_addrs->pc_quest_entered,
        (uint32_t)&mhfu_dispatch_quest_entered,
        &g_install[MHFU_EVENT_QUEST_ENTERED]);
    if (rc != 0) {
        mhfu_log("[framework] install_trampoline_for quest_entered: %d", rc);
        return rc;
    }
    mhfu_log("[framework] hook installed: quest_entered @ 0x%08lx (cave 0x%08lx)",
             (unsigned long)g_addrs->pc_quest_entered,
             (unsigned long)g_install[MHFU_EVENT_QUEST_ENTERED].wrapper_addr);

    return 0;
}

static int uninstall_trampolines(void)
{
    for (int i = 0; i < MHFU_EVENT_COUNT_; i++)
        uninstall_trampoline_for(&g_install[i]);
    return 0;
}

/* ------------------------------------------------------------------------ *
 * Mod loading.
 * ------------------------------------------------------------------------ */

static int load_mods(void)
{
    /* TODO: enumerate ms0:/PSP/PLUGINS/mhfu_framework/mods and
     * sceKernelLoadModule + sceKernelStartModule each .prx found.
     * For now, mods load via PPSSPP's normal plugin mechanism and
     * register against us via their own module_start. */
    return 0;
}

/* ------------------------------------------------------------------------ *
 * Region detection.
 * ------------------------------------------------------------------------ */

static int detect_region(void)
{
    /* TODO: read sceKernelGetGameInfo / sceUtilityGetSystemParamString
     * and select the matching region table. For now: default to EU. */
    g_addrs = &MHFU_REGION_EU;
    mhfu_log("[framework] region detection stubbed; defaulting to EU (%s)",
             g_addrs->game_id);
    return 0;
}

/* ------------------------------------------------------------------------ *
 * Module entry / exit.
 *
 * Modern pspsdk's crt0_prx.o supplies a default `module_start` that
 * calls `main()`, so user PRXes provide `main` (not `module_start`).
 * `module_stop` is still ours to define — it's not in crt0.
 * ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ *
 * Built-in demo callbacks (will later live in a separate hello_world.prx
 * once we wire up the PRX-import stub-library generation).
 *
 * These exist so the FIRST build of the framework PRX can demonstrate
 * the trampoline path end-to-end without also needing inter-PRX
 * symbol resolution.
 * ------------------------------------------------------------------------ */

/* Sentinel layout for callback firing:
 *   +0x18  QB_FIRED count  (increments each call)
 *   +0x1C  QE_FIRED count
 *   +0x20  QB_LAST_A0      (last $a0 the wrapper captured)
 *   +0x24  QE_LAST_A0
 *
 * MHFU_SENTINEL_BASE and sentinel_set are defined further down (near
 * main); forward-declare them so the demo callbacks compile.
 */
#define MHFU_SENTINEL_BASE 0x08AEFFE0u
static void sentinel_set(uint32_t offset, uint32_t value);

static void demo_on_quest_beginning(const mhfu_event_ctx_t *ctx)
{
    uint32_t n = *(volatile uint32_t *)(MHFU_SENTINEL_BASE + 0x18) + 1;
    sentinel_set(0x18, n);
    sentinel_set(0x20, ctx->a0);
    mhfu_log("[demo] * QUEST BEGINNING (built-in)");
}

static void demo_on_quest_entered(const mhfu_event_ctx_t *ctx)
{
    uint32_t n = *(volatile uint32_t *)(MHFU_SENTINEL_BASE + 0x1C) + 1;
    sentinel_set(0x1C, n);
    sentinel_set(0x24, ctx->a0);
    mhfu_log("[demo] * QUEST ENTERED");
}

/* Sentinel cells in a known-quiet RAM region so we can verify
 * end-to-end execution from a host debugger even when log file I/O
 * NIDs are unsupported by PPSSPP. Layout: four u32s starting at
 * 0x08AEFFE0 (end of the live-framework code cave, untouched by
 * the trampoline slots).
 *
 *   +0x00  STAGE       — high-water "what main() got to"
 *   +0x04  RC_INSTALL  — return code of install_trampolines()
 *   +0x08  CAVE_ADDR   — &g_code_cave[0] (where wrappers live)
 *   +0x0C  ANCHOR_RD   — value the framework read from anchor_pc
 *                          right after patching (for cross-check)
 */
static void sentinel_set(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(MHFU_SENTINEL_BASE + offset) = value;
}

/* Worker thread: poll until the game's EBOOT is resident at the anchor
 * PCs (Allegrex vector-store `sv.q` at op 0x3E), then install. Without
 * this delay, our patches land on uninitialised RAM and get overwritten
 * when the game's EBOOT loads (or when the user restores a savestate). */
static int install_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    sentinel_set(0x10, 0xC0DE0001);   /* worker started */
    /* Phase 1: wait until BOTH anchor PCs show the expected original
     * sv.q opcode (0x3E). Empirically the EBOOT's code section loads
     * in waves on PPSSPP — first quest_entered's region, then
     * quest_beginning's. Patching too early causes the game's later
     * load to overwrite slot 0. */
    for (int attempt = 0; attempt < 600; attempt++) {
        uint32_t w0 = *(volatile uint32_t *)(g_addrs->pc_quest_beginning);
        uint32_t w1 = *(volatile uint32_t *)(g_addrs->pc_quest_entered);
        if (((w0 >> 26) & 0x3F) == 0x3E && ((w1 >> 26) & 0x3F) == 0x3E) {
            /* Both regions loaded — wait a bit more for stability,
             * then re-check, then patch. */
            sceKernelDelayThread(500 * 1000);
            w0 = *(volatile uint32_t *)(g_addrs->pc_quest_beginning);
            w1 = *(volatile uint32_t *)(g_addrs->pc_quest_entered);
            if (((w0 >> 26) & 0x3F) == 0x3E && ((w1 >> 26) & 0x3F) == 0x3E) {
                sentinel_set(0x10, 0xC0DE0002);
                int rc = install_trampolines();
                sentinel_set(0x04, (uint32_t)rc);
                sentinel_set(0x08, (uint32_t)&g_code_cave[0]);
                sentinel_set(0x0C, *(volatile uint32_t *)(g_addrs->pc_quest_beginning));
                sentinel_set(0x10, (rc == 0) ? 0xC0DE0003u : 0xC0DEDEAD);
                /* Phase 2: keep watching — if either anchor gets
                 * un-patched by a later game-state event, re-install. */
                for (int re = 0; re < 1200; re++) {
                    sceKernelDelayThread(500 * 1000);
                    uint32_t a0 = *(volatile uint32_t *)(g_addrs->pc_quest_beginning);
                    uint32_t a1 = *(volatile uint32_t *)(g_addrs->pc_quest_entered);
                    if (((a0 >> 26) & 0x3F) != 0x02 ||
                        ((a1 >> 26) & 0x3F) != 0x02) {
                        sentinel_set(0x10, 0xC0DE0004);   /* re-installing */
                        /* uninstall the stale records first so install
                         * snapshots the new originals correctly. */
                        for (int i = 0; i < MHFU_EVENT_COUNT_; i++)
                            g_install[i].installed = 0;
                        install_trampolines();
                        sentinel_set(0x10, 0xC0DE0005);
                    }
                }
                return 0;
            }
        }
        sceKernelDelayThread(100 * 1000);
    }
    sentinel_set(0x10, 0xC0DE0E0E);
    return 0;
}

/* Diagnostic levels (set MHFU_DIAG_LEVEL to control what main() does):
 *   0 — main() returns 0 immediately. Nothing else.
 *   1 — call detect_region + register callbacks, then return.
 *   2 — full normal flow but no patching. Idle loop.
 *   3 — full normal flow WITH patching.
 */
#define MHFU_DIAG_LEVEL 3

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
#if MHFU_DIAG_LEVEL == 0
    /* Minimal plugin: just exit. If the game still black-screens here,
     * it's a PPSSPP-vs-MHFU plugin issue, not anything in our code. */
    return 0;
#else
    sentinel_set(0x00, 0xCAFE0001);
    mhfu_log("[framework] %s starting (diag=%d)", MOD_NAME, MHFU_DIAG_LEVEL);
    detect_region();
    sentinel_set(0x00, 0xCAFE0002);
    mhfu_register_event(MHFU_EVENT_QUEST_BEGINNING, demo_on_quest_beginning);
    mhfu_register_event(MHFU_EVENT_QUEST_ENTERED,   demo_on_quest_entered);
    load_mods();
    sentinel_set(0x00, 0xCAFE0004);
    mhfu_log("[framework] ready");
#if MHFU_DIAG_LEVEL == 1
    return 0;
#else
    if (MHFU_DIAG_LEVEL >= 3) install_worker(0, NULL);
    for (;;) sceKernelDelayThread(1000 * 1000);
    return 0;
#endif
#endif
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uninstall_trampolines();
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
    return 0;
}
