/*
 * mhfu_memgrow.prx — VSH plugin (seplugins/vsh.txt) that grows MHFU's user
 * partition just enough to fit the relocated Brute model, using the extra RAM on
 * PSP-2000/3000/Go (Slim+). PSP-1000 (Phat) has no extra RAM and is skipped.
 *
 * WHY a VSH plugin: sctrlHENSetMemory() only takes effect "on the next loadexec".
 * Called here in the XMB, the next loadexec is the user launching MHFU — so MHFU
 * boots with the bigger partition on its FIRST launch, with no self-reload and no
 * boot-loop risk. (A game plugin can't grow its own already-running boot.)
 *
 * WHY a MODEST grow (28 MB, not the full 52): "Use Extra Memory: Forced" = the full
 * 52 MB grow was confirmed to CRASH MHFU EU at the boot logo. A small grow keeps the
 * partition top just 4 MB into the extra RAM (ends ~0x0A400000) — enough for the
 * framework (~0.6 MB) + the Brute (~1.6 MB) with headroom, far from the 52 MB layout
 * that crashed. Tunable via GROW_P2_MB.
 *
 * Grows partition 2 (the USER partition) itself, so the room is user-accessible by
 * definition — the game engine reads our injected model with NO kernel addresses and
 * NO memory-protection register pokes. The framework allocs the Brute from the top of
 * this partition (PSP_SMEM_High), landing in the extra RAM, clear of MHFU's heap.
 */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <systemctrl.h>

PSP_MODULE_INFO("mhfu_memgrow", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

/* User-partition size in MB for MHFU's next launch (default is 24). p2+p8 <= 52.
 * 28 = +4 MB of extra RAM: fits framework + Brute with headroom, stays modest. */
#define GROW_P2_MB 28

/* libc-free helpers (keeps this PRX tiny, pulls no newlib) */
static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static int g_model, g_rc;

/* Deferred log thread: keep ms0 I/O off the plugin-load path (the memgrow call itself
 * has already happened in module_start, synchronously, before any game can launch). */
static int log_thread(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(2*1000*1000);
    char b[160]; char *q=b;
    q=puts_(q,"[memgrow] model=");        q=u2d(q,(unsigned)g_model);
    q=puts_(q," (0=Phat) setmemory(");     q=u2d(q,(unsigned)GROW_P2_MB);
    q=puts_(q,",0) rc=");                   q=u2d(q,(unsigned)g_rc);
    q=puts_(q,g_rc==0?" OK -> MHFU next launch gets the bigger partition\n"
                     :" (non-zero: not applied; Phat, already set, or unsupported)\n");
    SceUID fd=sceIoOpen("ms0:/PSP/mhfu_memgrow.txt",
                        PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
    if(fd>=0){ sceIoWrite(fd,b,(int)(q-b)); sceIoClose(fd); }
    return 0;
}

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Slim+ only — Phat (model 0) has no extra RAM; a grow there would fail/crash. */
    g_model = sceKernelGetModel();
    g_rc = -1;
    if (g_model > 0)
        g_rc = sctrlHENSetMemory(GROW_P2_MB, 0);   /* applies on next loadexec */

    SceUID th = sceKernelCreateThread("mhfu_memgrow_log", log_thread, 0x18, 0x1000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp){ (void)args;(void)argp; return 0; }
void _exit(int status){ (void)status; }
