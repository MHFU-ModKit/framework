/*
 * mhfu_memprobe.prx — tiny diagnostic plugin.
 *
 * Logs MHFU's free partition RAM to ms0:/PSP/mhfu_memprobe.txt every 2 s.
 *
 * IMPORTANT (2026-06-25): built like psplink's loader — a user-mode plugin that
 * defines module_start DIRECTLY and spawns its own thread, rather than using the
 * pspsdk main() pattern. The main() pattern makes the crt0 auto-create a main
 * thread + full newlib init at load, which FAILS on MHFU's bare 24 MB partition
 * ("could not be started 0x800200D9", before any code runs). psplink's pattern
 * loads fine on the same hardware, so we mirror it.
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_memprobe", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

/* tiny unsigned -> decimal (avoids pulling newlib printf) */
static char *u2d(char *p, unsigned v)
{
    char tmp[12]; int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) *p++ = tmp[--i];
    return p;
}
static char *puts_(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static int probe_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
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

/* psplink pattern: own thread with an explicit stack, return immediately. */
int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("mhfu_memprobe", probe_thread, 0x20,
                                      0x2000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, args, argp);
    return 0;
}

int module_stop(SceSize args, void *argp) { (void)args; (void)argp; return 0; }

/* newlib references this; provide a stub (we never exit). */
void _exit(int status) { (void)status; }
