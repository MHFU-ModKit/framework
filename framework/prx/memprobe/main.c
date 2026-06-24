/*
 * mhfu_memprobe.prx — tiny diagnostic plugin (no libc printf -> a few KB).
 *
 * Purpose: the full mhfu_framework.prx fails to load on real hardware with
 * 0x800200D9 (MEMBLOCK_ALLOC_FAILED) — out of room in MHFU's bare 24 MB user
 * partition. Before shrinking further (a risky custom Lua rebuild) we need the
 * ACTUAL free-memory budget. This plugin is a few KB — it loads if any plugin can —
 * and logs the free partition size to ms0:/PSP/mhfu_memprobe.txt every 2 s for ~1
 * minute: at the menu AND once a Giadrome quest is loaded (when the Brute allocates).
 *
 * Install: point game.txt at THIS prx (instead of mhfu_framework.prx), boot MHFU,
 * reach a Giadrome quest, then read ms0:/PSP/mhfu_memprobe.txt.
 *   - Lines present -> plugin loading works; the lowest maxfree is the budget.
 *   - File ABSENT  -> plugin loading itself is misconfigured (game.txt / ARK toggle),
 *     NOT a size problem.
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_memprobe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

/* tiny unsigned -> decimal (avoids pulling newlib printf, which is ~140 KB) */
static char *u2d(char *p, unsigned v)
{
    char tmp[12]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) *p++ = tmp[--i];
    return p;
}
static char *puts_(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    int i;
    for (i = 0; i < 30; i++) {
        char line[128];
        char *p = line;
        p = puts_(p, "[memprobe] t=");
        p = u2d(p, (unsigned)(i * 2));
        p = puts_(p, "s maxfree=");
        p = u2d(p, (unsigned)(sceKernelMaxFreeMemSize() / 1024));
        p = puts_(p, "KB totalfree=");
        p = u2d(p, (unsigned)(sceKernelTotalFreeMemSize() / 1024));
        p = puts_(p, "KB\n");
        SceUID fd = sceIoOpen("ms0:/PSP/mhfu_memprobe.txt",
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) { sceIoWrite(fd, line, (int)(p - line)); sceIoClose(fd); }
        sceKernelDelayThread(2 * 1000 * 1000);
    }
    return 0;
}

int module_stop(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
