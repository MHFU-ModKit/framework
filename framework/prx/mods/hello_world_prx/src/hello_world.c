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

/* The framework PRX must be loaded first; we depend on its exports. */
PSP_MODULE_STOP_AFTER("mhfu_framework");

static void on_quest_beginning(const mhfu_event_ctx_t *ctx)
{
    mhfu_log("[hello_world_prx] ★ QUEST BEGINNING");
    mhfu_log("  cell_value=%u (quest_timer)", (unsigned)ctx->cell_value);
    mhfu_log("  a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x",
             ctx->a0, ctx->a1, ctx->a2, ctx->a3);
    mhfu_log("  ra=0x%08x sp=0x%08x pc=0x%08x",
             ctx->ra, ctx->sp, ctx->pc);
}

static void on_quest_entered(const mhfu_event_ctx_t *ctx)
{
    mhfu_log("[hello_world_prx] ★ QUEST ENTERED — player spawned");
    mhfu_log("  area_index = %u", (unsigned)mhfu_get_area_index());
    mhfu_log("  screen_state = 0x%02x", (unsigned)mhfu_get_screen_state());
    mhfu_log("  quest_timer = %u (%u.%u seconds)",
             (unsigned)mhfu_get_quest_timer(),
             (unsigned)mhfu_get_quest_timer() / 30,
             ((unsigned)mhfu_get_quest_timer() % 30) * 10 / 3);
}

int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_on_quest_beginning(on_quest_beginning);
    mhfu_on_quest_entered(on_quest_entered);
    mhfu_log("[hello_world_prx] registered");
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    mhfu_unregister_event(MHFU_EVENT_QUEST_BEGINNING, on_quest_beginning);
    mhfu_unregister_event(MHFU_EVENT_QUEST_ENTERED, on_quest_entered);
    return 0;
}
