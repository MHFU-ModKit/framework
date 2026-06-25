/*
 * mhfu_boot.prx — split-bootstrap loader (psplink pattern).
 *
 * WHY: the full mhfu_framework.prx (~439 KB loaded image) fails to load on a real
 * PSP with 0x800200D9 (SCE_KERNEL_ERROR_MEMBLOCK_ALLOC_FAILED) — the kernel can't
 * find a 439 KB CONTIGUOUS block in the 24 MB user partition at the early moment
 * ARK loads game plugins (sceMediaSync hook), even though ~2.18 MB total is free
 * later. psplink avoids this exact problem by being a tiny bootstrap that loads
 * trivially early and then sceKernelLoadModule's its big module separately.
 *
 * WHAT THIS DOES: float-free, a few KB, loads fine at the early hook. From its own
 * thread it polls sceKernelMaxFreeMemSize() until partition 2 has a large enough
 * contiguous block, then loads + starts mhfu_framework.prx (which stays user-mode,
 * keeps its Lua VM). It logs the whole free-memory trajectory to
 * ms0:/PSP/mhfu_boot.txt, so one run also tells us whether the wall is
 * fragmentation/timing (deferral fixes it) or absolute capacity (must shrink).
 *
 * This is the REAL-PSP entry. game.txt should point at THIS prx, not the framework.
 * (PPSSPP keeps loading mhfu_framework.prx directly via plugin.ini — it has no load
 * problem there.)
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_boot", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

#define FRAMEWORK_NAME "mhfu_framework.prx"
#define FALLBACK_PATH  "ms0:/PSP/PLUGINS/mhfu_framework/" FRAMEWORK_NAME
/* Need a contiguous block for the framework's ~439 KB image + module-manager
 * overhead; gate the load on >=512 KB free (with margin). */
#define LOAD_THRESHOLD 0x80000u

/* tiny libc-free helpers (keeps this PRX a few KB, pulls no newlib) */
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static char *u2d(char *p, unsigned v)
{
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *p++ = t[--i];
    return p;
}
static char *puts_(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static void wlog(const char *line, int n)
{
    SceUID fd = sceIoOpen("ms0:/PSP/mhfu_boot.txt",
                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, line, n); sceIoClose(fd); }
}
static void log_msg(const char *s) { char b[160]; char *p = puts_(b, s); *p++ = '\n'; wlog(b, (int)(p - b)); }
static void log_kv(const char *s, unsigned v) { char b[160]; char *p = puts_(b, s); p = u2d(p, v); *p++ = '\n'; wlog(b, (int)(p - b)); }
static void log_path(const char *s, const char *path) { char b[320]; char *p = puts_(b, s); p = puts_(p, path); *p++ = '\n'; wlog(b, (int)(p - b)); }

static char g_path[256];

/* Build the framework path from our OWN plugin path (argp), like psplink's loader:
 * keep everything up to the last '/', then append the framework filename. */
static void build_path(SceSize args, void *argp)
{
    g_path[0] = 0;
    if (argp && args > 1) {
        const char *a = (const char *)argp;
        int i = 0, slash = -1;
        while (a[i] && i < 200) { g_path[i] = a[i]; if (a[i] == '/') slash = i; i++; }
        g_path[i] = 0;
        if (slash >= 0) g_path[slash + 1] = 0;   /* dir + trailing slash */
        else g_path[0] = 0;
    }
    if (g_path[0] == 0) {
        const char *f = FALLBACK_PATH; int i = 0;
        while (f[i]) { g_path[i] = f[i]; i++; } g_path[i] = 0;
        return;
    }
    { unsigned dl = slen(g_path); const char *f = FRAMEWORK_NAME; int i = 0;
      while (f[i] && dl + i < 255) { g_path[dl + i] = f[i]; i++; } g_path[dl + i] = 0; }
}

static int boot_thread(SceSize args, void *argp)
{
    build_path(args, argp);
    log_path("[boot] framework=", g_path);
    log_kv("[boot] start maxfree=", (unsigned)sceKernelMaxFreeMemSize());

    int i;
    for (i = 0; i < 60; i++) {
        unsigned mf = (unsigned)sceKernelMaxFreeMemSize();
        if (mf >= LOAD_THRESHOLD) {
            SceUID mod = sceKernelLoadModule(g_path, 0, NULL);
            if (mod >= 0) {
                log_kv("[boot] loadmodule OK at maxfree=", mf);
                int st = sceKernelStartModule(mod, args, argp, NULL, NULL);
                log_kv("[boot] startmodule rc=", (unsigned)st);
                log_msg("[boot] framework loaded — done");
                return 0;
            }
            log_kv("[boot] loadmodule FAILED, maxfree was=", mf);
            log_kv("[boot]   rc=", (unsigned)mod);
        } else {
            log_kv("[boot] waiting maxfree=", mf);
        }
        sceKernelDelayThread(1000 * 1000);  /* 1 s */
    }
    log_msg("[boot] GAVE UP after 60s — partition 2 never had a 512KB block "
            "(=> absolute capacity, not timing; the framework must shrink)");
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("mhfu_boot", boot_thread, 0x18, 0x2000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, args, argp);
    return 0;
}

int module_stop(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
void _exit(int status) { (void)status; }
