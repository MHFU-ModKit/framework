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
#include <pspsysmem.h>

#include "mhfu/log.h"
#include "internal.h"

#define MOD_NAME "mhfu_framework"

/* Linker-supplied bounds of THIS module's loaded image. _ftext = first text
 * byte (= load base), _end = first byte past .bss. At runtime they resolve to
 * the actual load addresses. */
extern "C" char _ftext[];
extern "C" char _end[];

/* ROOT-CAUSE FIX (RE'd 2026-06-19): PPSSPP's plugin loader places the PRX in
 * user-partition memory that the kernel's BlockAllocator still considers FREE,
 * so the game later allocates a thread stack (the big-monster CONSTRUCTION
 * thread, "user_main" uid≈0x119) whose stack lands INSIDE our PRX. As that
 * thread recurses, sw ra,(sp) overwrites our import stubs (sceKernelDelayThread
 * @ base+0x24830) -> the mhfu_deferred thread later jumps to garbage (0x0c000000)
 * and the game crashes on quest load. Observed: writer pc=0x08860540 sp=0x09d89ff0
 * inside [_ftext,_end). FIX: at boot, BEFORE the game spawns that thread, reserve
 * our own image range via sceKernelAllocPartitionMemory(PSP_SMEM_Addr) so the
 * BlockAllocator marks it used and the construction-thread stack is placed
 * elsewhere. */
static SceUID g_self_guard = -1;
static void reserve_self_memory(void)
{
    uint32_t base = ((uint32_t)(uintptr_t)_ftext) & ~0xFFFu;          /* page down */
    uint32_t end  = (((uint32_t)(uintptr_t)_end) + 0xFFFu) & ~0xFFFu; /* page up   */
    uint32_t size = end - base;
    /* Reserve at the exact base of our image. partition 2 = user. */
    g_self_guard = sceKernelAllocPartitionMemory(
        PSP_MEMORY_PARTITION_USER, "mhfu_self_guard", PSP_SMEM_Addr, size, (void *)base);
    if (g_self_guard >= 0) {
        void *got = sceKernelGetBlockHeadAddr(g_self_guard);
        mhfu_log("[framework] self-guard reserved [0x%08X,0x%08X) %uKB blk=0x%X got=0x%08X",
                 base, end, (unsigned)(size / 1024), (unsigned)g_self_guard,
                 (unsigned)(uintptr_t)got);
    } else {
        mhfu_log("[framework] self-guard FAILED rc=0x%08X for [0x%08X,0x%08X) — narrow fallback",
                 (unsigned)g_self_guard, base, end);
        /* Fallback: just bracket the observed collision zone (engine stack top
         * ~0x09D8A000 + our import stubs ~0x09D89CC0). */
        SceUID b2 = sceKernelAllocPartitionMemory(
            PSP_MEMORY_PARTITION_USER, "mhfu_zone_guard", PSP_SMEM_Addr,
            0x4000, (void *)0x09D88000u);
        mhfu_log("[framework] narrow self-guard [0x09D88000,+0x4000) rc/blk=0x%X",
                 (unsigned)b2);
        if (b2 >= 0) g_self_guard = b2;
    }
}

PSP_MODULE_INFO(MOD_NAME, 0, 1, 1);
PSP_MAIN_THREAD_ATTR(0);
/* Bound the newlib heap (Lua runs on its own 64 KB slab, not this heap). With the
 * module_start-direct build (build_prx.mak, -nostartfiles) there is no crt0 heap
 * grab, but keep this as a hard cap for any incidental newlib malloc. */
PSP_HEAP_SIZE_KB(256);

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

/* Real work, on our OWN thread (see module_start). Was main(); converted to the
 * psplink loader pattern so the crt0 main()/heap machinery never runs at load — that
 * machinery is what fails on MHFU's bare 24 MB partition (0x800200D9, before any
 * log). build_prx.mak (-nostartfiles) drops the crt0 entirely. */
static int framework_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_sentinel_set(0x00, 0xCAFE0001);
    mhfu_log("[framework] %s starting", MOD_NAME);

    /* FIRST: fence off our own image so the game can't allocate a thread stack
     * inside it (root cause of the quest-load crash — see reserve_self_memory). */
    reserve_self_memory();

    mhfu_region_detect();
    /* Pick the inject scratch region now (emulator raw window vs real-HW 4 MB
     * volatile) — fault-safe, before any model registers. */
    mhfu_xram_platform_init();
    mhfu_hookmgr_init();
    mhfu_sentinel_set(0x00, 0xCAFE0002);

    /* Spawn-poll runs independent of the trampolines. */
    start_thread("mhfu_spawn_poll", mhfu_monster_spawn_poll_thread);
    /* Applies deferred "quiet-screen" code patches (mhfu_patch_word_when_quiet). */
    start_thread("mhfu_deferred", mhfu_deferred_poll_thread);

    /* Bring up mods: each registers events/hooks and starts its own
     * threads in init(). */
    mhfu_mods_init_all();
    /* Install the quest buildTargets hook only if a mod subscribed. */
    mhfu_quest_init();
    mhfu_sentinel_set(0x00, 0xCAFE0004);

    mhfu_log("[framework] ready");

    /* Install event trampolines (this call blocks in its monitor loop;
     * main runs in its own thread so that's fine). */
    mhfu_install_worker_thread(0, NULL);

    for (;;) sceKernelDelayThread(1000 * 1000);
    return 0;
}

/* PRX entry (psplink loader pattern). The kernel calls this on the loader thread;
 * we spawn our own thread (explicit 256 KB stack for the Lua VM setup) and return
 * immediately, so no crt0 main()/heap reservation runs at load. */
extern "C" int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("mhfu_framework", framework_main, 0x18,
                                      0x40000, THREAD_ATTR_USER, NULL);
    if (th >= 0) {
        sceKernelStartThread(th, args, argp);
        return 0;
    }
    return th;
}

/* newlib references _exit; we never exit, provide a stub. */
extern "C" void _exit(int status) { (void)status; for (;;) sceKernelDelayThread(1000000); }

extern "C" int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_mods_shutdown_all();
    mhfu_uninstall_event_trampolines();
    extern void mhfu_log_close(void);
    mhfu_log_close();
    return 0;
}
