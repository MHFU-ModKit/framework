/* Framework logging — see include/mhfu/log.h. */
#include <pspiofilemgr.h>
#include <stdarg.h>
#include <stdio.h>

#include "mhfu/log.h"
#include "mhfu/memory.h"   /* mhfu_ms0_io_safe — never touch ms0 during savedata */

static SceUID g_log_fd = -1;

extern "C" void mhfu_log(const char *fmt, ...)
{
    /* Suppress ALL ms0 access (open included) unless in active gameplay (17/22).
     * Writing the Memory Stick while the SAVEDATA utility loads/saves freezes the PSP
     * (shared non-reentrant MS driver). Logging is diagnostic-only, so dropping
     * boot/menu lines is harmless; the in-quest/village lines we analyze still write. */
    if (!mhfu_ms0_io_safe()) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (g_log_fd < 0) {
        g_log_fd = sceIoOpen(
            "ms0:/PSP/PLUGINS/mhfu_framework/framework.log",
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
    }
    if (g_log_fd >= 0) {
        sceIoWrite(g_log_fd, buf, (size_t)n);
        sceIoWrite(g_log_fd, "\n", 1);
    }
}

extern "C" void mhfu_log_close(void)
{
    if (g_log_fd >= 0) { sceIoClose(g_log_fd); g_log_fd = -1; }
}
