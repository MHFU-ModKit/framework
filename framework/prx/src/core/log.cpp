/* Framework logging — see include/mhfu/log.h. */
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mhfu/log.h"
#include "mhfu/memory.h"   /* mhfu_ms0_io_safe — never touch ms0 during savedata */

static SceUID g_log_fd = -1;

/* 🔴 EARLY-LINE STASH. Every line logged before the game reaches an area used to
 * be DROPPED, and that made the framework's most expensive failure invisible: a
 * mod whose Lua fails to compile at boot logs the error during BOOT, so nothing
 * ever reaches framework.log and the mod is simply absent. That reads exactly
 * like a mod that loaded and did nothing — one cold boot, one quest and one walk
 * to find out, every time.
 *
 * So the boot lines are kept instead of dropped, and flushed the moment ms0 is
 * safe. 4 KB is enough for a plugin init plus a few mod loads; the count of what
 * did not fit is reported rather than hidden. */
#define EARLY_CAP 4096
static char     g_early[EARLY_CAP];
static unsigned g_early_n;
static unsigned g_early_dropped;

static void stash(const char *line, int n)
{
    if (g_early_n + (unsigned)n + 1u >= EARLY_CAP) { g_early_dropped++; return; }
    memcpy(g_early + g_early_n, line, (size_t)n);
    g_early_n += (unsigned)n;
    g_early[g_early_n++] = '\n';
}

static void flush_early(void)
{
    if (!g_early_n) return;
    unsigned n = g_early_n, dropped = g_early_dropped;
    g_early_n = 0;                       /* before the writes: they re-enter */
    g_early_dropped = 0;
    static const char hdr[] = "--- boot log (held until ms0 was safe) ---\n";
    sceIoWrite(g_log_fd, hdr, sizeof(hdr) - 1);
    sceIoWrite(g_log_fd, g_early, n);
    if (dropped) {
        char tail[80];
        int k = snprintf(tail, sizeof(tail), "--- %u more boot line(s) did not fit ---\n",
                         dropped);
        if (k > 0) sceIoWrite(g_log_fd, tail, (size_t)k);
    }
}

extern "C" void mhfu_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;

    /* Suppress ALL ms0 access (open included) unless in active gameplay (17/22).
     * Writing the Memory Stick while the SAVEDATA utility loads/saves freezes the PSP
     * (shared non-reentrant MS driver). */
    if (!mhfu_ms0_io_safe()) { stash(buf, n); return; }

    if (g_log_fd < 0) {
        g_log_fd = sceIoOpen(
            "ms0:/PSP/PLUGINS/mhfu_framework/framework.log",
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    }
    if (g_log_fd >= 0) {
        flush_early();
        sceIoWrite(g_log_fd, buf, (size_t)n);
        sceIoWrite(g_log_fd, "\n", 1);
    }
}

extern "C" void mhfu_log_close(void)
{
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
}
