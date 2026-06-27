/*
 * capture.cpp — low-fps PSP framebuffer screen capture over USB (psplink hostfs).
 *
 * Goal: record the live game (e.g. the Brute Tigrex) from REAL HARDWARE to the
 * Mac without an external capture card. The PSP's USB port, while a game runs,
 * is free for psplink's `usbhostfs.prx` to claim — that exposes the PC's
 * filesystem as `host0:`. We grab the framebuffer on a dedicated thread,
 * downscale, and stream raw records to `host0:/cap/stream.bin`, which
 * usbhostfs_pc writes onto the Mac. A host-side CLI (tools/psp_capture) tails
 * that file and turns it into PNG/mp4.
 *
 * Design constraints (from the user + the project's real-HW saga):
 *  - MUST NOT block the game if no USB host is connected. All host0: I/O runs on
 *    a private capture thread; the game thread + the lua worker never touch it.
 *    If the host daemon is absent, sceIoOpen("host0:/…") blocks ONLY this thread.
 *  - ZERO resident footprint until enabled (the char-select-freeze saga is
 *    footprint-sensitive). The thread + frame buffer are allocated lazily on the
 *    first mhfu_capture_set(1) and freed by the thread on stop. Nothing is
 *    spawned or allocated at boot/menu.
 *
 * Stream record (little-endian, written back-to-back, ONE open fd per session —
 * per-frame file open/close is a USB round-trip each and would tank the fps):
 *   u32 magic = 'PSPF' (file bytes 50 53 50 46)
 *   u32 frame#         (0-based)
 *   u16 w, u16 h       (downscaled dimensions)
 *   u16 fmt            (PSP pixel format: 0=565 1=5551 2=4444 3=8888)
 *   u16 bpp            (2 or 4)
 *   u32 len            (payload bytes = w*h*bpp)
 *   <payload>          (tightly packed pixels, native PSP format)
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspusb.h>
#include <pspusbbus.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"
#include "mhfu/log.h"

/* usbhostfs.prx exposes host0:, but its module_start ONLY registers the gadget
 * driver (sceUsbbdRegister) — it does NOT bring USB up. In stock psplink the
 * psplink.prx shell does the start+activate (psplink/util.c). We load
 * usbhostfs.prx via GAME.TXT (registers the driver) and do the bring-up
 * ourselves here so host0: actually connects to usbhostfs_pc on the Mac. */
#define CAP_HOSTFS_DRIVER "USBHostFSDriver"   /* HOSTFSDRIVER_NAME */
#define CAP_HOSTFS_PID    0x1C9               /* HOSTFSDRIVER_PID  */

#define CAP_SCREEN_W 480
#define CAP_SCREEN_H 272
#define CAP_MAGIC    0x46505350u   /* 'P''S''P''F' little-endian */

/* --- config (only honoured while NOT active) --- */
static int  g_cfg_scale    = 2;                          /* 1=full,2=half,…   */
static int  g_cfg_interval = 66;                         /* ms between grabs  */
static char g_cfg_path[160] = "host0:/cap/stream.bin";

/* --- live state --- */
static volatile int      g_run    = 0;   /* request: keep looping             */
static volatile int      g_active = 0;   /* thread is alive (start/stop guard) */
static SceUID            g_thread = -1;
static SceUID            g_buf_uid = -1;
static uint8_t          *g_buf    = 0;
static volatile uint32_t g_frames = 0;
static volatile uint32_t g_bytes  = 0;
static volatile int      g_last_err = 0;

typedef struct {
    uint32_t magic;
    uint32_t frame;
    uint16_t w, h;
    uint16_t fmt, bpp;
    uint32_t len;
} cap_rec_t;

static int g_usb_up = 0;

/* host0: target needs the usbhostfs USB link; ms0: (and anything else) is a
 * plain file write — no USB. */
static int path_is_host(const char *p)
{
    return (p[0] == 'h' && p[1] == 'o' && p[2] == 's' && p[3] == 't');
}

/* mkdir the directory portion of a path (one level; parent must exist). */
static void mkdir_parent(const char *path)
{
    char d[160];
    int n = 0, last = -1;
    while (path[n] && n < (int)sizeof(d) - 1) { d[n] = path[n]; if (path[n] == '/') last = n; n++; }
    if (last > 0) { d[last] = 0; sceIoMkdir(d, 0777); }
}

/* Bring the usbhostfs USB link up (driver already registered by usbhostfs.prx).
 * Mirrors psplink/util.c start_usbhost(). Logs return codes; in-quest/village
 * they reach framework.log (ms0 logging is screen-state gated), so a failed
 * host=… code there means usbhostfs.prx didn't load. On the Mac, a successful
 * activate is when usbhostfs_pc prints "Connected to device". */
static void cap_usb_up(void)
{
    if (g_usb_up) return;
    /* Deactivate any prior USB function first (CFW XMB-USB / a stale activate),
     * then start the bus + hostfs drivers and activate — forces the PSP to
     * present the hostfs device cleanly so the Mac re-enumerates it. */
    sceUsbDeactivate(CAP_HOSTFS_PID);
    int rb = sceUsbStart(PSP_USBBUS_DRIVERNAME, 0, 0);
    int rh = sceUsbStart(CAP_HOSTFS_DRIVER, 0, 0);
    int ra = sceUsbActivate(CAP_HOSTFS_PID);
    mhfu_log("[capture] usb up: bus=0x%08X host=0x%08X activate=0x%08X", rb, rh, ra);
    g_usb_up = 1;   /* proceed regardless; the host0: open is the real test */
}

static void cap_usb_down(void)
{
    if (!g_usb_up) return;
    sceUsbDeactivate(CAP_HOSTFS_PID);
    sceUsbStop(CAP_HOSTFS_DRIVER, 0, 0);
    sceUsbStop(PSP_USBBUS_DRIVERNAME, 0, 0);
    g_usb_up = 0;
}

/* Grab one frame, downscale, write a record. Returns 0 ok, <0 on write/grab err. */
static int grab_and_write(SceUID fd)
{
    void *top = 0;
    int   bw = 0, pf = 0;
    if (sceDisplayGetFrameBuf(&top, &bw, &pf, PSP_DISPLAY_SETBUF_IMMEDIATE) < 0)
        return -1;
    if (!top || bw <= 0) return -1;

    int bpp = (pf == PSP_DISPLAY_PIXEL_FORMAT_8888) ? 4 : 2;
    int s   = g_cfg_scale; if (s < 1) s = 1; if (s > 4) s = 4;
    int ow  = CAP_SCREEN_W / s;
    int oh  = CAP_SCREEN_H / s;

    /* Read through the uncached mirror (|0x40000000) so we never see a stale
     * cached copy of VRAM/the draw buffer while the GE is writing it. */
    uintptr_t base = ((uintptr_t)top) | 0x40000000u;
    uint8_t  *dst  = g_buf;

    if (bpp == 2) {
        const volatile uint16_t *src = (const volatile uint16_t *)base;
        uint16_t *d = (uint16_t *)dst;
        for (int y = 0; y < oh; y++) {
            const volatile uint16_t *row = src + (size_t)(y * s) * bw;
            for (int x = 0; x < ow; x++) *d++ = row[x * s];
        }
    } else {
        const volatile uint32_t *src = (const volatile uint32_t *)base;
        uint32_t *d = (uint32_t *)dst;
        for (int y = 0; y < oh; y++) {
            const volatile uint32_t *row = src + (size_t)(y * s) * bw;
            for (int x = 0; x < ow; x++) *d++ = row[x * s];
        }
    }

    cap_rec_t rec;
    rec.magic = CAP_MAGIC;
    rec.frame = g_frames;
    rec.w = (uint16_t)ow; rec.h = (uint16_t)oh;
    rec.fmt = (uint16_t)pf; rec.bpp = (uint16_t)bpp;
    rec.len = (uint32_t)(ow * oh * bpp);

    if (sceIoWrite(fd, &rec, sizeof(rec)) != (int)sizeof(rec)) return -2;
    if (sceIoWrite(fd, dst, (int)rec.len) != (int)rec.len)     return -3;

    g_frames++;
    g_bytes += sizeof(rec) + rec.len;
    return 0;
}

/* The whole capture lifecycle lives on this thread: alloc buffer, open host0:
 * (may block until usbhostfs_pc connects — only THIS thread waits), loop, then
 * close + free on stop. Self-contained so mhfu_capture_set(0) never frees a
 * buffer the thread might still be writing into. */
static int cap_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_active = 1;

    int s  = g_cfg_scale; if (s < 1) s = 1; if (s > 4) s = 4;
    int ow = CAP_SCREEN_W / s, oh = CAP_SCREEN_H / s;
    unsigned need = (unsigned)(ow * oh * 4) + 64;   /* worst case 4 bpp */

    g_buf_uid = sceKernelAllocPartitionMemory(
        2 /* USER */, "mhfu_capbuf", PSP_SMEM_Low, need, 0);
    if (g_buf_uid < 0) {
        mhfu_log("[capture] buf alloc FAILED rc=0x%08X (%uKB)", g_buf_uid, need / 1024);
        g_last_err = g_buf_uid; g_buf_uid = -1;
        goto done;
    }
    g_buf = (uint8_t *)sceKernelGetBlockHeadAddr(g_buf_uid);

    if (path_is_host(g_cfg_path))
        cap_usb_up();                 /* host0: needs the usbhostfs USB link */

    mkdir_parent(g_cfg_path);         /* ensure the target dir exists */
    {
        SceUID fd = sceIoOpen(g_cfg_path,
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd < 0) {
            mhfu_log("[capture] open %s FAILED rc=0x%08X — is the Mac host (usbhostfs_pc) running?",
                     g_cfg_path, fd);
            g_last_err = fd;
            goto done_free;
        }
        mhfu_log("[capture] streaming -> %s  scale=%d (%dx%d) ~%dms/frame",
                 g_cfg_path, s, ow, oh, g_cfg_interval);

        while (g_run) {
            sceDisplayWaitVblankStart();      /* reduce tearing; also paces a bit */
            int rc = grab_and_write(fd);
            if (rc < 0) {
                mhfu_log("[capture] grab/write err rc=%d, stopping (frames=%u)", rc, (unsigned)g_frames);
                g_last_err = rc;
                break;
            }
            int iv = g_cfg_interval - 16;     /* vblank already cost ~one frame  */
            if (iv > 0) sceKernelDelayThread(iv * 1000);
        }
        sceIoClose(fd);
        mhfu_log("[capture] stopped — %u frames, %uKB", (unsigned)g_frames, (unsigned)(g_bytes / 1024));
    }

done_free:
    if (g_buf_uid >= 0) { sceKernelFreePartitionMemory(g_buf_uid); g_buf_uid = -1; g_buf = 0; }
done:
    cap_usb_down();   /* drop the USB link so XMB mass-storage works again */
    g_active = 0;
    g_thread = -1;
    g_run    = 0;
    return 0;
}

/* ---- public C API (internal.h) ---- */

extern "C" void mhfu_capture_configure(int scale, int interval_ms, const char *path)
{
    if (g_active) return;                 /* config is locked while running */
    if (scale >= 1 && scale <= 4) g_cfg_scale = scale;
    if (interval_ms >= 10)        g_cfg_interval = interval_ms;
    if (path && path[0]) {
        unsigned n = 0; while (path[n] && n < sizeof(g_cfg_path) - 1) { g_cfg_path[n] = path[n]; n++; }
        g_cfg_path[n] = 0;
    }
}

extern "C" int mhfu_capture_set(int on)
{
    if (on) {
        if (g_active || g_thread >= 0) return 1;   /* already running */
        g_frames = 0; g_bytes = 0; g_last_err = 0;
        g_run = 1;
        SceUID th = sceKernelCreateThread("mhfu_capture",
                                          (SceKernelThreadEntry)cap_thread,
                                          0x20 /* below the lua worker (0x18) */,
                                          0x8000, PSP_THREAD_ATTR_USER, 0);
        if (th < 0) {
            mhfu_log("[capture] CreateThread FAILED rc=0x%08X", th);
            g_run = 0; g_last_err = th; return 0;
        }
        g_thread = th;
        sceKernelStartThread(th, 0, 0);
        return 1;
    }
    /* stop: just drop the run flag — the thread closes its fd and frees its
     * buffer itself on the next loop check / when its I/O returns. */
    g_run = 0;
    return 0;
}

extern "C" int mhfu_capture_status(int *frames, int *kb, int *err)
{
    if (frames) *frames = (int)g_frames;
    if (kb)     *kb     = (int)(g_bytes / 1024);
    if (err)    *err    = g_last_err;
    return g_active;
}
