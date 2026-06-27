/*
 * mhfu_himem_launcher — KERNEL-mode homebrew EBOOT launched from the XMB (PSP/GAME).
 *
 * GOAL: size the SEPARATE p8 partition (sctrlHENSetMemory(24,N) — partition 2 stays
 * 24 MB so MHFU's own RAM is untouched), then boot the MHFU ISO directly, so MHFU only
 * ever experiences a FRESH BOOT with p8 (the launcher tears down, not MHFU — MHFU's
 * teardown-with-p8 is what froze). A query plugin (game.txt) reports MHFU's live map.
 *
 * v1 failed 0x80010087: it used sctrlSESetUmdFile (records the path only) — the ISO
 * filesystem was never MOUNTED, so MHFU booted with no readable disc0. v2 tried KERNEL
 * mode too and failed to LOAD (0x8002013C LIBRARY_NOTFOUND — build.mak auto-links user
 * display/net libs whose imports don't resolve in a kernel module). v3 = back to
 * USER mode (which loads + runs fine; the CFW user stubs syscall into the same kernel
 * code) + the REAL fix: sctrlSESetDiscType + sctrlSEMountUmdFromFile, then
 * sctrlKernelLoadExecVSHDisc.
 *
 * SET_P8 (default 1): build with -DSET_P8=0 (or flip the default) for the ISO-launch
 * isolation variant — confirms MHFU boots via the launcher before testing p8.
 *
 * SAFE FAILURE: no ISO -> exit to XMB, no loadexec. A wedge clears on a power-cycle
 * (runtime memory setting, nothing auto-loads). Log: ms0:/PSP/mhfu_launcher.txt
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <systemctrl.h>
#include <systemctrl_se.h>          /* sctrlSESetDiscType, sctrlSEMountUmdFromFile */
#include <psploadexec_kernel.h>     /* SceKernelLoadExecVSHParam */
#include <string.h>

PSP_MODULE_INFO("MHFUHIMEM", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#ifndef SET_P8
#define SET_P8 1
#endif

#define P8_MB             8
#define DISC_EBOOT        "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
#define ISO_DIR           "ms0:/ISO"
#define LOGF              "ms0:/PSP/mhfu_launcher.txt"
#define ISO_DISC_GAME     0x10      /* ISO_DISC_TYPE_GAME */

static void logp(const char *s){
    SceUID fd=sceIoOpen(LOGF, PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND, 0777);
    if(fd>=0){ sceIoWrite(fd,s,strlen(s)); sceIoWrite(fd,"\n",1); sceIoClose(fd); }
}
static void logkv(const char *s, unsigned v){
    char b[96]; int i=0; while(s[i] && i<70){ b[i]=s[i]; i++; }
    const char *h="0123456789ABCDEF"; b[i++]='0'; b[i++]='x';
    int j; for(j=28;j>=0;j-=4) b[i++]=h[(v>>j)&0xF]; b[i]=0; logp(b);
}

/* first *.iso / *.cso in ms0:/ISO -> out = "ms0:/ISO/<name>" */
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
    logp("=== mhfu_himem_launcher v2 (kernel-mode + mount) ===");

    char iso[256];
    if(!find_iso(iso, sizeof(iso))){
        logp("no ISO in ms0:/ISO — aborting, exit to XMB");
        sceKernelDelayThread(800*1000);
        sctrlKernelExitVSH(NULL);
        return 0;
    }
    logp(iso);

#if SET_P8
    int rc = sctrlHENSetMemory(24, P8_MB);     /* p2 stays 24, size separate p8 */
    logkv("sctrlHENSetMemory(24,8) rc=", (unsigned)rc);
#else
    logp("SET_P8=0 — NOT sizing p8 (ISO-launch isolation test)");
#endif

    sctrlSESetDiscType(ISO_DISC_GAME);                 /* tell the system: GAME ISO */
    int mrc = sctrlSEMountUmdFromFile(iso, 0, 1);      /* MOUNT the ISO filesystem as disc0 */
    logkv("sctrlSEMountUmdFromFile rc=", (unsigned)mrc);
    logp("loadexec disc0 EBOOT ...");
    sceKernelDelayThread(1*1000*1000);                 /* flush log before teardown */

    struct SceKernelLoadExecVSHParam param;
    memset(&param,0,sizeof(param));
    param.size = sizeof(param);
    param.args = strlen(DISC_EBOOT)+1;
    param.argp = (void *)DISC_EBOOT;
    param.key  = "game";
    sctrlKernelLoadExecVSHDisc(DISC_EBOOT, &param);

    /* only reached if the loadexec failed */
    logp("!! loadexec returned — failed; resetting + exit to XMB");
#if SET_P8
    sctrlHENSetMemory(24, 0);
#endif
    sceKernelDelayThread(800*1000);
    sctrlKernelExitVSH(NULL);
    return 0;
}
