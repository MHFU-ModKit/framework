/*
 * M2 — load a 2nd big-monster AI overlay into a fresh relocated slot.
 * See docs/BIG_MONSTER_OVERLAY_RELOCATION.md and mhfu/bigmon_overlay.h.
 */
#include "mhfu/bigmon_overlay.h"
#include "mhfu/ovl_reloc.h"
#include "mhfu/log.h"

#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <psputils.h>          /* sceKernelDcacheWritebackAll / sceKernelIcacheInvalidateAll */
#include <string.h>

/* volatile-partition lock (UMD-decompression scratch, ~4MB) — not in the public
 * pspsysmem.h prototypes shipped here, declare it. */
extern "C" int sceKernelVolatileMemLock(int type, void **ptr, int *size);
extern "C" int sceKernelVolatileMemUnlock(int type);

/* Static-initializer entry: MWCC emits a list of {ctor_fn_ptr} (or {fn, arg}); the
 * common PS2/PSP form is a plain array of function pointers terminated by the
 * si_end bound. Call each as void(*)(void). */
typedef void (*ctor_fn)(void);

/* Stock-PPSSPP extra RAM: `memory=64` in plugin.ini grows raw RAM to 0x0C000000;
 * [0x0A000000,0x0C000000) is unused by the 32MB game -> a safe, persistent home.
 * A simple bump allocator hands out 64KB-aligned regions here (enough for ~80
 * overlays). Not valid on real PSP hardware (use partition 8 there). */
#define MHFU_XRAM_BASE 0x0A000000u
#define MHFU_XRAM_END  0x0C000000u
static uint32_t g_xram_bump = MHFU_XRAM_BASE;

/* idempotency: a placed overlay stays mapped for the session; re-calling returns
 * the same region instead of re-allocating / re-zeroing (guards multi-trigger). */
static int               g_placed = 0;
static mhfu_ovl_region_t g_region;

int mhfu_bigmon_load_relocated(const char *path, mhfu_ovl_region_t *out)
{
    if (!path || !out) return -1;
    if (g_placed) { *out = g_region; return 0; }
    memset(out, 0, sizeof(*out));

    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) { mhfu_log("[bigmon_ovl] open FAILED %s rc=0x%08X", path, fd); return -2; }

    /* file size */
    SceOff sz = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    uint32_t img_size = (uint32_t)sz;
    if (img_size < 0x40) { sceIoClose(fd); mhfu_log("[bigmon_ovl] too small"); return -3; }

    /* read the 64-byte header first to size the footprint */
    mhfu_mwo3_header_t hdr;
    if (sceIoRead(fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        memcmp(hdr.magic, "MWo3", 4) != 0) {
        sceIoClose(fd); mhfu_log("[bigmon_ovl] bad MWo3 header"); return -4;
    }
    uint32_t slot_base = MHFU_OVL_SLOT_EM;
    uint32_t foot_hi   = hdr.load_address + img_size + hdr.bss_size;   /* old VA end */
    uint32_t foot_size = foot_hi - slot_base;                          /* incl lower+upper bss */
    uint32_t img_off   = hdr.load_address - slot_base;                 /* file image offset in footprint (0x5080) */

    /* allocate footprint + 64KB alignment slack. The user partition is nearly
     * exhausted in normal play (~141KB), so fall back to the ~4MB VOLATILE
     * partition (UMD-decompression scratch, free during steady gameplay). */
    uint32_t need = foot_size + 0x10000;
    mhfu_log("[bigmon_ovl] need=%uKB  user maxfree=%uKB totalfree=%uKB",
             (unsigned)(need / 1024), (unsigned)(sceKernelMaxFreeMemSize() / 1024),
             (unsigned)(sceKernelTotalFreeMemSize() / 1024));
    /* Placement, in priority order:
     * (-1) NATIVE SLOT — if the em slot is FREE (no overlay loaded; host is a
     *      small-monster quest like Giadrome), place at the native slot (delta=0).
     *      The added monster's native entity+0x4C8 then resolves with NO rebind and
     *      NO race. (If the slot is OCCUPIED by another family, fall through to
     *      relocation so we don't clobber it.)
     *  (0) partition 2 — works if the allocator was resized (real-HW partition / fork)
     *  (1) EXTRA RAM   — stock PPSSPP `memory=64` grows raw RAM to 0x0C000000 but does
     *      NOT resize the partition-2 allocator, so place directly in the extra
     *      [0x0A000000,0x0C000000) which the 32MB game never touches (probed). PERSISTENT.
     *  (2) VOLATILE    — last resort, UNSAFE (game reclaims it on UMD streaming). */
    uint32_t blk;
    int placement;                       /* -1=native 0=partmem 1=extra-ram 2=volatile */
    int uid = -1;
    if (*(volatile uint32_t *)hdr.load_address == 0) {        /* em slot is empty */
        blk = slot_base; placement = -1;
        mhfu_log("[bigmon_ovl] em slot FREE -> NATIVE placement @0x%08X (delta=0, no rebind)",
                 (unsigned)hdr.load_address);
    } else {
    uid = sceKernelAllocPartitionMemory(2 /*USER*/, "mhfu_bigmon_ovl",
                                        PSP_SMEM_High, need, 0);
    if (uid < 0)
        uid = sceKernelAllocPartitionMemory(2, "mhfu_bigmon_ovl", PSP_SMEM_Low, need, 0);

    if (uid >= 0) {
        blk = (uint32_t)sceKernelGetBlockHeadAddr(uid); placement = 0;
    } else {
        uint32_t cand = (g_xram_bump + 0xFFFFu) & ~0xFFFFu;   /* 64KB-align bump */
        int mapped = 0;
        if (cand + need <= MHFU_XRAM_END) {
            volatile uint32_t *p = (volatile uint32_t *)cand;
            uint32_t save = *p; *p = 0xA5C3F00Du;
            mapped = (*p == 0xA5C3F00Du); *p = save;
        }
        if (mapped) {
            blk = cand; placement = 1; g_xram_bump = cand + need;
            mhfu_log("[bigmon_ovl] using EXTRA RAM (memory=64 grant) @0x%08X (%uKB left)",
                     (unsigned)blk, (unsigned)((MHFU_XRAM_END - g_xram_bump) / 1024));
        } else {
            void *vptr = 0; int vsz = 0;
            int vrc = sceKernelVolatileMemLock(0, &vptr, &vsz);
            if (vrc < 0 || !vptr || (uint32_t)vsz < need) {
                sceIoClose(fd);
                mhfu_log("[bigmon_ovl] alloc FAILED (no part2, no extra RAM, volatile rc=0x%08X)", vrc);
                return -5;
            }
            blk = (uint32_t)vptr; placement = 2;
            mhfu_log("[bigmon_ovl] using VOLATILE @0x%08X (UNSAFE fallback, reclaimed on streaming)",
                     (unsigned)blk);
        }
    }
    }
    int from_volatile = (placement == 2);

    /* choose region_base so that (region_base - slot_base) is 64KB-aligned, i.e.
     * region_base mod 0x10000 == slot_base mod 0x10000, and region_base >= blk. */
    uint32_t lo16 = slot_base & 0xFFFF;
    uint32_t region_base = ((blk - lo16 + 0xFFFF) & ~0xFFFFu) + lo16;
    if (region_base < blk) region_base += 0x10000;
    int32_t  delta = (int32_t)(region_base - slot_base);              /* multiple of 0x10000 */
    uint32_t image_dst = region_base + img_off;                       /* == hdr.load_address + delta */

    /* zero the whole footprint (lower bss, gaps, upper bss) */
    memset((void *)region_base, 0, foot_size);

    /* read the file image to its placed VA */
    int rd = sceIoRead(fd, (void *)(image_dst + sizeof(hdr)), img_size - sizeof(hdr));
    sceIoClose(fd);
    if (rd != (int)(img_size - sizeof(hdr))) {
        if (placement == 2) sceKernelVolatileMemUnlock(0); else if (placement == 0) sceKernelFreePartitionMemory(uid);
        mhfu_log("[bigmon_ovl] read FAILED rc=0x%08X", rd);
        return -6;
    }
    memcpy((void *)image_dst, &hdr, sizeof(hdr));   /* header (incl pre-reloc fields) */

    /* relocate in place */
    mhfu_ovl_reloc_stats_t st =
        mhfu_ovl_relocate((void *)image_dst, img_size, slot_base, delta);
    if (!st.ok) {
        if (placement == 2) sceKernelVolatileMemUnlock(0); else if (placement == 0) sceKernelFreePartitionMemory(uid);
        mhfu_log("[bigmon_ovl] relocate FAILED (delta=0x%08X)", (unsigned)delta);
        return -7;
    }

    /* code was modified: flush dcache + invalidate icache so the CPU runs it */
    sceKernelDcacheWritebackAll();
    sceKernelIcacheInvalidateAll();

    out->uid         = (placement == 0) ? uid : -1; /* -1 = extra-ram/volatile (no free) */
    out->region_base = region_base;
    out->delta       = delta;
    out->new_load    = st.new_base;
    out->img_size    = img_size;
    out->foot_size   = foot_size;
    out->si_start    = hdr.static_init_start + (uint32_t)delta;
    out->si_end      = hdr.static_init_end   + (uint32_t)delta;

    mhfu_log("[bigmon_ovl] '%s' placed @region=0x%08X load=0x%08X delta=0x%08X "
             "(jump=%u hilo=%u data=%u) si=[0x%08X,0x%08X)",
             hdr.name, (unsigned)region_base, (unsigned)st.new_base, (unsigned)delta,
             (unsigned)st.n_jump, (unsigned)st.n_hilo, (unsigned)st.n_data,
             (unsigned)out->si_start, (unsigned)out->si_end);
    g_region = *out; g_placed = 1;
    return 0;
}

/* Run the relocated overlay's static-initializers (call after placement, before
 * first use). Treated as an array of ctor function pointers in [si_start, si_end). */
extern "C" void mhfu_bigmon_run_static_inits(const mhfu_ovl_region_t *r)
{
    if (!r || r->si_start >= r->si_end) return;
    uint32_t n = 0;
    for (uint32_t p = r->si_start; p < r->si_end; p += 4) {
        uint32_t fn = *(volatile uint32_t *)p;
        if (fn >= r->region_base && fn < r->region_base + r->foot_size) {
            ((ctor_fn)fn)();
            n++;
        }
    }
    mhfu_log("[bigmon_ovl] ran %u static-initializers", (unsigned)n);
}
