/* mhfu_probe_empty — module_start is essentially a no-op (one benign syscall so the
 * PRX has a .lib.stub and links). Isolation test:
 *  boots to title -> loading a tiny user PRX + trivial module_start is fine; the
 *                    earlier crash is our THREAD/WORKER code.
 *  still crashes  -> merely LOADING + entering the PRX kills MHFU's boot. */
#include <pspkernel.h>
#include <pspthreadman.h>
PSP_MODULE_INFO("mhfu_probe_empty", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);
int module_start(SceSize a, void *p){ (void)a;(void)p; (void)sceKernelGetThreadId(); return 0; }
int module_stop (SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
