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
#include "internal.h"          /* mhfu_anchor_regs_t, mhfu_install_trampoline */

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
    uint32_t       applied_addr;  /* last buffer addr we overwrote (idempotency) */
} inject_entry_t;

static inject_entry_t g_tab[MHFU_INJECT_MAX];
static int            g_hook_installed;
static int            g_diag_n;        /* cap diagnostic log lines */

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t *)a; }
static inline int gmem_ok(uint32_t a) { return a >= 0x08000000u && a < 0x0A000000u; }

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
    return 0;
}

/* Prefix-trampoline dispatcher on get_subresource(pkg=a0, type=a1). Runs for
 * EVERY sub fetch of EVERY model; cheap, and only overwrites on a content match. */
extern "C" void mhfu_dispatch_get_subresource(const mhfu_anchor_regs_t *regs)
{
    uint32_t pkg = regs->a0, type = regs->a1;
    if (!gmem_ok(pkg) || type >= MHFU_MAX_SUBS) return;

    /* game package: same PAC layout. off=[pkg+4+type*8], size=[pkg+8+type*8]. */
    uint32_t cnt = rd32(pkg);
    if (cnt == 0 || cnt > 256 || type >= cnt) return;
    uint32_t goff  = rd32(pkg + 4 + type * 8);
    uint32_t gsize = rd32(pkg + 8 + type * 8);
    if (goff == 0 || gsize == 0) return;
    uint32_t gsub = pkg + goff;
    if (!gmem_ok(gsub) || !gmem_ok(gsub + gsize - 1)) return;
    uint32_t gmagic = rd32(gsub);

    /* Search ALL our subs by CONTENT (magic + size + 64-byte head), not by the
     * package's type index — the package's sub ORDER differs from our file's
     * (the big-mon package puts PMO at type 1 / skeleton at type 2, but our
     * file is skeleton=sub0 / PMO=sub1). The 64-byte head is the sub's UNEDITED
     * region (PAC/anim/skeleton header), so it equals the original game sub for
     * OUR species but not a same-size look-alike (e.g. player armor texture). */
    for (int i = 0; i < MHFU_INJECT_MAX; i++) {
        inject_entry_t *e = &g_tab[i];
        if (!e->used || !e->buf) continue;
        for (int j = 0; j < e->nsubs; j++) {
            sub_ent_t *s = &e->subs[j];
            if (s->size == 0 || s->magic != gmagic || s->size != gsize) continue;
            /* UNIQUE id: the fetched sub must byte-match the ORIGINAL species sub
             * (full compare) — a look-alike monster with a same-size skeleton and
             * a generic root bone will differ further in. Fall back to a 64B head
             * compare only if no .orig was provided. */
            int match;
            if (e->obuf)
                match = (memcmp((const void *)gsub, (const void *)(e->obuf + s->off), s->size) == 0);
            else {
                uint32_t pre = s->size < 64 ? s->size : 64;
                match = (memcmp((const void *)gsub, (const void *)(e->buf + s->off), pre) == 0);
            }
            /* diagnostic for skeleton-magic calls: see whether the Tigrex's own
             * skeleton even comes through + matches (capped). */
            if (gmagic == 0xC0000000u && g_diag_n < 24) { g_diag_n++;
                mhfu_log("[inject] skel call sz=%u match=%d (orig=%d)",
                         (unsigned)gsize, match, e->obuf ? 1 : 0); }
            if (match) {
                memcpy((void *)gsub, (const void *)(e->buf + s->off), s->size);
                sceKernelDcacheWritebackRange((void *)gsub, s->size);
                e->hits++;
                mhfu_log("[inject] OVERWROTE sub[%d] magic=0x%08X sz=%u", j, (unsigned)gmagic, (unsigned)s->size);
                return;
            }
        }
    }
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

    /* No get_subresource trampoline: that seam is POST-transform (it returns the
     * restructured sub, wrong format — proven 2026-06-16). The descriptor-table
     * scan in mhfu_inject_tick overwrites the RAW pre-transform buffer instead. */
    (void)g_hook_installed; (void)&mhfu_dispatch_get_subresource;
    mhfu_log("[inject] register file=%u path=%s (raw-buffer descriptor scan)", (unsigned)file_id, e->path);
    return 0;
}

/* Scan the overlay descriptor table for any registered file's RAW buffer and
 * overwrite it with our edited PAC. Content-gated: we only write when buffer@+4
 * byte-matches the ORIGINAL file (e->obuf) — so a same-size look-alike never
 * matches, and once we've written our edited bytes the entry no longer matches
 * (idempotent). Returns # of buffers overwritten this pass. Called every worker
 * tick AND in a tight burst around load (see mhfu_inject_burst). */
static int scan_descriptor_overwrite(void)
{
    uint32_t base = rd32(DESCR_TBL_PTR);
    if (!gmem_ok(base)) return 0;
    int n = 0;
    /* Cheap gate: compare only the PAC header prefix (count + sub (off,size) table)
     * — species-unique — against the original. Full 1.2 MB memcmp per slot per tick
     * would crush emulation. The 256 B prefix overlaps the sub table + start of the
     * skeleton; a same-size look-alike differs there. */
    const uint32_t GATE = 256;
    for (uint32_t s = 0; s < DESCR_MAX; s++) {
        uint32_t ent = base + s * DESCR_STRIDE;
        if (!gmem_ok(ent)) break;
        uint32_t buf = rd32(ent + DESCR_BUF_OFF);
        if (!gmem_ok(buf)) continue;
        for (int i = 0; i < MHFU_INJECT_MAX; i++) {
            inject_entry_t *e = &g_tab[i];
            if (!e->used || !e->buf || !e->obuf || e->file_size < 0x40) continue;
            if (!gmem_ok(buf + e->file_size - 1)) continue;
            /* already handled this buffer (the engine reads each file once per load;
             * survives header-identical edits where a prefix check could not). */
            if (e->applied_addr == buf) continue;
            uint32_t g = e->file_size < GATE ? e->file_size : GATE;
            /* act only on the UNEDITED original (a freshly populated buffer) */
            if (memcmp((const void *)buf, (const void *)e->obuf, g) != 0) continue;
            memcpy((void *)buf, (const void *)e->buf, e->file_size);
            e->applied_addr = buf;
            sceKernelDcacheWritebackRange((void *)buf, e->file_size);
            e->hits++; n++;
            mhfu_log("[inject] OVERWROTE raw buffer file=%u @0x%08X (%uB) slot=%u",
                     (unsigned)e->file_id, (unsigned)buf, (unsigned)e->file_size, (unsigned)s);
        }
    }
    return n;
}

/* Tight burst to beat the one-shot overlay transform during a section load: hammer
 * the descriptor scan for ~BURST_MS so we overwrite the raw buffer the instant it
 * is populated, before the engine reads it to build its transformed copy. */
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
        if (read_file(e) == 0)
            mhfu_log("[inject] loaded edit file=%u (%uB, %d subs) — overwrites raw buffer on load",
                     (unsigned)e->file_id, (unsigned)e->file_size, e->nsubs);
    }
    /* Every tick: catch the raw model buffer (also keeps it pinned if the render
     * reads it live). The burst (mhfu_inject_burst) is the pre-transform path. */
    scan_descriptor_overwrite();
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
