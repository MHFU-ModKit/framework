/* mhfu_partprobe v2 — KERNEL-mode memory-partition QUERY map (read-only, SAFE).
 *
 * v1 ALLOCATED from each partition + wrote a marker; allocating from a kernel
 * partition (5) wedged the kernel -> boot freeze. v2 only READS each partition's
 * descriptor via sceKernelQueryMemoryPartitionInfo (start addr / size / attr) and
 * sceKernelGetModel. No allocation, no writes, nothing held -> safe even for kernel
 * partitions, zero XMB risk.
 *
 * GOAL (unchanged): find whether any partition covers the USER-READABLE extra RAM
 * (start in 0x0A000000-0x0BFFFFFF) so a kernel module could hand the user-mode game
 * engine a readable buffer there WITHOUT resizing partition 2 (the XMB's partition).
 * Segment of the start address tells user-vs-kernel reachability:
 *   0x08800000-0x09FFFFFF user(main)   0x0A000000-0x0BFFFFFF user(EXTRA-RAM!)
 *   0x88800000-0x89FFFFFF kern(main)   0x8A000000-0x8BFFFFFF kern(extra)
 * Output: ms0:/PSP/mhfu_partprobe.txt
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>   /* sceKernelQueryMemoryPartitionInfo, sceKernelGetModel */
#include <pspiofilemgr.h>
#include <pspthreadman.h>

PSP_MODULE_INFO("mhfu_partprobe", PSP_MODULE_KERNEL, 1, 1);
PSP_MAIN_THREAD_ATTR(0);

static char *u2d(char *p, unsigned v){ char t[12]; int i=0; if(!v)t[i++]='0';
    while(v){t[i++]=(char)('0'+v%10);v/=10;} while(i)*p++=t[--i]; return p; }
static char *u2h(char *p, unsigned v){ const char*h="0123456789ABCDEF"; int i;
    *p++='0';*p++='x'; for(i=28;i>=0;i-=4)*p++=h[(v>>i)&0xF]; return p; }
static char *puts_(char *p,const char*s){ while(*s)*p++=*s++; return p; }

static void wline(const char *b, int n){
    SceUID fd=sceIoOpen("ms0:/PSP/mhfu_partprobe.txt",
                        PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND,0777);
    if(fd>=0){ sceIoWrite(fd,b,n); sceIoClose(fd); }
}

static const char *seg_of(unsigned a){
    if(a>=0x08800000u && a<0x0A000000u) return "user(main)";
    if(a>=0x0A000000u && a<0x0C000000u) return "user(EXTRA-RAM!)";
    if(a>=0x48800000u && a<0x4A000000u) return "user(main,uncached)";
    if(a>=0x4A000000u && a<0x4C000000u) return "user(EXTRA,uncached)";
    if(a>=0x88800000u && a<0x8A000000u) return "kern(main)";
    if(a>=0x8A000000u && a<0x8C000000u) return "kern(extra)";
    if(a==0u)                           return "-";
    return "?";
}

static int worker(SceSize a, void *p){ (void)a;(void)p;
    sceKernelDelayThread(20*1000*1000);   /* wait past boot (proven safe in v_kprobe) */

    char b[200]; char *q=b;
    q=puts_(q,"=== mhfu_partprobe v2: partition QUERY map (read-only) ===\n");
    wline(b,(int)(q-b));

    q=b;
    q=puts_(q,"[model] sceKernelGetModel="); q=u2d(q,(unsigned)sceKernelGetModel());
    q=puts_(q," (<=0 original/Phat, 1 slim+)\n");
    q=puts_(q,"[ref] userMax="); q=u2d(q,(unsigned)(sceKernelMaxFreeMemSize()/1024));
    q=puts_(q,"KB\n"); wline(b,(int)(q-b));

    int pid;
    for (pid = 1; pid <= 12; pid++) {
        PspSysmemPartitionInfo info;
        info.size = sizeof(info);
        info.startaddr = 0; info.memsize = 0; info.attr = 0;
        int rc = sceKernelQueryMemoryPartitionInfo(pid, &info);
        q=b;
        q=puts_(q,"[part ");  q=u2d(q,(unsigned)pid); q=puts_(q,"] ");
        if (rc < 0) {
            q=puts_(q,"query rc="); q=u2h(q,(unsigned)rc); *q++='\n';
        } else {
            q=puts_(q,"start="); q=u2h(q,info.startaddr);
            q=puts_(q," size="); q=u2d(q,info.memsize/1024); q=puts_(q,"KB");
            q=puts_(q," attr="); q=u2h(q,info.attr);
            q=puts_(q," end=");  q=u2h(q,info.startaddr+info.memsize);
            q=puts_(q," SEG="); q=puts_(q,seg_of(info.startaddr));
            *q++='\n';
        }
        wline(b,(int)(q-b));
    }

    q=b; q=puts_(q,"=== partprobe v2 done ===\n"); wline(b,(int)(q-b));
    return 0;
}

int module_start(SceSize a, void *p){
    SceUID th=sceKernelCreateThread("mhfu_partprobe", worker, 0x30, 0x2000, 0, NULL);
    if(th>=0) sceKernelStartThread(th, a, p);
    return 0;
}
int module_stop(SceSize a, void *p){ (void)a;(void)p; return 0; }
void _exit(int s){ (void)s; }
