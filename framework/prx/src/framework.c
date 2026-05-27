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

/* The anchor PC 0x0884CDFC runs every frame and writes area_index. We
 * keep a software change-filter so dispatchers fire only on the real
 * transitions. From this single anchor we drive BOTH:
 *   • mhfu_on_quest_entered      — fires once when area_index → 98
 *   • mhfu_on_map_section_entered — fires on every area_index transition
 * Mod authors filter by section_id inside their callback if they only
 * want a specific section. */
void mhfu_dispatch_quest_entered(const mhfu_anchor_regs_t *regs)
{
    uint16_t cur  = mhfu_get_area_index();
    uint16_t prev = g_last_area_index;
    if (cur == prev) return;
    g_last_area_index = cur;

    /* 1) Map-section change — fires on EVERY transition. */
    {
        mhfu_map_section_ctx_t mctx;
        mctx.event_id        = MHFU_EVENT_MAP_SECTION_ENTERED;
        mctx.section_id      = cur;
        mctx.prev_section_id = prev;
        mctx.quest_timer     = mhfu_get_quest_timer();
        mctx.screen_state    = mhfu_get_screen_state();
        mctx.is_in_quest_area = (mctx.screen_state == 17) ? 1 : 0;
        mctx._pad            = 0;
        for (int i = 0; i < g_n_cbs[MHFU_EVENT_MAP_SECTION_ENTERED]; i++)
            g_cbs[MHFU_EVENT_MAP_SECTION_ENTERED][i](&mctx);
    }

    /* 2) Quest-entered — only when transitioning TO 98 (in-area). */
    if (cur != 98) return;

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
 * Monster-spawn polling.
 *
 * No anchor PC for the entity-registry slot writes — PPSSPP's mem-BP
 * coverage doesn't catch them and we haven't found a single writer
 * instruction. The PRX runs ON the emulated PSP though, so polling
 * the registry every ~200 ms is cheap and reliable.
 *
 * Slot 0 is the player (skipped). Slots 1..20 are monsters; a slot
 * transitioning from 0 → non-zero is a fresh spawn. We remember every
 * ptr we've dispatched against so a stable ptr doesn't fire twice;
 * resetting when the slot returns to 0 lets the next spawn fire.
 * ------------------------------------------------------------------------ */

#define ENTITY_REGISTRY_ADDR  0x09C1213Cu
#define ENTITY_REGISTRY_SLOTS 21
#define ENTITY_OFF_SIZE_SCALE 0x024
#define ENTITY_OFF_ENTITY_ID  0x1E4
#define ENTITY_OFF_MONSTER_TY 0x1E8
#define ENTITY_OFF_HP         0x2E4

static uint32_t g_last_entity_ptrs[ENTITY_REGISTRY_SLOTS];

static void mhfu_dispatch_monster_spawned(int slot, uint32_t entity_ptr)
{
    if (g_n_cbs[MHFU_EVENT_MONSTER_SPAWNED] == 0) return;
    mhfu_monster_spawn_ctx_t ctx;
    ctx.event_id    = MHFU_EVENT_MONSTER_SPAWNED;
    ctx.slot        = slot;
    ctx.entity_ptr  = entity_ptr;
    ctx.monster_type = *(volatile uint8_t  *)(entity_ptr + ENTITY_OFF_MONSTER_TY);
    ctx.entity_id    = *(volatile uint8_t  *)(entity_ptr + ENTITY_OFF_ENTITY_ID);
    ctx.hp           = *(volatile uint16_t *)(entity_ptr + ENTITY_OFF_HP);
    uint32_t size_raw = *(volatile uint32_t *)(entity_ptr + ENTITY_OFF_SIZE_SCALE);
    /* type-pun via union to avoid strict-aliasing warnings */
    union { uint32_t u; float f; } cvt; cvt.u = size_raw; ctx.size_scale = cvt.f;
    for (int i = 0; i < g_n_cbs[MHFU_EVENT_MONSTER_SPAWNED]; i++)
        g_cbs[MHFU_EVENT_MONSTER_SPAWNED][i](&ctx);
}

static int monster_spawn_poll_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        /* 5 Hz */
        sceKernelDelayThread(200 * 1000);
        for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
            uint32_t cur = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
            uint32_t prev = g_last_entity_ptrs[slot];
            if (cur == 0) {
                /* Slot cleared — reset so the next reuse fires. */
                if (prev != 0) g_last_entity_ptrs[slot] = 0;
                continue;
            }
            if (cur == prev) continue;
            /* Sanity-check the pointer before dereferencing. */
            if (cur < 0x08000000u || cur > 0x0A000000u) continue;
            g_last_entity_ptrs[slot] = cur;
            mhfu_dispatch_monster_spawned(slot, cur);
        }
    }
    return 0;
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

static void demo_on_map_section_entered(const mhfu_map_section_ctx_t *ctx)
{
    mhfu_log("[demo] * MAP SECTION ENTERED  section=%u (prev=%u)  qtimer=%u",
             (unsigned)ctx->section_id,
             (unsigned)ctx->prev_section_id,
             (unsigned)ctx->quest_timer);
}

static void demo_on_monster_spawned(const mhfu_monster_spawn_ctx_t *ctx)
{
    /* size_scale is float; printf %f tends to be unreliable in PSP
     * crt0 builds — log the raw u32 instead so the value survives. */
    union { uint32_t u; float f; } cvt; cvt.f = ctx->size_scale;
    mhfu_log("[demo] * MONSTER SPAWNED  slot=%d  ptr=0x%08lx  type=0x%02x  hp=%u  size_raw=0x%08lx",
             ctx->slot, (unsigned long)ctx->entity_ptr,
             (unsigned)ctx->monster_type, (unsigned)ctx->hp,
             (unsigned long)cvt.u);
}

/* ------------------------------------------------------------------------ *
 * Embedded popo_growth mod.
 *
 * The standalone framework/prx/mods/popo_growth_prx/ builds a working
 * .prx but PPSSPP's plugin host hangs MHFU at boot when two plugin
 * PRXes are co-loaded (verified empirically 2026-05-25 — framework
 * alone boots in ~12s to menu; framework + popo_growth wedges screen
 * state at 0 forever, even with the popo mod degenerated to a no-op
 * register-and-return). The standalone mod source remains the canonical
 * reference for the live-framework / future-stable-stub path; for the
 * built-in build we inline the same logic here, behind
 * MHFU_EMBED_POPO_GROWTH so it can be toggled.
 * ------------------------------------------------------------------------ */

#define MHFU_EMBED_POPO_GROWTH 0
#define MHFU_EMBED_POPO_AGGRESSION 1
/* Section 22 (2026-05-26): swap popo vtable[8] (anim probability picker,
 * 0x08865254) to a PRX-side stub that overrides $a1 = picked-anim with a
 * host-controlled value. Combined with the popo_aggression heading-vec
 * writes, this gives sustained "all popos walking forward" control
 * without fighting the engine's per-frame anim selection.
 *
 * Mechanism: vtable swap at 0x089BC580 (popo vtable[8]). Runtime lookup
 * (lw $t9, ($s5); lw $t9, 0x20($t9); jalr $t9), no JIT issues. Already
 * proven by host-side demo at scripts/prefix_patch_demo.py.
 *
 * Self-contained — independent of popo_aggression. With both enabled,
 * aggression provides heading direction + the vt[8] override forces
 * walk-forward anim selection. */
#define MHFU_EMBED_POPO_VT8_OVERRIDE 1

/* Section 23 (2026-05-27): swap popo vtable[5] (per-frame tick, base impl
 * 0x08864634 = ObjBase::vtable_0x14). Per-frame tick body in C++ source
 * (mhp2g-decomp / obj_base.cpp):
 *
 *   void ObjBase::vtable_0x14() {
 *       if (memFn != 0) {
 *           (this->*memFn)();
 *           if (flags & ALIVE) {
 *               vtable_0x18();   // = popo vt[6] @ 0x09B062A8 (override)
 *               vtable_0x1C();   // = popo vt[7] @ 0x088646AC (base)
 *           }
 *       }
 *   }
 *
 * The state-handler chain (memFn -> vt[6] -> vt[7]) rotates the heading
 * vector at entity+0x10/14/18 every frame. Section 22.6 noted vt[14]
 * dispatcher had no direct writes; the actual writer is inside vt[6]'s
 * dispatch ladder (massive switch on entity+0x298 state byte). Rather
 * than chase the specific writer, we wrap the WHOLE per-frame tick with
 * a save-restore stub: snapshot heading on entry, call original, restore
 * heading on exit. Net effect = engine drift accumulation cancelled
 * frame-by-frame; heading sticks at whatever value the popo_aggression
 * thread last wrote, until the next 3 Hz aggression update.
 *
 * Branchless single-block stub (per §22b lessons — multi-block stubs
 * freeze PPSSPP JIT). MOVN selects between (drifted current value) vs
 * (saved value) based on g_vt5_freeze_enable. */
#define MHFU_EMBED_POPO_VT5_FREEZE 1

/* Section 24 (2026-05-27): hook the engine's UNIVERSAL heading rotator
 * function at EBOOT `0x08865044`. Identified live via mem-BP write trace
 * on entity+0x10. Confirmed shared between popo (type 0x46) AND anteka
 * (type 0x45) — same function, same interior PCs fire for both species.
 *
 * Signature (reconstructed):
 *   void heading_rotator(void *entity,    // $a0 — saved to $s0
 *                        float *out_buf,  // $a1 — caller's sp+0x10
 *                        u8 anim_id,      // $a2 — e.g. 6, 0x13
 *                        float *m_row0,   // $a3 — Euler angles row
 *                        float *m_row1,   // $t0 — speed/damping row
 *                        float *m_row2);  // $t1 — third row
 *
 * Fires ~1.5x/sec/popo at state transitions (NOT per-frame). 14 JAL
 * callers (3 EBOOT, 11 OVL_A). Writes computed heading to
 * entity+0x10..+0x18 via VFPU math.
 *
 * Hook design (Harmony-style PREFIX replace):
 *   1. Patch first 2 insns of 0x08865044 to `j stub; nop`.
 *   2. Stub checks entity type (entity+0x1E8).
 *   3. If type == popo (0x46) AND g_heading_enable: call C helper that
 *      computes chase-chain direction + writes entity+0x10..+0x18, then
 *      JR $ra to caller (skip original entirely).
 *   4. Otherwise: execute the saved original insns + j to original+0x8
 *      (continue original normally — anteka/tigrex unaffected).
 *
 * No race with engine: hook runs INSIDE the engine's call graph at the
 * exact moment heading would have been updated. Engine fires hook only
 * at state transitions (1.5x/sec/popo) — well under the 30 Hz halt
 * regime that fight-the-engine approaches tripped.
 */
#define MHFU_EMBED_POPO_HEADING_HOOK 1

#if MHFU_EMBED_POPO_GROWTH

#define POPO_MONSTER_TYPE      0x46
#define POPO_TARGET_AREA_INDEX 99
#define POPO_MAX_TRACKED       8
#define POPO_STEP_INTERVAL_MS  500

/* Per-entity size_scale lives at +0x024 but the game keeps mirrors at
 * +0x220, +0x224, +0x228 and +0x270 (per docs/agent_memory_map.md and
 * memory note monster_size_scalar). The game's per-frame sync routine
 * appears to copy one of the mirrors back into +0x024 each frame, so
 * writing only +0x024 has no visible effect — the original value is
 * restored within milliseconds. Solution: write all five cells. */
static const uint32_t POPO_SIZE_MIRRORS[] = {
    0x024, 0x220, 0x224, 0x228, 0x270,
};
#define POPO_SIZE_MIRROR_COUNT \
    (sizeof(POPO_SIZE_MIRRORS) / sizeof(POPO_SIZE_MIRRORS[0]))

static const float g_popo_size_steps[10] = {
    0.35f, 0.85f, 1.35f, 1.85f, 2.00f,
    1.85f, 1.35f, 0.85f, 0.35f, 0.35f,
};

typedef struct {
    uint32_t entity_ptr;
    float    original_size;
} tracked_popo_t;

static tracked_popo_t g_tracked_popos[POPO_MAX_TRACKED];
static volatile int   g_popo_active = 0;

static int popo_find_slot(uint32_t entity_ptr)
{
    for (int i = 0; i < POPO_MAX_TRACKED; i++)
        if (g_tracked_popos[i].entity_ptr == entity_ptr) return i;
    return -1;
}

static int popo_find_free_slot(void)
{
    for (int i = 0; i < POPO_MAX_TRACKED; i++)
        if (g_tracked_popos[i].entity_ptr == 0) return i;
    return -1;
}

static int popo_entity_still_alive(uint32_t entity_ptr)
{
    for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
        if (p == entity_ptr) return 1;
    }
    return 0;
}

static void popo_prune_dead(void)
{
    for (int i = 0; i < POPO_MAX_TRACKED; i++) {
        if (g_tracked_popos[i].entity_ptr == 0) continue;
        if (!popo_entity_still_alive(g_tracked_popos[i].entity_ptr)) {
            g_tracked_popos[i].entity_ptr = 0;
            g_tracked_popos[i].original_size = 0.0f;
        }
    }
}

static void popo_restore_and_purge(void)
{
    for (int i = 0; i < POPO_MAX_TRACKED; i++) {
        if (g_tracked_popos[i].entity_ptr == 0) continue;
        if (popo_entity_still_alive(g_tracked_popos[i].entity_ptr)) {
            union { uint32_t u; float f; } cvt;
            cvt.f = g_tracked_popos[i].original_size;
            for (unsigned m = 0; m < POPO_SIZE_MIRROR_COUNT; m++) {
                *(volatile uint32_t *)(g_tracked_popos[i].entity_ptr +
                                       POPO_SIZE_MIRRORS[m]) = cvt.u;
            }
        }
        g_tracked_popos[i].entity_ptr = 0;
        g_tracked_popos[i].original_size = 0.0f;
    }
}

static void popo_on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now_target = (ctx->section_id      == POPO_TARGET_AREA_INDEX);
    int was_target = (ctx->prev_section_id == POPO_TARGET_AREA_INDEX);

    if (now_target && !was_target) {
        g_popo_active = 1;
        mhfu_log("[popo_growth] ACTIVATE  section=%u (prev=%u)",
                 (unsigned)ctx->section_id, (unsigned)ctx->prev_section_id);
    } else if (was_target && !now_target) {
        g_popo_active = 0;
        popo_restore_and_purge();
        mhfu_log("[popo_growth] DEACTIVATE  left=%u now=%u",
                 (unsigned)ctx->prev_section_id, (unsigned)ctx->section_id);
    }
}

static void popo_on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != POPO_MONSTER_TYPE) return;
    if (popo_find_slot(ctx->entity_ptr) >= 0) return;
    int idx = popo_find_free_slot();
    if (idx < 0) return;
    union { uint32_t u; float f; } cvt;
    cvt.u = *(volatile uint32_t *)(ctx->entity_ptr + ENTITY_OFF_SIZE_SCALE);
    g_tracked_popos[idx].entity_ptr    = ctx->entity_ptr;
    g_tracked_popos[idx].original_size = cvt.f;
    mhfu_log("[popo_growth] tracking Popo @ 0x%08lx orig_size_raw=0x%08lx",
             (unsigned long)ctx->entity_ptr, (unsigned long)cvt.u);
}

static int popo_growth_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Step cadence stays 500 ms (10 steps × 500 ms = 5 s period). Within
     * each step we re-write the current size at frame-ish cadence so the
     * game's per-frame mirror-sync doesn't restore the original between
     * our step transitions. 30 ms is fine — 2 writes per game-logic
     * frame at 30 Hz, cheap, and visually solid. */
    int step = 0;
    int ticks_in_step = 0;
    const int TICK_MS = 30;
    const int TICKS_PER_STEP = POPO_STEP_INTERVAL_MS / TICK_MS;

    for (;;) {
        sceKernelDelayThread(TICK_MS * 1000);
        if (!g_popo_active) {
            ticks_in_step = 0; step = 0;
            continue;
        }
        popo_prune_dead();
        union { uint32_t u; float f; } cvt;
        cvt.f = g_popo_size_steps[step];
        for (int i = 0; i < POPO_MAX_TRACKED; i++) {
            uint32_t p = g_tracked_popos[i].entity_ptr;
            if (p == 0) continue;
            for (unsigned m = 0; m < POPO_SIZE_MIRROR_COUNT; m++) {
                *(volatile uint32_t *)(p + POPO_SIZE_MIRRORS[m]) = cvt.u;
            }
        }
        if (++ticks_in_step >= TICKS_PER_STEP) {
            ticks_in_step = 0;
            step = (step + 1) % (int)(sizeof(g_popo_size_steps) /
                                      sizeof(g_popo_size_steps[0]));
        }
    }
    return 0;
}

#endif /* MHFU_EMBED_POPO_GROWTH */

/* ------------------------------------------------------------------------ *
 * Embedded popo_aggression mod.
 *
 * Discovered 2026-05-26 (heading-vec investigation, see memory note
 * popo-motion-control-solved): monster motion is driven by the heading
 * vector at entity+0x010..+0x018; engine-side rotators in popo_ovl_B
 * over-write heading every game frame. Polling from the host debugger
 * loses the race (JIT-cached overlay code). PSP-side writes from a PRX
 * thread are CPU-coherent and win the race in practice — same pattern
 * popo_growth uses for the size-mirror sync.
 *
 * Behavior: on entering snow-map section 1 (area_index 99), every popo
 * in the entity registry is told to walk toward the NEXT popo in slot
 * order; the last wraps to the first. Forms a visible chase chain that
 * proves we control monster AI deterministically + at performant rate
 * (no flicker, no debugger fight).
 *
 * Same standalone-vs-embedded story as popo_growth: PPSSPP plugin-host
 * wedges with two PRXes co-loaded, so the canonical build embeds here
 * while the standalone source under mods/popo_aggression_prx/ is the
 * reference for the future stable-stub path.
 * ------------------------------------------------------------------------ */

/* Forward decls for vt[8] hook symbols — actual defs further down in the
 * MHFU_EMBED_POPO_VT8_OVERRIDE block. The vt[8] globals are NON-static
 * specifically so this extern decl works across blocks; agg_reconcile_state
 * below uses these to keep the swap in sync across savestate loads. */
#if MHFU_EMBED_POPO_VT8_OVERRIDE
#define VT8_POPO_VT8_SLOT 0x089BC580u
#define VT8_STUB_INSNS    32
extern volatile uint32_t g_vt8_stub_buf[VT8_STUB_INSNS];
extern volatile uint32_t g_vt8_override_enable;
extern volatile uint32_t g_vt8_request_install;
extern volatile int      g_vt8_installed;
void vt8_install(void);
void vt8_uninstall(void);
#endif

#if MHFU_EMBED_POPO_VT5_FREEZE
#define VT5_POPO_VT5_SLOT 0x089BC574u
#define VT5_STUB_INSNS    48
extern volatile uint32_t g_vt5_stub_buf[VT5_STUB_INSNS];
extern volatile uint32_t g_vt5_freeze_enable;
extern volatile uint32_t g_vt5_request_install;
extern volatile int      g_vt5_installed;
void vt5_install(void);
void vt5_uninstall(void);
#endif

#if MHFU_EMBED_POPO_HEADING_HOOK
#define HDG_ROTATOR_ADDR  0x08865044u
#define HDG_RESUME_ADDR   0x0886504Cu  /* original + 8 (after the 2 hijacked insns) */
#define HDG_STUB_INSNS    48
extern volatile uint32_t g_hdg_stub_buf[HDG_STUB_INSNS];
extern volatile uint32_t g_hdg_enable;
extern volatile uint32_t g_hdg_request_install;
extern volatile int      g_hdg_installed;
extern volatile uint32_t g_hdg_orig_insn0;
extern volatile uint32_t g_hdg_orig_insn1;
extern volatile uint32_t g_hdg_hit_count;
void hdg_install(void);
void hdg_uninstall(void);
/* C-side popo chase helper called from the stub. ABI: $a0 = entity. */
void hdg_chase_helper(uint32_t entity);
#endif

#if MHFU_EMBED_POPO_AGGRESSION

#define AGG_MONSTER_TYPE         0x46
#define AGG_TARGET_AREA_INDEX    99
#define AGG_MAX_TRACKED          8
/* Run every 333 ms (~3 Hz). Earlier 30 ms cadence halted popos because
 * the AI's per-frame wander cycle never got to complete between our
 * writes — popo would enter walk anim, our next tick would re-arrive
 * during anim transition, anim never advanced enough for root-motion.
 * Verified 2026-05-26 via host-side sweep: 3 Hz is the sweet spot
 * (tracking score +0.86 at this rate). PRX-side at 333 ms gives the
 * AI ~10 frames to integrate position before our next override. */
#define AGG_TICK_MS              333

/* Entity-relative offsets — verified 2026-05-26.
 * Heading + motion-gate cells. The alarmed-cell battery added in the
 * previous build (busy_bits / +0x0C0 / +0x0C4 / +0x298 / +0x460 / +0x63C)
 * made popos INVISIBLE in-game — entity stayed in registry but render
 * + hitbox dropped. The diff that found those values almost certainly
 * captured a popo passing through death (two SnS hits = ~40 dmg, likely
 * kill on small monster). DO NOT re-enable those writes without first
 * filtering the diff to HP > 0 samples only. */
#define AGG_OFF_HEADING          0x010      /* vec3 — motion direction */
#define AGG_OFF_POSITION         0x200      /* vec3 — world pos */
#define AGG_OFF_PURSUE_TARGET    0x1C8      /* u32 — herd-follow target */
#define AGG_OFF_ANIM_324         0x324      /* u16 — current anim ID */
#define AGG_OFF_STATE_334        0x334      /* u16 — motion-gate state byte */

#define AGG_STATE_LOCOMOTION     5
#define AGG_ANIM_WALK_FORWARD    1011

typedef struct {
    uint32_t entity_ptr;     /* 0 = empty */
    int      reg_slot;       /* original entity-registry slot, for ordering */
} tracked_aggressor_t;

static tracked_aggressor_t g_aggressors[AGG_MAX_TRACKED];
static volatile int        g_aggression_active = 0;

/* Section 23.12 — runtime cell-pin enable flags. Default 1 (pin everything).
 * Host-write 0 to isolate which of the four cell writes actually drives
 * the engine's locomotion. */
volatile uint32_t g_agg_pin_heading = 1;   /* +0x10..+0x18 vec3 */
volatile uint32_t g_agg_pin_state   = 1;   /* +0x334 u16 = 5 */
volatile uint32_t g_agg_pin_anim    = 1;   /* +0x324 u16 = 1011 */
volatile uint32_t g_agg_pin_pursue  = 1;   /* +0x1C8 u32 = 0 */

static int agg_find_slot(uint32_t entity_ptr)
{
    for (int i = 0; i < AGG_MAX_TRACKED; i++)
        if (g_aggressors[i].entity_ptr == entity_ptr) return i;
    return -1;
}

static int agg_find_free_slot(void)
{
    for (int i = 0; i < AGG_MAX_TRACKED; i++)
        if (g_aggressors[i].entity_ptr == 0) return i;
    return -1;
}

static int agg_entity_still_alive(uint32_t entity_ptr, int *out_slot)
{
    for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
        if (p == entity_ptr) {
            if (out_slot) *out_slot = slot;
            return 1;
        }
    }
    return 0;
}

static void agg_prune_dead(void)
{
    for (int i = 0; i < AGG_MAX_TRACKED; i++) {
        if (g_aggressors[i].entity_ptr == 0) continue;
        int slot;
        if (!agg_entity_still_alive(g_aggressors[i].entity_ptr, &slot)) {
            g_aggressors[i].entity_ptr = 0;
            g_aggressors[i].reg_slot   = 0;
        } else {
            /* Keep reg_slot fresh in case the registry shuffled. */
            g_aggressors[i].reg_slot = slot;
        }
    }
}

/* Aim `shooter` toward `target` via heading vec + motion-gate cells.
 * Cadence is 333 ms which gives the AI ~10 game frames between our
 * overrides to integrate root-motion. Writes per tick:
 *   • heading vec (always — engine over-writes every game frame)
 *   • state, anim, pursue (change-only — preserve engine's anim frame
 *     counter unless it has cycled to a non-walk state)
 *
 * Earlier alarmed-cell battery (+0x0BC, +0x0C0, +0x0C4, +0x298, +0x460,
 * +0x63C) made popos invisible — those values came from a hit-diff
 * tainted by the popo dying mid-window. Re-enable only after redoing
 * the diff with HP>0 filtering.
 */
static void agg_aim_at(uint32_t shooter_ptr, uint32_t target_ptr)
{
    union { uint32_t u; float f; } sx, sz, tx, tz, ux, uy, uz;
    sx.u = *(volatile uint32_t *)(shooter_ptr + AGG_OFF_POSITION + 0);
    sz.u = *(volatile uint32_t *)(shooter_ptr + AGG_OFF_POSITION + 8);
    tx.u = *(volatile uint32_t *)(target_ptr  + AGG_OFF_POSITION + 0);
    tz.u = *(volatile uint32_t *)(target_ptr  + AGG_OFF_POSITION + 8);
    float dx = tx.f - sx.f;
    float dz = tz.f - sz.f;
    float magsq = dx * dx + dz * dz;
    if (g_agg_pin_heading && magsq >= 1.0f) {
        float invmag = 1.0f / __builtin_sqrtf(magsq);
        ux.f = dx * invmag;
        uy.f = 0.0f;
        uz.f = dz * invmag;
        *(volatile uint32_t *)(shooter_ptr + AGG_OFF_HEADING + 0) = ux.u;
        *(volatile uint32_t *)(shooter_ptr + AGG_OFF_HEADING + 4) = uy.u;
        *(volatile uint32_t *)(shooter_ptr + AGG_OFF_HEADING + 8) = uz.u;
    }
    if (g_agg_pin_state) {
        uint16_t cur_state = *(volatile uint16_t *)(shooter_ptr + AGG_OFF_STATE_334);
        if (cur_state != AGG_STATE_LOCOMOTION) {
            *(volatile uint16_t *)(shooter_ptr + AGG_OFF_STATE_334) = AGG_STATE_LOCOMOTION;
        }
    }
    if (g_agg_pin_anim) {
        uint16_t cur_anim = *(volatile uint16_t *)(shooter_ptr + AGG_OFF_ANIM_324);
        if (cur_anim != AGG_ANIM_WALK_FORWARD) {
            *(volatile uint16_t *)(shooter_ptr + AGG_OFF_ANIM_324) = AGG_ANIM_WALK_FORWARD;
        }
    }
    if (g_agg_pin_pursue) {
        uint32_t cur_pursue = *(volatile uint32_t *)(shooter_ptr + AGG_OFF_PURSUE_TARGET);
        if (cur_pursue != 0) {
            *(volatile uint32_t *)(shooter_ptr + AGG_OFF_PURSUE_TARGET) = 0;
        }
    }
    (void)target_ptr;
}

#if MHFU_EMBED_POPO_HEADING_HOOK
/* Section 24b — OVL_A JAL caller list. These 11 sites all `jal 0x08865044`
 * inside popo's overlay. Patched at section-99 entry (= immediately after
 * popo overlay loads + before any state transition fires) so PPSSPP's JIT
 * translates OUR jal target (= stub) on first execution. */
static const uint32_t g_hdg_ovl_callers[] = {
    0x09A73058u, 0x09A730B4u,
    0x09A97248u, 0x09A977CCu, 0x09A978F4u, 0x09A97A18u,
    0x09A97B14u, 0x09A97D40u, 0x09A97F2Cu, 0x09A98488u, 0x09A98AC4u,
};
#define HDG_OVL_CALLER_COUNT (sizeof(g_hdg_ovl_callers) / sizeof(g_hdg_ovl_callers[0]))

static uint32_t g_hdg_ovl_saved[HDG_OVL_CALLER_COUNT];
static int      g_hdg_ovl_patched = 0;

static void hdg_patch_ovl_callers(void)
{
    if (g_hdg_ovl_patched) return;
    uint32_t patch = mips_jal((uint32_t)(uintptr_t)g_hdg_stub_buf);
    int patched_n = 0;
    for (unsigned i = 0; i < HDG_OVL_CALLER_COUNT; i++) {
        uint32_t addr = g_hdg_ovl_callers[i];
        uint32_t orig = *(volatile uint32_t *)addr;
        /* Verify it's `jal 0x08865044` — opcode 0x03 with target 0x02219511
         * (since 0x08865044 >> 2 = 0x02219511). */
        uint32_t expected = mips_jal(HDG_ROTATOR_ADDR);
        if (orig != expected) {
            mhfu_log("[hdg-ovl] skip 0x%08lx: got 0x%08lx want 0x%08lx",
                     (unsigned long)addr, (unsigned long)orig, (unsigned long)expected);
            continue;
        }
        g_hdg_ovl_saved[i] = orig;
        *(volatile uint32_t *)addr = patch;
        patched_n++;
    }
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    g_hdg_ovl_patched = 1;
    mhfu_log("[hdg-ovl] patched %d/%u OVL_A callers", patched_n, (unsigned)HDG_OVL_CALLER_COUNT);
}

static void hdg_unpatch_ovl_callers(void)
{
    if (!g_hdg_ovl_patched) return;
    for (unsigned i = 0; i < HDG_OVL_CALLER_COUNT; i++) {
        if (g_hdg_ovl_saved[i] != 0) {
            *(volatile uint32_t *)g_hdg_ovl_callers[i] = g_hdg_ovl_saved[i];
        }
    }
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    g_hdg_ovl_patched = 0;
    mhfu_log("[hdg-ovl] unpatched callers");
}
#endif

static void agg_on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now_target = (ctx->section_id      == AGG_TARGET_AREA_INDEX);
    int was_target = (ctx->prev_section_id == AGG_TARGET_AREA_INDEX);

    if (now_target && !was_target) {
        g_aggression_active = 1;
        mhfu_log("[popo_agg] ACTIVATE  section=%u (prev=%u)",
                 (unsigned)ctx->section_id, (unsigned)ctx->prev_section_id);
#if MHFU_EMBED_POPO_HEADING_HOOK
        /* Section 24b: section 99 (snow sec1) just entered → popo
         * overlay is freshly loaded into RAM, but no state transitions
         * have fired yet → JAL imms have NOT been JIT-translated.
         * Patch them NOW so PPSSPP's JIT picks up our redirect on
         * first execution. */
        hdg_patch_ovl_callers();
#endif
    } else if (was_target && !now_target) {
        g_aggression_active = 0;
        for (int i = 0; i < AGG_MAX_TRACKED; i++) {
            g_aggressors[i].entity_ptr = 0;
            g_aggressors[i].reg_slot   = 0;
        }
        mhfu_log("[popo_agg] DEACTIVATE  left=%u now=%u",
                 (unsigned)ctx->prev_section_id, (unsigned)ctx->section_id);
#if MHFU_EMBED_POPO_HEADING_HOOK
        hdg_unpatch_ovl_callers();
#endif
    }
}

static void agg_on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != AGG_MONSTER_TYPE) return;
    if (agg_find_slot(ctx->entity_ptr) >= 0) return;
    int idx = agg_find_free_slot();
    if (idx < 0) return;
    g_aggressors[idx].entity_ptr = ctx->entity_ptr;
    g_aggressors[idx].reg_slot   = ctx->slot;
    mhfu_log("[popo_agg] tracking popo @ 0x%08lx (reg slot %d)",
             (unsigned long)ctx->entity_ptr, ctx->slot);
}

/* Insertion-sort the tracked list by reg_slot. Cheap (≤ 8 items) and
 * gives a deterministic chase order matching the user-visible slot
 * numbering in the HUD. */
static void agg_sort_by_slot(uint32_t *out_ptrs, int *out_n)
{
    int n = 0;
    /* Collect non-empty entries first. */
    for (int i = 0; i < AGG_MAX_TRACKED; i++) {
        if (g_aggressors[i].entity_ptr != 0)
            out_ptrs[n++] = g_aggressors[i].entity_ptr;
    }
    *out_n = n;
    /* Sort by their current reg_slot — re-read from the registry since
     * popos can move slots between sweeps. */
    for (int i = 1; i < n; i++) {
        uint32_t p = out_ptrs[i];
        int slot_p = 0;
        agg_entity_still_alive(p, &slot_p);
        int j = i;
        while (j > 0) {
            int slot_prev = 0;
            agg_entity_still_alive(out_ptrs[j - 1], &slot_prev);
            if (slot_prev <= slot_p) break;
            out_ptrs[j] = out_ptrs[j - 1];
            j--;
        }
        out_ptrs[j] = p;
    }
}

/* Poll-based activation + vt[8] reconcile.
 *
 * Two scenarios where the section-transition callback (agg_on_section)
 * doesn't fire and we'd otherwise stay deactivated:
 *   1. The user loads a savestate already in section 99 — boot goes
 *      straight to "in area" without firing the 98→99 transition.
 *   2. The savestate restore wipes our previously-installed vt[8] swap
 *      (savestates capture all RAM, including the EBOOT vtable bytes).
 *
 * The aggression thread runs every 333 ms and is the natural place for
 * an idempotent reconcile: read area_index, infer should-be-active,
 * then make the install/uninstall + flag match the desired state. */
static void agg_reconcile_state(void)
{
    uint16_t area = *(volatile uint16_t *)0x08B0C7DCu;
    int should_be_active = (area == AGG_TARGET_AREA_INDEX);
    if (should_be_active && !g_aggression_active) {
        g_aggression_active = 1;
        mhfu_log("[popo_agg] AUTO-ACTIVATE  area=%u (poll path)", (unsigned)area);
    } else if (!should_be_active && g_aggression_active) {
        g_aggression_active = 0;
        for (int i = 0; i < AGG_MAX_TRACKED; i++) {
            g_aggressors[i].entity_ptr = 0;
            g_aggressors[i].reg_slot   = 0;
        }
        mhfu_log("[popo_agg] AUTO-DEACTIVATE  area=%u", (unsigned)area);
    }
#if MHFU_EMBED_POPO_VT8_OVERRIDE
    volatile uint32_t *slot = (volatile uint32_t *)VT8_POPO_VT8_SLOT;
    uint32_t cur  = *slot;
    uint32_t stub = (uint32_t)(uintptr_t)g_vt8_stub_buf;
    /* vt[8] AUTO-INSTALL DISABLED (v4, 2026-05-26).
     *
     * Both v2 (replace-style) and v3 (postfix with 32B frame, $ra at
     * +0x18, JAL not JALR — matching the framework's working wrapper
     * layout exactly) FROZE PPSSPP on section-1 load with PC=0.
     *
     * Diagnostic data from frozen state:
     *   • CPU PC = 0; RA = 0x08000020 (PSP startup); a2/a3 = 0xDEADBEEF.
     *   • Stub bytes had PPSSPP HLE markers (`0x6817xxxx`) at 3 block
     *     boundaries: entry, after JAL, branch target.
     *   • The framework's working wrappers have HLE markers at 2
     *     boundaries (entry + after JAL) and DO NOT crash.
     *   • Theory: PPSSPP's JIT mishandles short basic blocks (the
     *     branch-target block in my stub is ~7 insns, vs the wrapper's
     *     long linear blocks) OR mishandles JALR-entered code (the
     *     dispatcher reaches our stub via vtable JALR; framework
     *     wrappers are reached via patched J).
     *
     * The popo_aggression cell-pin path (writing entity+0x324) works
     * without the vt[8] swap — Section 20 verified this end-to-end
     * (visible chase chain, ~3 Hz). Keeping that as the production
     * mechanism; vt[8] swap stays opt-in for manual experiments.
     *
     * To experiment with the swap from the host:
     *   1. Write `1` to g_vt8_request_install (address printed in log).
     *   2. The aggression thread's next tick calls vt8_install().
     *   3. Optionally set g_vt8_override_enable = 1 for active override.
     *   4. Write `0` to g_vt8_request_install + `0` to override_enable
     *      to revert.
     */
    if (should_be_active && g_vt8_request_install && cur != stub) {
        g_vt8_installed = 0;
        vt8_install();
    } else if ((!should_be_active || !g_vt8_request_install) && cur == stub) {
        vt8_uninstall();
    }
#endif
#if MHFU_EMBED_POPO_VT5_FREEZE
    {
        volatile uint32_t *vt5_slot = (volatile uint32_t *)VT5_POPO_VT5_SLOT;
        uint32_t v5_cur  = *vt5_slot;
        uint32_t v5_stub = (uint32_t)(uintptr_t)g_vt5_stub_buf;
        /* Same opt-in pattern as vt[8]: only install when host writes
         * g_vt5_request_install = 1. Allows safe experimentation without
         * auto-binding the per-frame tick on every cold boot. */
        if (should_be_active && g_vt5_request_install && v5_cur != v5_stub) {
            g_vt5_installed = 0;
            vt5_install();
        } else if ((!should_be_active || !g_vt5_request_install) && v5_cur == v5_stub) {
            vt5_uninstall();
        }
    }
#endif
#if MHFU_EMBED_POPO_HEADING_HOOK
    /* The heading hook is installed AT PRX INIT (in install_worker)
     * because live install after first JIT translation fails to take
     * effect (PPSSPP JIT cache). Re-install if the patched bytes
     * disappear (e.g., after a savestate load that captured pre-patch
     * memory). */
    {
        uint32_t cur_w0 = *(volatile uint32_t *)HDG_ROTATOR_ADDR;
        uint32_t want_w0 = mips_j((uint32_t)(uintptr_t)g_hdg_stub_buf);
        if (g_hdg_installed && cur_w0 != want_w0) {
            /* Bytes reverted (savestate?). Re-patch. */
            g_hdg_installed = 0;
            hdg_install();
        }
    }
#endif
}

/* The savestate-load path doesn't fire monster_spawned events (no slot
 * transitions during a savestate restore), so on poll activation we
 * scan the registry ourselves to populate the tracked list. */
static void agg_repopulate_from_registry(void)
{
    for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
        if (p == 0) continue;
        if (agg_find_slot(p) >= 0) continue;
        uint8_t mt = *(volatile uint8_t *)(p + 0x1E8);
        if (mt != AGG_MONSTER_TYPE) continue;
        int idx = agg_find_free_slot();
        if (idx < 0) break;
        g_aggressors[idx].entity_ptr = p;
        g_aggressors[idx].reg_slot   = slot;
        mhfu_log("[popo_agg] poll-tracked popo @ 0x%08lx (reg slot %d)",
                 (unsigned long)p, slot);
    }
}

static int popo_aggression_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(AGG_TICK_MS * 1000);
        agg_reconcile_state();
        if (!g_aggression_active) continue;
        agg_repopulate_from_registry();
        agg_prune_dead();
        uint32_t ptrs[AGG_MAX_TRACKED];
        int n = 0;
        agg_sort_by_slot(ptrs, &n);
        if (n < 2) continue;
        /* Cyclic chase chain: popo[i] aims at popo[(i+1) % n].
         * With n popos: 0 → 1 → 2 → ... → (n-1) → 0. */
        for (int i = 0; i < n; i++) {
            uint32_t shooter = ptrs[i];
            uint32_t target  = ptrs[(i + 1) % n];
            agg_aim_at(shooter, target);
        }
    }
    return 0;
}

#endif /* MHFU_EMBED_POPO_AGGRESSION */

/* ------------------------------------------------------------------------ *
 * Embedded popo vt[8] override hook (Section 22, 2026-05-26).
 *
 * Per-frame dispatcher (0x09AC5228..0x09AC5520, fully disasmed in
 * docs/POPO_AI_STATE_HANDLER_VTABLE.md) calls popo's vtable[8] inside
 * its action-record loop:
 *
 *     0x09AC5488  lw   $t9, ($s5)          ; entity vtable
 *     0x09AC548C  andi $a1, $s2, 0xffff    ; a1 = anim_category
 *     0x09AC5490  lw   $t9, 0x20($t9)      ; vtable[8]  (popo: 0x08865254)
 *     0x09AC5494  jalr $t9                  ; → probability picker
 *     0x09AC5498  move $a0, $s5             ; (delay)
 *     ...
 *     0x09AC54A0  beqz $a1, ...skip apply   ; vt[8] WRITES $a1 = picked anim
 *
 * So vt[8] is both an input port (a1=category in) AND output port
 * (a1=specific anim out). Our stub:
 *   1. Increment hit counter (diagnostic).
 *   2. If override disabled → tail-call original 0x08865254 unchanged.
 *   3. If enabled → load g_vt8_override_anim into $a1, set $v0=1, return.
 *
 * The dispatcher's BEQZ $a1 check then proceeds, $a1 holds OUR anim,
 * and z_un_08863e70(entity+0x80, $a1, ...) applies the override anim.
 *
 * Popo vtable is at 0x089BC560 (verified live, see scripts/prefix_patch_demo.py).
 * Slot 8 → offset 0x20 → cell 0x089BC580. Original ptr 0x08865254.
 *
 * Cooperates with popo_aggression: aggression writes heading vec at
 * 3 Hz (the engine reads heading per-frame for direction), our hook
 * forces walk-forward anim selection every time the engine asks
 * vt[8] for an anim choice. Result = sustained chase chain without
 * cell-write flicker on +0x324.
 * ------------------------------------------------------------------------ */

#if MHFU_EMBED_POPO_VT8_OVERRIDE

#define VT8_POPO_VTABLE_BASE       0x089BC560u
/* VT8_POPO_VT8_SLOT (0x089BC580) and VT8_STUB_INSNS (32) are already
 * defined in the forward-decl block above so the aggression thread can
 * reference them; redefining here would conflict — the values match. */
#define VT8_ORIGINAL_PTR           0x08865254u
#define VT8_DEFAULT_ANIM           1011u   /* WALK_FORWARD per popo_aggression */

/* Stub buffer + control globals — NON-static so the forward-decl block
 * above can extern-declare them. Kept volatile so the compiler doesn't
 * cache reads/writes from the polling thread. */
__attribute__((aligned(4)))
volatile uint32_t g_vt8_stub_buf[VT8_STUB_INSNS];

static volatile uint32_t g_vt8_override_anim   = VT8_DEFAULT_ANIM;
volatile uint32_t        g_vt8_override_enable = 0;
volatile uint32_t        g_vt8_request_install = 0;   /* host-write 1 to opt-in */
static volatile uint32_t g_vt8_hit_count       = 0;
static volatile uint32_t g_vt8_original_ptr    = 0;
volatile int             g_vt8_installed       = 0;

/* Helper: lui+ori sequence to load full 32-bit value into a reg.
 * Caller passes the reg id and an emit buffer; returns # of insns (2). */
static inline int vt8_emit_li32(uint32_t *out, uint32_t reg, uint32_t value)
{
    out[0] = mips_lui(reg, (uint16_t)(value >> 16));
    out[1] = mips_ori(reg, reg, (uint16_t)(value & 0xFFFF));
    return 2;
}

/* POSTFIX patch — original always runs first, then we conditionally
 * override $a1.
 *
 * Bug history (2026-05-26):
 *   v1: replace-style stub (skip original entirely). CRASHED inside
 *       0x08863e70 because $a2 was uninit — original sets it.
 *   v2: postfix stub with 16-byte frame, save $ra at $sp+0. FROZE on
 *       section-1 load with PC=0. Cause: MIPS o32 ABI reserves
 *       $sp+0..+0x0F as $a0..$a3 SAVE AREA. The called original may
 *       write $a0 to $sp+0, overwriting our $ra save. Then our
 *       `lw $ra, 0($sp)` reads entity ptr; `jr $ra` jumps into popo
 *       memory → eventual PC=0 → freeze.
 *   v3 (current): 32-byte frame, save $ra at $sp+0x18 (mirrors the
 *       framework's working wrapper layout, which uses 0x40 frame +
 *       $ra at $sp+0x18). The 0x10..0x1F region is "ours" — original
 *       has no business writing there.
 *
 *   addiu  $sp, $sp, -32                ; 32-byte frame
 *   sw     $ra, 0x18($sp)               ; $ra at +0x18 (beyond arg save area)
 *
 *   ; hit counter (diagnostic)
 *   lui    $t0, hi(&g_vt8_hit_count)
 *   ori    $t0, $t0, lo(&g_vt8_hit_count)
 *   lw     $t1, 0($t0)
 *   addiu  $t1, $t1, 1
 *   sw     $t1, 0($t0)
 *
 *   ; call original via JAL (absolute, no $t9 setup needed — saves 2 insns
 *   ; and avoids JALR which appears to interact with PPSSPP JIT differently
 *   ; than the framework wrappers' JAL dispatcher pattern).
 *   jal    VT8_ORIGINAL_PTR             ; preserves $a0..$a3 contract;
 *                                       ; original returns $a1 = picked anim
 *   nop                                 ; delay slot
 *
 *   ; check enable
 *   lui    $t0, hi(&g_vt8_override_enable)
 *   ori    $t0, $t0, lo(&g_vt8_override_enable)
 *   lw     $t2, 0($t0)
 *   beq    $t2, $zero, done             ; +5  (skip override branch)
 *   nop                                 ; delay slot
 *
 *   ; ALSO skip if original returned $a1 == 0 (engine's "skip apply"
 *   ; signal — we honour it).
 *   beq    $a1, $zero, done             ; +3
 *   nop                                 ; delay slot
 *
 *   ; override path: $a1 = g_vt8_override_anim
 *   lui    $t0, hi(&g_vt8_override_anim)
 *   ori    $t0, $t0, lo(&g_vt8_override_anim)
 *   lw     $a1, 0($t0)
 *
 *  done:
 *   lw     $ra, 0x18($sp)
 *   addiu  $sp, $sp, 32
 *   jr     $ra
 *   nop                                 ; delay slot
 *
 * 22 instructions, 88 bytes. Fits in VT8_STUB_INSNS=32.
 */
static void vt8_build_stub(void)
{
    uint32_t *s = (uint32_t *)g_vt8_stub_buf;
    int i = 0;

    uint32_t hit_addr    = (uint32_t)(uintptr_t)&g_vt8_hit_count;
    uint32_t enable_addr = (uint32_t)(uintptr_t)&g_vt8_override_enable;
    uint32_t anim_addr   = (uint32_t)(uintptr_t)&g_vt8_override_anim;

    /* Prologue — 32-byte frame, save $ra ABOVE the arg-save area
     * ($sp+0..+0x0F is reserved by the o32 ABI for the callee to spill
     * $a0..$a3). v2 bug: saved $ra at $sp+0 → original's $a0 spill
     * clobbered it → garbage $ra on return → PC=0 freeze. */
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -32);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);

    /* Hit counter */
    i += vt8_emit_li32(&s[i], MIPS_REG_T0, hit_addr);
    s[i++] = mips_lw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_T1, 1);
    s[i++] = mips_sw(MIPS_REG_T1, 0, MIPS_REG_T0);

    /* Call original via JAL (absolute target). Saves 2 insns vs JALR
     * setup. Both PSP user RAM and EBOOT live in the same 256 MiB
     * segment so JAL's 28-bit encoded target reaches. */
    s[i++] = mips_jal(VT8_ORIGINAL_PTR);
    s[i++] = MIPS_NOP;

    /* Check enable. If disabled → skip 7 insns + delay-slot down to
     * epilogue. */
    i += vt8_emit_li32(&s[i], MIPS_REG_T0, enable_addr);
    s[i++] = mips_lw(MIPS_REG_T2, 0, MIPS_REG_T0);
    s[i++] = mips_beq(MIPS_REG_T2, MIPS_REG_ZERO, 7);
    s[i++] = MIPS_NOP;

    /* Skip override if original returned $a1 == 0 (engine "skip apply"
     * signal — honouring it avoids running the applier on stale state). */
    s[i++] = mips_beq(MIPS_REG_A1, MIPS_REG_ZERO, 4);
    s[i++] = MIPS_NOP;

    /* Override: $a1 = g_vt8_override_anim */
    i += vt8_emit_li32(&s[i], MIPS_REG_T0, anim_addr);
    s[i++] = mips_lw(MIPS_REG_A1, 0, MIPS_REG_T0);

    /* Epilogue */
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 32);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
}

void vt8_install(void)
{
    if (g_vt8_installed) return;
    vt8_build_stub();
    /* Flush dcache + invalidate icache so the freshly written stub
     * bytes are seen by the CPU when it executes them. */
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    /* Save original then swap. */
    volatile uint32_t *slot = (volatile uint32_t *)VT8_POPO_VT8_SLOT;
    g_vt8_original_ptr = *slot;
    *slot = (uint32_t)(uintptr_t)g_vt8_stub_buf;
    sceKernelDcacheWritebackInvalidateAll();
    g_vt8_installed = 1;
    mhfu_log("[vt8] installed: stub=0x%08lx orig=0x%08lx slot=0x%08lx",
             (unsigned long)g_vt8_stub_buf,
             (unsigned long)g_vt8_original_ptr,
             (unsigned long)VT8_POPO_VT8_SLOT);
}

void vt8_uninstall(void)
{
    if (!g_vt8_installed) return;
    volatile uint32_t *slot = (volatile uint32_t *)VT8_POPO_VT8_SLOT;
    *slot = g_vt8_original_ptr;
    sceKernelDcacheWritebackInvalidateAll();
    g_vt8_installed = 0;
    g_vt8_override_enable = 0;
    mhfu_log("[vt8] uninstalled: restored 0x%08lx, hits=%lu",
             (unsigned long)g_vt8_original_ptr,
             (unsigned long)g_vt8_hit_count);
}

/* Install/uninstall is now driven entirely by the popo_aggression thread's
 * agg_reconcile_state() (poll path), which works for both cold-boot
 * navigation AND savestate-load. No separate map-section callback needed. */

#endif /* MHFU_EMBED_POPO_VT8_OVERRIDE */

/* ------------------------------------------------------------------------ *
 * Embedded popo vt[5] freeze hook (Section 23, 2026-05-27).
 *
 * Wraps popo's per-frame tick (ObjBase::vtable_0x14 base impl at 0x08864634)
 * with a save-restore stub that cancels the engine's per-frame heading
 * drift. The aggression thread writes a fresh heading toward the chase
 * target every 333 ms; this stub pins that value across the intervening
 * frames so the engine's per-frame rotators (inside vt[6] = 0x09B062A8's
 * state-dispatch ladder, and inside the monster's memFn state handler)
 * can't drift heading between aggression updates.
 *
 * Stub structure (branchless single basic block per §22b lessons):
 *   Prologue (save $ra, $s0..$s3)
 *   $s0 = entity ($a0)
 *   Hit counter ++
 *   Snapshot heading: $s1=*(0x10), $s2=*(0x14), $s3=*(0x18)
 *   JAL original 0x08864634   (delay slot: nop)
 *   Load g_vt5_freeze_enable into $t8
 *   For each of 3 heading components:
 *      lw   $t1, off($s0)        ; load current (after original = drifted)
 *      movn $t1, $sN, $t8        ; if enable!=0, $t1 = saved
 *      sw   $t1, off($s0)
 *   Epilogue
 * ------------------------------------------------------------------------ */

#if MHFU_EMBED_POPO_VT5_FREEZE

#define VT5_POPO_VTABLE_BASE       0x089BC560u
#define VT5_ORIGINAL_PTR           0x08864634u

/* Save-area layout in our 64-byte frame:
 *   $sp+0x00..0x0F : o32 arg-save area (callee may spill $a0..$a3 here)
 *   $sp+0x18       : $ra
 *   $sp+0x1C       : $s0  (entity ptr)
 *   $sp+0x20       : $s1  (saved heading.x)
 *   $sp+0x24       : $s2  (saved heading.y)
 *   $sp+0x28       : $s3  (saved heading.z)
 */
__attribute__((aligned(4)))
volatile uint32_t g_vt5_stub_buf[VT5_STUB_INSNS];

volatile uint32_t g_vt5_freeze_enable  = 0;
volatile uint32_t g_vt5_request_install = 0;
static volatile uint32_t g_vt5_hit_count = 0;
static volatile uint32_t g_vt5_original_ptr = 0;
volatile int             g_vt5_installed = 0;

static inline int vt5_emit_li32(uint32_t *out, uint32_t reg, uint32_t value)
{
    out[0] = mips_lui(reg, (uint16_t)(value >> 16));
    out[1] = mips_ori(reg, reg, (uint16_t)(value & 0xFFFF));
    return 2;
}

static void vt5_build_stub(void)
{
    uint32_t *s = (uint32_t *)g_vt5_stub_buf;
    int i = 0;

    uint32_t hit_addr    = (uint32_t)(uintptr_t)&g_vt5_hit_count;
    uint32_t enable_addr = (uint32_t)(uintptr_t)&g_vt5_freeze_enable;

    /* Prologue: 64-byte frame, save $ra + $s0..$s3. */
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -64);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S0, 0x1C, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S1, 0x20, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S2, 0x24, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S3, 0x28, MIPS_REG_SP);

    /* $s0 = this (entity). */
    s[i++] = mips_move(MIPS_REG_S0, MIPS_REG_A0);

    /* Hit counter ++ (diagnostic). */
    i += vt5_emit_li32(&s[i], MIPS_REG_T0, hit_addr);
    s[i++] = mips_lw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_T1, 1);
    s[i++] = mips_sw(MIPS_REG_T1, 0, MIPS_REG_T0);

    /* Snapshot heading vec into $s1..$s3 BEFORE original runs.
     * Heading lives at entity+0x10..+0x18 per CLAUDE.md (verified live
     * Section 20 — aggression mod writes here every 333 ms). */
    s[i++] = mips_lw(MIPS_REG_S1, 0x10, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_S2, 0x14, MIPS_REG_S0);
    s[i++] = mips_lw(MIPS_REG_S3, 0x18, MIPS_REG_S0);

    /* Restore $a0 = entity (we clobbered $s0 above but $a0 is unchanged).
     * Original wants $a0 = this. */
    /* (already there — JAL only modifies $ra) */

    /* Call original (vt[5] base = ObjBase::vtable_0x14). It runs memFn
     * + vt[6] + vt[7] — any of which may rotate the heading vec. */
    s[i++] = mips_jal(VT5_ORIGINAL_PTR);
    s[i++] = MIPS_NOP;   /* delay slot */

    /* Load freeze-enable flag into $t8. */
    i += vt5_emit_li32(&s[i], MIPS_REG_T0, enable_addr);
    s[i++] = mips_lw(MIPS_REG_T8, 0, MIPS_REG_T0);

    /* Branchless conditional restore for each heading component:
     *   $t1 = lw off($s0)              ; current (post-original = drifted)
     *   movn $t1, $sN, $t8             ; if enable != 0, $t1 = saved
     *   sw $t1, off($s0)
     */
    s[i++] = mips_lw(MIPS_REG_T1, 0x10, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S1, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x10, MIPS_REG_S0);

    s[i++] = mips_lw(MIPS_REG_T1, 0x14, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S2, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x14, MIPS_REG_S0);

    s[i++] = mips_lw(MIPS_REG_T1, 0x18, MIPS_REG_S0);
    s[i++] = mips_movn(MIPS_REG_T1, MIPS_REG_S3, MIPS_REG_T8);
    s[i++] = mips_sw(MIPS_REG_T1, 0x18, MIPS_REG_S0);

    /* Epilogue. */
    s[i++] = mips_lw(MIPS_REG_S3, 0x28, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S2, 0x24, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S1, 0x20, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_S0, 0x1C, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 64);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;   /* delay slot */

    /* Fill rest of buffer with NOPs so any stray exec hits a clean stop. */
    while (i < VT5_STUB_INSNS) s[i++] = MIPS_NOP;
}

void vt5_install(void)
{
    if (g_vt5_installed) return;
    vt5_build_stub();
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    volatile uint32_t *slot = (volatile uint32_t *)VT5_POPO_VT5_SLOT;
    g_vt5_original_ptr = *slot;
    *slot = (uint32_t)(uintptr_t)g_vt5_stub_buf;
    sceKernelDcacheWritebackInvalidateAll();
    g_vt5_installed = 1;
    mhfu_log("[vt5] installed: stub=0x%08lx orig=0x%08lx slot=0x%08lx",
             (unsigned long)g_vt5_stub_buf,
             (unsigned long)g_vt5_original_ptr,
             (unsigned long)VT5_POPO_VT5_SLOT);
}

void vt5_uninstall(void)
{
    if (!g_vt5_installed) return;
    volatile uint32_t *slot = (volatile uint32_t *)VT5_POPO_VT5_SLOT;
    *slot = g_vt5_original_ptr;
    sceKernelDcacheWritebackInvalidateAll();
    g_vt5_installed = 0;
    g_vt5_freeze_enable = 0;
    mhfu_log("[vt5] uninstalled: restored 0x%08lx, hits=%lu",
             (unsigned long)g_vt5_original_ptr,
             (unsigned long)g_vt5_hit_count);
}

#endif /* MHFU_EMBED_POPO_VT5_FREEZE */

/* ------------------------------------------------------------------------ *
 * Section 24 — heading rotator hook (Harmony-style PREFIX replace).
 *
 * Hooks EBOOT function 0x08865044 (the universal heading-rotation
 * routine for all monster types). Stub structure:
 *
 *     ; check enable flag — if disabled, fall through to original
 *     lui   $t0, hi(&g_hdg_enable)
 *     ori   $t0, $t0, lo(&g_hdg_enable)
 *     lw    $t0, 0($t0)
 *     beq   $t0, $zero, fallthrough_orig
 *     nop                              ; delay slot
 *
 *     ; check entity type — only intercept popo (0x46)
 *     lbu   $t0, 0x1E8($a0)
 *     addiu $t1, $zero, 0x46
 *     bne   $t0, $t1, fallthrough_orig
 *     nop                              ; delay slot
 *
 *     ; popo path — setup frame, call C helper, return
 *     addiu $sp, $sp, -0x20
 *     sw    $ra, 0x18($sp)
 *     sw    $s0, 0x10($sp)
 *     move  $s0, $a0
 *     ; hit counter
 *     lui   $t0, hi(&g_hdg_hit_count)
 *     ori   $t0, $t0, lo(&g_hdg_hit_count)
 *     lw    $t1, 0($t0)
 *     addiu $t1, $t1, 1
 *     sw    $t1, 0($t0)
 *     ; call C helper
 *     jal   hdg_chase_helper
 *     move  $a0, $s0                   ; (delay) a0 = entity
 *     ; epilogue + return to caller (skip original)
 *     lw    $s0, 0x10($sp)
 *     lw    $ra, 0x18($sp)
 *     addiu $sp, $sp, 0x20
 *     jr    $ra
 *     nop                              ; delay slot
 *
 *  fallthrough_orig:
 *     ; Execute the 2 saved original instructions, then jump back.
 *     <g_hdg_orig_insn0>              ; addiu $sp, $sp, -0x50
 *     <g_hdg_orig_insn1>              ; sw $ra, 0x1c($sp)
 *     j     HDG_RESUME_ADDR             ; jump to 0x0886504C
 *     nop                              ; delay slot
 * ------------------------------------------------------------------------ */

#if MHFU_EMBED_POPO_HEADING_HOOK

__attribute__((aligned(4)))
volatile uint32_t g_hdg_stub_buf[HDG_STUB_INSNS];

/* Section 24: enabled by default. The stub runs unconditionally inside
 * 0x08865044's call path (because the patch is baked at PRX init,
 * before JIT translation) but type-checks per entity, so only popos
 * are re-routed; anteka/tigrex fall through to the original. */
volatile uint32_t g_hdg_enable          = 1;
volatile uint32_t g_hdg_request_install = 0;
volatile int      g_hdg_installed       = 0;
volatile uint32_t g_hdg_orig_insn0      = 0;
volatile uint32_t g_hdg_orig_insn1      = 0;
volatile uint32_t g_hdg_hit_count       = 0;

/* C-level chase helper. Called from the stub with $a0 = entity.
 * Computes chase-chain heading: this-popo → next-popo (by tracked-list
 * sort order) → writes entity+0x10/+0x14/+0x18.
 *
 * Reuses g_aggressors[] tracked list + agg_sort_by_slot() so the
 * existing spawn-tracking + slot-ordering logic carries over. */
void hdg_chase_helper(uint32_t entity)
{
    uint8_t mt = *(volatile uint8_t *)(entity + 0x1E8);
    if (mt != AGG_MONSTER_TYPE) return;   /* only popo */

    /* Find this entity's index in the tracked list. */
    int my_idx = -1;
    for (int i = 0; i < AGG_MAX_TRACKED; i++) {
        if (g_aggressors[i].entity_ptr == entity) { my_idx = i; break; }
    }
    if (my_idx < 0) return;

    /* Get current sorted-by-slot list. */
    uint32_t ptrs[AGG_MAX_TRACKED];
    int n = 0;
    agg_sort_by_slot(ptrs, &n);
    if (n < 2) return;

    /* Find this popo's position in the sorted list. */
    int pos = -1;
    for (int i = 0; i < n; i++) {
        if (ptrs[i] == entity) { pos = i; break; }
    }
    if (pos < 0) return;

    /* Chase chain: aim at next popo. Last one wraps to first. */
    uint32_t target = ptrs[(pos + 1) % n];

    /* Compute (target.x, target.z) − (self.x, self.z), normalize, write. */
    union { uint32_t u; float f; } sx, sz, tx, tz, ux, uz;
    sx.u = *(volatile uint32_t *)(entity + 0x200);
    sz.u = *(volatile uint32_t *)(entity + 0x208);
    tx.u = *(volatile uint32_t *)(target + 0x200);
    tz.u = *(volatile uint32_t *)(target + 0x208);
    float dx = tx.f - sx.f;
    float dz = tz.f - sz.f;
    float magsq = dx * dx + dz * dz;
    if (magsq < 1.0f) return;
    float invmag = 1.0f / __builtin_sqrtf(magsq);
    ux.f = dx * invmag;
    uz.f = dz * invmag;
    *(volatile uint32_t *)(entity + 0x10) = ux.u;
    *(volatile uint32_t *)(entity + 0x14) = 0;
    *(volatile uint32_t *)(entity + 0x18) = uz.u;
}

static inline int hdg_emit_li32(uint32_t *out, uint32_t reg, uint32_t value)
{
    out[0] = mips_lui(reg, (uint16_t)(value >> 16));
    out[1] = mips_ori(reg, reg, (uint16_t)(value & 0xFFFF));
    return 2;
}

static void hdg_build_stub(void)
{
    uint32_t *s = (uint32_t *)g_hdg_stub_buf;
    int i = 0;

    uint32_t enable_addr  = (uint32_t)(uintptr_t)&g_hdg_enable;
    uint32_t hit_addr     = (uint32_t)(uintptr_t)&g_hdg_hit_count;
    uint32_t helper_addr  = (uint32_t)(uintptr_t)&hdg_chase_helper;

    /* --- header: check enable + type, jump to fallthrough if neither matches --- */

    /* Load enable flag into $t0. */
    i += hdg_emit_li32(&s[i], MIPS_REG_T0, enable_addr);
    s[i++] = mips_lw(MIPS_REG_T0, 0, MIPS_REG_T0);
    /* Insns from here to fallthrough_orig: stub-relative offsets.
     * Layout:
     *   +0..+1  : lui + ori for enable
     *   +2      : lw enable
     *   +3      : beq enable, 0, fallthrough        ← we are here, offset 3
     *   +4      : nop (delay)
     *   +5..+6  : lbu type + addiu compare reg
     *   +7      : bne type, popo_const, fallthrough ← second branch
     *   +8      : nop (delay)
     *   +9..+25 : popo body (17 insns: addiu sp, sw ra/s0, move, hit counter
     *             [lui+ori+lw+addiu+sw], jal helper, move a0/s0, lw s0/ra,
     *             addiu sp, jr ra, nop)
     *   +26     : fallthrough_orig: orig_insn0
     *   +27     : orig_insn1
     *   +28     : j HDG_RESUME_ADDR
     *   +29     : nop
     */
    int beq_pos = i;
    s[i++] = mips_beq(MIPS_REG_T0, MIPS_REG_ZERO, 0);   /* patched below */
    s[i++] = MIPS_NOP;

    s[i++] = mips_lbu(MIPS_REG_T0, 0x1E8, MIPS_REG_A0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_ZERO, AGG_MONSTER_TYPE);
    int bne_pos = i;
    s[i++] = mips_bne(MIPS_REG_T0, MIPS_REG_T1, 0);     /* patched below */
    s[i++] = MIPS_NOP;

    /* popo body */
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_S0, 0x10, MIPS_REG_SP);
    s[i++] = mips_move(MIPS_REG_S0, MIPS_REG_A0);

    /* hit counter ++ (diagnostic) */
    i += hdg_emit_li32(&s[i], MIPS_REG_T0, hit_addr);
    s[i++] = mips_lw(MIPS_REG_T1, 0, MIPS_REG_T0);
    s[i++] = mips_addiu(MIPS_REG_T1, MIPS_REG_T1, 1);
    s[i++] = mips_sw(MIPS_REG_T1, 0, MIPS_REG_T0);

    /* JAL helper */
    s[i++] = mips_jal(helper_addr);
    s[i++] = mips_move(MIPS_REG_A0, MIPS_REG_S0);   /* delay: pass entity */

    /* Epilogue + return (skip original entirely) */
    s[i++] = mips_lw(MIPS_REG_S0, 0x10, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;

    /* fallthrough_orig label = current i. Patch branches to point here. */
    int fallthrough = i;
    /* MIPS branch offset is in INSTRUCTIONS, signed, relative to PC+4
     * (PC of the delay slot). So target_insn_offset_from_branch =
     * fallthrough - (branch_pos + 1). */
    int16_t beq_off = (int16_t)(fallthrough - (beq_pos + 1));
    int16_t bne_off = (int16_t)(fallthrough - (bne_pos + 1));
    s[beq_pos] = mips_beq(MIPS_REG_T0, MIPS_REG_ZERO, beq_off);
    s[bne_pos] = mips_bne(MIPS_REG_T0, MIPS_REG_T1, bne_off);

    /* Saved original instructions + jump back. */
    s[i++] = g_hdg_orig_insn0;
    s[i++] = g_hdg_orig_insn1;
    s[i++] = mips_j(HDG_RESUME_ADDR);
    s[i++] = MIPS_NOP;

    /* Fill rest with NOP. */
    while (i < HDG_STUB_INSNS) s[i++] = MIPS_NOP;
}

void hdg_install(void)
{
    if (g_hdg_installed) return;
    /* Capture original first 2 instructions BEFORE building stub
     * (build_stub embeds them in the fallthrough path). */
    g_hdg_orig_insn0 = *(volatile uint32_t *)HDG_ROTATOR_ADDR;
    g_hdg_orig_insn1 = *(volatile uint32_t *)(HDG_ROTATOR_ADDR + 4);
    hdg_build_stub();
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    /* Patch target with j stub + nop. */
    uint32_t patch_j = mips_j((uint32_t)(uintptr_t)g_hdg_stub_buf);
    *(volatile uint32_t *)HDG_ROTATOR_ADDR = patch_j;
    *(volatile uint32_t *)(HDG_ROTATOR_ADDR + 4) = MIPS_NOP;
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    g_hdg_installed = 1;
    mhfu_log("[hdg] installed: stub=0x%08lx orig0=0x%08lx orig1=0x%08lx",
             (unsigned long)g_hdg_stub_buf,
             (unsigned long)g_hdg_orig_insn0,
             (unsigned long)g_hdg_orig_insn1);
}

void hdg_uninstall(void)
{
    if (!g_hdg_installed) return;
    *(volatile uint32_t *)HDG_ROTATOR_ADDR = g_hdg_orig_insn0;
    *(volatile uint32_t *)(HDG_ROTATOR_ADDR + 4) = g_hdg_orig_insn1;
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    g_hdg_installed = 0;
    g_hdg_enable = 0;
    mhfu_log("[hdg] uninstalled: restored, hits=%lu",
             (unsigned long)g_hdg_hit_count);
}

#endif /* MHFU_EMBED_POPO_HEADING_HOOK */

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
#if MHFU_EMBED_POPO_HEADING_HOOK
                /* Section 24: install the heading-rotator hook NOW —
                 * BEFORE the EBOOT function at 0x08865044 gets first-
                 * called and JIT-translated by PPSSPP. Live install
                 * after first JIT translation does NOT take effect.
                 * Auto-enabled (g_hdg_enable default flipped to 1 in
                 * the declaration). Stub type-checks per entity, so
                 * only popos (type 0x46) get re-routed; anteka/tigrex
                 * fall through to the original. */
                {
                    uint32_t orig_w0 = *(volatile uint32_t *)HDG_ROTATOR_ADDR;
                    /* Verify it's the expected `addiu $sp, $sp, -0x50`
                     * (encoding 0x27BDFFB0). Don't patch garbage. */
                    if (orig_w0 == 0x27BDFFB0u) {
                        g_hdg_request_install = 1;   /* mark wanted */
                        hdg_install();
                    } else {
                        mhfu_log("[hdg] 0x%08lx unexpected (got 0x%08lx, want 0x27BDFFB0); skip",
                                 (unsigned long)HDG_ROTATOR_ADDR,
                                 (unsigned long)orig_w0);
                    }
                }
#endif
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
    mhfu_on_quest_beginning(demo_on_quest_beginning);
    mhfu_on_quest_entered(demo_on_quest_entered);
    mhfu_on_map_section_entered(demo_on_map_section_entered);
    mhfu_on_monster_spawned(demo_on_monster_spawned);
#if MHFU_EMBED_POPO_GROWTH
    mhfu_on_map_section_entered(popo_on_section);
    mhfu_on_monster_spawned(popo_on_monster);
#endif
#if MHFU_EMBED_POPO_AGGRESSION
    mhfu_on_map_section_entered(agg_on_section);
    mhfu_on_monster_spawned(agg_on_monster);
#endif
/* vt[8] override install is driven by agg_reconcile_state() in the
 * popo_aggression thread (poll-based, savestate-safe). No section
 * callback registration needed here. */
    load_mods();
    sentinel_set(0x00, 0xCAFE0004);

    /* Spawn the monster-spawn polling thread BEFORE we hand off to
     * install_worker (which has its own multi-minute monitor loop).
     * The poll thread is independent of the trampolines — it just
     * watches the entity registry and fires MHFU_EVENT_MONSTER_SPAWNED
     * callbacks when slots populate. */
    SceUID th = sceKernelCreateThread(
        "mhfu_spawn_poll", monster_spawn_poll_thread,
        0x18, 0x1000, 0, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, 0, NULL);
        mhfu_log("[framework] spawn-poll thread started (uid=0x%08lx)",
                 (unsigned long)th);
    } else {
        mhfu_log("[framework] sceKernelCreateThread spawn-poll failed: %d", th);
    }

#if MHFU_EMBED_POPO_GROWTH
    {
        SceUID pth = sceKernelCreateThread(
            "popo_growth", popo_growth_thread,
            0x18, 0x1000, 0, NULL);
        if (pth >= 0) {
            sceKernelStartThread(pth, 0, NULL);
            mhfu_log("[popo_growth] embedded mod thread started (uid=0x%08lx)",
                     (unsigned long)pth);
        } else {
            mhfu_log("[popo_growth] embedded thread create failed: %d", pth);
        }
        /* Publish BSS addresses so an external runner can force-activate
         * the mod + plant a fake-spawn pointer for end-to-end verification
         * without actually being in the snow quest. */
        mhfu_log("[popo_growth] g_popo_active=0x%08lx g_tracked_popos=0x%08lx",
                 (unsigned long)&g_popo_active,
                 (unsigned long)&g_tracked_popos[0]);
        sentinel_set(0x28, (uint32_t)(uintptr_t)&g_popo_active);
        sentinel_set(0x2C, (uint32_t)(uintptr_t)&g_tracked_popos[0]);
    }
#endif
#if MHFU_EMBED_POPO_AGGRESSION
    {
        SceUID ath = sceKernelCreateThread(
            "popo_aggression", popo_aggression_thread,
            0x18, 0x1000, 0, NULL);
        if (ath >= 0) {
            sceKernelStartThread(ath, 0, NULL);
            mhfu_log("[popo_agg] embedded mod thread started (uid=0x%08lx)",
                     (unsigned long)ath);
        } else {
            mhfu_log("[popo_agg] embedded thread create failed: %d", ath);
        }
        mhfu_log("[popo_agg] g_aggression_active=0x%08lx g_aggressors=0x%08lx",
                 (unsigned long)&g_aggression_active,
                 (unsigned long)&g_aggressors[0]);
        sentinel_set(0x30, (uint32_t)(uintptr_t)&g_aggression_active);
        sentinel_set(0x34, (uint32_t)(uintptr_t)&g_aggressors[0]);
        mhfu_log("[popo_agg] cell-pin flags: heading=0x%08lx state=0x%08lx anim=0x%08lx pursue=0x%08lx",
                 (unsigned long)&g_agg_pin_heading,
                 (unsigned long)&g_agg_pin_state,
                 (unsigned long)&g_agg_pin_anim,
                 (unsigned long)&g_agg_pin_pursue);
        sentinel_set(0x48, (uint32_t)(uintptr_t)&g_agg_pin_heading);
        sentinel_set(0x4C, (uint32_t)(uintptr_t)&g_agg_pin_state);
        sentinel_set(0x50, (uint32_t)(uintptr_t)&g_agg_pin_anim);
        sentinel_set(0x54, (uint32_t)(uintptr_t)&g_agg_pin_pursue);
    }
#endif
#if MHFU_EMBED_POPO_VT8_OVERRIDE
    mhfu_log("[vt8] AUTO-INSTALL DISABLED (caused freeze v2+v3). "
             "Host opt-in: write 1 to g_vt8_request_install=0x%08lx, "
             "then optionally g_vt8_override_enable=0x%08lx",
             (unsigned long)&g_vt8_request_install,
             (unsigned long)&g_vt8_override_enable);
    mhfu_log("[vt8] g_vt8_override_anim=0x%08lx (default %u)",
             (unsigned long)&g_vt8_override_anim,
             (unsigned)VT8_DEFAULT_ANIM);
    sentinel_set(0x38, (uint32_t)(uintptr_t)&g_vt8_request_install);
    sentinel_set(0x3C, (uint32_t)(uintptr_t)&g_vt8_override_enable);
#endif
#if MHFU_EMBED_POPO_VT5_FREEZE
    mhfu_log("[vt5] heading-freeze stub READY (auto-install opt-in). "
             "Host write 1 to g_vt5_request_install=0x%08lx, "
             "then g_vt5_freeze_enable=0x%08lx",
             (unsigned long)&g_vt5_request_install,
             (unsigned long)&g_vt5_freeze_enable);
    sentinel_set(0x40, (uint32_t)(uintptr_t)&g_vt5_request_install);
    sentinel_set(0x44, (uint32_t)(uintptr_t)&g_vt5_freeze_enable);
#endif
#if MHFU_EMBED_POPO_HEADING_HOOK
    mhfu_log("[hdg] heading-rotator hook READY. "
             "Host write 1 to g_hdg_request_install=0x%08lx, "
             "then g_hdg_enable=0x%08lx. Hit counter @ 0x%08lx",
             (unsigned long)&g_hdg_request_install,
             (unsigned long)&g_hdg_enable,
             (unsigned long)&g_hdg_hit_count);
    sentinel_set(0x58, (uint32_t)(uintptr_t)&g_hdg_request_install);
    sentinel_set(0x5C, (uint32_t)(uintptr_t)&g_hdg_enable);
    sentinel_set(0x60, (uint32_t)(uintptr_t)&g_hdg_hit_count);
#endif

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
