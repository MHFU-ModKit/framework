/*
 * mhfu_memprobe.prx — DEFERRED-WORKER variant (the "survive the boot phase" design).
 *
 * The crash mechanism (research-confirmed): PRO CFW calls our module_start
 * SYNCHRONOUSLY on the KERNEL loader thread (SceKernelLoadExecThread) during the
 * boot sequence, before the game's EBOOT runs. That kernel thread has a tiny stack
 * (~4-16 KB) and the FPU disabled. So doing ANYTHING heavy in module_start — file
 * I/O (descends iofilemgr -> sceLowIO and eats the stack), or racing the loadexec's
 * own ms0 read — corrupts that thread -> wild jump -> Coprocessor-Unusable crash.
 *
 * THE DESIGN: module_start does the bare minimum — create + start ONE worker thread
 * (which gets its OWN user-mode stack) and return IMMEDIATELY. No I/O, no float, no
 * deep call chains on the loader stack. The worker then SLEEPS past the boot/loadexec
 * phase (so we're safely back in userspace, game up, no I/O race) and only THEN
 * touches ms0. This is psplink's model and the canonical PRO-CFW-safe plugin shape.
 *
 * TEST OUTCOME:
 *  - File ms0:/PSP/mhfu_memprobe.txt appears with "worker ... t=5s..." lines and the
 *    game keeps running  ->  WE SURVIVED THE BOOT PHASE. The plugin route is viable;
 *    the framework just needs the same thin-module_start + deferred-worker shape.
 *  - Still 0x800200D9 / crash  ->  even a thin module_start + worker isn't enough;
 *    next step is an even-lighter deferral (e.g. only create the thread, defer the
 *    start) or gating on a game-ready signal.
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_memprobe", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

static char *u2d(char *p, unsigned v)
{
    char t[12]; int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *p++ = t[--i];
    return p;
}
static char *puts_(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

/* Runs on its OWN user-mode thread/stack — NOT the kernel loader stack. */
static int worker_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Wait out the boot/loadexec phase before ANY ms0 I/O. The fault happened in the
     * first ~2 s (during the disc load); 5 s puts us comfortably past it. */
    sceKernelDelayThread(5 * 1000 * 1000);
    int i;
    for (i = 0; i < 20; i++) {
        char line[128];
        char *p = line;
        p = puts_(p, "[memprobe] worker (deferred) t=");
        p = u2d(p, (unsigned)(5 + i * 2));
        p = puts_(p, "s maxfree=");
        p = u2d(p, (unsigned)(sceKernelMaxFreeMemSize() / 1024));
        p = puts_(p, "KB\n");
        SceUID fd = sceIoOpen("ms0:/PSP/mhfu_memprobe.txt",
                              PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
        if (fd >= 0) { sceIoWrite(fd, line, (int)(p - line)); sceIoClose(fd); }
        sceKernelDelayThread(2 * 1000 * 1000);
    }
    return 0;
}

/* THIN: runs on the kernel loader thread's tiny stack. Create + start the worker
 * (its own user stack, USER attr, 16 KB) and return immediately. Nothing else. */
int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("mhfu_worker", worker_thread,
                                      0x30, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (th >= 0) sceKernelStartThread(th, args, argp);
    return 0;
}

int module_stop(SceSize args, void *argp) { (void)args; (void)argp; return 0; }
void _exit(int status) { (void)status; }
