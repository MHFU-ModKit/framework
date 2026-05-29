/*
 * mhfu_framework.prx — module entry + boot sequence.
 *
 * crt0_prx supplies module_start, which spawns a thread for main() and
 * returns — critical for PPSSPP plugin compatibility (a synchronous idle
 * loop in module_start would block sceKernelStartModule and black-screen
 * the boot). We provide main() (user-mode, attr 0) + module_stop.
 *
 * Boot order: detect region -> init hook manager -> start the
 * monster-spawn poll thread -> init the mod table (each mod registers its
 * events/hooks/threads) -> install the event trampolines (worker thread
 * waits for the EBOOT to be resident, then patches) -> idle.
 */
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>

#include "mhfu/log.h"
#include "internal.h"

#define MOD_NAME "mhfu_framework"

PSP_MODULE_INFO(MOD_NAME, 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

/* Sentinel cells in a quiet RAM region for host-debugger verification
 * even when log I/O NIDs are unsupported. Layout (u32s from base):
 *   +0x00 STAGE  +0x04 install rc  +0x08 cave addr  +0x0C anchor readback
 *   +0x10 worker high-water */
extern "C" void mhfu_sentinel_set(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(MHFU_SENTINEL_BASE + offset) = value;
}

static void start_thread(const char *name, int (*entry)(SceSize, void *))
{
    SceUID th = sceKernelCreateThread(name, (SceKernelThreadEntry)entry,
                                      0x18, 0x1000, 0, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, 0, NULL);
        mhfu_log("[framework] thread '%s' started (uid=0x%08lx)",
                 name, (unsigned long)th);
    } else {
        mhfu_log("[framework] thread '%s' create failed: %d", name, th);
    }
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    mhfu_sentinel_set(0x00, 0xCAFE0001);
    mhfu_log("[framework] %s starting", MOD_NAME);

    mhfu_region_detect();
    mhfu_hookmgr_init();
    mhfu_sentinel_set(0x00, 0xCAFE0002);

    /* Spawn-poll runs independent of the trampolines. */
    start_thread("mhfu_spawn_poll", mhfu_monster_spawn_poll_thread);

    /* Bring up mods: each registers events/hooks and starts its own
     * threads in init(). */
    mhfu_mods_init_all();
    mhfu_sentinel_set(0x00, 0xCAFE0004);

    mhfu_log("[framework] ready");

    /* Install event trampolines (this call blocks in its monitor loop;
     * main runs in its own thread so that's fine). */
    mhfu_install_worker_thread(0, NULL);

    for (;;) sceKernelDelayThread(1000 * 1000);
    return 0;
}

extern "C" int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_mods_shutdown_all();
    mhfu_uninstall_event_trampolines();
    extern void mhfu_log_close(void);
    mhfu_log_close();
    return 0;
}
