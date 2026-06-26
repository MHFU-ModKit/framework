/*
 * mhfu_himem_launcher — homebrew EBOOT launched from the XMB (PSP/GAME).
 *
 * THE IDEA (user's): every freeze we hit was a `loadexec` issued while MHFU was the
 * one tearing down (self-reload, HOME-exit) with the p8 partition pending. This
 * launcher flips that: it sizes the SEPARATE p8 partition (sctrlHENSetMemory(24,N),
 * partition 2 stays 24 MB so MHFU's own RAM is untouched), then loadexecs the MHFU
 * ISO directly — so MHFU only ever experiences a FRESH BOOT with p8; the *launcher*
 * tears down, not MHFU. A query plugin (game.txt) reports the map on MHFU's side.
 *
 * sctrlHENSetMemory fails (-1) from vsh, so we must NOT route through the XMB — the
 * launcher must do the disc loadexec itself.
 *
 * SAFE FAILURE: if no ISO is found we just exit to the XMB (no loadexec). If the
 * loadexec wedges, a plain power-cycle clears the runtime memory setting (no plugin
 * is auto-loaded by this path, so no Recovery needed). Log: ms0:/PSP/mhfu_launcher.txt
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <systemctrl.h>
#include <systemctrl_se.h>          /* sctrlSESetUmdFile */
#include <psploadexec_kernel.h>     /* SceKernelLoadExecVSHParam */
#include <string.h>

PSP_MODULE_INFO("MHFUHIMEM", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define P8_MB       8
#define DISC_EBOOT  "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
#define ISO_DIR     "ms0:/ISO"
#define LOGF        "ms0:/PSP/mhfu_launcher.txt"

static void logp(const char *s){
    SceUID fd=sceIoOpen(LOGF, PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND, 0777);
    if(fd>=0){ sceIoWrite(fd,s,strlen(s)); sceIoWrite(fd,"\n",1); sceIoClose(fd); }
}
static void logkv(const char *s, unsigned v){
    char b[96]; int i=0; while(s[i] && i<70){ b[i]=s[i]; i++; }
    const char *h="0123456789ABCDEF"; b[i++]='0'; b[i++]='x';
    int j; for(j=28;j>=0;j-=4) b[i++]=h[(v>>j)&0xF]; b[i]=0; logp(b);
}

/* find the first *.iso / *.cso in ms0:/ISO -> out = "ms0:/ISO/<name>" */
static int find_iso(char *out, int outsz){
    SceUID d=sceIoDopen(ISO_DIR);
    if(d<0){ logkv("sceIoDopen(ms0:/ISO) failed rc=", (unsigned)d); return 0; }
    SceIoDirent ent; int found=0;
    memset(&ent,0,sizeof(ent));
    while(sceIoDread(d,&ent)>0){
        const char *n=ent.d_name; int l=(int)strlen(n);
        if(l>4){
            const char *e=n+l-4;
            int iso=(e[0]=='.')&&(e[1]=='i'||e[1]=='I')&&(e[2]=='s'||e[2]=='S')&&(e[3]=='o'||e[3]=='O');
            int cso=(e[0]=='.')&&(e[1]=='c'||e[1]=='C')&&(e[2]=='s'||e[2]=='S')&&(e[3]=='o'||e[3]=='O');
            if(iso||cso){
                int p=0; const char *pre=ISO_DIR "/";
                while(pre[p]){ out[p]=pre[p]; p++; }
                int i=0; while(n[i] && p<outsz-1) out[p++]=n[i++];
                out[p]=0; found=1; break;
            }
        }
        memset(&ent,0,sizeof(ent));
    }
    sceIoDclose(d);
    return found;
}

int main(int argc, char *argv[]){
    (void)argc;(void)argv;
    sceKernelDelayThread(500*1000);
    logp("=== mhfu_himem_launcher ===");

    char iso[256];
    if(!find_iso(iso, sizeof(iso))){
        logp("no ISO in ms0:/ISO — aborting, exit to XMB");
        sceKernelDelayThread(800*1000);
        sctrlKernelExitVSH(NULL);
        return 0;
    }
    logp(iso);

    int rc = sctrlHENSetMemory(24, P8_MB);     /* p2 stays 24, size separate p8 */
    logkv("sctrlHENSetMemory(24,8) rc=", (unsigned)rc);

    sctrlSESetUmdFile(iso);                     /* mount this ISO as disc0 on next boot */
    logp("set umd file; loadexec disc0 EBOOT ...");
    sceKernelDelayThread(1*1000*1000);          /* flush log before teardown */

    struct SceKernelLoadExecVSHParam param;
    memset(&param,0,sizeof(param));
    param.size = sizeof(param);
    param.args = strlen(DISC_EBOOT)+1;
    param.argp = (void *)DISC_EBOOT;
    param.key  = "game";
    sctrlKernelLoadExecVSHDisc(DISC_EBOOT, &param);

    /* only reached if the loadexec failed */
    logp("!! loadexec returned — failed; resetting + exit to XMB");
    sctrlHENSetMemory(24, 0);
    sceKernelDelayThread(800*1000);
    sctrlKernelExitVSH(NULL);
    return 0;
}
