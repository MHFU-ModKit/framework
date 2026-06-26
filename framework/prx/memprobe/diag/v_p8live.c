/* mhfu_p8live — does sctrlHENSetMemory(24, N) take effect LIVE (same boot, no
 * loadexec)?  Single boot, NO reload, and it RESETS itself at the end so the exit is
 * clean. Tests the hypothesis: maybe the extra RAM is available without a restart.
 *
 *   query map (BEFORE) -> sctrlHENSetMemory(24, P8_MB) -> query map (AFTER, live)
 *   -> did a USER partition appear at 0x0A000000? -> sctrlHENSetMemory(24, 0) reset.
 *
 * The result is logged DURING the boot, before any exit, so it's captured even if
 * the exit misbehaves. After the reset the pending config is default again, so a
 * normal HOME->Quit should work; if it still freezes, just power-cycle.
 * Output: ms0:/PSP/mhfu_p8live.txt
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <systemctrl.h>

PSP_MODULE_INFO("mhfu_p8live", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

#define P8_MB 8

static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *u2h(char *p, unsigned v){ const char*h="0123456789ABCDEF"; int i;
    *p++='0';*p++='x'; for(i=28;i>=0;i-=4)*p++=h[(v>>i)&0xF]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static void wline(const char *b, int n){
    SceUID fd=sceIoOpen("ms0:/PSP/mhfu_p8live.txt",
                        PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
    if(fd>=0){ sceIoWrite(fd,b,n); sceIoClose(fd); }
}
static void logs(const char*s){ char b[200]; char*p=puts_(b,s); *p++='\n'; wline(b,(int)(p-b)); }
static void logkv(const char*s,unsigned v){ char b[200]; char*p=puts_(b,s); p=u2h(p,v); *p++='\n'; wline(b,(int)(p-b)); }

/* log the map; return 1 if any partition starts in [0x0A000000,0x0C000000) with a
 * user attr (low 2 bits set) = user-accessible extra RAM. */
static int query_map(void){
    int found=0, pid;
    for(pid=1; pid<=12; pid++){
        PspSysmemPartitionInfo info; info.size=sizeof(info);
        info.startaddr=0; info.memsize=0; info.attr=0;
        int rc=sceKernelQueryMemoryPartitionInfo(pid,&info);
        char b[200]; char*q=b;
        q=puts_(q,"  [part "); q=u2d(q,(unsigned)pid); q=puts_(q,"] ");
        if(rc<0){ q=puts_(q,"rc="); q=u2h(q,(unsigned)rc); }
        else{
            q=puts_(q,"start="); q=u2h(q,info.startaddr);
            q=puts_(q," size="); q=u2d(q,info.memsize/1024); q=puts_(q,"KB");
            q=puts_(q," attr="); q=u2h(q,info.attr);
            if(info.startaddr>=0x0A000000u && info.startaddr<0x0C000000u){
                q=puts_(q,(info.attr&3)?" <== USER EXTRA-RAM":" <== extra(kernel)");
                if(info.attr&3) found=1;
            }
        }
        *q++='\n'; wline(b,(int)(q-b));
    }
    return found;
}

static int worker(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(15*1000*1000);

    logs("=== mhfu_p8live: set p8 + LIVE re-query (no reload, self-reset) ===");
    logkv("[model]=", (unsigned)sceKernelGetModel());

    logs("--- map BEFORE sctrlHENSetMemory ---");
    query_map();

    int rc = sctrlHENSetMemory(24, P8_MB);
    logkv("sctrlHENSetMemory(24,8) rc=", (unsigned)rc);

    logs("--- map AFTER (live, same boot, NO reload) ---");
    int found = query_map();

    logs(found ? "LIVE RESULT: USER extra RAM at 0x0A000000 appeared WITHOUT a reload!"
               : "LIVE RESULT: no live change — sctrlHENSetMemory is deferred-only");

    int rc2 = sctrlHENSetMemory(24, 0);          /* reset so the exit is clean */
    logkv("reset sctrlHENSetMemory(24,0) rc=", (unsigned)rc2);
    logs("=== done — default restored; HOME->Quit should work (else power-cycle) ===");
    return 0;
}

int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_p8live", worker, 0x18, 0x2000, 0, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
