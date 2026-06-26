/* mhfu_p8test2 — does sctrlHENSetMemory(24, N) [partition 2 stays 24 MB, size the
 * SEPARATE p8 partition] give USER-accessible extra RAM (start 0x0A000000), WITHOUT
 * the self-reload that wedged us. The user manually exits + relaunches MHFU to apply
 * the pending config (no sctrlKernelLoadExec teardown -> no wedge).
 *
 * Flow, marker ms0:/PSP/mhfu_p8_marker:
 *   boot 1 (no marker): baseline map -> sctrlHENSetMemory(24, P8_MB) -> write marker
 *                       -> log "EXIT (HOME->Quit) and RELAUNCH MHFU to apply".
 *   boot 2 (marker):    map -> did a user partition appear at 0x0A000000? -> RESULT
 *                       -> sctrlHENSetMemory(24, 0) reset -> remove marker. Done.
 *
 * RECOVERY: if the relaunch boot freezes (MHFU rejects p8), POWER-CYCLE fully — the
 * runtime sctrlHENSetMemory clears on cold boot, then this plugin's boot-2 branch
 * tidies the marker. No Recovery menu needed.
 * Output: ms0:/PSP/mhfu_p8test.txt
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <systemctrl.h>           /* sctrlHENSetMemory */

PSP_MODULE_INFO("mhfu_p8test2", PSP_MODULE_KERNEL, 1, 1);
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

    if(marker_exists()){
        logs("=== boot 2 (POST-RELAUNCH, p8 should be active) ===");
        int found = query_map();
        logs(found ? "RESULT: USER-accessible EXTRA RAM at 0x0A000000 — p8 PATH WORKS"
                   : "RESULT: no USER partition at 0x0A000000 — p8 maps kernel-only or not applied");
        int rc = sctrlHENSetMemory(24, 0);
        logkv("reset sctrlHENSetMemory(24,0) rc=", (unsigned)rc);
        sceIoRemove(MARKER);
        logs("=== done — default restored, safe to quit ===");
    } else {
        logs("=== boot 1 (PRE-SET baseline) ===");
        query_map();
        int rc = sctrlHENSetMemory(24, P8_MB);
        logkv("sctrlHENSetMemory(24,8) rc=", (unsigned)rc);
        marker_write();
        logs(">>> NOW: exit MHFU (HOME -> Quit Game), then RELAUNCH MHFU to apply p8 <<<");
    }
    return 0;
}

int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_p8test2", worker, 0x18, 0x2000, 0, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
