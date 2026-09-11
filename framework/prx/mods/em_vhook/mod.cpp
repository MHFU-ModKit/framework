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
 *   abi slot 32 (vt+0x88)  enter-action            — the call that ENTERS a
 *                          behaviour pair; args (entity, main, id, mode)
 *
 * v3.0 (issues #15 and #16) puts four data-driven capabilities on them. The
 * stubs read a config block on every dispatch, so all of it is retargetable
 * from Lua without a rebuild; the public surface is include/mhfu/em_vhook.h.
 *
 *   SUBSTITUTION  slot-32 PRE   the engine's own (main, id) is rewritten to a
 *                 declared pair BEFORE the species enter-action runs. That is
 *                 the whole point: enter-action is where a pair is PROVISIONED
 *                 (em75's main-1 translator sets the charge's run budget
 *                 +0x76C, then act_set writes +0x298/+0x299), so a substituted
 *                 charge ends into the skid the way a native one does instead
 *                 of parking for 38 s with its hitbox spent — which is what a
 *                 pair written straight into the cells did (tools/em_chain.py).
 *   REQUEST       slot-29 PRE   a pair Lua wants entered now, issued on the
 *                 game thread inside the next AI frame through the engine's
 *                 own dispatcher 0x09AC89F0 (so, provisioned too).
 *   RULES         slot-29 PRE   a native 30 Hz brain: "in pair P for N frames,
 *                 player at [lo,hi), receding -> enter Q", cooldown, budget.
 *   BUDGET        slot-32 POST + slot-29 one-shot: the +0x414 override of v2,
 *                 unchanged (§13 of the ABI doc for why post, and why one-shot).
 *
 * 🔴 WHY THE PRE-HOOK REWRITES THE ARGUMENTS AND NOT THE CELLS. Issue #15 was
 * filed saying the stub "must write both". Reading the enter-action end to end
 * (em_chain.py, 2026-09-11) says otherwise: on the normal path the arguments
 * lead — the translator calls act_set, which is the only writer of the cells,
 * from the (rewritten) arguments; neither enter-action 0x09D3D608 nor any
 * translator reads +0x298/+0x299 first. The "cells lead" trace in §6b is the
 * snapshot-RESTORE path (0x09AC7258), not a decision. And enter-action has an
 * early return with no act_set (mode 3/4 while pinned, +0x6DB == 0): a stub
 * that pre-wrote the cells there would leave the phase machine running a pair
 * nobody provisioned — exactly the Lua failure this seam exists to end. So the
 * stub rewrites a1/a2 only, and counts, after the call, whether the cells took
 * our pair (`sub_landed`). The phase machine and the enter-action agree because
 * one function wrote both.
 *
 * 🔴 BOTH STUBS ARE BRANCHLESS; the slot-29 one is also FRAME-FREE. The engine's
 * big-monster construction-thread stack sits INSIDE the PRX image (~0x09D8A000),
 * so a C-call frame from a hook is what clobbered the Lua VM in the long
 * "Lua VM corruption" hunt. Every decision is a MOVN/MOVZ select so each stub
 * is a single basic block; the slot-29 stub's one call (the request / a rule
 * firing) is a `jalr` whose TARGET is selected — the engine's dispatcher or a
 * `jr ra` in our own block — so the jalr always executes, and its ra and the
 * step's arguments are spilled to config words, never to the stack. The
 * slot-32 stub keeps v2's 16-byte hand-written frame (see build_act_stub in
 * stubs.h for why calling the original first is mandatory there).
 *
 * ⚠️ A vtable slot's VALUE is static EBOOT data, but the code behind it is only
 * valid while THAT species' overlay is resident. We therefore latch onto the
 * vtable of a big monster the engine has actually spawned, and restore on quest
 * exit — never a hardcoded species. Every rule, substitution and request is
 * dropped at the same moment: a new quest's script re-declares them at spawn.
 *
 * ⚠️ One config block per LATCHED VTABLE, not per entity. Two live monsters of
 * the latched species would share the dwell counter and the distance; the
 * ports this serves run one.
 */
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"
#include "mhfu/em_vhook.h"
#include "stubs.h"
#include <pspsysmem.h>
#include <stddef.h>

#define MOD_ID "em_vhook"

/* abi slot k lives at vptr + 8 + 4k (the vptr points at the zero
 * (offset-to-top, typeinfo) pair). See docs/EM_OVERLAY_ABI.md §2 — older notes
 * in this repo number slots as vptr+4k, so doc_slot = abi_slot + 2. */
#define VT_SLOT(k)      (8u + 4u * (k))
#define SLOT_AI_STEP    29u          /* vt+0x7C, ~30 Hz */
#define SLOT_ENTER_ACT  32u          /* vt+0x88, (entity, main, id, mode) */

/* The 17 species entity vtables all live in this EBOOT band. A sanity gate so a
 * corrupt entity pointer can never make us scribble somewhere arbitrary. */
#define VT_LO           0x089BB000u
#define VT_HI           0x089BF800u

/* The config block, field for field the layout stubs.h indexes by offset. */
typedef struct {
    uint8_t  from_mask, from_sub, to_main, to_sub;
    uint32_t left;
} cfg_sub_t;
typedef struct {
    uint8_t  from_mask, from_sub, to_main, to_sub, mode, flags, _pad0, _pad1;
    uint32_t min_frames, d2_lo, d2_hi, left, fired, last_fire, cooldown, _pad2;
} cfg_rule_t;
typedef struct {
    uint8_t  want_main, want_sub, armed, arm29;    /* +0x00 */
    uint32_t patch_off, patch_val, sink;            /* +0x04 */
    uint32_t ai_ticks, act_enters, last_pair;       /* +0x10 */
    uint32_t canary;                                /* +0x1C */
    uint32_t prev_pair, prev2_pair;                 /* +0x20 */
    uint32_t frames;                                /* +0x28 */
    uint32_t ra_spill, a_spill[4];                  /* +0x2C */
    uint32_t d2, d2_prev, brain_fires, scratch;     /* +0x40 */
    uint32_t req_pending;                           /* +0x50 */
    uint8_t  req_main, req_sub, req_mode, _pad;     /* +0x54 */
    uint32_t req_done, req_result;                  /* +0x58 */
    cfg_sub_t subs[EM_VHOOK_SUBS];                  /* +0x60 */
    uint32_t sub_hits, sub_landed, sub_last_in;     /* +0x80 */
    uint32_t sub_pending, sub_to_pending;           /* +0x8C */
    uint32_t ring_idx;                              /* +0x94 */
    uint32_t ring[EM_VHOOK_RING];                   /* +0x98 */
    cfg_rule_t rules[EM_VHOOK_RULES];               /* +0xB8 */
} em_vhook_cfg_t;

static_assert(offsetof(em_vhook_cfg_t, prev_pair)   == CFG_PREV,        "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, frames)      == CFG_FRAMES,      "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, ra_spill)    == CFG_RA_SPILL,    "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, a_spill)     == CFG_A0_SPILL,    "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, d2)          == CFG_D2,          "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, scratch)     == CFG_SCRATCH,     "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, req_pending) == CFG_REQ_PENDING, "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, req_main)    == CFG_REQ_MAIN,    "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, req_result)  == CFG_REQ_RESULT,  "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, subs)        == CFG_SUB_BASE,    "cfg layout");
static_assert(sizeof(cfg_sub_t)                     == SUB_STRIDE,      "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, sub_hits)    == CFG_SUB_HITS,    "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, sub_to_pending) == CFG_SUB_TO_PEND, "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, ring_idx)    == CFG_RING_IDX,    "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, ring)        == CFG_RING,        "cfg layout");
static_assert(offsetof(em_vhook_cfg_t, rules)       == CFG_RULE_BASE,   "cfg layout");
static_assert(sizeof(cfg_rule_t)                    == RULE_STRIDE,     "cfg layout");
static_assert(offsetof(cfg_rule_t, min_frames)      == RULE_MIN_FRAMES, "cfg layout");
static_assert(offsetof(cfg_rule_t, cooldown)        == RULE_COOLDOWN,   "cfg layout");
static_assert(sizeof(em_vhook_cfg_t)                == CFG_SIZE,        "cfg layout");

#define CFG_CANARY_VAL 0x5645484Bu   /* 'VEHK' */

/* 🔴 THE STUBS AND THE CONFIG MUST NOT LIVE IN THE PRX IMAGE.
 *
 * v1.0 put them in .bss, which linked them near the TOP of the image
 * ([0x09D65000,0x09DD9000) -> stubs at ~0x09DD1E00). The engine parks thread
 * stacks inside that image, and a stack based at the top growing DOWN reaches
 * 0x09DD1E00 after only ~0x7000 bytes. Measured consequence: rock stable while
 * the hunter idled in base camp (shallow stack), then PPSSPP spun at 131% CPU on
 * one run and exited outright on another, both during heavy activity. Nothing
 * was wrong with the stub logic — its bytes were being overwritten by stack.
 *
 * The debugger-driven proofs (Stage B/C) never saw this because they wrote their
 * stub to 0x08A5E000, outside the PRX. So: allocate from the user partition,
 * exactly as entity.cpp does for clones, and keep only pointers here. */
#define RET_INSNS   4     /* jr ra; nop, padded to 16 bytes */
#define BLOCK_BYTES ((STUB_AI_INSNS + STUB_ACT_INSNS + RET_INSNS) * 4 + CFG_SIZE + 128)

static uint32_t *g_stub_ai;
static uint32_t *g_stub_act;
static uint32_t *g_stub_ret;
static em_vhook_cfg_t *g_cfgp;
static SceUID g_block = -1;

static void cfg_reset_live(void)
{
    /* everything the stubs derive from the running monster */
    g_cfgp->prev_pair = g_cfgp->prev2_pair = 0xFFFFFFFFu;  /* (0,0) packs to 0: a
        zeroed history would read as "in (0,0) for two ticks" and fire the
        one-shot on install */
    g_cfgp->frames = 0;
    g_cfgp->d2 = g_cfgp->d2_prev = 0;
    g_cfgp->sub_pending = g_cfgp->sub_to_pending = 0;
}

static int alloc_block(void)
{
    if (g_block >= 0) return 0;
    /* 🔴 LOW, not High. PSP_SMEM_High landed the block at 0x0BFFFD00 — inside
     * PPSSPP's raw extra-RAM window (0x0B000000..0x0C000000, what inject.cpp
     * uses for its xram copy). Stubs EXECUTED from there fine while unarmed,
     * but the moment the (branchless) store retargeted from the in-block sink
     * to entity+0x414 in normal RAM, PPSSPP EXITED — twice, reproducibly. The
     * identical store from a stub at 0x08A5E000 (normal RAM) had already run a
     * full A/B/A without trouble in tools/wrap_em_slot.py. Keep the stub in
     * ordinary user RAM; never in the extra-RAM window. */
    g_block = sceKernelAllocPartitionMemory(2, "em_vhook", PSP_SMEM_Low,
                                            BLOCK_BYTES, 0);
    if (g_block < 0) {
        mhfu_log("[%s] partition alloc FAILED (%d) - refusing to install",
                 MOD_ID, (int)g_block);
        return -1;
    }
    uint8_t *base = (uint8_t *)sceKernelGetBlockHeadAddr(g_block);
    if ((uintptr_t)base >= 0x0A000000u) {
        mhfu_log("[%s] block at 0x%08X is in the extra-RAM window - refusing",
                 MOD_ID, (unsigned)(uintptr_t)base);
        sceKernelFreePartitionMemory(g_block);
        g_block = -1;
        return -1;
    }
    base = (uint8_t *)(((uintptr_t)base + 63) & ~(uintptr_t)63);
    g_stub_ai  = (uint32_t *)base;
    g_stub_act = (uint32_t *)(base + STUB_AI_INSNS * 4);
    g_stub_ret = (uint32_t *)(base + (STUB_AI_INSNS + STUB_ACT_INSNS) * 4);
    g_cfgp     = (em_vhook_cfg_t *)(base + (STUB_AI_INSNS + STUB_ACT_INSNS + RET_INSNS) * 4);
    for (unsigned k = 0; k < sizeof(*g_cfgp) / 4; k++)
        ((uint32_t *)g_cfgp)[k] = 0;
    cfg_reset_live();
    mhfu_log("[%s] block @0x%08X (outside the PRX image), cfg @0x%08X",
             MOD_ID, (unsigned)(uintptr_t)base, (unsigned)(uintptr_t)g_cfgp);
    return 0;
}

static uint32_t g_vtable;            /* the species vtable we latched onto */
static uint32_t g_orig_ai;
static uint32_t g_orig_act;
static int      g_installed;

/* --- slot 29: count, request, rules, one-shot budget, then tail-call. ------
 *
 * Why the budget seam exists at all. A slot-32 post-hook owns `entity+0x414`
 * for 15 of the 27 timer-gated actions; the other 12 re-seed it in their
 * handler's **phase-0 block**, which runs on the FIRST slot-29 tick — one frame
 * after any slot-32 hook can write. Measured: armed on (2,ANY) -> 1234, `(2,24)`
 * held our value 13/13 while `(2,16)` counted down from its own 150.
 * `em_phase_map.py <ovl> --budget-owner` names both sets.
 *
 * 🔴 THE ONE-SHOT IS THE DESIGN. Re-asserting the budget every frame would not
 * "set the duration", it would FREEZE it — the handler decrements, we put it
 * back, and the action never ends. So we write on exactly ONE tick per action
 * instance: the SECOND one. Tick 1 is the phase-0 tick, where the handler seeds
 * its own literal AFTER us (this stub is a pre-hook); tick 2 is the first
 * phase-1 tick, which only decrements, so our value lands and then counts down
 * naturally from there.
 *
 *      write  iff  matched  &&  prev == cur  &&  prev2 != cur
 *
 * The request and the rules run BEFORE that, so a pair they enter this frame
 * has its phase-0 tick in the original step that follows, and the one-shot
 * lands on its second tick like any other — the same clock as an engine entry.
 *
 * ⚠️ This one runs at ~30 Hz, which is why it stays FRAME-FREE: the engine
 * parks thread stacks inside the PRX image, and frequency is what turns a
 * marginal stack cost into a collision. Its one `jalr` spills ra and the step's
 * arguments to config words (not reentrant — and it need not be: the step is
 * dispatched once per frame from one site, and nothing under enter-action
 * dispatches it).
 */
static void build_ai_stub(uint32_t original)
{
    uint32_t cfg = (uint32_t)(uintptr_t)g_cfgp;
    uint32_t ret = (uint32_t)(uintptr_t)g_stub_ret;
    int overflow = 0;
    int n = emv_build_ai_stub(g_stub_ai, STUB_AI_INSNS, cfg, original, ret, &overflow);
    if (overflow) {
        mhfu_log("[%s] ai stub is %d insns > STUB_AI_INSNS %d - installing a plain "
                 "trampoline instead", MOD_ID, n, STUB_AI_INSNS);
        g_stub_ai[0] = mips_j(original);
        g_stub_ai[1] = MIPS_NOP;
        return;
    }
    for (int k = n; k < STUB_AI_INSNS; k++) g_stub_ai[k] = MIPS_NOP;
    mhfu_log("[%s] ai stub: request + %d rules + one-shot budget, %d insns, frame-free",
             MOD_ID, EM_VHOOK_RULES, n);
}

/* --- slot 32: substitution PRE part, the original, v2's POST part. ---------
 *
 * 🔴 THE POST ORDERING FOR THE BUDGET IS THE WHOLE POINT. v1.0/v1.1 stored and
 * then TAIL-CALLED the original, so the species code ran last and simply
 * overwrote us: armed on all nine gated (2,x) actions, exactly 1 of 85 samples
 * ever read our value back. Reversed, on `(2,9)`:
 *
 *     forced +0x414 | value read back | longest occupancy of (2,9)
 *     --            | 498             | 3.30 s
 *     1500          | 1499            | 3.15 s
 *     30            |   30            | 0.55 s
 *
 * The action clock is ours. docs/EM_OVERLAY_ABI.md §13.
 *
 * ⚠️ This stub therefore uses a 16-byte STACK FRAME, which the file header's
 * "frame-free" rule otherwise forbids. That rule exists because the engine parks
 * thread stacks inside the PRX image, and a hook's C-call frame is what clobbered
 * the Lua VM once. Four things make this one different, and none of them
 * generalise to a C callback:
 *   - it is 16 bytes of hand-written asm, not a C frame plus everything C calls;
 *   - the function it wraps allocates 0x20 itself and then calls deeper, so our
 *     addition to the high-water mark is noise;
 *   - the identical frame ran on this exact dispatch path 100+ times per run
 *     across three multi-minute debugger sessions with no incident;
 *   - the stubs and config do not live in the image at all (see alloc_block).
 * Slot 32 fires twice per transition and can be re-entered from a handler (the
 * (2,9) handler itself calls vt+0x88), so its ra goes on the stack — the
 * reentrant answer — not in a config word.
 *
 * ⚠️ The budget match keys on the pair ACTUALLY ENTERED (after substitution),
 * off the frame, rather than entity+0x298/+0x299: act_set runs INSIDE the
 * original, so before the call those bytes still hold the PREVIOUS action.
 *
 * Branchless throughout: compute BOTH candidate values/addresses and pick with
 * MOVN. On a miss the substitution leaves a1/a2 as they came and the budget
 * store lands in cfg.sink, which nothing reads. Armed and unarmed runs execute
 * an identical instruction stream — the property that made the extra-RAM crash
 * diagnosable.
 */
static void build_act_stub(uint32_t original)
{
    uint32_t cfg = (uint32_t)(uintptr_t)g_cfgp;
    int overflow = 0;
    int n = emv_build_act_stub(g_stub_act, STUB_ACT_INSNS, cfg, original, &overflow);
    if (overflow) {
        mhfu_log("[%s] act stub is %d insns > STUB_ACT_INSNS %d - installing a plain "
                 "trampoline instead", MOD_ID, n, STUB_ACT_INSNS);
        g_stub_act[0] = mips_j(original);
        g_stub_act[1] = MIPS_NOP;
        return;
    }
    for (int k = n; k < STUB_ACT_INSNS; k++) g_stub_act[k] = MIPS_NOP;
    mhfu_log("[%s] act stub: %d-entry substitution PRE, original, budget POST; "
             "%d insns, frame 0x%X", MOD_ID, EM_VHOOK_SUBS, n, ACT_FRAME);
}

/* ------------------------------------------------------------ public API */

extern "C" int em_vhook_installed(void) { return g_installed; }

/* v2 surface — the +0x414 budget override. A mod (or the Lua host) can retarget
 * it without a rebuild, because the stubs read the config every dispatch. */
extern "C" void em_vhook_arm(uint8_t main_state, uint8_t sub_state,
                             uint32_t off, uint32_t val)
{
    if (!g_cfgp) return;
    g_cfgp->want_main = main_state;
    g_cfgp->want_sub  = sub_state;
    g_cfgp->patch_off = off;
    g_cfgp->patch_val = val;
    g_cfgp->armed     = 1;
    mhfu_log("[%s] armed: (%u,%u) -> entity+0x%X = %u",
             MOD_ID, main_state, sub_state, (unsigned)off, (unsigned)val);
}

/* Pick which seam owns the armed action's `entity+0x414` budget.
 *   off (default)  slot 32 only. Correct for the 15 gated actions whose handler
 *                  merely CONSUMES the budget.
 *   on             slot 29 as well — a one-shot on the action's second frame.
 *                  Needed for the 12 whose phase-0 block re-seeds the budget
 *                  after any slot-32 hook has already written.
 * `em_phase_map.py <ovl> --budget-owner` says which set an action is in. */
extern "C" void em_vhook_seam29(uint8_t on)
{
    if (!g_cfgp) return;
    g_cfgp->arm29 = on ? 1 : 0;
    g_cfgp->prev_pair = g_cfgp->prev2_pair = 0xFFFFFFFFu;   /* no stale one-shot */
    mhfu_log("[%s] slot-29 budget seam %s", MOD_ID, on ? "ON" : "off");
}

extern "C" void em_vhook_stats(uint32_t *ai, uint32_t *acts, uint32_t *last)
{
    if (!g_cfgp) { if (ai) *ai = 0; if (acts) *acts = 0; if (last) *last = 0; return; }
    if (ai)   *ai   = g_cfgp->ai_ticks;
    if (acts) *acts = g_cfgp->act_enters;
    if (last) *last = g_cfgp->last_pair;
}

/* v3: substitution table. `count` 0 clears; EM_VHOOK_UNLIMITED is standing. */
extern "C" void em_vhook_substitute(int slot, uint8_t from_mask, uint8_t from_sub,
                                    uint8_t to_main, uint8_t to_sub, uint32_t count)
{
    if (!g_cfgp || slot < 0 || slot >= EM_VHOOK_SUBS) return;
    cfg_sub_t *s = &g_cfgp->subs[slot];
    /* order: disable first (left = 0 is the stub's off switch), then the
     * bytes, then enable — a dispatch racing in between sees off or the
     * complete new entry, never a half-written one */
    s->left = 0;
    s->from_mask = count ? from_mask : 0;
    s->from_sub  = from_sub;
    s->to_main   = to_main;
    s->to_sub    = to_sub;
    s->left      = count;
    if (count)
        mhfu_log("[%s] substitute[%d]: main mask 0x%02X sub %s -> (%u,%u) x%s",
                 MOD_ID, slot, from_mask,
                 from_sub == EM_VHOOK_SUB_ANY ? "any" : "exact", to_main, to_sub,
                 count == EM_VHOOK_UNLIMITED ? "standing" : "n");
    else
        mhfu_log("[%s] substitute[%d]: cleared", MOD_ID, slot);
}

/* v3: a pair to enter on the next AI frame, through the engine's dispatcher. */
extern "C" int em_vhook_request(uint8_t main_state, uint8_t sub_state, uint8_t mode)
{
    if (!g_cfgp || !g_installed) return 0;
    g_cfgp->req_pending = 0;
    g_cfgp->req_main = main_state;
    g_cfgp->req_sub  = sub_state;
    g_cfgp->req_mode = mode;
    g_cfgp->req_pending = 1;
    return 1;
}

/* v3: a native brain rule. NULL clears the slot. Distances come in units and
 * go into the block squared, as raw f32 bits, which is what the stub compares. */
static uint32_t f32_bits(float f) { union { float f; uint32_t u; } v; v.f = f; return v.u; }

extern "C" void em_vhook_rule(int slot, const em_vhook_rule_t *r)
{
    if (!g_cfgp || slot < 0 || slot >= EM_VHOOK_RULES) return;
    cfg_rule_t *c = &g_cfgp->rules[slot];
    c->left = 0;                                    /* off while we write */
    if (!r || r->count == 0 || r->from_mask == 0) {
        c->from_mask = 0;
        mhfu_log("[%s] rule[%d]: cleared", MOD_ID, slot);
        return;
    }
    float lo = r->dist_lo < 0 ? 0 : r->dist_lo;
    float hi = r->dist_hi < 0 ? 0 : r->dist_hi;
    c->from_mask  = r->from_mask;
    c->from_sub   = r->from_sub;
    c->to_main    = r->to_main;
    c->to_sub     = r->to_sub;
    c->mode       = r->mode;
    c->flags      = r->flags;
    c->min_frames = r->min_frames;
    c->d2_lo      = f32_bits(lo * lo);
    c->d2_hi      = f32_bits(hi * hi);
    c->cooldown   = r->cooldown;
    c->fired      = 0;
    c->last_fire  = 0;
    c->left       = r->count;
    mhfu_log("[%s] rule[%d]: main mask 0x%02X sub %s, >=%u frames, d in [%d,%d)%s%s "
             "-> enter (%u,%u,m%u), cooldown %u, x%s",
             MOD_ID, slot, r->from_mask,
             r->from_sub == EM_VHOOK_SUB_ANY ? "any" : "exact",
             (unsigned)r->min_frames, (int)lo, (int)hi,
             (r->flags & EM_VHOOK_RULE_RECEDING) ? ", receding" : "",
             (r->flags & EM_VHOOK_RULE_CLOSING)  ? ", closing"  : "",
             r->to_main, r->to_sub, r->mode, (unsigned)r->cooldown,
             r->count == EM_VHOOK_UNLIMITED ? "standing" : "n");
}

extern "C" void em_vhook_clear(void)
{
    if (!g_cfgp) return;
    g_cfgp->req_pending = 0;
    for (int i = 0; i < EM_VHOOK_SUBS; i++)  { g_cfgp->subs[i].left = 0; g_cfgp->subs[i].from_mask = 0; }
    for (int i = 0; i < EM_VHOOK_RULES; i++) { g_cfgp->rules[i].left = 0; g_cfgp->rules[i].from_mask = 0; }
}

static float bits_f32(uint32_t u) { union { float f; uint32_t u; } v; v.u = u; return v.f; }
static float sqrt_approx(float x)
{
    /* the status is for logs; `sqrt.s` is one Allegrex instruction */
    if (!(x > 0)) return 0;
    return __builtin_sqrtf(x);
}

extern "C" void em_vhook_status(em_vhook_status_t *out)
{
    if (!out) return;
    for (unsigned k = 0; k < sizeof(*out) / 4; k++) ((uint32_t *)out)[k] = 0;
    if (!g_cfgp) return;
    out->installed   = (uint32_t)g_installed;
    out->ai_ticks    = g_cfgp->ai_ticks;
    out->act_enters  = g_cfgp->act_enters;
    out->last_pair   = g_cfgp->last_pair;
    out->frames      = g_cfgp->frames;
    out->dist        = sqrt_approx(bits_f32(g_cfgp->d2));
    out->sub_hits    = g_cfgp->sub_hits;
    out->sub_landed  = g_cfgp->sub_landed;
    out->sub_last_in = g_cfgp->sub_last_in;
    out->brain_fires = g_cfgp->brain_fires;
    out->req_pending = g_cfgp->req_pending;
    out->req_done    = g_cfgp->req_done;
    out->req_result  = g_cfgp->req_result;
    out->ring_idx    = g_cfgp->ring_idx;
    for (int i = 0; i < EM_VHOOK_RING; i++)  out->ring[i] = g_cfgp->ring[i];
    for (int i = 0; i < EM_VHOOK_RULES; i++) { out->rule_fired[i] = g_cfgp->rules[i].fired;
                                               out->rule_left[i]  = g_cfgp->rules[i].left; }
    for (int i = 0; i < EM_VHOOK_SUBS; i++)  out->sub_left[i] = g_cfgp->subs[i].left;
}

/* ------------------------------------------------------------ latch */

static void install_for(uint32_t entity)
{
    if (g_installed || !entity) return;
    if (alloc_block() < 0) return;
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

    cfg_reset_live();
    emv_build_ret_stub(g_stub_ret);
    g_stub_ret[2] = MIPS_NOP; g_stub_ret[3] = MIPS_NOP;
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
    em_vhook_clear();
    mhfu_log("[%s] restored vtable 0x%08X (ai_ticks=%u act_enters=%u sub %u/%u "
             "req %u brain %u)", MOD_ID, (unsigned)g_vtable,
             (unsigned)g_cfgp->ai_ticks, (unsigned)g_cfgp->act_enters,
             (unsigned)g_cfgp->sub_hits, (unsigned)g_cfgp->sub_landed,
             (unsigned)g_cfgp->req_done, (unsigned)g_cfgp->brain_fires);
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
     * big-monster spawn. Everything declared for the old monster goes too. */
    uninstall();
    g_cfgp->ai_ticks = g_cfgp->act_enters = 0;
    g_cfgp->sub_hits = g_cfgp->sub_landed = g_cfgp->brain_fires = 0;
    g_cfgp->req_done = 0;
}

static int em_vhook_init(void)
{
    if (alloc_block() < 0) return -1;
    g_cfgp->patch_off = 0x414;     /* the action countdown; see em_phase_map.py */
    g_cfgp->patch_val = 900;
    g_cfgp->want_main = 0xFF;      /* matches nothing until armed */
    g_cfgp->want_sub  = 0xFF;
    g_cfgp->canary    = CFG_CANARY_VAL;
    mhfu_on_monster_spawned(on_spawn);
    mhfu_on_quest_beginning(on_quest);
    mhfu_log("[%s] v3.0 ready; cfg @0x%08X, stubs @0x%08X / 0x%08X", MOD_ID,
             (unsigned)(uintptr_t)g_cfgp,
             (unsigned)(uintptr_t)g_stub_ai, (unsigned)(uintptr_t)g_stub_act);
    return 0;
}

static void em_vhook_shutdown(void) { uninstall(); }

MHFU_MOD(.id = MOD_ID, .version = "3.0",
         .needs = 0, .conflicts = 0,
         .init = em_vhook_init, .shutdown = em_vhook_shutdown);
