/*
 * tigrex_inject — load a Tigrex into the Giadrome quest by retagging the
 * quest's monster before the engine builds its targets, so the engine
 * does the native load + spawn. Then tame + resize the spawned Tigrex.
 *
 * Mechanism + RE: docs/QUEST_LOADING_LIFECYCLE.md (Section 31). All the
 * raw quest-record / aggro / size pokes now live in the framework's
 * typed API (mhfu/quest.h, monster.h, entity.h) — this mod just states
 * intent.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"

#define MOD_ID      "tigrex_inject"
#define TAME_STATE  5      /* passive, non-aggressive AI state */
#define TIGREX_SIZE 0.5f

static volatile int g_replaced = 0;

/* Edit the quest list before the engine builds targets (framework owns
 * the buildTargets hook + JIT-safe install timing). */
static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (mhfu_quest_has(ctx->quest, MON_GIADROME) &&
        mhfu_quest_replace_monster(ctx->quest, MON_GIADROME, MON_TIGREX) == MHFU_HOOK_OK)
        g_replaced = 1;
}

/* Keep the spawned Tigrex calm + small. The engine rewrites the engage
 * flag each frame, so this must refresh faster than ~2 Hz. */
static int tame_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(300 * 1000);
        if (!g_replaced || mhfu_get_screen_state() != 17) continue;
        mhfu_species_set_detection(MON_TIGREX, 0.0f);   /* block NEW aggro (sticks) */
        uint32_t t[4];
        int n = mhfu_entities_of_type(MON_TIGREX, t, 4);
        for (int i = 0; i < n; i++) {
            mhfu_entity_calm(t[i]);                      /* clear current engage */
            mhfu_entity_set_ai_state(t[i], TAME_STATE);
            mhfu_entity_set_size(t[i], TIGREX_SIZE);
        }
    }
    return 0;
}

static int tigrex_init(void)
{
    mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                    (mhfu_event_cb_t)(void *)on_targets_building, 0);
    SceUID th = sceKernelCreateThread(MOD_ID, (SceKernelThreadEntry)tame_thread,
                                      0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

MHFU_MOD(.id = MOD_ID, .version = "2.0",
         .needs = 0, .conflicts = "popo_heading_hook",
         .init = tigrex_init, .shutdown = 0);
