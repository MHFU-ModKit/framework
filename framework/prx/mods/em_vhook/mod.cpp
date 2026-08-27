/*
 * em_vhook — take over a big monster's AI by patching its species VTABLE.
 *
 * A big monster's AI is a per-species MWo3 overlay (em*.ovl), and the engine
 * reaches it through a 61-slot C++ vtable in the EBOOT whose slots point into
 * overlay text. That vtable is WRITABLE at runtime and the engine re-reads the
 * slot on EVERY dispatch — both proven live (docs/EM_OVERLAY_ABI.md §9):
 *
 *   Stage A  all 9 mandatory slots match the offline trace; a sentinel written
 *            into an unused species' vtable reads back and restores.
 *   Stage B  pointing slot 29 at a `jr ra; v0=0` stub FROZE the Tigrex solid
 *            (1 distinct (main,sub,phase) in 3 s vs 2), and restoring resumed
 *            the combat ladder.
 *   Stage C  pointing slot 29 at a trampoline that TAIL-CALLS the original is
 *            indistinguishable from baseline — 5 transitions / 1.60 s mean
 *            dwell in both. Wrapping is free.
 *
 * So a mod does not need to author, compile or inject an overlay. It writes one
 * word. This mod does exactly that, for the two seams worth having (measured,
 * not assumed — tools/wrap_em_slot.py --mode count):
 *
 *   abi slot 29 (vt+0x7C)  the per-frame AI step   — 30.8 dispatches/second
 *   abi slot 32 (vt+0x88)  enter-action            — exactly 2 per (main,sub)
 *                          transition, 0 otherwise; args (entity, main, sub, mode)
 *
 * 🔴 THE STUBS ARE FRAME-FREE AND BRANCHLESS, deliberately. The engine's
 * big-monster construction-thread stack sits INSIDE the PRX image (~0x09D8A000),
 * so a C-call frame from a hook is what clobbered the Lua VM in the long
 * "Lua VM corruption" hunt. These stubs touch no stack at all: a few loads, a
 * MOVN, one store, then `j original`. Selection is done with MOVN rather than a
 * branch so the whole thing stays a single basic block.
 *
 * ⚠️ A vtable slot's VALUE is static EBOOT data, but the code behind it is only
 * valid while THAT species' overlay is resident. We therefore latch onto the
 * vtable of a big monster the engine has actually spawned, and restore on quest
 * exit — never a hardcoded species.
 */
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"

#define MOD_ID "em_vhook"

/* abi slot k lives at vptr + 8 + 4k (the vptr points at the zero
 * (offset-to-top, typeinfo) pair). See docs/EM_OVERLAY_ABI.md §2 — older notes
 * in this repo number slots as vptr+4k, so doc_slot = abi_slot + 2. */
#define VT_SLOT(k)      (8u + 4u * (k))
#define SLOT_AI_STEP    29u          /* vt+0x7C, ~30 Hz */
#define SLOT_ENTER_ACT  32u          /* vt+0x88, (entity, main, sub, mode) */

#define ENT_MAIN        0x298        /* u8 */
#define ENT_SUB         0x299        /* u8 */

/* The 17 species entity vtables all live in this EBOOT band. A sanity gate so a
 * corrupt entity pointer can never make us scribble somewhere arbitrary. */
#define VT_LO           0x089BB000u
#define VT_HI           0x089BF800u

/* Runtime-configurable so an experiment does not need a rebuild.
 * Layout is FIXED — the stubs index it by byte offset. */
typedef struct {
    uint8_t  want_main;      /* +0x00  match this (main,sub) ... */
    uint8_t  want_sub;       /* +0x01 */
    uint8_t  armed;          /* +0x02  0 = stub stores to sink only */
    uint8_t  pad;            /* +0x03 */
    uint32_t patch_off;      /* +0x04  ... then store to entity+patch_off */
    uint32_t patch_val;      /* +0x08  ... this value */
    uint32_t sink;           /* +0x0C  harmless target when unmatched */
    uint32_t ai_ticks;       /* +0x10  slot-29 dispatch counter */
    uint32_t act_enters;     /* +0x14  slot-32 dispatch counter */
    uint32_t last_pair;      /* +0x18  last (main<<8)|sub seen at enter-action */
} em_vhook_cfg_t;

static em_vhook_cfg_t g_cfg __attribute__((aligned(16)));

#define CFG_WANT_MAIN 0x00
#define CFG_WANT_SUB  0x01
#define CFG_PATCH_OFF 0x04
#define CFG_PATCH_VAL 0x08
#define CFG_SINK      0x0C
#define CFG_AI_TICKS  0x10
#define CFG_ACT_ENTER 0x14
#define CFG_LAST_PAIR 0x18

#define STUB_INSNS 24
static uint32_t g_stub_ai[STUB_INSNS]  __attribute__((aligned(64)));
static uint32_t g_stub_act[STUB_INSNS] __attribute__((aligned(64)));

static uint32_t g_vtable;            /* the species vtable we latched onto */
static uint32_t g_orig_ai;
static uint32_t g_orig_act;
static int      g_installed;

/* --- slot 29: count, then tail-call. Frame-free, branchless, 7 insns. ----- */
static void build_ai_stub(uint32_t original)
{
    uint32_t cfg = (uint32_t)(uintptr_t)&g_cfg;
    int i = 0;
    uint32_t *s = g_stub_ai;
    s[i++] = mips_lui(MIPS_REG_T7, (uint16_t)(cfg >> 16));
    s[i++] = mips_ori(MIPS_REG_T7, MIPS_REG_T7, (uint16_t)cfg);
    s[i++] = mips_lw (MIPS_REG_T0, CFG_AI_TICKS, MIPS_REG_T7);
    s[i++] = mips_addiu(MIPS_REG_T0, MIPS_REG_T0, 1);
    s[i++] = mips_sw (MIPS_REG_T0, CFG_AI_TICKS, MIPS_REG_T7);
    s[i++] = mips_j(original);
    s[i++] = MIPS_NOP;                                   /* j delay slot */
    while (i < STUB_INSNS) s[i++] = MIPS_NOP;
}

/* --- slot 32: on a matching (main,sub), store patch_val at entity+patch_off.
 *
 * Branchless selection: compute BOTH candidate store addresses and pick with
 * MOVN. On a miss the store lands in cfg.sink, which nothing reads. That keeps
 * the stub one basic block, so the JIT cannot mis-handle it, and costs one
 * pointless word write per dispatch (slot 32 fires ~0.7/s — free).
 */
static void build_act_stub(uint32_t original)
{
    uint32_t cfg = (uint32_t)(uintptr_t)&g_cfg;
    int i = 0;
    uint32_t *s = g_stub_act;
    s[i++] = mips_lui(MIPS_REG_T7, (uint16_t)(cfg >> 16));
    s[i++] = mips_ori(MIPS_REG_T7, MIPS_REG_T7, (uint16_t)cfg);

    /* bookkeeping: ++act_enters, last_pair = (main<<8)|sub */
    s[i++] = mips_lw (MIPS_REG_T0, CFG_ACT_ENTER, MIPS_REG_T7);
    s[i++] = mips_addiu(MIPS_REG_T0, MIPS_REG_T0, 1);
    s[i++] = mips_sw (MIPS_REG_T0, CFG_ACT_ENTER, MIPS_REG_T7);

    s[i++] = mips_lbu(MIPS_REG_T0, ENT_MAIN, MIPS_REG_A0);   /* live main */
    s[i++] = mips_lbu(MIPS_REG_T1, ENT_SUB,  MIPS_REG_A0);   /* live sub  */
    s[i++] = mips_sw (MIPS_REG_T1, CFG_LAST_PAIR, MIPS_REG_T7);

    s[i++] = mips_lbu(MIPS_REG_T2, CFG_WANT_MAIN, MIPS_REG_T7);
    s[i++] = mips_lbu(MIPS_REG_T3, CFG_WANT_SUB,  MIPS_REG_T7);
    s[i++] = mips_xor(MIPS_REG_T0, MIPS_REG_T0, MIPS_REG_T2);
    s[i++] = mips_xor(MIPS_REG_T1, MIPS_REG_T1, MIPS_REG_T3);
    s[i++] = mips_or (MIPS_REG_T0, MIPS_REG_T0, MIPS_REG_T1); /* 0 iff match */
    s[i++] = mips_sltiu(MIPS_REG_T2, MIPS_REG_T0, 1);         /* 1 iff match */

    s[i++] = mips_lw (MIPS_REG_T4, CFG_PATCH_OFF, MIPS_REG_T7);
    s[i++] = mips_lw (MIPS_REG_T5, CFG_PATCH_VAL, MIPS_REG_T7);
    s[i++] = mips_addu(MIPS_REG_T4, MIPS_REG_A0, MIPS_REG_T4); /* &entity[off] */
    s[i++] = mips_addiu(MIPS_REG_T6, MIPS_REG_T7, CFG_SINK);   /* &cfg.sink   */
    s[i++] = mips_movn(MIPS_REG_T6, MIPS_REG_T4, MIPS_REG_T2); /* pick on match */
    s[i++] = mips_sw (MIPS_REG_T5, 0, MIPS_REG_T6);

    s[i++] = mips_j(original);
    s[i++] = MIPS_NOP;                                   /* j delay slot */
    while (i < STUB_INSNS) s[i++] = MIPS_NOP;
}

/* Public control surface — a mod (or the Lua host) can retarget the override
 * without a rebuild, because the stubs read the config every dispatch. */
extern "C" void em_vhook_arm(uint8_t main_state, uint8_t sub_state,
                             uint32_t off, uint32_t val)
{
    g_cfg.want_main = main_state;
    g_cfg.want_sub  = sub_state;
    g_cfg.patch_off = off;
    g_cfg.patch_val = val;
    g_cfg.armed     = 1;
    mhfu_log("[%s] armed: (%u,%u) -> entity+0x%X = %u",
             MOD_ID, main_state, sub_state, (unsigned)off, (unsigned)val);
}

extern "C" void em_vhook_stats(uint32_t *ai, uint32_t *acts, uint32_t *last)
{
    if (ai)   *ai   = g_cfg.ai_ticks;
    if (acts) *acts = g_cfg.act_enters;
    if (last) *last = g_cfg.last_pair;
}

static void install_for(uint32_t entity)
{
    if (g_installed || !entity) return;
    uint32_t vt = mhfu_read_u32(entity);
    if (vt < VT_LO || vt >= VT_HI) {
        mhfu_log("[%s] entity 0x%08X vtable 0x%08X outside the species band "
                 "- not a big monster, skipping", MOD_ID,
                 (unsigned)entity, (unsigned)vt);
        return;
    }
    g_vtable  = vt;
    g_orig_ai  = mhfu_read_u32(vt + VT_SLOT(SLOT_AI_STEP));
    g_orig_act = mhfu_read_u32(vt + VT_SLOT(SLOT_ENTER_ACT));

    build_ai_stub(g_orig_ai);
    build_act_stub(g_orig_act);
    mhfu_flush_caches();

    /* Data writes into EBOOT rodata — NOT code patches, so the JIT's
     * already-translated-function trap does not apply and no quiet window is
     * needed (docs/EM_OVERLAY_ABI.md §9, CLAUDE.md "vtable swaps are immune"). */
    mhfu_write_u32(vt + VT_SLOT(SLOT_AI_STEP),   (uint32_t)(uintptr_t)g_stub_ai);
    mhfu_write_u32(vt + VT_SLOT(SLOT_ENTER_ACT), (uint32_t)(uintptr_t)g_stub_act);
    g_installed = 1;

    mhfu_log("[%s] vtable 0x%08X: slot29 0x%08X -> 0x%08X, slot32 0x%08X -> 0x%08X",
             MOD_ID, (unsigned)vt, (unsigned)g_orig_ai,
             (unsigned)(uintptr_t)g_stub_ai, (unsigned)g_orig_act,
             (unsigned)(uintptr_t)g_stub_act);
}

static void uninstall(void)
{
    if (!g_installed) return;
    mhfu_write_u32(g_vtable + VT_SLOT(SLOT_AI_STEP),   g_orig_ai);
    mhfu_write_u32(g_vtable + VT_SLOT(SLOT_ENTER_ACT), g_orig_act);
    g_installed = 0;
    mhfu_log("[%s] restored vtable 0x%08X (ai_ticks=%u act_enters=%u)",
             MOD_ID, (unsigned)g_vtable,
             (unsigned)g_cfg.ai_ticks, (unsigned)g_cfg.act_enters);
}

static void on_spawn(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (!ctx) return;
    install_for(ctx->entity_ptr);
}

static void on_quest(const mhfu_event_ctx_t *ctx)
{
    (void)ctx;
    /* A new quest may load a DIFFERENT species overlay, so the slots we saved
     * no longer describe resident code. Drop the hook and re-latch on the next
     * big-monster spawn. */
    uninstall();
    g_cfg.ai_ticks = g_cfg.act_enters = 0;
}

static int em_vhook_init(void)
{
    g_cfg.patch_off = 0x414;     /* the action countdown; see em_phase_map.py */
    g_cfg.patch_val = 900;
    g_cfg.want_main = 0xFF;      /* matches nothing until armed */
    g_cfg.want_sub  = 0xFF;
    mhfu_on_monster_spawned(on_spawn);
    mhfu_on_quest_beginning(on_quest);
    mhfu_log("[%s] ready; cfg @0x%08X, stubs @0x%08X / 0x%08X", MOD_ID,
             (unsigned)(uintptr_t)&g_cfg,
             (unsigned)(uintptr_t)g_stub_ai, (unsigned)(uintptr_t)g_stub_act);
    return 0;
}

static void em_vhook_shutdown(void) { uninstall(); }

MHFU_MOD(.id = MOD_ID, .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = em_vhook_init, .shutdown = em_vhook_shutdown);
