/* mhfu_p8test — does sctrlHENSetMemory(24, N) [size the SEPARATE p8 partition,
 * leave partition 2 = 24 MB] yield a USER-accessible partition in the extra RAM
 * (start 0x0A000000-0x0BFFFFFF)?  Tested via a CONTROLLED self-reload so the new
 * config only ever reaches MHFU's reload, never the XMB.
 *
 * Flow, guarded by marker ms0:/PSP/mhfu_p8_marker (=> self-reloads at most ONCE,
 * no boot-loop):
 *   boot 1 (no marker): log baseline map -> sctrlHENSetMemory(24, P8_MB) -> write
 *                       marker -> self-reload THIS game (apitype+file from
 *                       sceKernelInit*).
 *   boot 2 (marker):    log map -> did a user partition appear at 0x0A000000?
 *                       -> RESET sctrlHENSetMemory(24, 0) -> remove marker. Done.
 *                       Nothing persists; default config restored before any XMB return.
 *
 * RECOVERY: if the reload boot freezes (MHFU can't boot with p8), POWER-CYCLE fully —
 * sctrlHENSetMemory is a runtime override, a cold boot clears it back to the CFW
 * default. If still stuck, disable the plugin in the Recovery menu.
 * Output: ms0:/PSP/mhfu_p8test.txt
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>     /* QueryMemoryPartitionInfo, GetModel */
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <pspinit.h>              /* sceKernelInitApitype, sceKernelInitFileName */
#include <systemctrl.h>           /* sctrlHENSetMemory, sctrlKernelLoadExecVSHWithApitype */

PSP_MODULE_INFO("mhfu_p8test", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

#define P8_MB   8
#define MARKER  "ms0:/PSP/mhfu_p8_marker"

static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *u2h(char *p, unsigned v){ const char*h="0123456789ABCDEF"; int i;
    *p++='0';*p++='x'; for(i=28;i>=0;i-=4)*p++=h[(v>>i)&0xF]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static void wline(const char *b, int n){
    SceUID fd=sceIoOpen("ms0:/PSP/mhfu_p8test.txt",
                        PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
    if(fd>=0){ sceIoWrite(fd,b,n); sceIoClose(fd); }
}
static void logs(const char*s){ char b[200]; char*p=puts_(b,s); *p++='\n'; wline(b,(int)(p-b)); }
static void logkv(const char*s,unsigned v){ char b[200]; char*p=puts_(b,s); p=u2h(p,v); *p++='\n'; wline(b,(int)(p-b)); }

static int marker_exists(void){ SceUID fd=sceIoOpen(MARKER,PSP_O_RDONLY,0);
    if(fd>=0){ sceIoClose(fd); return 1; } return 0; }
static void marker_write(void){ SceUID fd=sceIoOpen(MARKER,PSP_O_WRONLY|PSP_O_CREAT|PSP_O_TRUNC,0777);
    if(fd>=0){ sceIoWrite(fd,"1",1); sceIoClose(fd); } }

/* log the partition map; return 1 if any partition starts in the USER extra-RAM
 * window [0x0A000000,0x0C000000) (i.e. user-accessible extra RAM appeared). */
static int query_map(void){
    int found=0, pid;
    logkv("[model] sceKernelGetModel=", (unsigned)sceKernelGetModel());
    for(pid=1; pid<=12; pid++){
        PspSysmemPartitionInfo info; info.size=sizeof(info);
        info.startaddr=0; info.memsize=0; info.attr=0;
        int rc=sceKernelQueryMemoryPartitionInfo(pid,&info);
        char b[200]; char*q=b;
        q=puts_(q,"[part "); q=u2d(q,(unsigned)pid); q=puts_(q,"] ");
        if(rc<0){ q=puts_(q,"rc="); q=u2h(q,(unsigned)rc); }
        else{
            q=puts_(q,"start="); q=u2h(q,info.startaddr);
            q=puts_(q," size="); q=u2d(q,info.memsize/1024); q=puts_(q,"KB");
            q=puts_(q," attr="); q=u2h(q,info.attr);
            if(info.startaddr>=0x0A000000u && info.startaddr<0x0C000000u){
                q=puts_(q," <== USER EXTRA-RAM"); found=1;
            }
        }
        *q++='\n'; wline(b,(int)(q-b));
    }
    return found;
}

static int worker(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(15*1000*1000);     /* reload boot: disc already spun up */

    int post = marker_exists();
    logs(post ? "=== boot 2 (POST-RELOAD, p8 should be active) ==="
              : "=== boot 1 (PRE-SET baseline) ===");
    int found = query_map();

    if(post){
        logs(found ? "RESULT: USER-accessible EXTRA RAM at 0x0A000000 — p8 PATH WORKS"
                   : "RESULT: no user partition at 0x0A000000 — p8 stays kernel-only");
        int rc = sctrlHENSetMemory(24, 0);          /* restore default BEFORE any XMB return */
        logkv("reset sctrlHENSetMemory(24,0) rc=", (unsigned)rc);
        sceIoRemove(MARKER);
        logs("=== test done — default config restored, safe to quit ===");
    } else {
        int rc = sctrlHENSetMemory(24, P8_MB);
        logkv("sctrlHENSetMemory(24,8) rc=", (unsigned)rc);
        marker_write();
        int apitype = sceKernelInitApitype();
        char *fn = sceKernelInitFileName();
        logkv("self-reload apitype=", (unsigned)apitype);
        logs(fn ? fn : "(initfilename NULL)");
        sceKernelDelayThread(2*1000*1000);          /* flush log before loadexec */
        sctrlKernelLoadExecVSHWithApitype(apitype, fn, NULL);
        logs("!! LOADEXEC RETURNED — reload failed (will reset on next boot)");
        sctrlHENSetMemory(24, 0);                    /* undo if reload didn't take */
    }
    return 0;
}

int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_p8test", worker, 0x18, 0x2000, 0, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
