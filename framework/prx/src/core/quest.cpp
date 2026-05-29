/*
 * Quest domain — read/edit the active quest's monster list, and the
 * framework-owned buildTargets hook that fires
 * MHFU_EVENT_QUEST_TARGETS_BUILDING.
 *
 * This is where the raw quest-record pokes live, concentrated once.
 * Layout (memory quest-singleton-monster-list, Section 31):
 *   Quest::objectPtr  @ 0x08A62C7C -> Quest singleton
 *   rec_base          = Quest+0x50  (parsed .mib buffer)
 *   big-monster list A = rec_base + *(rec_base+0x14); nodes are 0x10 bytes:
 *     +0x08 mon-header offset (0 = END), +0x0C full-record offset
 *   record: emId @+0x00 (u16); spawn X @+0x20 (f32), Z @+0x28 (f32)
 *   buildTargets call site @ 0x08869EEC -> 0x0886D4D8
 */
#include "mhfu/quest.h"
#include "mhfu/hooks.h"
#include "mhfu/memory.h"
#include "mhfu/log.h"
#include "internal.h"

#define QUEST_OBJPTR      0x08A62C7Cu
#define RECBASE_OFF       0x50u
#define LISTA_HDR_OFF     0x14u    /* rec_base+0x14 holds the list-A offset */
#define NODE_STRIDE       0x10u
#define NODE_HDR_OFF      0x08u
#define NODE_REC_OFF      0x0Cu
#define REC_X_OFF         0x20u
#define REC_Z_OFF         0x28u
#define BUILDTARGETS_SITE 0x08869EECu
#define BUILDTARGETS      0x0886D4D8u

/* Per-species quest-record field set. Only Tigrex is decoded; add rows
 * as other species' record layouts are confirmed. */
typedef struct {
    mhfu_monster_id_t id;
    uint8_t  model_idx;   /* +0x05 */
    uint32_t hp;          /* +0x08 */
    uint16_t rec_id;      /* +0x1C */
    uint16_t flags;       /* +0x32 */
} species_record_t;

static const species_record_t SPECIES[] = {
    { MON_TIGREX, 0x04, 0x6D, 0xF692, 0x0960 },
};

static const species_record_t *species_lookup(mhfu_monster_id_t id)
{
    for (unsigned i = 0; i < sizeof(SPECIES) / sizeof(SPECIES[0]); i++)
        if (SPECIES[i].id == id) return &SPECIES[i];
    return 0;
}

static uint32_t list_a(mhfu_quest_t q)
{
    if (!mhfu_mem_valid(q)) return 0;
    uint32_t recb = mhfu_read_u32(q + RECBASE_OFF);
    if (!mhfu_mem_valid(recb)) return 0;
    uint32_t la = recb + mhfu_read_u32(recb + LISTA_HDR_OFF);
    return mhfu_mem_valid(la) ? la : 0;
}

static uint32_t recbase(mhfu_quest_t q) { return mhfu_read_u32(q + RECBASE_OFF); }

/* --- public API --- */

extern "C" mhfu_quest_t mhfu_quest_current(void)
{
    uint32_t q = mhfu_read_u32(QUEST_OBJPTR);
    return mhfu_mem_valid(q) ? q : 0;
}

extern "C" int mhfu_quest_monster_count(mhfu_quest_t q)
{
    uint32_t la = list_a(q);
    if (!la) return 0;
    int n = 0;
    for (uint32_t node = la; mhfu_read_u32(node + NODE_HDR_OFF) != 0 && n < 8;
         node += NODE_STRIDE) n++;
    return n;
}

extern "C" int mhfu_quest_has(mhfu_quest_t q, mhfu_monster_id_t id)
{
    uint32_t la = list_a(q), recb = recbase(q);
    if (!la || !mhfu_mem_valid(recb)) return 0;
    for (uint32_t node = la; mhfu_read_u32(node + NODE_HDR_OFF) != 0; node += NODE_STRIDE) {
        uint32_t roff = mhfu_read_u32(node + NODE_REC_OFF);
        if (roff && (mhfu_read_u16(recb + roff) & 0xFF) == (uint16_t)id) return 1;
    }
    return 0;
}

extern "C" mhfu_hook_rc_t mhfu_quest_replace_monster(mhfu_quest_t q,
                                                     mhfu_monster_id_t from,
                                                     mhfu_monster_id_t to)
{
    const species_record_t *sp = species_lookup(to);
    if (!sp) { mhfu_log("[quest] replace: species 0x%02x record layout unknown",
                        (unsigned)to); return MHFU_HOOK_BADARG; }
    uint32_t la = list_a(q), recb = recbase(q);
    if (!la || !mhfu_mem_valid(recb)) return MHFU_HOOK_BADARG;

    for (uint32_t node = la; mhfu_read_u32(node + NODE_HDR_OFF) != 0; node += NODE_STRIDE) {
        uint32_t hoff = mhfu_read_u32(node + NODE_HDR_OFF);
        uint32_t roff = mhfu_read_u32(node + NODE_REC_OFF);
        if (!roff) continue;
        uint32_t rec = recb + roff;
        if ((mhfu_read_u16(rec) & 0xFF) != (uint16_t)from) continue;
        /* retag record + mon-header to the new species (keep native coords) */
        mhfu_write_u16(rec + 0x00, (uint16_t)to);
        mhfu_write_u8 (rec + 0x05, sp->model_idx);
        mhfu_write_u32(rec + 0x08, sp->hp);
        mhfu_write_u16(rec + 0x1C, sp->rec_id);
        mhfu_write_u16(rec + 0x32, sp->flags);
        mhfu_write_u16(recb + hoff, (uint16_t)to);
        mhfu_log("[quest] replaced 0x%02x -> 0x%02x", (unsigned)from, (unsigned)to);
        return MHFU_HOOK_OK;
    }
    return MHFU_HOOK_BADARG;   /* `from` not in this quest */
}

extern "C" mhfu_hook_rc_t mhfu_quest_add_monster(mhfu_quest_t q, mhfu_monster_id_t id,
                                                 float x, float z)
{
    (void)q; (void)id; (void)x; (void)z;
    /* A 2nd big monster overruns a per-quest provisioned spawn array
     * (crash). Until the cap is understood, refuse rather than crash —
     * use mhfu_quest_replace_monster. */
    mhfu_log("[quest] add_monster blocked by spawn cap; use replace_monster");
    return MHFU_HOOK_NOSPACE;
}

/* --- framework-owned buildTargets hook --- */

/* PREFIX helper: $a0 = Quest (the delay slot at the call site set it
 * before our stub ran). Fire the event so subscribers can edit the list
 * before the engine builds + the loading screen loads models. */
static void quest_targets_helper(uint32_t quest)
{
    mhfu_quest_ctx_t ctx;
    ctx.event_id = MHFU_EVENT_QUEST_TARGETS_BUILDING;
    ctx.quest    = mhfu_mem_valid(quest) ? quest : 0;
    mhfu_registry_fire(MHFU_EVENT_QUEST_TARGETS_BUILDING, &ctx);
}

extern "C" void mhfu_quest_init(void)
{
    if (mhfu_registry_count(MHFU_EVENT_QUEST_TARGETS_BUILDING) == 0) return;  /* no subscribers */
    mhfu_install_call_wrapper(BUILDTARGETS_SITE, BUILDTARGETS,
                              quest_targets_helper, MHFU_WRAP_PREFIX, "framework");
    mhfu_log("[quest] buildTargets wrapper queued (TARGETS_BUILDING subscribers present)");
}
