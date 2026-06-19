/*
 * brute_overlay_hook — B-hook infrastructure for the Brute Tigrex port.
 *
 * ROOT CAUSE (confirmed by static disasm 2026-06-18):
 *   Joint builder 0x088dc40c ABI = (entity a0, bone_array_ptr a1, skel_blob a2).
 *   Disasm proof: `move s2,a2` then `jal 0x088dc748` delay-slot `move a0,s2` —
 *   the bone-count reader receives a2 as its input.
 *
 *   On a Brute quest: a2 = some bad xram ptr (not the skeleton blob — likely the
 *   anim data or an uninitialised ptr in the PAC sub-table).  [a2] != 0xC0000000,
 *   [a2+4] = garbage bone count → builder tries to alloc huge joint array → crash.
 *
 *   The REAL Brute skeleton is at relocated PAC base + 0x40 = 0x0B000040.
 *   Magic = 0xC0000000, bone_count = 46.
 *
 *   Additionally, a1 (bone_array_ptr) is computed as skel_blob + some offset Δ.
 *   When a2 is wrong, a1 is also wrong (same bad base).  We compute Δ = a1 − a2
 *   from the incoming values and apply it to the real skel: new_a1 = real_skel + Δ.
 *
 * FIX — PRIORITY-1 HOOK (joint_builder_fix):
 *   Entry-trampoline 0x088dc40c.
 *   - Log a0/a1/a2/[a2]/[a2+4] on first call always.
 *   - If [a2]==0xC0000000 (valid skel): passthrough unchanged.
 *   - Else: find real_skel by xram scan, compute delta=a1−a2,
 *           set a2=real_skel, a1=real_skel+delta.
 *   - Tail-call real builder body (entry+8, skipping our J+NOP).
 *
 * INSTALL:
 *   mhfu_patch_word_when_quiet(JB_PATCH_ADDR=0x088dc444, orig, J stub, MOD_ID).
 *   0x088dc444 is pure EBOOT → always JIT-cold at TITLE/MENU → safe.
 *
 * ADDING MORE OVERRIDES:
 *   Add an entry to g_overrides[].  OVR_CALL_SITE = patch a jal call site
 *   (use when you only need a0; framework builds the wrapper).  OVR_ENTRY_PATCH
 *   = full entry redirect (use when you need a1..a3 or must tail-call original).
 *   Enable by setting .enabled=1 once the address is confirmed.
 *
 * REFERENCES:
 *   CLAUDE.md — Section 26 quiet-gate, JIT SMC limitation
 *   docs/AI_SCRIPTING_ENGINE.md §32k — executor seam
 *   framework/prx/include/mhfu/hooks.h — mhfu_patch_word_when_quiet,
 *       mhfu_install_call_wrapper, mhfu_hook_function
 *   framework/prx/src/core/install.cpp — wrapper stub builder
 */

#include <pspthreadman.h>
#include <string.h>
#include "mhfu/mhfu.h"
#include "mhfu/mips.h"   /* mips_j / mips_lw / MIPS_REG_* — not pulled in by mhfu.h */

#define MOD_ID  "brute_overlay_hook"

/* ------------------------------------------------------------------ addresses */

/* Joint builder: 0x088dc40c(entity a0, bone_array_ptr a1, skel_blob a2).
 * ABI confirmed by static disasm: move s2,a2; jal 0x088dc748; move a0,s2
 * — the bone-count reader (0x088dc748) receives s2 (= a2).
 *
 * PATCH POINT: NOT the entry 0x088dc40c.  Patching the entry's `addiu sp,-0x70`
 * with J+NOP makes the DELAY SLOT `sw ra,0x2c(sp)` (at +4) execute with the
 * UN-adjusted sp → corrupts the caller's stack.
 *
 * Instead we patch 0x088dc444 = `move s2,a2` — AFTER the full prologue
 * (sp alloc, ra/s-regs saved, s5=entity set at 0x088dc43c, s4=a1 set at
 * 0x088dc440).  The delay slot (0x088dc448 = `mov.s f22,f12`) is harmless.
 * Resume point = 0x088dc44c (instruction after the displaced `move s2,a2`
 * and its delay slot).
 *
 * At the patch point:
 *   s5 = entity  (set by prologue; DO NOT TOUCH)
 *   s4 = a1 = bone_array_ptr  (set at 0x088dc440)
 *   a2 = skel_blob  (live incoming arg; s2 not yet set)
 *   Stack/ra already saved by prologue.
 * Our stub: call C helper(s5, s4, a2), write s2=corrected_a2, s4=corrected_a1,
 * J 0x088dc44c. */
#define JOINT_BUILDER_FN    0x088dc40cu   /* function entry (for reference) */
#define JB_PATCH_ADDR       0x088dc444u   /* `move s2,a2` — our actual patch word */
#define JB_RESUME_ADDR      0x088dc44cu   /* past displaced insn + delay slot */

/* xram range: the relocate path places our Brute PAC at 0x0B000000.
 * The skeleton sub is at PAC+0x40 = 0x0B000040 (confirmed: magic=0xC0000000,
 * bone_count=46).  We scan [XRAM_BASE, XRAM_SCAN_END) for the skeleton magic
 * so the hook works even if the PAC base shifts slightly. */
#define XRAM_BASE           0x0B000000u
#define XRAM_SCAN_END       0x0B100000u   /* 1 MB scan window — skeleton is at +0x40 */
#define SKEL_MAGIC          0xC0000000u
#define BRUTE_BONE_COUNT    46u

/* Tigrex host bone count (sanity reference). */
#define TIGREX_BONE_COUNT   49u

/* ------------------------------------------------------------------ helpers */

/* 1 if addr is in xram (our relocated PAC range). */
static inline int in_xram(uint32_t addr)
{
    return addr >= XRAM_BASE && addr < 0x0C000000u;
}

/* Find the Brute skeleton blob: scan xram for magic 0xC0000000, verify
 * bone_count at +4 == BRUTE_BONE_COUNT.  Returns the blob ptr or 0. */
static uint32_t find_brute_skeleton(void)
{
    /* Fast path: skeleton is almost always at PAC+0x40 = 0x0B000040. */
    uint32_t fast = XRAM_BASE + 0x40u;
    if (mhfu_read_u32(fast) == SKEL_MAGIC &&
        mhfu_read_u32(fast + 4) == BRUTE_BONE_COUNT)
        return fast;

    /* Slow scan: iterate by u32 until we find the magic + correct bone count.
     * Bounded to XRAM_SCAN_END.  Runs only when the fast path misses (first
     * call or if PAC base shifted). */
    for (uint32_t p = XRAM_BASE; p + 8 <= XRAM_SCAN_END; p += 4) {
        if (mhfu_read_u32(p) == SKEL_MAGIC &&
            mhfu_read_u32(p + 4) == BRUTE_BONE_COUNT)
            return p;
    }
    return 0;
}

/* ------------------------------------------------------------------ joint_builder_fix
 *
 * Patch point context (0x088dc444, post-prologue):
 *   s5 = entity               (saved by prologue at 0x088dc43c)
 *   s4 = bone_array_ptr       (set at 0x088dc440 `move s4,a1`)
 *   a2 = skel_blob            (live; s2 not yet set — that's the insn we displaced)
 *
 * On a Brute quest a2 is a bad xram ptr (anim data or uninit PAC sub-table
 * entry) so [a2] != 0xC0000000.  Because s4 = bad_a2 + Δ, s4 is also wrong.
 * Fix: compute Δ = s4 − a2, find real_skel, set s2=real_skel, s4=real_skel+Δ.
 *
 * Called from the stub as:
 *   joint_builder_fix_c(entity=s5, orig_s4=s4, orig_a2=a2)
 * Returns: corrected s2 (= skel_blob) in v0.
 * Writes: corrected s4 (= bone_array_ptr) into g_fixed_s4 for stub to load.
 */

/* Stub writes corrected s4 here; loaded back via LUI+LW after JAL. */
static volatile uint32_t g_fixed_s4 = 0;

/* Stack-free diagnostics (plain stores from fix_c; no log frame). Inspect via
 * debugger or a worker-thread log. g_dbg_fires>0 => the Brute fix fired. */
volatile uint32_t g_dbg_fires     = 0;
volatile uint32_t g_dbg_orig_a2   = 0;
volatile uint32_t g_dbg_real_skel = 0;
volatile uint32_t g_dbg_new_s4    = 0;
volatile uint32_t g_dbg_bonecount = 0;

/* Called from stub: entity=s5, orig_s4=s4 (bone_array_ptr), orig_a2=a2 (skel_blob).
 * Returns corrected s2 (skel_blob) in v0; stores corrected s4 in g_fixed_s4. */
/* CRITICAL: this runs on the big-mon CONSTRUCTION thread, whose stack sits inside
 * the PRX (~0x09D8A000). mhfu_log()'s vsnprintf frame (~300 B) tips that stack
 * into our import stubs / Lua VM -> the "mhfu_deferred jump to 0c000000" and the
 * "lua_host read 0x27bd001c" crashes (RE'd 2026-06-19). So fix_c MUST be
 * stack-cheap: NO mhfu_log here. Diagnostics go to sentinel cells (a plain store,
 * no frame), readable from the worker thread / debugger. */
uint32_t joint_builder_fix_c(uint32_t entity, uint32_t orig_s4, uint32_t orig_a2)
{
    (void)entity;
    uint32_t magic = mhfu_read_u32(orig_a2);

    /* Valid skeleton (native monster, or an already-correct ptr): pass through. */
    if (magic == SKEL_MAGIC) {
        g_fixed_s4 = orig_s4;
        return orig_a2;
    }

    /* Bad-magic a2 = the Brute's mis-computed skeleton ptr. In the in-place v7
     * path this ptr is in GMEM (engine raw/restructured buffer), NOT xram, so we
     * do NOT gate on in_xram — any non-skel a2 gets corrected. Find the real
     * 46-bone Brute skeleton (lives in xram e->buf @ 0x0B000040, loaded by the
     * inject read_file). */
    uint32_t real_skel = find_brute_skeleton();
    if (!real_skel) {
        g_fixed_s4 = orig_s4;
        return orig_a2;
    }

    /* Rebase s4 (bone_array_ptr): disasm showed a1 = skel_blob + Δ, so when a2 is
     * the bad base, s4 carries the same bad base + Δ. new_s4 = real_skel + Δ. */
    uint32_t delta  = orig_s4 - orig_a2;
    uint32_t new_s4 = real_skel + delta;
    g_fixed_s4 = new_s4;

    /* Stack-free diagnostics: plain global stores (no frame). Readable via the
     * worker tick / debugger. g_dbg_fires>0 proves the fix fired. */
    g_dbg_fires++;
    g_dbg_orig_a2 = orig_a2;
    g_dbg_real_skel = real_skel;
    g_dbg_new_s4 = new_s4;
    g_dbg_bonecount = mhfu_read_u32(real_skel + 4);

    return real_skel;   /* v0 → stub writes this into s2 */
}

/* ------------------------------------------------------------------ stub
 *
 * Patch point: 0x088dc444 (`move s2,a2`), AFTER the full function prologue.
 * The prologue has already:
 *   - allocated the stack frame (ADDIU sp,sp,-0x70 at 0x088dc40c)
 *   - saved ra, s2-s7 into the frame
 *   - set s5 = a0 (entity) at 0x088dc43c
 *   - set s4 = a1 (bone_array_ptr) at 0x088dc440
 * At our patch point: a2 = skel_blob (live), s2 = not yet set.
 *
 * The delay slot (0x088dc448 = `mov.s f22,f12`) runs harmlessly before J.
 * Resume = 0x088dc44c (instruction after the two displaced words).
 *
 * No extra frame needed: prologue owns the stack.  We MAY freely JAL a C
 * helper because ra is already saved to the frame and s-regs survive JAL.
 * a0-a3/v0-v1/t-regs are clobbered by the JAL and re-established later.
 *
 * Register plan:
 *   s5  = entity (prologue; DO NOT TOUCH)
 *   s4  = bone_array_ptr (prologue; we update it if bad)
 *   a2  = skel_blob (live at patch; pass as arg a2 to C helper)
 *   v0  = corrected s2 returned from C helper
 *   g_fixed_s4 = corrected s4 stored by C helper; loaded via LUI+LW
 *
 * Layout (10 instructions + padding to 12):
 *   [0]  MOVE  a0, s5         entity → arg0
 *   [1]  MOVE  a1, s4         bone_array_ptr → arg1
 *   [2]  JAL   joint_builder_fix_c   (a2 = skel_blob already in place)
 *   [3]  NOP                  delay slot
 *   [4]  MOVE  s2, v0         corrected skel_blob
 *   [5]  LUI   at, HI16(&g_fixed_s4)
 *   [6]  LW    s4, LO16(&g_fixed_s4)(at)   corrected bone_array_ptr
 *   [7]  J     JB_RESUME_ADDR (0x088dc44c)
 *   [8]  NOP                  delay slot
 *
 * 9 insns; padded to 12.
 */
#define JB_STUB_INSNS  12
static uint32_t g_jb_stub[JB_STUB_INSNS]
    __attribute__((aligned(64)));   /* cache-line aligned */

/* Raw R-type encoder (op=0): rd = rs <fn> rt. mips.h lacks subu/addu/xor. */
#define MIPS_R3(rs, rt, rd, fn) \
    ((uint32_t)(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | (fn)))
#define MIPS_FN_SUBU  0x23u
#define MIPS_FN_ADDU  0x21u
#define MIPS_FN_XOR   0x26u

/* FRAME-FREE, BRANCHLESS inline skeleton fix.
 *
 * ROOT CAUSE (RE'd 2026-06-19): the old stub did `jal joint_builder_fix_c`. That
 * C call pushes a stack frame on the big-mon CONSTRUCTION thread, whose stack
 * sits right at the PRX edge (~0x09D8A000). The extra frame tips the stack into
 * our PRX (import stubs / Lua VM) -> the "mhfu_deferred -> 0c000000" and
 * "lua_host read 0x27bd001c" crashes. It fired even for the native passthrough
 * (no Brute), which is why the crash reproduced with the joint-fix on and ZERO
 * Brute load. WITHOUT the stub, the Brute spawns cleanly (only the FK bad-skel
 * crash remains). So the fix MUST add NO frame: do it inline in MIPS, no jal.
 *
 * Logic (s2 = displaced `move s2,a2` target = skel ptr; s4 = bone_array_ptr):
 *   t0 = [a2]                    magic of incoming skel ptr
 *   t1 = 0xC0000000              SKEL_MAGIC
 *   t5 = t0 ^ t1                 0 iff a valid skeleton (native passthrough)
 *   t2 = 0x0B000040              the real 46-bone Brute skel (xram e->buf + 0x40)
 *   t3 = s4 - a2                 bone_array offset relative to the (bad) skel base
 *   t4 = t2 + t3                 rebased bone_array = real_skel + delta
 *   s2 = a2                      default (== the displaced instruction)
 *   movn s2, t2, t5              if bad skel -> s2 = real_skel
 *   movn s4, t4, t5              if bad skel -> s4 = rebased bone_array
 *   j 0x088dc44c                 resume past the displaced move + its delay slot
 * Single basic block, no branch, no jal -> JIT-safe and stack-neutral. Clobbers
 * t0-t5 only (the old jal clobbered a0-a3/v0-v1/t0-t9 and resumed fine, so the
 * joint builder tolerates caller-saved clobber at the resume point). */
static void build_jb_stub(void)
{
    int i = 0;
    uint32_t *s = g_jb_stub;
    s[i++] = mips_lw(MIPS_REG_T0, 0, MIPS_REG_A2);
    s[i++] = mips_lui(MIPS_REG_T1, 0xC000);
    s[i++] = MIPS_R3(MIPS_REG_T0, MIPS_REG_T1, MIPS_REG_T5, MIPS_FN_XOR);
    s[i++] = mips_lui(MIPS_REG_T2, 0x0B00);
    s[i++] = mips_ori(MIPS_REG_T2, MIPS_REG_T2, 0x0040);
    s[i++] = MIPS_R3(MIPS_REG_S4, MIPS_REG_A2, MIPS_REG_T3, MIPS_FN_SUBU);
    s[i++] = MIPS_R3(MIPS_REG_T2, MIPS_REG_T3, MIPS_REG_T4, MIPS_FN_ADDU);
    s[i++] = mips_move(MIPS_REG_S2, MIPS_REG_A2);
    s[i++] = mips_movn(MIPS_REG_S2, MIPS_REG_T2, MIPS_REG_T5);
    s[i++] = mips_movn(MIPS_REG_S4, MIPS_REG_T4, MIPS_REG_T5);
    s[i++] = mips_j(JB_RESUME_ADDR);
    s[i++] = MIPS_NOP;                               /* j delay slot */

    while (i < JB_STUB_INSNS) s[i++] = MIPS_NOP;

    mhfu_flush_caches();
    (void)joint_builder_fix_c;   /* kept for reference; no longer called */
}

/* ------------------------------------------------------------------ generic override table
 *
 * For overrides that only need a0 (no a1/a2 modification), the table-driven
 * mhfu_install_call_wrapper path is simpler.  Add entries here as asset
 * confirms more construction call sites.
 *
 * override_kind_t:
 *   OVR_CALL_SITE   — patch a jal call-site (framework handles stub + gate).
 *                     Handler receives a0 only; a1..a3 may be clobbered.
 *   OVR_ENTRY_PATCH — handled ad-hoc (see joint_builder above); not used
 *                     in this table since those need custom stub layouts.
 */
typedef enum { OVR_CALL_SITE = 0, OVR_ENTRY_PATCH } override_kind_t;
typedef int  override_mode_t;

typedef struct {
    const char      *name;
    uint32_t         call_site;
    uint32_t         target_fn;
    override_kind_t  kind;
    override_mode_t  mode;
    void           (*handler)(uint32_t a0);
    int              enabled;
} override_entry_t;

/* ---- placeholder handlers (fill in as asset confirms addresses) ---------- */

static void fk_bufsz_log(uint32_t a0)
{
    /* Log-only for now: once sizeof(Joint) is confirmed, compute
     *   brute_size = BRUTE_BONE_COUNT * (orig_return / TIGREX_BONE_COUNT)
     * and write it back.  For now just records that this path fires. */
    mhfu_log("[bovh] fk_bufsz called a0=0x%08X", (unsigned)a0);
}

/* ---- table --------------------------------------------------------------- */

static override_entry_t g_overrides[] = {
    /* FK output-buffer sizing — log only until sizeof(Joint) confirmed.
     * Fill FK_BUFSZ_CALL_SITE once asset provides the jal address. */
    {
        .name      = "fk_bufsz_log",
        .call_site = 0x00000000u,   /* [OPEN] jal-site of FK_BUFSZ_FN 0x08863198 */
        .target_fn = 0x08863198u,
        .kind      = OVR_CALL_SITE,
        .mode      = MHFU_WRAP_PREFIX,
        .handler   = fk_bufsz_log,
        .enabled   = 0,
    },
    /* Sentinel */
    { .name = 0 },
};

static int install_table_overrides(void)
{
    int ok = 0, skip = 0;
    for (override_entry_t *e = g_overrides; e->name; e++) {
        if (!e->enabled) { skip++; continue; }
        mhfu_hook_rc_t rc = mhfu_install_call_wrapper(
            e->call_site, e->target_fn, e->handler, e->mode, MOD_ID);
        if (rc == MHFU_HOOK_OK) {
            mhfu_log("[bovh] queued '%s' @0x%08X", e->name, (unsigned)e->call_site);
            ok++;
        } else {
            mhfu_log("[bovh] FAILED '%s' rc=%d", e->name, (int)rc);
        }
    }
    if (skip)
        mhfu_log("[bovh] %d table overrides skipped (OPEN addresses)", skip);
    return ok;
}

/* ------------------------------------------------------------------ identity / spawn poll */

static volatile uint32_t g_brute_entity = 0;

static int spawn_poll_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    uint32_t last = 0;
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        uint8_t scr = mhfu_get_screen_state();
        if (scr != 17) {
            if (g_brute_entity) { g_brute_entity = 0; last = 0; }
            continue;
        }
        uint32_t buf[4];
        int n = mhfu_entities_of_type(MON_TIGREX, buf, 4);
        uint32_t ent = (n > 0) ? buf[0] : 0;
        if (ent != last) {
            g_brute_entity = ent;
            last = ent;
            if (ent) mhfu_log("[bovh] Brute entity @ 0x%08X", (unsigned)ent);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ mod lifecycle */

static int brute_overlay_hook_init(void)
{
    mhfu_log("[bovh] init — joint_builder fix for Brute Tigrex (46 bones)");

    /* Build and install the joint-builder stub.
     * Patch point: 0x088dc444 (`move s2,a2`), POST-prologue.
     * Delay slot (0x088dc448 `mov.s f22,f12`) runs harmlessly before J.
     * Resume: 0x088dc44c. */
    build_jb_stub();
    uint32_t orig_patch = mhfu_read_u32(JB_PATCH_ADDR);
    mhfu_hook_rc_t rc = mhfu_patch_word_when_quiet(
        JB_PATCH_ADDR,
        orig_patch,
        mips_j((uint32_t)(uintptr_t)g_jb_stub),
        MOD_ID);
    if (rc == MHFU_HOOK_OK)
        mhfu_log("[bovh] joint_builder fix queued @0x%08X -> stub 0x%08X",
                 JB_PATCH_ADDR, (unsigned)(uintptr_t)g_jb_stub);
    else
        mhfu_log("[bovh] joint_builder fix FAILED rc=%d", (int)rc);

    /* Install table-driven overrides (none enabled yet). */
    install_table_overrides();

    /* Spawn-poll thread DISABLED: the joint-fix identifies the Brute self-
     * contained ([a2] is an xram ptr with bad magic), so g_brute_entity is
     * unused by the active override. The poll thread was crashing (bad exec
     * address) — removed. Re-add (with a larger stack) only if a future
     * override needs the pinned entity.
     * (void)spawn_poll_thread; */
    (void)spawn_poll_thread;
    (void)g_brute_entity;

    return 0;
}

static void brute_overlay_hook_shutdown(void)
{
    mhfu_unhook_owner(MOD_ID);
    g_brute_entity = 0;
}

MHFU_MOD(.id      = MOD_ID,
         .version = "0.2",
         .needs   = 0,   /* pure-C joint-fix; runs with or without lua_host */
         .conflicts = 0,
         .init    = brute_overlay_hook_init,
         .shutdown = brute_overlay_hook_shutdown);
