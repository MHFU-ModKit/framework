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
#include "mhfu/mips.h"         /* mips_* encoders for the game-thread detour */
#include "mhfu/ai.h"           /* mhfu_on_ai_overlay_loaded (JIT-cold install window) */
#include "mhfu/events.h"       /* MHFU_EVENT_MAP_SECTION_ENTERED repatch */
#include "internal.h"          /* mhfu_anchor_regs_t, cave_alloc, flush_caches */

#include <pspiofilemgr.h>
#include <psputils.h>          /* sceKernelDcacheWritebackRange */
#include <pspthreadman.h>      /* sceKernelDelayThread */
#include <string.h>
#include <stdio.h>             /* snprintf */

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
static uint32_t g_xram_bump = MHFU_INJECT_XRAM_LO;

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
} inject_entry_t;

static inject_entry_t g_tab[MHFU_INJECT_MAX];
static int            g_hook_installed;

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline int gmem_ok(uint32_t a) { return a >= 0x08000000u && a < 0x0A000000u; }

static void arm_model_setup_hook(void);        /* defined below (game-thread detour) */
static int  install_model_setup_hook(void);

static inject_entry_t *find_entry(uint32_t file_id)
{
    for (int i = 0; i < MHFU_INJECT_MAX; i++)
        if (g_tab[i].used && g_tab[i].file_id == file_id) return &g_tab[i];
    return 0;
}

static int xram_mapped(uint32_t addr)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    uint32_t save = *p; *p = 0xA5C30F04u;
    int ok = (*p == 0xA5C30F04u); *p = save;
    return ok;
}

static uint32_t xram_alloc(uint32_t n)
{
    uint32_t a = (g_xram_bump + 15u) & ~15u;
    if (a + n > MHFU_INJECT_XRAM_HI) return 0;
    if (!xram_mapped(a)) return 0;
    g_xram_bump = a + n;
    return a;
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

/* Prefix-trampoline on get_subresource(pkg=a0, type=a1). The big-mon transform
 * calls this ON THE RAW count=7 buffer to extract subs (captured live 2026-06-18:
 * a0=raw buffer, before the cache is built). As a PREFIX we overwrite the raw
 * buffer in place BEFORE get_subresource returns the sub pointer -> the transform
 * extracts OUR data. EBOOT code => JIT-warm, no overlay-timing race. */
extern "C" void mhfu_dispatch_get_subresource(const mhfu_anchor_regs_t *regs)
{
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
    /* Keep each edited PAC fresh in xram; reload when the memstick file changes. */
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used) continue;
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
