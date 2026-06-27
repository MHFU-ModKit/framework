/*
 * inject.cpp — Phase 4 live in-RAM model/skeleton/animation injection.
 * See include/mhfu/inject.h for the design narrative.
 *
 * SEAM (RE'd live 2026-06-16, memory `phase4-load-seam-is-overlay`): the
 * big-monster model load runs in the species OVERLAY, but every raw sub-resource
 * is handed to the EBOOT parsers through ONE chokepoint:
 *   get_subresource 0x088B89B0(pkg a0, type a1) -> pkg + [pkg + 4 + type*8]
 * (the loaded package uses the same PAC layout as our files: u32 count, then
 * (off,size) pairs). We install a prefix-trampoline there: at entry we compute
 * the sub pointer ourselves and, if the fetched sub matches our edited PAC's sub
 * of that type (by magic + size), overwrite it in place BEFORE the parser
 * (pmoCompile / skeleton-build 0x088642FC / anim setup) reads it -> the engine
 * builds OUR data. Content-gated (size is species-specific) -> no fileId needed.
 * Applies on the next quest load (re-enter the area). Pure runtime.
 */
#include "mhfu/inject.h"
#include "mhfu/log.h"
#include "mhfu/memory.h"       /* mhfu_ms0_io_safe — never touch ms0 during savedata */
#include "mhfu/mips.h"         /* mips_* encoders for the game-thread detour */
#include "mhfu/hooks.h"        /* mhfu_patch_word — repoint the volatile import stubs */
#include "mhfu/ai.h"           /* mhfu_on_ai_overlay_loaded (JIT-cold install window) */
#include "mhfu/events.h"       /* MHFU_EVENT_MAP_SECTION_ENTERED repatch */
#include "internal.h"          /* mhfu_anchor_regs_t, cave_alloc, flush_caches */

#include <pspiofilemgr.h>
#include <psputils.h>          /* sceKernelDcacheWritebackRange */
#include <pspthreadman.h>      /* sceKernelDelayThread */
#include <string.h>
#include <stdio.h>             /* snprintf */
#include <pspsysmem.h>         /* *FreeMemSize (real HW boot diagnostics) */
#include <pspsuspend.h>        /* sceKernelVolatileMemTryLock (real HW 4 MB scratch) */
#include <stdarg.h>            /* realhw_dbg */

#define GETSUB_ADDR    0x088B89B0u   /* get_subresource(pkg, type) — POST-transform (unused) */

/* Overlay resource descriptor table (RE'd live 2026-06-17, memory
 * `phase4-descriptor-table-seam`): [DESCR_TBL_PTR] -> table base; entries stride
 * 0xC: flags u16 @+0, fileId u16 @+2, raw-file buffer u32 @+4. The big-monster
 * model's RAW on-disk PAC (count=7) lands in buffer@+4 verbatim (byte-identical to
 * the data_files/file_0NNNNN.bin) BEFORE the overlay restructures it. We overwrite
 * that buffer with our edited PAC so the engine transforms OUR data. Content-gated
 * (full match vs the original file) so only the intended species is touched. */
#define DESCR_TBL_PTR  0x09A4F0D0u
#define DESCR_STRIDE   0x0Cu
#define DESCR_MAX      0x200u        /* engine scans 0x200 slots (resource_reg) */
#define DESCR_FID_OFF  0x02u
#define DESCR_BUF_OFF  0x04u

/* --- racefree game-thread overwrite (2026-06-17) --------------------------
 * The worker-thread descriptor scan raced the engine's model parse: a 1.2 MB
 * memcpy on the lua_host worker collided with the GE display-list builder
 * z_un_0886477c on the game thread -> torn read -> "Invalid Read 0x80" crash.
 * Fix: drive the overwrite SYNCHRONOUSLY on the game thread from an entry-detour
 * on the per-entity model-setup function 0x09AC4D30 (the raw-buffer consumer —
 * it descends into the GE builder). Confirmed live to run per-frame (~80/s); on
 * its FIRST call after a section load the raw descriptor buffer is filled and
 * pristine (== .orig), so the detour overwrites it BEFORE the same function's
 * body transforms it -> the engine builds OUR bytes, sequential on one thread,
 * no concurrency. Per-frame thereafter the content gate (applied_addr / ==orig)
 * makes it a no-op. 0x09AC4D30 lives in the AI overlay (0x09ABF200..), so we
 * install it from the overlay-loaded helper's JIT-cold window — exactly like the
 * action executor 0x09AC5228 (ai.cpp), and re-patch on section roam. */
#define MODEL_SETUP_ENTRY     0x09AC4D30u
#define MODEL_SETUP_DISP0     0x27BDFFE0u   /* addiu $sp,$sp,-0x20 */
#define MODEL_SETUP_DISP1     0xAFBF001Cu   /* sw    $ra,0x1C($sp) */
#define MODEL_SETUP_RESUME    0x09AC4D38u

/* Inject scratch in the stock-PPSSPP `memory=64` extra RAM, disjoint from
 * bigmon_overlay (0x0A000000) and the clone pool (0x0A800000). */
#define MHFU_INJECT_XRAM_LO   0x0B000000u
#define MHFU_INJECT_XRAM_HI   0x0C000000u
static uint32_t g_xram_bump = MHFU_INJECT_XRAM_LO;   /* RAW (emulator) mode bump */

#define MHFU_INJECT_MAX   8
#define MHFU_MAX_SUBS     8

typedef struct {
    uint32_t off;     /* byte offset of the sub data within the PAC file */
    uint32_t size;    /* sub size (bytes) */
    uint32_t magic;   /* first u32 of the sub (0xC0000000 skel / 'pmo\0' / 0x64 anim) */
} sub_ent_t;

typedef struct {
    int            used;
    uint32_t       file_id;
    char           path[160];
    int            primed;
    SceOff         st_size;
    ScePspDateTime st_mtime;
    uint32_t       buf;           /* xram copy of the edited PAC (0 = none) */
    uint32_t       buf_cap;
    uint32_t       obuf;          /* xram copy of the ORIGINAL PAC (for unique ID) */
    uint32_t       obuf_cap;
    uint32_t       file_size;
    int            nsubs;
    sub_ent_t      subs[MHFU_MAX_SUBS];
    uint32_t       hits;          /* diagnostic: # of in-game overwrites */
    uint32_t       applied_addr;  /* last buffer addr we overwrote (diagnostic) */
    /* Diff fingerprint: the first WORD where edit != orig. The PAC header (first
     * 256 B = count + sub table) is identical pristine-vs-edited (edits are deep
     * geometry), so a header gate can't tell them apart and an address-based
     * idempotency check wrongly skips a buffer the engine RELOADS at the same
     * address. We gate on this differing word instead: == orig -> pristine
     * (overwrite); == edit -> already done (skip); naturally re-fires on reload. */
    int            has_diff;
    uint32_t       diff_off;      /* word-aligned byte offset of first difference */
    uint32_t       diff_orig;     /* orig word at diff_off */
    uint32_t       diff_edit;     /* edit word at diff_off */
    /* RELOCATE mode (Phase 5, topology-GROW): instead of a same-size in-place
     * overwrite, `buf` holds a BIGGER replacement PAC in xram and `obuf` holds the
     * ORIGINAL (un-grown) PAC for header matching. At get_subresource we rewrite the
     * caller's a0 (pkg) from the engine's fixed-size raw buffer to our grown buf, so
     * the transform reads OUR larger PMO and builds a larger decoded draw buffer —
     * the only no-disk path that can exceed the raw buffer's fixed heap block. */
    int            relocate;
    uint32_t       orig_size;     /* size of the ORIGINAL PAC (for the 256B match) */
    uint32_t       redirects;     /* diagnostic: # of a0 redirects performed */
    /* Lazy VOLATILE staging (real HW): on hardware we must NOT stage the big PACs (and
     * thus NOT lock the 4 MB volatile partition) at registration — holding volatile
     * starved the savedata utility and froze "loading saves from memory card" at
     * character select. Instead we keep the orig's 256-byte header here for matching,
     * and stage the grown PAC into volatile only when the Brute model actually loads in
     * a quest (stage_relocate(), from the get_subresource match). PPSSPP RAW mode (no
     * volatile to conflict with) still stages eagerly at registration. */
    char           orig_path[160];
    uint8_t        orig_hdr[256];
    int            staged;        /* grown PAC loaded into xram (buf valid)? */
} inject_entry_t;

static inject_entry_t g_tab[MHFU_INJECT_MAX];
static int            g_hook_installed;

/* --- platform diagnostics --------------------------------------------------
 * Appends one line to ms0:/PSP/mhfu_brute_debug.txt so a single run on a real PSP
 * carries enough info to fix the next build (free mem, region pick, alloc result,
 * whether the redirect fired). Harmless on PPSSPP (writes to its memstick ms0). */
static void realhw_dbg(const char *fmt, ...)
{
    /* Per-call open/write/close on ms0 — never during the savedata window (freezes the
     * non-reentrant MS driver). Only the in-quest/village diagnostics survive, which is
     * exactly where the usage probe + redirect lines we care about are written. */
    if (!mhfu_ms0_io_safe()) return;
    char line[224];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    SceUID fd = sceIoOpen("ms0:/PSP/mhfu_brute_debug.txt",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}
#define RHW_DBG(...) realhw_dbg(__VA_ARGS__)

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline int gmem_ok(uint32_t a) { return a >= 0x08000000u && a < 0x0A000000u; }

static void arm_model_setup_hook(void);        /* defined below (game-thread detour) */
static int  install_model_setup_hook(void);
static int  stage_relocate(inject_entry_t *e); /* lazy: stage grown PAC -> xram (locks
                                                * volatile on real HW), on first match */

static inject_entry_t *find_entry(uint32_t file_id)
{
    for (int i = 0; i < MHFU_INJECT_MAX; i++)
        if (g_tab[i].used && g_tab[i].file_id == file_id) return &g_tab[i];
    return 0;
}

/* --- inject scratch RAM: ONE binary, region picked at bootstrap ------------
 * Two regions, auto-selected once:
 *  - PPSSPP (memory=64): the raw flat extra-RAM window [0x0B000000,0x0C000000)
 *    (32 MB) — the validated emulator path; big mods can spill freely.
 *  - real PSP hardware: the 4 MB VOLATILE partition (sceKernelVolatileMemTryLock)
 *    — user-accessible normal RAM, NO partition resize. We deliberately do NOT
 *    grow the user partition: ARK-4 "Use Extra Memory: Forced" was confirmed
 *    (2026-06-25) to crash MHFU EU at the boot logo BY ITSELF, no mod loaded
 *    (Coprocessor-unusable in SceKernelLoadExecThread) — a grown user partition is
 *    not tolerated by this game's loader. The ~1.6 MB Brute + ~1.2 MB orig key
 *    (~2.8 MB) fits in 4 MB; held for the session. Works on every model (Phat too).
 *    TRADE-OFF: while we hold the volatile RAM, MHFU paths that need it (some
 *    load/savedata/utility dialogs) may block or fail.
 * DETECTION is fault-safe. The raw-window probe WRITES 0x0B000000, which FAULTS on
 * real hardware, so we only probe when sceKernelMaxFreeMemSize() reports more free
 * than a stock 24 MB user partition can ever yield (>32 MB) — true only under
 * PPSSPP memory=64. On hardware that gate is false, so we never touch the raw
 * window and take volatile. With this build, ARK "Use Extra Memory" should be
 * OFF/Auto (NOT Forced). Diagnostics -> ms0:/PSP/mhfu_brute_debug.txt. */
enum { XR_UNDECIDED = 0, XR_RAW, XR_VOLATILE };
static int      g_xram_mode;
static uint32_t g_vol_base, g_vol_size, g_vol_bump;   /* volatile region */
static int      g_vol_locked;                         /* 0=unattempted 1=ok -1=fail */
/* Early-prelock arming (real HW): set on quest depart so the volatile lock+stage is
 * attempted BEFORE section streaming grabs volatile; cleared on quest exit (release). */
static int      g_prelock_armed;
/* Master switch for the volatile LOCK path. 0 = recon build: never lock volatile (so the
 * read-only usage probe measures the GAME's true in-quest usage). Flip to 1 for the
 * squat/lock action build. Keeps lock attempts out of the usage measurement. */
static int      g_xram_lock_enabled = 0;

/* Probe-write a word. Fault-safe ONLY behind the maxfree gate (never reached on
 * real hardware); also used per-block in RAW mode where the window is known good. */
static int xram_raw_probe(uint32_t addr)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    uint32_t save = *p; *p = 0xA5C30F04u;
    int ok = (*p == 0xA5C30F04u); *p = save;
    return ok;
}

/* Decide the inject region once, early (called from bootstrap). Cheap + fault-safe;
 * does NOT lock volatile (that's deferred to first alloc so we deny the game its
 * volatile RAM for as short a window as possible). */
extern "C" void mhfu_xram_platform_init(void)
{
    if (g_xram_mode) return;
    uint32_t maxfree = (uint32_t)sceKernelMaxFreeMemSize();
    RHW_DBG("[xram] platform decide: maxfree=%uKB totalfree=%uKB",
            (unsigned)(maxfree / 1024),
            (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    if (maxfree > 0x02000000u && xram_raw_probe(MHFU_INJECT_XRAM_LO)) {
        g_xram_mode = XR_RAW;
        g_xram_bump = MHFU_INJECT_XRAM_LO;
        mhfu_log("[inject] xram = RAW window 0x%08X (emulator, %uMB free)",
                 MHFU_INJECT_XRAM_LO, (unsigned)(maxfree / (1024 * 1024)));
        RHW_DBG("[xram] mode=RAW 0x%08X (emulator)", MHFU_INJECT_XRAM_LO);
    } else {
        g_xram_mode = XR_VOLATILE;   /* lock deferred to first alloc */
        mhfu_log("[inject] xram = VOLATILE 4MB (hardware-safe; locked on first use)");
        RHW_DBG("[xram] mode=VOLATILE (hardware; lock deferred)");
    }
}

/* Lock the 4 MB volatile partition. RETRY-CAPABLE: only a SUCCESS is cached
 * (g_vol_locked == 1 short-circuits). A failure (the game holds volatile — e.g.
 * mid section-streaming) leaves g_vol_locked == -1 but does NOT permanently block:
 * the next call re-attempts TryLock. This is what lets the early-prelock driver
 * keep retrying through a quest depart until it catches a free-volatile window. */
static int volatile_ensure(void)
{
    if (g_vol_locked > 0) return 1;
    void *ptr = 0; int size = 0;
    int rc = sceKernelVolatileMemTryLock(0, &ptr, &size);
    if (rc < 0 || !ptr || size <= 0) {
        if (g_vol_locked != -1)
            RHW_DBG("[xram] VOLATILE TryLock busy rc=0x%08X (game holds it) -> retry",
                    (unsigned)rc);
        g_vol_locked = -1;            /* last attempt failed; retry next call */
        return 0;
    }
    g_vol_base = (uint32_t)(uintptr_t)ptr;
    g_vol_size = (uint32_t)size;
    g_vol_bump = g_vol_base;
    g_vol_locked = 1;
    RHW_DBG("[xram] VOLATILE lock ok base=0x%08X size=%uKB",
            (unsigned)g_vol_base, (unsigned)(g_vol_size / 1024));
    return 1;
}

static uint32_t xram_alloc(uint32_t n)
{
    if (!g_xram_mode) mhfu_xram_platform_init();

    if (g_xram_mode == XR_RAW) {
        uint32_t a = (g_xram_bump + 15u) & ~15u;
        if (a + n > MHFU_INJECT_XRAM_HI) return 0;
        if (!xram_raw_probe(a)) return 0;
        g_xram_bump = a + n;
        return a;
    }
    /* XR_VOLATILE mode (real HW) = the 4 MB volatile partition (0x08400000), a SEPARATE
     * user-accessible region — never the game's own 24 MB heap. (The old memgrow/grown-
     * partition-2 attempt is removed: a PSP_SMEM_High alloc from partition 2 lands INSIDE
     * MHFU's live heap, eating its ~2.2 MB in-game free -> mid-quest OOM. The volatile pivot
     * leaves the game heap untouched and is reclaimed on quest exit.) volatile_ensure()
     * is retry-capable, so a busy lock (game mid-streaming) just fails this alloc; the
     * early-prelock driver retries on the next 10 Hz tick until it catches a free window. */
    if (!volatile_ensure()) return 0;
    uint32_t a = (g_vol_bump + 15u) & ~15u;       /* 16 B align */
    if (a + n > g_vol_base + g_vol_size) {
        RHW_DBG("[xram] VOLATILE exhausted need=%uKB used=%uKB cap=%uKB",
                (unsigned)(n / 1024), (unsigned)((g_vol_bump - g_vol_base) / 1024),
                (unsigned)(g_vol_size / 1024));
        return 0;
    }
    g_vol_bump = a + n;
    RHW_DBG("[xram] VOLATILE alloc need=%uKB addr=0x%08X (used=%uKB/%uKB)",
            (unsigned)(n / 1024), (unsigned)a,
            (unsigned)((g_vol_bump - g_vol_base) / 1024), (unsigned)(g_vol_size / 1024));
    return a;
}

/* --- real-HW volatile early-prelock / release (the 4 MB-volatile pivot) -----
 * On real hardware the engine grabs the 4 MB volatile partition DURING section
 * streaming, so the lazy "lock at the model-load get_subresource seam" raced and
 * TryLock-failed 0x802B0200. Instead we lock+stage EARLY: armed at quest depart
 * (quest_beginning, before streaming), retried each 10 Hz tick until a free-volatile
 * window is caught, and RELEASED on quest exit so the post-quest save isn't frozen.
 * All three are no-ops on PPSSPP RAW mode (no volatile, staged eagerly). */

/* Try to lock volatile + stage every relocate PAC now. Armed-gated (set by
 * mhfu_inject_xram_arm_prelock at quest depart). Returns 1 when everything that
 * needs staging is staged (or there's nothing to do); 0 means "volatile busy,
 * retry me next tick". Cheap when busy: only a TryLock, no 1.6 MB read. */
/* Volatile-availability probe (diagnostic). On each game-state change the poll calls
 * this to TEST whether the 4 MB volatile partition is grabbable in THIS state, then
 * immediately unlocks on success so the game is never denied. Maps the free-window:
 * we need to know if volatile is ever free in the steady-state village (streaming
 * done) vs. only busy during the quest-depart load. Logs to ms0:/PSP/mhfu_brute_debug.txt. */
extern "C" void mhfu_inject_xram_probe_avail(uint8_t scr, uint16_t area)
{
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode != XR_VOLATILE) return;            /* PPSSPP RAW: N/A */
    if (g_vol_locked > 0) {                            /* we already hold it (mid-quest) */
        RHW_DBG("[vprobe] scr=%u area=%u: WE hold volatile", (unsigned)scr, (unsigned)area);
        return;
    }
    void *ptr = 0; int size = 0;
    int rc = sceKernelVolatileMemTryLock(0, &ptr, &size);
    if (rc >= 0 && ptr && size > 0) {
        sceKernelVolatileMemUnlock(0);                 /* give it right back */
        RHW_DBG("[vprobe] scr=%u area=%u: volatile FREE base=0x%08X size=%uKB",
                (unsigned)scr, (unsigned)area, (unsigned)(uintptr_t)ptr,
                (unsigned)(size / 1024));
    } else {
        RHW_DBG("[vprobe] scr=%u area=%u: volatile BUSY rc=0x%08X",
                (unsigned)scr, (unsigned)area, (unsigned)rc);
    }
}

/* --- read-only volatile USAGE probe (squat feasibility, Option A) -----------
 * The state map shows volatile is the game's per-QUEST scratch (free in village
 * scr=22, busy in-quest scr=17). To squat our Brute in the unused part, measure how
 * much the game touches: snapshot per-64KB-block checksums in the village (pre-quest
 * baseline), then compare in-quest -> the highest CHANGED block = the game's usage
 * high-water; everything above it is a free tail we could squat in. STRICTLY
 * read-only: never writes the game's locked region, so zero risk. */
#define VOL_BASE 0x08400000u
#define VOL_END  0x08800000u
#define VOL_BLK  0x10000u      /* 64 KB */
#define VOL_NBLK 64u           /* (VOL_END-VOL_BASE)/VOL_BLK */
static uint32_t g_vol_csum[VOL_NBLK];
static int      g_vol_have_base;

static uint32_t vol_blk_csum(uint32_t base)
{
    uint32_t s = 0;
    for (uint32_t o = 0; o < VOL_BLK; o += 0x800)        /* 32 samples / 64 KB */
        s = s * 131u + *(volatile uint32_t *)(base + o);
    return s;
}

/* Capture the pre-quest baseline. Call in the village (scr=22) where volatile is
 * free and holds the about-to-depart state. Cheap (2K reads); refreshes each call. */
extern "C" void mhfu_inject_xram_usage_baseline(void)
{
    for (uint32_t b = 0; b < VOL_NBLK; b++)
        g_vol_csum[b] = vol_blk_csum(VOL_BASE + b * VOL_BLK);
    g_vol_have_base = 1;
}

/* In-quest: which blocks did the game change vs. the village baseline. Logs the
 * usage high-water + free-tail size whenever it grows. Call ~1 Hz while scr=17. */
extern "C" void mhfu_inject_xram_usage_scan(void)
{
    if (!g_vol_have_base) return;
    int hi = -1, nchg = 0;
    for (uint32_t b = 0; b < VOL_NBLK; b++) {
        if (vol_blk_csum(VOL_BASE + b * VOL_BLK) != g_vol_csum[b]) { hi = (int)b; nchg++; }
    }
    static int s_hi = -2, s_n = -1;
    if (hi == s_hi && nchg == s_n) return;               /* unchanged -> quiet */
    s_hi = hi; s_n = nchg;
    uint32_t top = (hi >= 0) ? VOL_BASE + (uint32_t)(hi + 1) * VOL_BLK : VOL_BASE;
    RHW_DBG("[vusage] game-used<=0x%08X (%d/%u blk=%uKB) FREE-TAIL 0x%08X+ = %uKB %s",
            (unsigned)top, nchg, (unsigned)VOL_NBLK, (unsigned)((top - VOL_BASE) / 1024),
            (unsigned)top, (unsigned)((VOL_END - top) / 1024),
            ((VOL_END - top) >= 0x199000u) ? "(>=1.6MB OK)" : "(<1.6MB)");
}

extern "C" int mhfu_inject_xram_prelock(void)
{
    if (!g_prelock_armed) return 1;
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode != XR_VOLATILE) return 1;     /* RAW (emulator): nothing to prelock */
    /* Throttle staging retries to ~1 Hz (poll is 10 Hz): a busy lock just retries, and
     * hammering it every 100 ms floods the log with STAGE FAILED. 1 Hz still catches a
     * free window fast. */
    static uint32_t s_throttle = 0;
    if ((s_throttle++ % 10) != 0) return 0;
    int all = 1;
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->relocate || e->buf) continue;  /* already staged */
        e->staged = 0;                              /* clear the don't-retry guard */
        if (!stage_relocate(e)) all = 0;            /* volatile busy -> try again next tick */
    }
    return all;
}

/* Arm early-prelock at quest depart. Runs on the engine's quest-commit thread, so it
 * does ONLY the cheap non-blocking TryLock — claiming the 4 MB volatile at the earliest
 * point, before the loading screen starts streaming and grabs it. The heavy 1.6 MB stage
 * READ is deferred to the 10 Hz poll thread (mhfu_inject_xram_prelock), keeping file I/O
 * off the touchy engine construction path. If this TryLock loses the race (volatile
 * already busy), the poll retries the lock each tick until it frees. */
extern "C" void mhfu_inject_xram_arm_prelock(void)
{
    if (!g_xram_lock_enabled) { RHW_DBG("[xram] arm skipped (lock disabled — recon build)"); return; }
    if (g_prelock_armed) return;
    g_prelock_armed = 1;
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode == XR_VOLATILE) volatile_ensure();   /* cheap claim; no read here */
    RHW_DBG("[xram] prelock ARMED (quest depart); vol_locked=%d", g_vol_locked);
}

/* Release volatile on quest exit. The staged Brute PACs LIVE in volatile, so they
 * are invalidated here (buf=0) and re-staged on the next quest's prelock. Frees the
 * 4 MB so the post-quest reward save / savedata utility doesn't freeze. No-op unless
 * we actually hold volatile. */
extern "C" void mhfu_inject_xram_release(void)
{
    g_prelock_armed = 0;
    if (g_xram_mode != XR_VOLATILE || g_vol_locked <= 0) return;
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (e->used && e->relocate) {              /* its buf was in volatile -> gone now */
            e->buf = 0; e->buf_cap = 0; e->staged = 0; e->redirects = 0;
        }
    }
    g_vol_base = g_vol_size = g_vol_bump = 0;
    sceKernelVolatileMemUnlock(0);
    g_vol_locked = 0;
    RHW_DBG("[xram] VOLATILE released (quest exit) -> save-safe; re-stage next quest");
    mhfu_log("[inject] volatile released on quest exit (save-safe)");
}

/* --- Volatile RECON: quest-depart lock-hold feasibility test ----------------
 * A PURE lock-hold experiment (NO Brute staging) to settle the one open unknown:
 * does MHFU itself need the 4 MB volatile partition DURING an in-quest section
 * load? (Scenario 1 = no -> we can squat our Brute there; Scenario 2 = yes ->
 * the whole-partition mutex makes squatting impossible without an interposer.)
 *
 * Method: acquire volatile at QUEST DEPART (mhfu_dispatch_quest_beginning — the
 * quest-timer 0->nonzero commit, the earliest in-quest-flow point: past savedata,
 * before section streaming), HOLD it across the load, and observe the (native)
 * model load via the poll heartbeat:
 *   - section loads fine while we hold -> Scenario 1 (squat viable).
 *   - section freezes/fails           -> Scenario 2 (need the lock-API interposer).
 * A non-holding village probe (scr=22, TryLock+instant Unlock) separately maps
 * whether volatile is free at idle.
 *
 * SAVE-SAFETY (the freeze class we keep hitting): we acquire ONLY on the positive
 * quest-depart event (never at menu/char-select/village-idle), and RELEASE on the
 * EARLIEST of quest-area exit (17->!17), quest-timer clear, or title/menu (scr 1/4)
 * -> the lock is never held into a savedata window. The acquire runs on the engine
 * quest-commit thread, so it does ONLY the cheap non-blocking TryLock there (NO ms0
 * I/O); the 10 Hz poll flushes the log. No-op on PPSSPP RAW. Toggle g_recon_enabled. */
static int      g_recon_enabled = 0;       /* OBSERVE build: lock-HOLD recon OFF (it would
                                            * deadlock the game's blocking Lock = freeze);
                                            * the vobs pass-through hook below does the work */
static int      g_recon_held;              /* we currently hold volatile (the test) */
static uint32_t g_recon_base, g_recon_size;
static int      g_recon_arm_log;           /* 0 none / 1 acquired / 2 busy (poll flushes) */
static uint32_t g_recon_arm_rc;

/* Quest-depart acquire (engine quest-commit thread): cheap non-blocking TryLock + HOLD.
 * No ms0 I/O here (wrong thread / unsafe screen-state) — result is logged by the poll. */
extern "C" void mhfu_inject_xram_recon_arm(void)
{
    if (!g_recon_enabled || g_recon_held) return;
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode != XR_VOLATILE) return;            /* PPSSPP RAW: no volatile */
    void *ptr = 0; int size = 0;
    int rc = sceKernelVolatileMemTryLock(0, &ptr, &size);
    if (rc >= 0 && ptr && size > 0) {
        g_recon_held = 1;
        g_recon_base = (uint32_t)(uintptr_t)ptr;
        g_recon_size = (uint32_t)size;
        g_recon_arm_log = 1;
    } else {
        g_recon_arm_log = 2;
        g_recon_arm_rc  = (uint32_t)rc;
    }
}

/* Poll-thread driver (10 Hz): flush the acquire result, non-holding village probe,
 * save-safe release, and a held-heartbeat that proves the section loaded while held. */
extern "C" void mhfu_inject_xram_recon_tick(uint8_t scr)
{
    if (!g_recon_enabled) return;
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode != XR_VOLATILE) return;            /* PPSSPP RAW: N/A */

    static uint8_t  s_prev     = 0xFF;
    static uint32_t s_beat     = 0;
    static uint32_t s_vprobe   = 0;
    static int      s_in_quest = 0;     /* reached in-quest (scr=17) while holding */
    static uint32_t s_hold_tk  = 0;     /* ticks held (abort-timeout guard)        */
    static int      s_rel_log  = 0;     /* pending release reason: 1 exit 2 menu 3 timeout */

    /* (1) DURABLE acquire-result log: only clear g_recon_arm_log once we actually wrote
     * it (ms0-safe = scr 17/22). The acquire happens during the depart LOAD (scr not
     * 17/22) where ms0 I/O is gated off, so we must hold the result until scr=17. */
    if (g_recon_arm_log && mhfu_ms0_io_safe()) {
        if (g_recon_arm_log == 1)
            RHW_DBG("[recon] ACQUIRED volatile at quest-depart base=0x%08X size=%uKB -> HELD into load",
                    (unsigned)g_recon_base, (unsigned)(g_recon_size / 1024));
        else
            RHW_DBG("[recon] quest-depart TryLock BUSY rc=0x%08X (already held at depart = Scenario 2?)",
                    (unsigned)g_recon_arm_rc);
        g_recon_arm_log = 0;
    }

    /* (2) HOLD ACROSS THE WHOLE LOAD; SAVE-SAFE release on the earliest of:
     *   - quest-area exit (ONLY after we actually reached the quest — so the depart
     *     loading screen, which is also !=17, does NOT trigger it), OR
     *   - title/menu (scr 1/4), OR
     *   - abort timeout (committed but never reached the quest within ~15 s).
     * The old quest_timer==0 release is REMOVED: the timer is 0 during the depart
     * loading screen, so it was dropping the lock mid-load (the exact window to hold). */
    if (g_recon_held) {
        s_hold_tk++;
        if (scr == 17 && !s_in_quest) {     /* first in-quest tick while holding */
            s_in_quest = 1;
            RHW_DBG("[recon] HELD volatile across load INTO quest (scr=17) area=%u "
                    "-> Scenario 1 (game did NOT need volatile to load the section)",
                    (unsigned)mhfu_get_area_index());
        }
        int leaving = (s_in_quest && s_prev == 17 && scr != 17);
        int at_menu = (scr == 0x01 || scr == 0x04);
        int timeout = (!s_in_quest && s_hold_tk > 150);    /* ~15 s @ 10 Hz */
        if (leaving || at_menu || timeout) {
            sceKernelVolatileMemUnlock(0);
            g_recon_held = 0;
            s_rel_log = leaving ? 1 : (at_menu ? 2 : 3);
            s_in_quest = 0; s_hold_tk = 0;
            g_recon_base = g_recon_size = 0;
        }
    }
    if (s_rel_log && mhfu_ms0_io_safe()) {     /* durable release-reason log */
        RHW_DBG("[recon] RELEASED volatile (%s) -> save-safe",
                s_rel_log == 1 ? "quest-exit" : (s_rel_log == 2 ? "menu" : "abort-timeout"));
        s_rel_log = 0;
    }

    /* non-holding village(22) probe: is volatile free at idle? (TryLock + instant Unlock) */
    if (!g_recon_held && scr == 22 && (s_vprobe++ % 20) == 0) {
        void *ptr = 0; int size = 0;
        int rc = sceKernelVolatileMemTryLock(0, &ptr, &size);
        if (rc >= 0 && ptr && size > 0) {
            sceKernelVolatileMemUnlock(0);
            RHW_DBG("[recon] village(22): volatile FREE base=0x%08X size=%uKB",
                    (unsigned)(uintptr_t)ptr, (unsigned)(size / 1024));
        } else {
            RHW_DBG("[recon] village(22): volatile BUSY rc=0x%08X", (unsigned)rc);
        }
    }

    /* held-heartbeat (~1 Hz). RHW_DBG is ms0-gated to scr 17/22, so the FIRST line
     * after acquire prints once we reach in-quest scr=17 = the section loaded into
     * the quest WHILE we held volatile = Scenario 1 confirmed for this run. */
    if (g_recon_held && (s_beat++ % 10) == 0)
        RHW_DBG("[recon] HOLDING volatile scr=%u area=%u base=0x%08X (load survived)",
                (unsigned)scr, (unsigned)mhfu_get_area_index(), (unsigned)g_recon_base);

    s_prev = scr;
}

/* --- Volatile lock-API OBSERVE hook (real HW) -------------------------------
 * MHFU EU imports the BLOCKING sceKernelVolatileMemLock @ import stub 0x0890E0B0
 * and sceKernelVolatileMemUnlock @ 0x0890E0B8 (it does NOT use TryLock). We repoint
 * each stub to a same-ABI C wrapper that calls the REAL function (pure pass-through
 * — zero behavior change), records the call into a small SPSC ring, and returns v0
 * to the game caller (the game's $ra is preserved across our `j`, so a normal return
 * lands back in the game). The poll thread drains the ring to the log ONLY when
 * ms0-safe (scr 17/22), so Lock/Unlock that happen during the depart LOAD (ms0-unsafe)
 * are still captured. Answers: when/how often the game locks, the (ptr,size) it gets
 * back, whether it ever unlocks mid-quest, and the full lock lifecycle. Real-HW only
 * (XR_VOLATILE); on PPSSPP RAW it is never installed. */
#define VOBS_LOCK_STUB   0x0890E0B0u
#define VOBS_UNLOCK_STUB 0x0890E0B8u

typedef struct { uint8_t kind; uint8_t scr; uint16_t area;
                 uint32_t a0, ptr, size; int32_t rc; uint32_t seq; } vobs_evt_t;
#define VOBS_RING 32
static vobs_evt_t        g_vobs_ring[VOBS_RING];
static volatile uint32_t g_vobs_head, g_vobs_tail;   /* game thread pushes, poll drains */
static uint32_t          g_vobs_seq;
static int               g_vobs_installed;

static void vobs_push(uint8_t kind, uint32_t a0, uint32_t ptr, uint32_t size, int rc)
{
    uint32_t h = g_vobs_head;
    vobs_evt_t *e = &g_vobs_ring[h % VOBS_RING];
    e->kind = kind; e->a0 = a0; e->ptr = ptr; e->size = size; e->rc = rc;
    e->scr = mhfu_get_screen_state(); e->area = mhfu_get_area_index(); e->seq = ++g_vobs_seq;
    g_vobs_head = h + 1;                              /* publish after fill */
}

/* --- Volatile INTERPOSER ("proxy"): carve a private slice from the game's lock -----
 * vobs proved the game locks the whole 4 MB once at quest depart and never unlocks it.
 * So in the Lock wrapper, AFTER the real Lock returns (partition mapped, lock held, on
 * the game's thread = the ONLY provably-safe window to touch volatile), we stage every
 * relocate PAC (the Brute) into the TOP of the partition, then hand the game a REDUCED
 * size so it streams into the BOTTOM. The existing get_subresource redirect then points
 * the engine at our Brute (e->buf now lives in volatile-top). Every byte of OUR volatile
 * access is confined to this held window — no unlocked reads/writes anywhere. If the game
 * ignores the reduced size and uses the top, the Brute renders garbled / the load fails
 * (recoverable — don't save) = the precondition-1 verdict. Toggle g_proxy_enabled. */
static int      g_proxy_enabled = 1;
static uint32_t g_proxy_reserved;       /* bytes carved off the top (0 = transparent) */
static int      g_proxy_done;           /* relocate PACs placed into volatile-top      */

/* Read a grown PAC file into [dst, dst+cap). Returns the file size, 0 on any failure.
 * Runs on the game thread inside the held Lock window, BEFORE the game streams the
 * section (so no contention with the game's own load reads). */
static uint32_t proxy_read_pac(const char *path, uint32_t dst, uint32_t cap)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { RHW_DBG("[proxy] open FAILED %s rc=0x%08X", path, (unsigned)fd); return 0; }
    SceOff sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    uint32_t fsz = (uint32_t)sz;
    if (fsz < 0x40 || fsz > cap) {
        sceIoClose(fd);
        RHW_DBG("[proxy] %s size=%uB > cap=%uB -> skip", path, (unsigned)fsz, (unsigned)cap);
        return 0;
    }
    int rd = sceIoRead(fd, (void *)dst, (int)fsz);
    sceIoClose(fd);
    if (rd != (int)fsz) { RHW_DBG("[proxy] read short rc=0x%08X", (unsigned)rd); return 0; }
    sceKernelDcacheWritebackRange((void *)dst, fsz);
    return fsz;
}

/* Stage all un-staged relocate entries into the TOP of the held partition; set e->buf
 * so the get_subresource redirect uses volatile-top. Returns the reserve size (bytes
 * carved off the top), 0 if nothing staged (-> caller stays transparent). */
static uint32_t proxy_stage(uint32_t lock_base, uint32_t lock_size)
{
    if (!g_proxy_enabled || g_proxy_done) return g_proxy_reserved;
    uint32_t reserve = 0;                          /* pass 1: sum grown sizes (16 KB-aligned) */
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->relocate || e->buf) continue;
        SceUID fd = sceIoOpen(e->path, PSP_O_RDONLY, 0);
        if (fd < 0) continue;
        uint32_t fsz = (uint32_t)sceIoLseek(fd, 0, PSP_SEEK_END);
        sceIoClose(fd);
        reserve += (fsz + 0x3FFFu) & ~0x3FFFu;
    }
    if (reserve == 0) return 0;
    if (reserve > lock_size / 2) {                 /* refuse to take more than half the partition */
        RHW_DBG("[proxy] reserve=%uKB too big for %uKB -> stay transparent",
                (unsigned)(reserve / 1024), (unsigned)(lock_size / 1024));
        g_proxy_done = 1;                          /* don't re-attempt; native fallback */
        return 0;
    }
    uint32_t bump = lock_base + lock_size - reserve;   /* pass 2: place into the TOP slice */
    RHW_DBG("[proxy] reserve=%uKB top=[0x%08X,0x%08X) game gets [0x%08X,0x%08X)",
            (unsigned)(reserve / 1024), (unsigned)bump, (unsigned)(lock_base + lock_size),
            (unsigned)lock_base, (unsigned)bump);
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->relocate || e->buf) continue;
        uint32_t cap = (lock_base + lock_size) - bump;
        uint32_t fsz = proxy_read_pac(e->path, bump, cap);
        if (!fsz) continue;
        e->buf = bump; e->buf_cap = (fsz + 0x3FFFu) & ~0x3FFFu;
        e->file_size = fsz; e->staged = 1;
        RHW_DBG("[proxy] staged file=%u grown=%uB @0x%08X (volatile top)",
                (unsigned)e->file_id, (unsigned)fsz, (unsigned)bump);
        bump += e->buf_cap;
    }
    g_proxy_reserved = reserve;
    g_proxy_done = 1;
    return reserve;
}

/* Repointed-stub landing pads: same ABI as the originals, so `return` (jr $ra) goes
 * straight back to the game caller with v0. Run on the GAME thread, possibly mid-load
 * (ms0-unsafe) -> NO logging here, only a cheap ring push (proxy_stage uses RHW_DBG,
 * which self-gates on ms0 safety). */
extern "C" int mhfu_vobs_lock(int unk, void **pptr, int *psize)
{
    int rc = sceKernelVolatileMemLock(unk, pptr, psize);     /* REAL, blocking — we hold 4 MB */
    if (rc >= 0 && pptr && psize) {
        uint32_t base = (uint32_t)(uintptr_t)*pptr;
        uint32_t full = (uint32_t)*psize;
        uint32_t reserve = proxy_stage(base, full);          /* place Brute in the TOP */
        if (reserve) {
            *psize = full - reserve;                         /* hand the game the BOTTOM only */
            vobs_push(3, (uint32_t)unk, base, *psize, rc);   /* kind 3 = SHRUNK */
        } else {
            vobs_push(1, (uint32_t)unk, base, full, rc);     /* transparent (nothing to stage) */
        }
    } else {
        vobs_push(1, (uint32_t)unk, 0, 0, rc);
    }
    return rc;
}
extern "C" int mhfu_vobs_unlock(int unk)
{
    int rc = sceKernelVolatileMemUnlock(unk);
    vobs_push(2, (uint32_t)unk, 0, 0, rc);
    return rc;
}

/* Repoint the two import stubs. Real-HW only; idempotent. Patch the delay-slot word
 * (the resolved `syscall N`) to NOP first, then the jump, so the stub is never left as
 * `j wrapper` + stale `syscall`. mhfu_patch_word flushes the caches. */
extern "C" void mhfu_vobs_install(void)
{
    if (g_vobs_installed) return;
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode != XR_VOLATILE) return;           /* PPSSPP RAW: never hook */
    mhfu_patch_word(VOBS_LOCK_STUB   + 4, MIPS_NOP, "vobs");
    mhfu_patch_word(VOBS_LOCK_STUB,       mips_j((uint32_t)(uintptr_t)&mhfu_vobs_lock),   "vobs");
    mhfu_patch_word(VOBS_UNLOCK_STUB + 4, MIPS_NOP, "vobs");
    mhfu_patch_word(VOBS_UNLOCK_STUB,     mips_j((uint32_t)(uintptr_t)&mhfu_vobs_unlock), "vobs");
    g_vobs_installed = 1;
    RHW_DBG("[vobs] installed: Lock stub 0x%08X -> 0x%08X, Unlock stub 0x%08X -> 0x%08X",
            (unsigned)VOBS_LOCK_STUB,   (unsigned)(uintptr_t)&mhfu_vobs_lock,
            (unsigned)VOBS_UNLOCK_STUB, (unsigned)(uintptr_t)&mhfu_vobs_unlock);
}

/* Drain the observe ring to the log. Poll thread; ms0-gated so events buffered during
 * a load flush once we reach scr 17/22 (never lose them to the ms0 gate). */
extern "C" void mhfu_vobs_flush(void)
{
    if (!g_vobs_installed || !mhfu_ms0_io_safe()) return;
    while (g_vobs_tail != g_vobs_head) {
        vobs_evt_t *e = &g_vobs_ring[g_vobs_tail % VOBS_RING];
        if (e->kind == 1)
            RHW_DBG("[vobs] #%u LOCK(unk=%u) -> rc=0x%08X ptr=0x%08X size=%uKB @scr=%u area=%u",
                    (unsigned)e->seq, (unsigned)e->a0, (unsigned)e->rc, (unsigned)e->ptr,
                    (unsigned)(e->size / 1024), (unsigned)e->scr, (unsigned)e->area);
        else if (e->kind == 3)
            RHW_DBG("[vobs] #%u LOCK SHRUNK(unk=%u) -> game gets base=0x%08X size=%uKB "
                    "(we reserved %uKB top) @scr=%u area=%u",
                    (unsigned)e->seq, (unsigned)e->a0, (unsigned)e->ptr,
                    (unsigned)(e->size / 1024), (unsigned)(g_proxy_reserved / 1024),
                    (unsigned)e->scr, (unsigned)e->area);
        else
            RHW_DBG("[vobs] #%u UNLOCK(unk=%u) -> rc=0x%08X @scr=%u area=%u",
                    (unsigned)e->seq, (unsigned)e->a0, (unsigned)e->rc,
                    (unsigned)e->scr, (unsigned)e->area);
        g_vobs_tail++;
    }
}

/* Parse the edited PAC (in e->buf) into the sub table: u32 count, then
 * count*(u32 off, u32 size); magic = first u32 of each sub. */
static void parse_subs(inject_entry_t *e)
{
    e->nsubs = 0;
    if (!e->buf || e->file_size < 8) return;
    uint32_t cnt = rd32(e->buf);
    if (cnt > MHFU_MAX_SUBS) cnt = MHFU_MAX_SUBS;
    for (uint32_t t = 0; t < cnt; t++) {
        uint32_t off  = rd32(e->buf + 4 + t * 8);
        uint32_t size = rd32(e->buf + 8 + t * 8);
        if (off == 0 || size == 0 || off + size > e->file_size) {
            e->subs[t].off = e->subs[t].size = e->subs[t].magic = 0;
            e->nsubs = t + 1;
            continue;
        }
        e->subs[t].off   = off;
        e->subs[t].size  = size;
        e->subs[t].magic = rd32(e->buf + off);
        e->nsubs = t + 1;
    }
}

static int read_file(inject_entry_t *e)
{
    SceUID fd = sceIoOpen(e->path, PSP_O_RDONLY, 0);
    if (fd < 0) { mhfu_log("[inject] open FAILED %s rc=0x%08X", e->path, (unsigned)fd); return -1; }
    SceOff sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    uint32_t fsz = (uint32_t)sz;
    if (fsz < 0x40) { sceIoClose(fd); mhfu_log("[inject] file too small %u", (unsigned)fsz); return -2; }

    if (e->buf == 0 || fsz > e->buf_cap) {
        uint32_t cap = (fsz + 0xFFFu) & ~0xFFFu;
        uint32_t b = xram_alloc(cap);
        if (!b) { sceIoClose(fd); mhfu_log("[inject] xram exhausted (need %uKB)", (unsigned)(cap / 1024)); return -3; }
        e->buf = b; e->buf_cap = cap;
    }
    int rd = sceIoRead(fd, (void *)e->buf, (int)fsz);
    sceIoClose(fd);
    if (rd != (int)fsz) { mhfu_log("[inject] read short rc=0x%08X", (unsigned)rd); return -4; }
    e->file_size = fsz;
    parse_subs(e);

    /* Also load the ORIGINAL PAC (<path>.orig) for unique species identification:
     * we overwrite a fetched sub only if it byte-matches the ORIGINAL species sub
     * (unique), then write our EDITED bytes — so look-alike monsters never match. */
    char opath[176];
    snprintf(opath, sizeof(opath), "%s.orig", e->path);
    SceUID ofd = sceIoOpen(opath, PSP_O_RDONLY, 0);
    if (ofd >= 0) {
        if (e->obuf == 0 || fsz > e->obuf_cap) {
            uint32_t cap = (fsz + 0xFFFu) & ~0xFFFu;
            uint32_t b = xram_alloc(cap);
            if (b) { e->obuf = b; e->obuf_cap = cap; }
        }
        if (e->obuf) {
            int ord = sceIoRead(ofd, (void *)e->obuf, (int)fsz);
            if (ord != (int)fsz) { e->obuf = 0; mhfu_log("[inject] .orig read short"); }
            else mhfu_log("[inject] loaded ORIGINAL %s for unique match", opath);
        }
        sceIoClose(ofd);
    } else {
        e->obuf = 0;
        mhfu_log("[inject] no .orig (%s) -> falling back to 64B-prefix match", opath);
    }

    /* Compute the diff fingerprint: first WORD where edit != orig. */
    e->has_diff = 0;
    if (e->obuf && e->buf) {
        for (uint32_t o = 0; o + 4 <= fsz; o += 4) {
            uint32_t ew = rd32(e->buf + o), ow = rd32(e->obuf + o);
            if (ew != ow) {
                e->has_diff = 1; e->diff_off = o; e->diff_orig = ow; e->diff_edit = ew;
                break;
            }
        }
        if (e->has_diff)
            mhfu_log("[inject] diff fingerprint @+0x%X orig=0x%08X edit=0x%08X",
                     (unsigned)e->diff_off, (unsigned)e->diff_orig, (unsigned)e->diff_edit);
        else
            mhfu_log("[inject] WARNING edit == orig (no geometry change?)");
    }
    return 0;
}

/* Core racefree primitive: if `buf` is a registered file's RAW pre-transform PAC
 * buffer, sitting pristine (== .orig), overwrite the WHOLE thing with our edit.
 * Gates: 256 B header == .orig (species-unique) AND the diff-fingerprint word ==
 * orig (pristine, since the header is identical pristine-vs-edited). Returns 1 on
 * overwrite. Used by BOTH the get_subresource hook (pre-transform consumption
 * point, the proven correct timing) and the descriptor-table scan. */
static int try_overwrite_buffer(uint32_t buf)
{
    if (!gmem_ok(buf)) return 0;
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->buf || !e->obuf || !e->has_diff || e->file_size < 0x40) continue;
        if (!gmem_ok(buf + e->file_size - 1)) continue;
        uint32_t g = e->file_size < 256 ? e->file_size : 256;
        if (memcmp((const void *)buf, (const void *)e->obuf, g) != 0) continue;  /* not our species */
        uint32_t cur = rd32(buf + e->diff_off);
        if (cur == e->diff_edit) return 0;     /* already our edit */
        if (cur != e->diff_orig) continue;     /* not pristine (mid-write / other) */
        memcpy((void *)buf, (const void *)e->buf, e->file_size);
        e->applied_addr = buf;
        sceKernelDcacheWritebackRange((void *)buf, e->file_size);
        e->hits++;
        mhfu_log("[inject] OVERWROTE raw buffer file=%u @0x%08X (%uB)",
                 (unsigned)e->file_id, (unsigned)buf, (unsigned)e->file_size);
        return 1;
    }
    return 0;
}

/* RELOCATE: if a0 is the engine's raw buffer for a relocate-registered species
 * (its first 256 B == our stored ORIGINAL header), rewrite a0 to point at our
 * BIGGER replacement PAC in xram. The trampoline reloads a0 from the stack slot
 * after we return (see trampoline.cpp), so get_subresource runs on OUR buffer and
 * the transform reformats our larger PMO. Returns 1 if redirected. */
static int try_redirect_pkg(mhfu_anchor_regs_t *regs)
{
    uint32_t a0 = regs->a0;
    if (!gmem_ok(a0) || !gmem_ok(a0 + 255)) return 0;
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->relocate || e->orig_size < 0x40) continue;
        /* Match against the stored ORIGINAL header (256 B in user RAM — no xram/volatile
         * needed just to recognize the engine's raw buffer). */
        if (memcmp((const void *)a0, e->orig_hdr, 256) != 0) continue; /* not our species */
        /* It's ours and we're in-quest at the model load -> NOW stage the grown PAC into
         * volatile (locks it here, the moment it's really needed). PPSSPP RAW staged it
         * at registration so e->buf is already set and this is a no-op. */
        if (!e->buf && !stage_relocate(e)) continue;   /* stage failed -> let engine use native */
        regs->a0 = e->buf;                 /* -> grown PAC in xram */
        if (e->redirects == 0) {
            mhfu_log("[inject] RELOCATE redirect file=%u a0 0x%08X -> 0x%08X (grown %uB)",
                     (unsigned)e->file_id, (unsigned)a0, (unsigned)e->buf, (unsigned)e->file_size);
            RHW_DBG("[realhw] redirect FIRED file=%u a0=0x%08X -> 0x%08X grown=%uB",
                    (unsigned)e->file_id, (unsigned)a0, (unsigned)e->buf,
                    (unsigned)e->file_size);
        }
        e->redirects++;
        return 1;
    }
    return 0;
}

/* Prefix-trampoline on get_subresource(pkg=a0, type=a1). The big-mon transform
 * calls this ON THE RAW count=7 buffer to extract subs (captured live 2026-06-18:
 * a0=raw buffer, before the cache is built). RELOCATE entries redirect a0 to a
 * grown PAC; same-size entries overwrite the raw buffer in place. EBOOT code =>
 * JIT-warm, no overlay-timing race. */
extern "C" void mhfu_dispatch_get_subresource(const mhfu_anchor_regs_t *regs)
{
    if (try_redirect_pkg((mhfu_anchor_regs_t *)regs)) return;
    try_overwrite_buffer(regs->a0);
}

int mhfu_inject_register(uint32_t file_id, const char *path)
{
    if (!path || !path[0]) return -1;
    inject_entry_t *e = find_entry(file_id);
    if (!e) {
        for (int i = 0; i < MHFU_INJECT_MAX; i++)
            if (!g_tab[i].used) { e = &g_tab[i]; break; }
        if (!e) { mhfu_log("[inject] table full"); return -2; }
        memset(e, 0, sizeof(*e));
        e->used = 1;
        e->file_id = file_id;
    }
    snprintf(e->path, sizeof(e->path), "%s", path);

    /* PRIMARY racefree seam (proven 2026-06-18): prefix-trampoline on get_subresource
     * 0x088B89B0. The big-mon transform calls it on the RAW count=7 buffer; we
     * overwrite the buffer there, synchronously, before the sub is extracted. EBOOT
     * code => JIT-warm, installs cleanly. (The earlier "post-transform" verdict was
     * for a DIFFERENT caller passing the restructured package; the raw-buffer caller
     * is pre-transform.) */
    if (!g_hook_installed) {
        int rc = mhfu_install_trampoline(GETSUB_ADDR, (uint32_t)&mhfu_dispatch_get_subresource);
        g_hook_installed = (rc == 0);
        mhfu_log("[inject] get_subresource trampoline @0x%08X rc=%d", GETSUB_ADDR, rc);
    }

    /* Belt-and-suspenders: also arm the 0x09AC4D30 game-thread detour (a per-frame
     * descriptor scan). It fires post-transform so it can't fix the render alone,
     * but it keeps the raw buffer pinned to our edit and costs only a gated scan. */
    arm_model_setup_hook();

    mhfu_log("[inject] register file=%u path=%s (getsub seam @0x%08X + detour @0x%08X)",
             (unsigned)file_id, e->path, GETSUB_ADDR, MODEL_SETUP_ENTRY);
    return 0;
}

/* Load a PAC file into a fresh xram block. Returns base (0 on failure); sets *out_sz. */
static uint32_t load_pac_to_xram(const char *path, uint32_t *out_sz)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { mhfu_log("[inject] reloc open FAILED %s rc=0x%08X", path, (unsigned)fd); return 0; }
    SceOff sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    uint32_t fsz = (uint32_t)sz;
    if (fsz < 0x40) { sceIoClose(fd); return 0; }
    uint32_t cap = (fsz + 0xFFFu) & ~0xFFFu;
    uint32_t b = xram_alloc(cap);
    if (!b) { sceIoClose(fd); mhfu_log("[inject] reloc xram exhausted (%uKB)", (unsigned)(cap / 1024)); return 0; }
    int rd = sceIoRead(fd, (void *)b, (int)fsz);
    sceIoClose(fd);
    if (rd != (int)fsz) { mhfu_log("[inject] reloc read short %s", path); return 0; }
    *out_sz = fsz;
    return b;
}

/* Deferred staging of the grown PAC into xram. On real HW (VOLATILE) this is the point
 * the 4 MB volatile partition is finally locked — driven from try_redirect_pkg, i.e. the
 * instant the Brute model loads in a quest. On PPSSPP (RAW) it's called eagerly at
 * registration (no volatile, nothing to defer). Idempotent: returns 1 if buf is staged. */
static int stage_relocate(inject_entry_t *e)
{
    if (e->buf) return 1;
    if (e->staged) return 0;          /* already attempted and failed — don't retry-spam */
    e->staged = 1;
    uint32_t gsz = 0;
    e->buf = load_pac_to_xram(e->path, &gsz);   /* VOLATILE: volatile_ensure() locks here */
    if (!e->buf) {
        mhfu_log("[inject] reloc grown stage FAILED %s", e->path);
        RHW_DBG("[realhw] relocate STAGE FAILED file=%u (volatile lock/read?) -> native",
                (unsigned)e->file_id);
        return 0;
    }
    e->file_size = gsz;        /* grown size */
    e->buf_cap   = (gsz + 0xFFFu) & ~0xFFFu;
    parse_subs(e);             /* parse the GROWN sub table (for diagnostics) */
    mhfu_log("[inject] RELOCATE staged file=%u grown=%uB@0x%08X",
             (unsigned)e->file_id, (unsigned)gsz, (unsigned)e->buf);
    RHW_DBG("[realhw] relocate STAGED (in-quest) file=%u grown=%uB@0x%08X",
            (unsigned)e->file_id, (unsigned)gsz, (unsigned)e->buf);
    return 1;
}

/* RELOCATE registration (Phase 5 topology-grow): grown_path = the BIGGER edited PAC,
 * orig_path = the ORIGINAL (un-grown) PAC used to recognize the engine's raw buffer.
 * We keep only the orig's 256-byte header (user RAM) for matching; the grown PAC is
 * staged into xram lazily at the get_subresource match. On RAW (PPSSPP) we stage it
 * immediately (the proven path); on VOLATILE (real HW) we defer so the volatile
 * partition stays free for the game's savedata/utility paths until a Brute quest. */
int mhfu_inject_register_relocate(uint32_t file_id, const char *grown_path,
                                  const char *orig_path)
{
    if (!grown_path || !orig_path) return -1;
    inject_entry_t *e = find_entry(file_id);
    if (!e) {
        for (int i = 0; i < MHFU_INJECT_MAX; i++)
            if (!g_tab[i].used) { e = &g_tab[i]; break; }
        if (!e) { mhfu_log("[inject] table full"); return -2; }
    }
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->file_id = file_id;
    e->relocate = 1;
    snprintf(e->path,      sizeof(e->path),      "%s", grown_path);
    snprintf(e->orig_path, sizeof(e->orig_path), "%s", orig_path);

    /* Read only the orig's 256-byte header + its size — enough to recognize the engine's
     * raw buffer at get_subresource. No xram, no volatile lock. */
    SceUID ofd = sceIoOpen(orig_path, PSP_O_RDONLY, 0);
    if (ofd < 0) { e->used = 0; mhfu_log("[inject] reloc orig open FAILED %s", orig_path); return -3; }
    int hrd = sceIoRead(ofd, e->orig_hdr, (int)sizeof(e->orig_hdr));
    SceOff osz = sceIoLseek(ofd, 0, PSP_SEEK_END);
    sceIoClose(ofd);
    if (hrd != (int)sizeof(e->orig_hdr) || osz < 0x40) {
        e->used = 0; mhfu_log("[inject] reloc orig header short %s", orig_path); return -3;
    }
    e->orig_size = (uint32_t)osz;

    if (!g_hook_installed) {
        int rc = mhfu_install_trampoline(GETSUB_ADDR, (uint32_t)&mhfu_dispatch_get_subresource);
        g_hook_installed = (rc == 0);
        mhfu_log("[inject] get_subresource trampoline @0x%08X rc=%d", GETSUB_ADDR, rc);
    }

    /* Decide region now (cheap, fault-safe). RAW (emulator) -> stage eagerly, proven.
     * VOLATILE (hardware) -> defer staging to the first in-quest match (stage_relocate). */
    if (!g_xram_mode) mhfu_xram_platform_init();
    if (g_xram_mode == XR_VOLATILE) {
        mhfu_log("[inject] RELOCATE register (lazy/volatile) file=%u grown=%s orig=%s (%uB)",
                 (unsigned)file_id, grown_path, orig_path, (unsigned)osz);
        RHW_DBG("[realhw] relocate REGISTERED (lazy) file=%u grown=%s orig_size=%uB"
                " — volatile deferred to quest", (unsigned)file_id, grown_path, (unsigned)osz);
        return 0;
    }
    /* RAW: stage now (no volatile to conflict with). */
    if (!stage_relocate(e)) { e->used = 0; mhfu_log("[inject] reloc eager stage FAILED"); return -3; }
    mhfu_log("[inject] RELOCATE register file=%u grown=%uB@0x%08X orig=%uB",
             (unsigned)file_id, (unsigned)e->file_size, (unsigned)e->buf, (unsigned)osz);
    return 0;
}

/* Scan the overlay descriptor table for any registered file's RAW buffer and
 * overwrite it (via try_overwrite_buffer). Secondary to the get_subresource seam;
 * keeps the buffer pinned to our edit. Returns # overwritten this pass. */
static int scan_descriptor_overwrite(void)
{
    uint32_t base = rd32(DESCR_TBL_PTR);
    if (!gmem_ok(base)) return 0;
    int n = 0;
    for (uint32_t s = 0; s < DESCR_MAX; s++) {
        uint32_t ent = base + s * DESCR_STRIDE;
        if (!gmem_ok(ent)) break;
        uint32_t buf = rd32(ent + DESCR_BUF_OFF);
        if (!gmem_ok(buf)) continue;
        n += try_overwrite_buffer(buf);
    }
    return n;
}

/* --- game-thread entry-detour on the model-setup consumer 0x09AC4D30 ------- */

static uint32_t *g_model_wrapper;
static int       g_model_hook_installed;
static int       g_model_arm;           /* overlay-loaded + section subscribe done */

/* Synchronous game-thread overwrite: runs at the very entry of 0x09AC4D30,
 * before its body reads the raw buffer. Arg regs are preserved by the wrapper. */
extern "C" void mhfu_inject_model_setup_dispatch_c(void)
{
    scan_descriptor_overwrite();
}

/* Wrapper (18 insns; cave 20): save a0-a3 + ra, call the dispatch, restore,
 * replay the 2 displaced prologue insns of 0x09AC4D30, jump back to +8. The
 * replayed prologue runs on the ENGINE's frame (our own -0x20/+0x20 balanced
 * first), identical to what the un-patched entry would have executed. */
static void build_model_setup_wrapper(uint32_t *w, uint32_t dispatch_addr)
{
    int i = 0;
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    w[i++] = mips_sw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_sw   (MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_jal  (dispatch_addr);
    w[i++] = MIPS_NOP;                              /* delay slot */
    w[i++] = mips_lw   (MIPS_REG_A0, 0x10, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A1, 0x14, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A2, 0x08, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_A3, 0x0C, MIPS_REG_SP);
    w[i++] = mips_lw   (MIPS_REG_RA, 0x18, MIPS_REG_SP);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    w[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);   /* replay displaced #0 */
    w[i++] = mips_sw   (MIPS_REG_RA, 0x1C, MIPS_REG_SP);    /* replay displaced #1 */
    w[i++] = mips_j    (MODEL_SETUP_RESUME);
    w[i++] = MIPS_NOP;                              /* J delay slot */
    while (i < 20) w[i++] = MIPS_NOP;
}

/* Patch 0x09AC4D30 -> our wrapper, but ONLY when its bytes are the original
 * prologue (JIT-cold) and a model edit is actually registered. Self-gating so
 * re-fire from the overlay/section repatch is a safe no-op. */
static int install_model_setup_hook(void)
{
    if (g_model_hook_installed) return 0;
    int any = 0;
    for (int i = 0; i < MHFU_INJECT_MAX; i++)
        if (g_tab[i].used && g_tab[i].buf && g_tab[i].obuf) { any = 1; break; }
    if (!any) return 0;                              /* nothing to inject — skip */

    uint32_t *w = g_model_wrapper;
    if (!w) {
        w = mhfu_cave_alloc(20);
        if (!w) { mhfu_log("[inject] cave exhausted for model-setup wrapper"); return -1; }
        build_model_setup_wrapper(w, (uint32_t)(uintptr_t)&mhfu_inject_model_setup_dispatch_c);
        mhfu_flush_caches();
        g_model_wrapper = w;
    }

    uint32_t cur0 = *(volatile uint32_t *)MODEL_SETUP_ENTRY;
    uint32_t cur1 = *(volatile uint32_t *)(MODEL_SETUP_ENTRY + 4);
    if (cur0 != MODEL_SETUP_DISP0 || cur1 != MODEL_SETUP_DISP1) {
        mhfu_log("[inject] model-setup entry not original (0x%08X) — skip patch",
                 (unsigned)cur0);
        return -1;
    }
    *(volatile uint32_t *)MODEL_SETUP_ENTRY       = mips_j((uint32_t)(uintptr_t)w);
    *(volatile uint32_t *)(MODEL_SETUP_ENTRY + 4) = MIPS_NOP;
    mhfu_flush_caches();
    g_model_hook_installed = 1;
    mhfu_log("[inject] model-setup detour (re)patched @ 0x%08X (wrapper 0x%08X)",
             MODEL_SETUP_ENTRY, (unsigned)(uintptr_t)w);
    return 0;
}

/* Overlay just (re)loaded JIT-cold -> bytes pristine: (re)install the detour. */
extern "C" void mhfu_inject_overlay_loaded_cb(const mhfu_ai_overlay_ctx_t * /*ctx*/)
{
    g_model_hook_installed = 0;
    install_model_setup_hook();
}

/* Belt-and-suspenders: a section roam can re-translate our patch away; re-arm
 * on each section entry (self-gates on the original prologue). */
extern "C" void mhfu_inject_section_repatch_cb(const void * /*ctx*/)
{
    g_model_hook_installed = 0;
    install_model_setup_hook();
}

/* Arm the install triggers once (called from mhfu_inject_register). */
static void arm_model_setup_hook(void)
{
    if (g_model_arm) return;
    mhfu_on_ai_overlay_loaded(mhfu_inject_overlay_loaded_cb, 0);
    mhfu_register_event(MHFU_EVENT_MAP_SECTION_ENTERED,
                        (mhfu_event_cb_t)(void *)mhfu_inject_section_repatch_cb);
    g_model_arm = 1;
}

/* Legacy worker-thread hammer (the racing path). No longer wired into the
 * worker tick — kept for manual/diagnostic use only. The game-thread detour
 * above is the production racefree path. */
#define BURST_MS 1500
void mhfu_inject_burst(void)
{
    for (int t = 0; t < BURST_MS / 5; t++) {
        scan_descriptor_overwrite();
        sceKernelDelayThread(5 * 1000);   /* 5 ms -> ~200 Hz */
    }
}

void mhfu_inject_tick(void)
{
    /* Per-tick sceIoGetstat below is ms0 I/O — skip it outside active gameplay so it
     * can't race the savedata utility (relocate entries already skip it; this also
     * covers any non-relocate mod). */
    if (!mhfu_ms0_io_safe()) return;
    /* Keep each edited PAC fresh in xram; reload when the memstick file changes. */
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used) continue;
        if (e->relocate) continue;   /* fully set up at register; read_file would wipe obuf */
        SceIoStat st;
        memset(&st, 0, sizeof(st));
        if (sceIoGetstat(e->path, &st) < 0) continue;
        int changed = !e->primed
                    || e->st_size != st.st_size
                    || memcmp(&e->st_mtime, &st.sce_st_mtime, sizeof(ScePspDateTime)) != 0;
        if (!changed) continue;
        e->st_size = st.st_size; e->st_mtime = st.sce_st_mtime; e->primed = 1;
        if (read_file(e) == 0) {
            mhfu_log("[inject] loaded edit file=%u (%uB, %d subs) — game-thread detour applies on load",
                     (unsigned)e->file_id, (unsigned)e->file_size, e->nsubs);
            /* Files just became available: arm the detour if a section is
             * already loaded (overlay-loaded fire may have preceded the load). */
            install_model_setup_hook();
        }
    }
    /* NOTE: the worker no longer overwrites the raw buffer — that 1.2 MB memcpy
     * raced the engine's parse on the game thread (crash). The overwrite now runs
     * synchronously from the 0x09AC4D30 entry-detour (install_model_setup_hook).
     * The worker's only job here is keeping the edited PAC fresh in xram. */
}

uint32_t mhfu_inject_now(uint32_t file_id)
{
    inject_entry_t *e = find_entry(file_id);
    if (!e) return 0;
    if (read_file(e) != 0) return 0;
    e->primed = 0;
    return e->hits;   /* # of in-game overwrites so far (diagnostic) */
}

uint32_t mhfu_inject_locate(uint32_t file_id)
{
    inject_entry_t *e = find_entry(file_id);
    return e ? e->hits : 0;   /* repurposed: overwrite count */
}
