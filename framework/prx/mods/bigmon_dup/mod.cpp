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
/* N-GROUP cap test (this session): each extra Tigrex goes into its OWN target
 * group, so 2 extra => 3 groups => (hypothesis) 3 engine managers => 3 fully
 * combat-resolved, DAMAGING Tigrex. Same family => resources resident (no forge,
 * no overlay relocation). Source Tigrex ~ (11209,9493); offset each extra so
 * they don't stack. add_monster writes rec+0x20=X, rec+0x28=Z when nonzero. */
/* §48 cap: the engine honors only 2 big-mon target groups (a 3rd corrupts
 * adjacent quest data). Keep EXTRA_N=1 = the safe 2-Tigrex split. */
#define EXTRA_N  1              /* extra big monsters (total = 1 + EXTRA_N = 2) */
static const float SPAWN[EXTRA_N][2] = {
    { 14209.0f, 9493.0f },     /* 2nd Tigrex */
};

static void on_targets_building(const mhfu_quest_ctx_t *ctx)
{
    if (!ctx->quest) return;
    if (!mhfu_quest_has(ctx->quest, DUP_ID)) return;
    /* N-GROUP split: each add_monster appends a list-A node; the framework postfix
     * places each extra into target[1..n] + raises Quest+0x67C to 1+n -> engine
     * provisions one manager per group. Same family => the (disarmed) forge isn't
     * needed. If the engine spawns + damages all of them, the ~2 cap is broken. */
    int added = 0;
    for (int i = 0; i < EXTRA_N; i++) {
        if (mhfu_quest_add_monster(ctx->quest, DUP_ID, SPAWN[i][0], SPAWN[i][1]) == MHFU_HOOK_OK) {
            added++;
            mhfu_log("[%s] added extra #%d 0x%02x @(%.0f,%.0f) as own group",
                     MOD_ID, added, (unsigned)DUP_ID, SPAWN[i][0], SPAWN[i][1]);
        }
    }
    mhfu_log("[%s] %d extra group(s) queued -> %d big-mon groups total",
             MOD_ID, added, 1 + added);
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
