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

#define MHFU_EMBED_POPO_GROWTH 1

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
    mhfu_on_quest_beginning(demo_on_quest_beginning);
    mhfu_on_quest_entered(demo_on_quest_entered);
    mhfu_on_map_section_entered(demo_on_map_section_entered);
    mhfu_on_monster_spawned(demo_on_monster_spawned);
#if MHFU_EMBED_POPO_GROWTH
    mhfu_on_map_section_entered(popo_on_section);
    mhfu_on_monster_spawned(popo_on_monster);
#endif
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
