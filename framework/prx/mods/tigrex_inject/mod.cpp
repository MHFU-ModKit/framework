/*
 * tigrex_inject — load a non-resident monster into a running quest by
 * editing the quest's native big-monster list right after the engine
 * builds it, so the engine itself does the full load+unpack+VRAM+spawn.
 *
 * Mechanism (Section 31, verified 2026-05-29 — see
 * docs/QUEST_LOADING_LIFECYCLE.md): redirect the single
 * `jal buildTargets` at 0x08869EEC to a branchless wrapper stub. The
 * stub runs an add/replace helper that retags the quest's list-A node
 * to Tigrex (REPLACE, default) before the loading-screen model-load
 * reads it, calls the real buildTargets, then a split helper (ADD mode).
 * A poll thread calms (Tigrex sight radius -> 0, engage -> 0) + resizes.
 *
 * Install is gated on a TITLE/MENU screen-state (Section 26) so the patch
 * lands before PPSSPP's JIT pre-caches the target block. The JAL-site
 * patch is claimed through mhfu_patch_word("tigrex_inject", ...) so the
 * framework restores it on shutdown.
 */
#include <pspthreadman.h>

#include "mhfu/mhfu.h"
#include "mhfu/mips.h"

#define MOD_ID "tigrex_inject"

#define QINJ_BUILD_JAL_SITE  0x08869EECu  /* `jal buildTargets` */
#define QINJ_BUILDTARGETS    0x0886D4D8u
#define QINJ_ADDTARGET       0x0886A4CCu  /* addTarget(Quest, idx, record) */
#define QINJ_TARGETS_OFF     0x768u
#define QINJ_TARGET_STRIDE   0x2Cu
#define QINJ_EMID_OFF        0x14u
#define QINJ_COUNT_OFF       0x1Eu
#define QINJ_RECBASE_OFF     0x50u
#define QINJ_NODE_STRIDE     0x10u
#define QINJ_NODE_HDROFF     0x08u
#define QINJ_NODE_RECOFF     0x0Cu
#define QINJ_FREE_REC        0x1C00u
#define QINJ_FREE_HDR        0x1C80u
#define QINJ_MONHDR_COPY     0x10u
#define QINJ_MAX_BIGMON      4
#define QINJ_REC_X_OFF       0x20u
#define QINJ_REC_Z_OFF       0x28u
#define QINJ_GIADROME_EMID   0x4D
#define QINJ_TIGREX_EMID     0x4B
#define QINJ_TIGREX_SIGHT    0x09BC1030u  /* Tigrex species sight/detect radius */
#define QINJ_REC_COPY        0x60u

/* Native snow-map big-monster spawn (valid spawn tile). Section 1 init
 * spawn crashes (no big-monster tile there); they reach section 1 by
 * roaming, and rendering one in section 1 needs draw-chain RE. */
#define QINJ_TIGREX_SPAWN_X  9548.0f
#define QINJ_TIGREX_SPAWN_Z  6834.0f
#define QINJ_SECTION1_X      15940.0f
#define QINJ_SECTION1_Z      10536.0f

#define QINJ_STUB_INSNS      16
#define QINJ_REC_WORDS       0x20

static const uint32_t g_size_offs[5] = {0x024, 0x220, 0x224, 0x228, 0x270};

typedef void (*qinj_addtarget_fn)(uint32_t quest, int idx, void *record);

volatile uint32_t g_qinj_stub_buf[QINJ_STUB_INSNS];
static volatile uint32_t g_qinj_rec[QINJ_REC_WORDS];   /* persists; engine holds its ptr */

static volatile int   g_qinj_enable   = 1;
static volatile int   g_qinj_require_giadrome = 1;
static volatile int   g_qinj_force_coords = 0;   /* section-1 spawn = invisible+inert */
static volatile int   g_qinj_replace  = 1;       /* retag Giadrome -> Tigrex (1 monster) */
static volatile int   g_qinj_installed = 0;
static volatile int   g_qinj_done      = 0;
static volatile uint32_t g_qinj_hits   = 0;
static volatile uint32_t g_qinj_rec_src = 0;
static volatile int   g_qinj_t1_emid   = -1;
static volatile int   g_qinj_split_done = 0;
static volatile int   g_qinj_prep_done  = 0;
static volatile int   g_qinj_calm      = 1;
static volatile int   g_qinj_touch_entity = 1;
static volatile int   g_qinj_resize    = 1;
static volatile float g_qinj_size      = 0.5f;
static volatile uint32_t g_qinj_tigrex_ent = 0;
static volatile int   g_qinj_tame_state = 5;

static inline int qinj_in_ram(uint32_t a) { return a >= 0x08800000u && a < 0x0A000000u; }

/* Add a Tigrex node to big-monster list A in the parsed quest buffer at
 * recb. 0 = N/A, 1 = already present, 2 = added. */
static int qinj_add_node_at(uint32_t recb)
{
    if (!qinj_in_ram(recb)) return 0;
    uint32_t listA = recb + *(volatile uint32_t *)(recb + 0x14);
    if (!qinj_in_ram(listA)) return 0;
    uint32_t n0_hdr = *(volatile uint32_t *)(listA + QINJ_NODE_HDROFF);
    uint32_t n0_rec = *(volatile uint32_t *)(listA + QINJ_NODE_RECOFF);
    if (n0_hdr == 0 || n0_rec == 0) return 0;
    uint32_t rec0 = recb + n0_rec;
    if (!qinj_in_ram(rec0)) return 0;
    if (g_qinj_require_giadrome && (*(volatile uint16_t *)rec0 & 0xFF) != QINJ_GIADROME_EMID)
        return 0;

    uint32_t node = listA; int idx = 0;
    while (*(volatile uint32_t *)(node + QINJ_NODE_HDROFF) != 0 && idx < 6) {
        uint32_t roff = *(volatile uint32_t *)(node + QINJ_NODE_RECOFF);
        if (roff && (*(volatile uint16_t *)(recb + roff) & 0xFF) == QINJ_TIGREX_EMID)
            return 1;
        node += QINJ_NODE_STRIDE; idx++;
    }
    if (idx == 0 || idx >= QINJ_MAX_BIGMON) return 0;

    uint32_t trec = recb + QINJ_FREE_REC;
    uint32_t thdr = recb + QINJ_FREE_HDR;
    for (uint32_t i = 0; i < QINJ_REC_COPY; i++)
        *(volatile uint8_t *)(trec + i) = *(volatile uint8_t *)(rec0 + i);
    *(volatile uint16_t *)(trec + 0x00) = (uint16_t)QINJ_TIGREX_EMID;
    *(volatile uint8_t  *)(trec + 0x05) = 0x04;
    *(volatile uint32_t *)(trec + 0x08) = 0x6D;
    *(volatile uint16_t *)(trec + 0x1C) = 0xF692;
    *(volatile uint16_t *)(trec + 0x32) = 0x0960;
    *(volatile float *)(trec + QINJ_REC_X_OFF) = QINJ_TIGREX_SPAWN_X;
    *(volatile float *)(trec + QINJ_REC_Z_OFF) = QINJ_TIGREX_SPAWN_Z;
    if (g_qinj_force_coords) {
        *(volatile float *)(trec + QINJ_REC_X_OFF) = QINJ_SECTION1_X;
        *(volatile float *)(trec + QINJ_REC_Z_OFF) = QINJ_SECTION1_Z;
    }
    for (uint32_t i = 0; i < QINJ_MONHDR_COPY; i++)
        *(volatile uint8_t *)(thdr + i) = *(volatile uint8_t *)(recb + n0_hdr + i);
    *(volatile uint16_t *)(thdr + 0x00) = (uint16_t)QINJ_TIGREX_EMID;

    *(volatile uint32_t *)(node + 0x00)             = 1;
    *(volatile uint32_t *)(node + 0x04)             = 0;
    *(volatile uint32_t *)(node + QINJ_NODE_HDROFF) = QINJ_FREE_HDR;
    *(volatile uint32_t *)(node + QINJ_NODE_RECOFF) = QINJ_FREE_REC;
    uint32_t endn = node + QINJ_NODE_STRIDE;
    for (int k = 0; k < 4; k++) *(volatile uint32_t *)(endn + k * 4) = 0;
    g_qinj_rec_src = rec0;
    return 2;
}

/* REPLACE: retag the quest's existing big-monster node (Giadrome) ->
 * Tigrex in place. 1 big monster, native coords, no spawn-cap overflow. */
static int qinj_replace_node0(uint32_t recb)
{
    if (!qinj_in_ram(recb)) return 0;
    uint32_t listA = recb + *(volatile uint32_t *)(recb + 0x14);
    if (!qinj_in_ram(listA)) return 0;
    uint32_t n0_hdr = *(volatile uint32_t *)(listA + QINJ_NODE_HDROFF);
    uint32_t n0_rec = *(volatile uint32_t *)(listA + QINJ_NODE_RECOFF);
    if (n0_hdr == 0 || n0_rec == 0) return 0;
    uint32_t rec0 = recb + n0_rec;
    if (!qinj_in_ram(rec0)) return 0;
    int em0 = *(volatile uint16_t *)rec0 & 0xFF;
    if (em0 == QINJ_TIGREX_EMID) return 1;
    if (g_qinj_require_giadrome && em0 != QINJ_GIADROME_EMID) return 0;
    *(volatile uint16_t *)(rec0 + 0x00) = (uint16_t)QINJ_TIGREX_EMID;
    *(volatile uint8_t  *)(rec0 + 0x05) = 0x04;
    *(volatile uint32_t *)(rec0 + 0x08) = 0x6D;
    *(volatile uint16_t *)(rec0 + 0x1C) = 0xF692;
    *(volatile uint16_t *)(rec0 + 0x32) = 0x0960;
    *(volatile uint16_t *)(recb + n0_hdr) = (uint16_t)QINJ_TIGREX_EMID;
    if (g_qinj_force_coords) {
        *(volatile float *)(rec0 + QINJ_REC_X_OFF) = QINJ_SECTION1_X;
        *(volatile float *)(rec0 + QINJ_REC_Z_OFF) = QINJ_SECTION1_Z;
    }
    g_qinj_rec_src = rec0;
    return 2;
}

/* PREP: add the node to the parsed buffer (literal rec_base) as soon as
 * the ".mib" is parsed ("2NDG" magic), before the model-load reads it.
 * (ADD mode only; REPLACE is handled in the begin hook.) */
static void qinj_prep_inject(void)
{
    if (!g_qinj_enable || g_qinj_prep_done || g_qinj_replace) return;
    uint32_t recb = 0x08A5C440u;
    if (*(volatile uint32_t *)(recb + 0x04) != 0x47444E32u) return;  /* "2NDG" */
    int r = qinj_add_node_at(recb);
    if (r >= 1) {
        g_qinj_prep_done = 1;
        mhfu_log("[qinj] PREP %s rec_base=0x%08lx",
                 r == 2 ? "INJECTED node" : "already present", (unsigned long)recb);
    }
}

/* BEGIN prefix (before buildTargets): ensure the node exists / retag. */
extern "C" void mhfu_quest_inject_helper(uint32_t quest)
{
    g_qinj_hits++;
    if (!g_qinj_enable || g_qinj_done) return;
    if (!qinj_in_ram(quest)) return;
    uint32_t recb = *(volatile uint32_t *)(quest + QINJ_RECBASE_OFF);
    int r = g_qinj_replace ? qinj_replace_node0(recb) : qinj_add_node_at(recb);
    if (r == 0) return;
    g_qinj_t1_emid = QINJ_TIGREX_EMID;
    g_qinj_done = 1;
    mhfu_log("[qinj] BEGIN %s r=%d rec_base=0x%08lx",
             g_qinj_replace ? "REPLACE" : "ADD", r, (unsigned long)recb);
}

/* POSTFIX (after buildTargets): in ADD mode, move the Tigrex into its own
 * target group so the spawner actually spawns it. */
extern "C" void mhfu_quest_split_helper(uint32_t quest)
{
    if (!g_qinj_enable || g_qinj_replace) return;
    if (!qinj_in_ram(quest)) return;
    uint32_t t0 = quest + QINJ_TARGETS_OFF;
    if (*(volatile int8_t *)(t0 + QINJ_COUNT_OFF) != 2) return;
    uint32_t trec = *(volatile uint32_t *)(t0 + 0x04);
    if (!qinj_in_ram(trec)) return;
    if ((*(volatile uint16_t *)trec & 0xFF) != QINJ_TIGREX_EMID) return;

    *(volatile uint32_t *)(t0 + 0x04) = 0;
    *(volatile uint8_t  *)(t0 + 0x18) = 0;
    *(volatile uint8_t  *)(t0 + 0x1D) = 0;
    *(volatile int8_t   *)(t0 + QINJ_COUNT_OFF) = 1;
    ((qinj_addtarget_fn)QINJ_ADDTARGET)(quest, 1, (void *)trec);

    g_qinj_split_done = 1;
    g_qinj_t1_emid = *(volatile int8_t *)(quest + QINJ_TARGETS_OFF
                                          + QINJ_TARGET_STRIDE + QINJ_EMID_OFF) & 0xFF;
    mhfu_log("[qinj] SPLIT: group0->1 trec=0x%08lx target1.emId=0x%02x",
             (unsigned long)trec, g_qinj_t1_emid);
}

static void qinj_build_stub(void)
{
    uint32_t *s = (uint32_t *)g_qinj_stub_buf;
    int i = 0;
    uint32_t helper  = (uint32_t)(uintptr_t)&mhfu_quest_inject_helper;
    uint32_t helperB = (uint32_t)(uintptr_t)&mhfu_quest_split_helper;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(helper);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(QINJ_BUILDTARGETS);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(helperB);
    s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
    while (i < QINJ_STUB_INSNS) s[i++] = MIPS_NOP;
}

static void qinj_install(void)
{
    if (g_qinj_installed) return;
    qinj_build_stub();
    mhfu_flush_caches();
    uint32_t patch = mips_jal((uint32_t)(uintptr_t)g_qinj_stub_buf);
    if (mhfu_patch_word(QINJ_BUILD_JAL_SITE, patch, MOD_ID) != MHFU_HOOK_OK) {
        mhfu_log("[qinj] patch refused (conflict?)");
        return;
    }
    g_qinj_installed = 1;
    mhfu_log("[qinj] INSTALLED: JAL@0x%08x -> stub 0x%08lx",
             QINJ_BUILD_JAL_SITE, (unsigned long)(uintptr_t)g_qinj_stub_buf);
}

/* Calm + resize the spawned Tigrex (memory tigrex-aggro-disable +
 * monster-size-scalar). Cheap; safe to call every in-quest poll tick. */
static void qinj_calm_resize(void)
{
    if (g_qinj_calm) *(volatile float *)QINJ_TIGREX_SIGHT = 0.0f;
    if (!g_qinj_touch_entity) return;
    static const uint32_t det_offs[6] = {0x064C, 0x0650, 0x0654, 0x067C, 0x0680, 0x0684};
    for (int slot = 1; slot < MHFU_ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = mhfu_entity_at(slot);
        if (!qinj_in_ram(p)) continue;
        uint8_t t = mhfu_entity_type(p);
        if (g_qinj_calm) {
            for (int k = 0; k < 6; k++) *(volatile float *)(p + det_offs[k]) = 0.0f;
            *(volatile float *)(p + MHFU_ENT_ENGAGE_FLAG) = 0.0f;
        }
        if (t == QINJ_TIGREX_EMID) {
            g_qinj_tigrex_ent = p;
            *(volatile uint8_t  *)(p + MHFU_ENT_AI_STATE) = (uint8_t)g_qinj_tame_state;
            *(volatile uint32_t *)(p + 0x322) = 0;   /* stimTag */
            *(volatile uint32_t *)(p + 0x6D8) = 0;   /* fleeState */
            if (g_qinj_resize)
                for (int k = 0; k < 5; k++) *(volatile float *)(p + g_size_offs[k]) = g_qinj_size;
        }
    }
}

static int tigrex_inject_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_log("[qinj] quest-target inject thread started");
    for (;;) {
        int fast = (g_qinj_installed && !g_qinj_prep_done && !g_qinj_replace) || g_qinj_done;
        sceKernelDelayThread((fast ? 50 : 300) * 1000);
        if (!g_qinj_enable) continue;
        uint8_t scr = mhfu_get_screen_state();

        if (g_qinj_installed && !g_qinj_prep_done) qinj_prep_inject();

        /* Install gate (Section 26): patch only at TITLE(0x04)/MENU(0x01)
         * so the JIT hasn't cached buildTargets' caller yet. Verify the
         * expected `jal buildTargets` is present (savestate restore lag). */
        if (!g_qinj_installed && (scr == 0x01 || scr == 0x04)) {
            uint32_t w = *(volatile uint32_t *)QINJ_BUILD_JAL_SITE;
            if (w == mips_jal(QINJ_BUILDTARGETS)) qinj_install();
        }

        if (scr == 17 && g_qinj_done) qinj_calm_resize();
    }
    return 0;
}

static int tigrex_init(void)
{
    SceUID th = sceKernelCreateThread("mhfu_tigrex",
                                      (SceKernelThreadEntry)tigrex_inject_thread,
                                      0x18, 0x1000, 0, NULL);
    if (th < 0) { mhfu_log("[qinj] thread create failed: %d", th); return -1; }
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

static void tigrex_shutdown(void)
{
    g_qinj_enable = 0;   /* JAL-site patch is restored by the framework (mhfu_unhook_owner) */
}

MHFU_MOD(.id = MOD_ID, .version = "1.0",
         .needs = 0, .conflicts = "popo_heading_hook",
         .init = tigrex_init, .shutdown = tigrex_shutdown);
