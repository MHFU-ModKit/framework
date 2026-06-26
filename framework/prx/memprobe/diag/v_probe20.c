/* mhfu_probe20 — thin module_start spawns a worker that waits 20s (well past the
 * slow real-HW UMD-from-ISO boot) BEFORE any ms0 I/O, then writes the free-RAM
 * numbers. If the earlier 5s crash was the worker's first I/O racing the still-
 * loading game, 20s survives and we finally get maxfree. */
#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
PSP_MODULE_INFO("mhfu_probe20", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static int worker(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(20*1000*1000);          /* wait out the whole boot */
    int i; for(i=0;i<30;i++){
        char line[160]; char *q=line;
        q=puts_(q,"[probe20] t="); q=u2d(q,(unsigned)(20+i*3));
        q=puts_(q,"s maxfree="); q=u2d(q,(unsigned)(sceKernelMaxFreeMemSize()/1024));
        q=puts_(q,"KB totalfree="); q=u2d(q,(unsigned)(sceKernelTotalFreeMemSize()/1024));
        q=puts_(q,"KB\n");
        SceUID fd=sceIoOpen("ms0:/PSP/mhfu_probe20.txt",
                            PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
        if(fd>=0){ sceIoWrite(fd,line,(int)(q-line)); sceIoClose(fd); }
        sceKernelDelayThread(3*1000*1000);
    }
    return 0;
}
int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_probe20", worker, 0x30, 0x4000,
                                    PSP_THREAD_ATTR_USER, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
