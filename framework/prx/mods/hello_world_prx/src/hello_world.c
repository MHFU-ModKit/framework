/*
 * hello_world_prx — sample mod for the MHFU framework, PRX form.
 *
 * Mirrors mods/hello_world/mod.py exactly in event coverage. When the
 * framework dispatches MHFU_EVENT_QUEST_BEGINNING or _QUEST_ENTERED,
 * this mod's callbacks log a "hello world" line plus a peek at live
 * state.
 *
 * Build via pspdev Docker — see framework/prx/README.md.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <stdio.h>

#include "mhfu_framework.h"

PSP_MODULE_INFO("hello_world", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

/* Load order: PPSSPP starts PRXes in PSP/PLUGINS in alphabetical order
 * by directory name, so naming the framework's folder "mhfu_framework"
 * and this mod's folder "hello_world" (or any later-sorted name)
 * guarantees the framework loads first. */

static void on_quest_beginning(const mhfu_event_ctx_t *ctx)
{
    mhfu_log("[hello_world_prx] * QUEST BEGINNING");
    mhfu_log("  cell_value=%u (quest_timer)", (unsigned)ctx->cell_value);
    mhfu_log("  a0=0x%08lx a1=0x%08lx a2=0x%08lx a3=0x%08lx",
             (unsigned long)ctx->a0, (unsigned long)ctx->a1,
             (unsigned long)ctx->a2, (unsigned long)ctx->a3);
    mhfu_log("  ra=0x%08lx sp=0x%08lx pc=0x%08lx",
             (unsigned long)ctx->ra, (unsigned long)ctx->sp,
             (unsigned long)ctx->pc);
}

static void on_quest_entered(const mhfu_event_ctx_t *ctx)
{
    (void)ctx;
    unsigned timer = (unsigned)mhfu_get_quest_timer();
    mhfu_log("[hello_world_prx] * QUEST ENTERED -- player spawned");
    mhfu_log("  area_index = %u", (unsigned)mhfu_get_area_index());
    mhfu_log("  screen_state = 0x%02x", (unsigned)mhfu_get_screen_state());
    mhfu_log("  quest_timer = %u (%u.%u seconds)",
             timer, timer / 30, (timer % 30) * 10 / 3);
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    mhfu_on_quest_beginning(on_quest_beginning);
    mhfu_on_quest_entered(on_quest_entered);
    mhfu_log("[hello_world_prx] registered");
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_unregister_event(MHFU_EVENT_QUEST_BEGINNING,
                          (mhfu_event_cb_t)(void *)on_quest_beginning);
    mhfu_unregister_event(MHFU_EVENT_QUEST_ENTERED,
                          (mhfu_event_cb_t)(void *)on_quest_entered);
    return 0;
}
