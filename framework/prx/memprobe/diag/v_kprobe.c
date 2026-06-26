/* mhfu_kprobe — KERNEL-mode probe. The user partition is full (a 2.8KB USER plugin
 * OOMs with 0x800200D9), but a kernel module (usbhostfs, 16KB) loaded fine -> the
 * kernel partition has room. This loads THERE (kernel), waits past boot, and logs
 * the USER-partition free memory. Two answers in one boot:
 *   (a) does OUR kernel module load at all on real HW (vs OOM)?
 *   (b) the exact free budget in the user partition (the framework-fit number). */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_kprobe", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static int worker(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(20*1000*1000);
    int i; for(i=0;i<30;i++){
        char line[160]; char *q=line;
        q=puts_(q,"[kprobe] t=");   q=u2d(q,(unsigned)(20+i*3));
        q=puts_(q,"s userMax=");    q=u2d(q,(unsigned)(sceKernelMaxFreeMemSize()/1024));
        q=puts_(q,"KB userTot=");   q=u2d(q,(unsigned)(sceKernelTotalFreeMemSize()/1024));
        q=puts_(q,"KB\n");
        SceUID fd=sceIoOpen("ms0:/PSP/mhfu_kprobe.txt",
                            PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
        if(fd>=0){ sceIoWrite(fd,line,(int)(q-line)); sceIoClose(fd); }
        sceKernelDelayThread(3*1000*1000);
    }
    return 0;
}
/* KERNEL thread (attr 0, no ATTR_USER): stack from the kernel partition. */
int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_kprobe", worker, 0x30, 0x2000, 0, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
