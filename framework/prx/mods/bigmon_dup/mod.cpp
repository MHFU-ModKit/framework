/*
 * bigmon_dup — spawn a SECOND instance of the quest's existing big monster,
 * same family, natively (Section 48).
 *
 * Mechanism (this-session RE): buildTargets (0x0886D4D8) is UNCAPPED — it walks
 * the parsed-.mib list-A and builds a target for every node into group 0. So
 * appending a 2nd same-species node => group 0 count 2 => the engine provisions
 * + spawns both from the one resident overlay. No 2nd target group, no forge, no
 * overlay relocation. The edit must land before buildTargets runs, so it's done
 * in the QUEST_TARGETS_BUILDING (prefix) callback the framework fires.
 *
 * This is the same-family half of "N big monsters". Different-family additionally
 * needs the relocated 2nd overlay (mhfu_bigmon_load_relocated) — see
 * docs/BIG_MONSTER_OVERLAY_RELOCATION.md.
 */
#include "mhfu/mhfu.h"

#define MOD_ID   "bigmon_dup"
#define DUP_ID   MON_TIGREX     /* duplicate the quest's Tigrex */
/* absolute spawn coords for the 2nd instance (source Tigrex ~11209,9493 -> offset
 * so they don't stack). add_monster writes rec+0x20=X, rec+0x28=Z when nonzero. */
#define DUP_X    14209.0f
#define DUP_Z    9493.0f

static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (!mhfu_quest_has(ctx->quest, DUP_ID)) return;
    /* 2-GROUP split path (native dual-big representation): add_monster appends a
     * list-A node AND its postfix moves the 2nd into target[1] + raises Quest+0x67C
     * to 2 -> engine makes a 2nd manager -> 2nd entity. Same family => resources
     * resident => the (disarmed) forge isn't needed. */
    if (mhfu_quest_add_monster(ctx->quest, DUP_ID, DUP_X, DUP_Z) == MHFU_HOOK_OK)
        mhfu_log("[%s] added 2nd 0x%02x @(%.0f,%.0f) via 2-group split",
                 MOD_ID, (unsigned)DUP_ID, DUP_X, DUP_Z);
}

static int dup_init(void)
{
    mhfu_hook_event(MHFU_EVENT_QUEST_TARGETS_BUILDING,
                    (mhfu_event_cb_t)(void *)on_targets_building, 0);
    return 0;
}

MHFU_MOD(.id = MOD_ID, .version = "1.0",
         .needs = 0, .conflicts = "tigrex_inject",
         .init = dup_init, .shutdown = 0);
