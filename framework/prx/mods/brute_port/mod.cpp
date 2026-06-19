/*
 * brute_port — pure-C Brute Tigrex port: model inject + quest swap.
 *
 * No Lua VM on the hot path.  Goal: get the Brute to LOAD + RENDER.
 *
 * What this mod does:
 *   1. QUEST_TARGETS_BUILDING: swap Giadrome → Tigrex in the quest target
 *      list (same as tigrex_inject) so the engine natively loads the Tigrex
 *      model PAC (file_06185) at quest selection.
 *   2. At init: register the Brute PAC relocate so inject.cpp redirects
 *      get_subresource to our Brute model instead of the native Tigrex.
 *
 * Companion: brute_overlay_hook (must also be enabled) patches
 *   0x088dc444 to fix the bad a2 skeleton ptr on construction.
 *
 * No spawn/tick/AI for this build — static render test only.
 *
 * APIs:
 *   mhfu/quest.h  — mhfu_quest_has, mhfu_quest_replace_monster
 *   mhfu/inject.h — mhfu_inject_register_relocate
 *   mhfu/events.h — MHFU_EVENT_QUEST_TARGETS_BUILDING
 */

#include "mhfu/mhfu.h"
#include "mhfu/inject.h"

#define MOD_ID  "brute_port"

/* file_06185 is the PAC the engine loads for the native-Tigrex-quest Tigrex.
 * Confirmed live (entity+0x50 ptr → descriptor table scan). */
#define TIGREX_FILE_ID  6185u

/* v7 = LAYOUT-IDENTICAL PAC: each sub padded to the exact native file_06185
 * byte offset+size (total 1216512 == native), only the CONTENT foreign. This
 * makes the engine's restructure/staging place the skeleton sub where native's
 * would land (the v6 compacted layout put skel at base+0x29744 = inside anim ->
 * bad joint-builder ptr -> construction corruption). Same size as native means
 * we use the PROVEN same-size IN-PLACE overwrite at the get_subresource seam
 * (mhfu_inject_register) instead of the relocate a0-rewrite + xram. The .orig
 * sibling (native file_06185) is the species-unique match key. */
#define BRUTE_PAC  "ms0:/PSP/PLUGINS/mhfu_framework/inject/brute_tigrex_v7.bin"

static volatile int g_swapped = 0;

/* Called by framework's buildTargets prefix before the engine builds targets.
 * Replace Giadrome → Tigrex so the engine does the native model load. */
static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (g_swapped) return;
    if (mhfu_quest_has(ctx->quest, MON_GIADROME) &&
        mhfu_quest_replace_monster(ctx->quest, MON_GIADROME, MON_TIGREX) == MHFU_HOOK_OK) {
        g_swapped = 1;
        mhfu_log("[brute_port] swapped Giadrome->Tigrex in quest targets");
    }
}

static int brute_port_init(void)
{
    mhfu_log("[brute_port] init — pure-C render test (no Lua)");

    /* Same-size IN-PLACE register (v7 is byte-layout identical to native,
     * 1216512 B).  inject.cpp installs the get_subresource trampoline; at load
     * it overwrites the engine's raw count=7 buffer in place with our v7 BEFORE
     * the transform reads the subs -> the engine restructures OUR data on the
     * game thread (racefree, proven path). The .orig sibling identifies the
     * species + computes the diff fingerprint gate. */
    int rc = mhfu_inject_register(TIGREX_FILE_ID, BRUTE_PAC);
    if (rc == 0)
        mhfu_log("[brute_port] inject_register fid=%u path=%s", TIGREX_FILE_ID, BRUTE_PAC);
    else
        mhfu_log("[brute_port] inject_register FAILED rc=%d", rc);

    /* No lua_host worker in this build -> nothing calls mhfu_inject_tick().
     * Prime the edit + .orig + diff fingerprint NOW so the get_subresource seam
     * has the data ready when the engine loads the model. (read_file via _now;
     * the engine buffer isn't present yet so no overwrite happens here.) */
    uint32_t primed = mhfu_inject_now(TIGREX_FILE_ID);
    mhfu_log("[brute_port] primed v7 (hits-so-far=%u)", primed);

    /* Hook buildTargets so we can swap the quest monster before load. */
    mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                    (mhfu_event_cb_t)(void *)on_targets_building, 0);

    return 0;
}

static void brute_port_shutdown(void)
{
    mhfu_unhook_owner(MOD_ID);
    g_swapped = 0;
}

MHFU_MOD(.id       = MOD_ID,
         .version  = "0.1",
         .needs    = "brute_overlay_hook",
         .conflicts = "tigrex_inject",
         .init     = brute_port_init,
         .shutdown = brute_port_shutdown);
