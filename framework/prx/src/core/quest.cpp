/*
 * Quest domain — read/edit the active quest's monster list, and the
 * framework-owned buildTargets wrapper that fires
 * MHFU_EVENT_QUEST_TARGETS_BUILDING (prefix) + finalizes a pending ADD
 * (postfix).
 *
 * Layout (memory quest-singleton-monster-list, Section 31 + cap Section 38):
 *   Quest::objectPtr  @ 0x08A62C7C -> Quest singleton
 *   rec_base          = Quest+0x50  (parsed .mib buffer, fixed 0x08A5C440)
 *   big-monster list A = rec_base + *(rec_base+0x14); nodes are 0x10 bytes:
 *     +0x08 mon-header offset (0 = END), +0x0C full-record offset
 *   record: emId @+0x00 (u16); spawn X @+0x20 (f32), Z @+0x28 (f32)
 *   buildTargets call site @ 0x08869EEC -> 0x0886D4D8
 *
 *   --- big-monster CAP (Section 38, 2026-06-09) ---
 *   The engine holds up to TWO big-monster groups in Quest.targets[2]:
 *     target[0] @ Quest+0x768, target[1] @ Quest+0x794   (stride 0x2C)
 *     per group: definitions[5] @+0x00, emId[] @+0x14, bytes_0x19[] @+0x19,
 *                count (u16) @+0x1E
 *     Quest+0x67C = big-monster-GROUP count (1 single / 2 dual)
 *   A native 2-big quest = both groups filled (count 1 each) + Quest+0x67C=2.
 *   So ADD = put the new monster in target[1] + raise Quest+0x67C 1->2
 *   (the old "add crashes" was almost certainly the missing +0x67C bump).
 */
#include "mhfu/quest.h"
#include "mhfu/hooks.h"
#include "mhfu/memory.h"
#include "mhfu/mips.h"
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

/* --- big-monster AI-SCRIPT resource load (C-hybrid, Section 39) ---
 * The big-mon setup state machine (0x088B996C) registers each resource via
 * registerScriptResource then SPINS on a context-global completion poll
 * (z_un_088901e0 = ctx->vt[0x38]) in state 3 until the streaming context has
 * fully drained. We PREFIX that poll: register our added monster's script
 * resources (dedup-safe) BEFORE the poll runs → the engine's own state-3 wait
 * blocks for our blobs too, then resumes. No race, no fighting. */
#define REGISTER_SCRIPT_RES 0x08890144u   /* registerScriptResource(ctx, res_id, slot) */
#define BIGMON_RES_CTX      0x08A631A0u   /* [this] = streaming ctx (a0 the natives pass) */
/* z_un_088901e0 = the GENERIC streaming-completion poll (ctx->vt[0x38]); EVERY
 * barrier-poll site calls it, so an entry detour here fires for any quest's load
 * (a species-specific call site like 0x088B9A88 is NOT hit for every monster). */
#define POLL_FN             0x088901E0u
#define POLL_FN_LUI_V0_89D  0x3C02089Du   /* original first insn: lui v0,0x89D */
#define SCRIPT_STRUCT_ADDR  0x09D56448u   /* per-species fixed script struct (Tigrex) */
#define MAX_SCRIPT_RES      2u
#define MAX_MOUNTS          6u

/* archive MOUNT primitive: z_un_088ddbd8(ctx, type, id, flags) loads a monster's
 * resource archive so its script res-ids become resolvable; z_un_088ddf10(ctx,type)
 * polls "loaded?". ctx for mounts/poll = [0x09A4F0D0] (native overlay-init's a0). */
#define MOUNT_FN     0x088DDBD8u
#define MOUNT_POLL   0x088DDF10u
#define MOUNT_CTX    0x09A4F0D0u

/* QuestTarget array (cap, Section 38) */
#define BIGMON_COUNT_OFF  0x67Cu   /* Quest+0x67C = big-mon group count */
#define TARGET0_OFF       0x768u
#define TARGET_STRIDE     0x2Cu
#define TGT_DEF0_OFF      0x00u    /* definitions[0]  */
#define TGT_EMID_OFF      0x14u    /* emId[0]         */
#define TGT_B19_OFF       0x19u    /* bytes_0x19[0]   */
#define TGT_COUNT_OFF     0x1Eu    /* count (u16)     */

/* rec_base tail scratch for a fabricated record + mon-header (used data
 * ends ~+0x1B84; tail is free, Section 31). */
#define SCRATCH_REC_OFF   0x1C00u  /* 0x40-byte record   */
#define SCRATCH_HDR_OFF   0x1C40u  /* 0x10-byte mon-hdr  */

/* Per-species quest-record field set. Only Tigrex is decoded; add rows
 * as other species' record layouts are confirmed. */
typedef struct {
    mhfu_monster_id_t id;
    uint8_t  model_idx;   /* +0x05 */
    uint32_t hp;          /* +0x08 */
    uint16_t rec_id;      /* +0x1C */
    uint16_t flags;       /* +0x32 */
    /* Forge of the native per-monster resource load (Section 39). The overlay-init
     * MOUNTS these archives (z_un_088ddbd8 type,id) then REGISTERS the script res-ids.
     * Replicating both for an ADDED monster fills its script struct. */
    struct { uint8_t type; uint16_t id; } mounts[MAX_MOUNTS];
    uint8_t  mount_n;
    uint16_t script_res[MAX_SCRIPT_RES];
    uint8_t  script_slot[MAX_SCRIPT_RES];
} species_record_t;

static const species_record_t SPECIES[] = {
    { MON_TIGREX, 0x04, 0x6D, 0xF692, 0x0960,
      { {0xD,0x17CE},{0xE,0x17CC},{0xF,0x17CD},{0x7,0x51},{0x8,0x1517},{0x0,0x171B} }, 6,
      { 0x17DD, 0x1611 }, { 3, 4 } },
};

static const species_record_t *species_lookup(mhfu_monster_id_t id)
{
    for (unsigned i = 0; i < sizeof(SPECIES) / sizeof(SPECIES[0]); i++)
        if (SPECIES[i].id == id) return &SPECIES[i];
    return 0;
}

static uint32_t recbase(mhfu_quest_t q) { return mhfu_read_u32(q + RECBASE_OFF); }

static uint32_t list_a(mhfu_quest_t q)
{
    if (!mhfu_mem_valid(q)) return 0;
    uint32_t recb = recbase(q);
    if (!mhfu_mem_valid(recb)) return 0;
    uint32_t la = recb + mhfu_read_u32(recb + LISTA_HDR_OFF);
    return mhfu_mem_valid(la) ? la : 0;
}

/* --- pending-ADD state (set in prefix, consumed in postfix) --- */
static volatile uint32_t g_add_quest   = 0;
static volatile uint32_t g_add_rec_abs = 0;   /* abs ptr of the fabricated record */
static volatile uint32_t g_add_id      = 0;

/* --- script-resource injection (set by ADD, consumed at the barrier poll) ---
 * g_script_n>0 while a non-resident ADD's scripts still need registering. Reset
 * each quest build (prefix); set by mhfu_quest_add_monster. */
static volatile int      g_script_n        = 0;
static volatile uint16_t g_script_res[MAX_SCRIPT_RES]  = { 0, 0 };
static volatile uint8_t  g_script_slot[MAX_SCRIPT_RES] = { 0, 0 };
static volatile int      g_mount_n          = 0;
static volatile uint8_t  g_mount_type[MAX_MOUNTS] = { 0 };
static volatile uint16_t g_mount_id[MAX_MOUNTS]   = { 0 };
static volatile int      g_forge_phase      = 0;   /* 0=mount 1=wait+register 2=done */

typedef int (*reg_script_fn)(uint32_t ctx, uint32_t res_id, uint32_t slot);
typedef int (*mount_res_fn)(uint32_t ctx, uint32_t type, uint32_t id, uint32_t flags);
typedef int (*mount_poll_fn)(uint32_t ctx, uint32_t type);

/* ENTRY-detour on the generic streaming-completion poll. Fires each poll-tick
 * during loading. FORGES the native per-monster resource load for the ADDED
 * monster: phase 0 mounts its archives (z_un_088ddbd8), phase 1 waits all loaded
 * (z_un_088ddf10) then registers its script res-ids — exactly what the primary's
 * overlay-init does. The engine's own poll then waits until our blobs stream, so
 * the added monster's script struct (0x09D56448) fills before its AI ticks. */
static void bigmon_resource_poll_prefix(uint32_t a0)
{
    (void)a0;
    if (g_script_n <= 0) return;

    static volatile int reentry = 0;
    if (reentry) return;
    reentry = 1;

    if (mhfu_read_u32(SCRIPT_STRUCT_ADDR)) {        /* struct filled → done */
        if (g_forge_phase != 9) { mhfu_log("[quest] forge DONE: struct0=0x%08X",
                                  (unsigned)mhfu_read_u32(SCRIPT_STRUCT_ADDR)); g_forge_phase = 9; }
        g_script_n = 0; reentry = 0; return;
    }

    uint32_t mctx = mhfu_read_u32(MOUNT_CTX);
    if (!mhfu_mem_valid(mctx)) { reentry = 0; return; }
    mount_res_fn  mount = (mount_res_fn)MOUNT_FN;
    mount_poll_fn mpoll = (mount_poll_fn)MOUNT_POLL;
    reg_script_fn reg   = (reg_script_fn)REGISTER_SCRIPT_RES;

    if (g_forge_phase == 0) {                        /* mount all archives once */
        for (int i = 0; i < g_mount_n && i < (int)MAX_MOUNTS; i++)
            mount(mctx, g_mount_type[i], g_mount_id[i], 0);
        mhfu_log("[quest] forge: mounted %d archives (ctx=0x%08X)", g_mount_n, (unsigned)mctx);
        g_forge_phase = 1; reentry = 0; return;
    }
    if (g_forge_phase == 1) {                         /* wait all loaded, then register */
        int all = 1;
        for (int i = 0; i < g_mount_n && i < (int)MAX_MOUNTS; i++)
            if (mpoll(mctx, g_mount_type[i]) == 0) { all = 0; break; }
        if (!all) { reentry = 0; return; }
        uint32_t sctx = mhfu_read_u32(BIGMON_RES_CTX);
        int r0 = -99, r1 = -99;
        if (g_script_res[0]) r0 = reg(sctx, g_script_res[0], g_script_slot[0]);
        if (g_script_n > 1 && g_script_res[1]) r1 = reg(sctx, g_script_res[1], g_script_slot[1]);
        mhfu_log("[quest] forge: archives loaded; reg(0x%04X)=%d reg(0x%04X)=%d sctx=0x%08X",
                 g_script_res[0], r0, g_script_res[1], r1, (unsigned)sctx);
        g_forge_phase = 2;
    }
    reentry = 0;
}

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

extern "C" int mhfu_quest_first_monster(mhfu_quest_t q)
{
    uint32_t la = list_a(q), recb = recbase(q);
    if (!la || !mhfu_mem_valid(recb)) return -1;
    uint32_t roff = mhfu_read_u32(la + NODE_REC_OFF);
    if (!roff) return -1;
    return (int)(mhfu_read_u16(recb + roff) & 0xFF);
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
        mhfu_write_u16(rec + 0x00, (uint16_t)to);
        mhfu_write_u8 (rec + 0x05, sp->model_idx);
        mhfu_write_u32(rec + 0x08, sp->hp);
        mhfu_write_u16(rec + 0x1C, sp->rec_id);
        mhfu_write_u16(rec + 0x32, sp->flags);
        mhfu_write_u16(recb + hoff, (uint16_t)to);
        mhfu_log("[quest] replaced 0x%02x -> 0x%02x", (unsigned)from, (unsigned)to);
        return MHFU_HOOK_OK;
    }
    return MHFU_HOOK_BADARG;
}

/* ADD a 2nd big monster (Section 38). MUST be called from a
 * MHFU_EVENT_QUEST_TARGETS_BUILDING (prefix) subscriber: we fabricate a
 * record + mon-header in the rec_base tail and append a list-A node so the
 * loading-screen model-load reads it (native model load, Section 31). The
 * group-1 wiring + Quest+0x67C bump is finalized in the postfix half of the
 * framework's buildTargets wrapper (after buildTargets has built group 0). */
extern "C" mhfu_hook_rc_t mhfu_quest_add_monster(mhfu_quest_t q, mhfu_monster_id_t id,
                                                 float x, float z)
{
    const species_record_t *sp = species_lookup(id);
    if (!sp) { mhfu_log("[quest] add: species 0x%02x layout unknown", (unsigned)id);
               return MHFU_HOOK_BADARG; }
    uint32_t la = list_a(q), recb = recbase(q);
    if (!la || !mhfu_mem_valid(recb)) return MHFU_HOOK_BADARG;

    /* find a source record to clone (the quest's first big monster) */
    uint32_t src = 0;
    for (uint32_t node = la; mhfu_read_u32(node + NODE_HDR_OFF) != 0; node += NODE_STRIDE) {
        uint32_t roff = mhfu_read_u32(node + NODE_REC_OFF);
        if (roff) { src = recb + roff; break; }
    }
    if (!src) return MHFU_HOOK_BADARG;

    uint32_t rec = recb + SCRATCH_REC_OFF;
    uint32_t hdr = recb + SCRATCH_HDR_OFF;

    /* clone 0x40 bytes of the source record, then retag species fields */
    for (uint32_t o = 0; o < 0x40; o += 4)
        mhfu_write_u32(rec + o, mhfu_read_u32(src + o));
    mhfu_write_u16(rec + 0x00, (uint16_t)id);
    mhfu_write_u8 (rec + 0x05, sp->model_idx);
    mhfu_write_u32(rec + 0x08, sp->hp);
    mhfu_write_u16(rec + 0x1C, sp->rec_id);
    mhfu_write_u16(rec + 0x32, sp->flags);
    if (x != 0.0f) mhfu_write_f32(rec + REC_X_OFF, x);
    if (z != 0.0f) mhfu_write_f32(rec + REC_Z_OFF, z);

    /* mon-header { u16 emId; u16 0; u8 0xFF x12 } */
    mhfu_write_u32(hdr + 0x00, (uint32_t)id);
    mhfu_write_u32(hdr + 0x04, 0xFFFFFFFFu);
    mhfu_write_u32(hdr + 0x08, 0xFFFFFFFFu);
    mhfu_write_u32(hdr + 0x0C, 0xFFFFFFFFu);

    /* append a list-A node at the END marker, then re-terminate */
    uint32_t end = la;
    while (mhfu_read_u32(end + NODE_HDR_OFF) != 0) end += NODE_STRIDE;
    mhfu_write_u32(end + 0x00, 1);                 /* flag */
    mhfu_write_u32(end + 0x04, 0);
    mhfu_write_u32(end + NODE_HDR_OFF, SCRATCH_HDR_OFF);
    mhfu_write_u32(end + NODE_REC_OFF, SCRATCH_REC_OFF);
    mhfu_write_u32(end + NODE_STRIDE + NODE_HDR_OFF, 0);   /* new END */

    g_add_quest   = q;
    g_add_rec_abs = rec;
    g_add_id      = (uint32_t)id;

    /* FORGE DISARMED (Section 39-44): the abandoned manual mount/register forge
     * crashed; the script is now provided by an OVERLAY RELOCATION + bind
     * (mhfu_bigmon_load_relocated + entity+0x4C8 rebind, done from the mod). Keep
     * g_script_n = 0 so the 0x088901E0 poll-hook stays a no-op. The add still wires
     * the list-A node + target[1] (model load + native spawn). */
    g_script_n   = 0;
    g_mount_n    = 0;
    g_forge_phase = 9;

    mhfu_log("[quest] add 0x%02x queued: record @0x%08X (forge DISARMED; script via relocation+bind)",
             (unsigned)id, (unsigned)rec);
    return MHFU_HOOK_OK;
}

/* DUPLICATE the quest's existing big monster `id` as a 2ND instance of the SAME
 * family (Section 48). Unlike mhfu_quest_add_monster this does NOT split into a
 * 2nd target group / bump Quest+0x67C: buildTargets is uncapped and lumps every
 * list-A node into group 0, so two same-species nodes => group 0 count 2, sharing
 * the one resident overlay (no forge, no relocation). We just clone the source
 * record (offset its spawn coords by dx/dz so the two don't stack) and append a
 * list-A node; the engine builds + provisions + spawns both natively.
 * MUST be called from a MHFU_EVENT_QUEST_TARGETS_BUILDING (prefix) subscriber. */
extern "C" mhfu_hook_rc_t mhfu_quest_clone_monster(mhfu_quest_t q,
                                                   mhfu_monster_id_t id,
                                                   float dx, float dz)
{
    uint32_t la = list_a(q), recb = recbase(q);
    if (!la || !mhfu_mem_valid(recb)) return MHFU_HOOK_BADARG;

    /* find the source node + record for species `id` */
    uint32_t src = 0;
    for (uint32_t node = la; mhfu_read_u32(node + NODE_HDR_OFF) != 0; node += NODE_STRIDE) {
        uint32_t roff = mhfu_read_u32(node + NODE_REC_OFF);
        if (roff && (mhfu_read_u16(recb + roff) & 0xFF) == (uint16_t)id) { src = recb + roff; break; }
    }
    if (!src) { mhfu_log("[quest] clone: species 0x%02x not in quest", (unsigned)id);
                return MHFU_HOOK_BADARG; }

    uint32_t rec = recb + SCRATCH_REC_OFF;
    uint32_t hdr = recb + SCRATCH_HDR_OFF;

    /* clone 0x40 bytes verbatim (same species => no field retag needed) */
    for (uint32_t o = 0; o < 0x40; o += 4)
        mhfu_write_u32(rec + o, mhfu_read_u32(src + o));
    /* offset spawn coords so the 2nd instance doesn't stack on the 1st */
    if (dx != 0.0f) mhfu_write_f32(rec + REC_X_OFF, mhfu_read_f32(src + REC_X_OFF) + dx);
    if (dz != 0.0f) mhfu_write_f32(rec + REC_Z_OFF, mhfu_read_f32(src + REC_Z_OFF) + dz);

    /* mon-header { u16 emId; u16 0; u8 0xFF x12 } (mirror the add path) */
    mhfu_write_u32(hdr + 0x00, (uint32_t)id);
    mhfu_write_u32(hdr + 0x04, 0xFFFFFFFFu);
    mhfu_write_u32(hdr + 0x08, 0xFFFFFFFFu);
    mhfu_write_u32(hdr + 0x0C, 0xFFFFFFFFu);

    /* append a list-A node at the END marker, then re-terminate */
    uint32_t end = la;
    while (mhfu_read_u32(end + NODE_HDR_OFF) != 0) end += NODE_STRIDE;
    mhfu_write_u32(end + 0x00, 1);                 /* flag */
    mhfu_write_u32(end + 0x04, 0);
    mhfu_write_u32(end + NODE_HDR_OFF, SCRATCH_HDR_OFF);
    mhfu_write_u32(end + NODE_REC_OFF, SCRATCH_REC_OFF);
    mhfu_write_u32(end + NODE_STRIDE + NODE_HDR_OFF, 0);   /* new END */

    /* NB: do NOT set g_add_quest -> postfix stays a no-op -> both monsters
     * remain in group 0 (count 2), which is what same-family wants. */
    mhfu_log("[quest] cloned 0x%02x -> 2nd instance, record @0x%08X (no split, group0 count2)",
             (unsigned)id, (unsigned)rec);
    return MHFU_HOOK_OK;
}

/* --- framework-owned buildTargets wrapper (prefix + postfix) --- */

static void quest_targets_prefix(uint32_t quest)
{
    g_add_quest   = 0;             /* arm fresh each build */
    g_script_n    = 0;             /* no forge unless this build ADDs */
    g_mount_n     = 0;
    g_forge_phase = 0;
    mhfu_quest_ctx_t ctx;
    ctx.event_id = MHFU_EVENT_QUEST_TARGETS_BUILDING;
    ctx.quest    = mhfu_mem_valid(quest) ? quest : 0;
    mhfu_registry_fire(MHFU_EVENT_QUEST_TARGETS_BUILDING, &ctx);
}

/* Runs AFTER buildTargets built group 0. If an ADD is pending, put the new
 * monster in target[1] (group 1, count 1), keep group 0 at count 1, and
 * raise Quest+0x67C to 2 — matching a native dual-big quest (Section 38). */
static void quest_targets_postfix(uint32_t quest)
{
    if (!g_add_quest || g_add_quest != quest || !mhfu_mem_valid(quest)) return;
    uint32_t t0 = quest + TARGET0_OFF;
    uint32_t t1 = t0 + TARGET_STRIDE;

    /* SPAWN: a 2nd DIFFERENT-species monster needs its own group. buildTargets
     * lumped both list-A monsters into group 0 — move the 2nd into group 1 and
     * raise Quest+0x67C to 2 (native dual pattern). The forge poll-hook loads the
     * 2nd monster's scripts (group-0 resolver only covers the primary). */
    mhfu_write_u16(t0 + TGT_COUNT_OFF, 1);
    mhfu_write_u32(t1 + TGT_DEF0_OFF, g_add_rec_abs);
    mhfu_write_u32(t1 + TGT_EMID_OFF, g_add_id);
    mhfu_write_u8 (t1 + TGT_B19_OFF, 1);
    mhfu_write_u16(t1 + TGT_COUNT_OFF, 1);
    mhfu_write_u32(quest + BIGMON_COUNT_OFF, 2);

    mhfu_log("[quest] add finalized: group1 emId=0x%02x +0x67C=2 (forge will load scripts)",
             (unsigned)g_add_id);
    g_add_quest = 0;
}

extern "C" void mhfu_quest_init(void)
{
    if (mhfu_registry_count(MHFU_EVENT_QUEST_TARGETS_BUILDING) == 0) return;

    /* custom stub: prefix(fire event) -> buildTargets -> postfix(finalize add).
     * The generic single-helper wrapper can't do both halves on one site. */
    uint32_t *s = mhfu_cave_alloc(20);
    if (!s) { mhfu_log("[quest] cave exhausted; no buildTargets wrapper"); return; }
    uint32_t pre  = (uint32_t)(uintptr_t)&quest_targets_prefix;
    uint32_t post = (uint32_t)(uintptr_t)&quest_targets_postfix;
    int i = 0;
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
    s[i++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_sw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(pre);            s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(BUILDTARGETS);   s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_A0, 0x10, MIPS_REG_SP);
    s[i++] = mips_jal(post);           s[i++] = MIPS_NOP;
    s[i++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
    s[i++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
    s[i++] = mips_jr(MIPS_REG_RA);
    s[i++] = MIPS_NOP;
    while (i < 20) s[i++] = MIPS_NOP;
    mhfu_flush_caches();

    mhfu_patch_word_when_quiet(BUILDTARGETS_SITE, mips_jal(BUILDTARGETS),
                               mips_jal((uint32_t)(uintptr_t)s), "framework");
    mhfu_log("[quest] buildTargets prefix+postfix wrapper queued");

    /* C-hybrid (Section 39): ENTRY-detour the generic completion poll
     * z_un_088901e0 so an ADD's AI-script resources get registered before the
     * engine's drain wait — for ANY quest (a species-specific call site isn't
     * always hit). Single-word J at entry (delay slot's orig `lw a0` runs
     * harmlessly); the stub calls our helper then replays the 6-insn original. */
    uint32_t *w = mhfu_cave_alloc(16);
    if (w) {
        uint32_t h = (uint32_t)(uintptr_t)&bigmon_resource_poll_prefix;
        int j = 0;
        w[j++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, -0x20);
        w[j++] = mips_sw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
        w[j++] = mips_jal(h);  w[j++] = MIPS_NOP;
        w[j++] = mips_lw(MIPS_REG_RA, 0x18, MIPS_REG_SP);
        w[j++] = mips_addiu(MIPS_REG_SP, MIPS_REG_SP, 0x20);
        /* replay z_un_088901e0: lui v0,0x89D; lw a0,-0x3BCC(v0); lw t9,0(a0); lw t9,0x38(t9); jr t9 */
        w[j++] = mips_lui(MIPS_REG_V0, 0x89D);
        w[j++] = mips_lw(MIPS_REG_A0, (int16_t)-0x3BCC, MIPS_REG_V0);
        w[j++] = mips_lw(MIPS_REG_T9, 0x0, MIPS_REG_A0);
        w[j++] = mips_lw(MIPS_REG_T9, 0x38, MIPS_REG_T9);
        w[j++] = mips_jr(MIPS_REG_T9);
        w[j++] = MIPS_NOP;
        while (j < 16) w[j++] = MIPS_NOP;
        mhfu_flush_caches();
        mhfu_hook_rc_t pr = mhfu_patch_word_when_quiet(
            POLL_FN, POLL_FN_LUI_V0_89D, mips_j((uint32_t)(uintptr_t)w), "framework");
        mhfu_log("[quest] poll-fn entry detour queued @ 0x%08X stub=0x%08X (rc=%d)",
                 POLL_FN, (unsigned)(uintptr_t)w, (int)pr);
    } else {
        mhfu_log("[quest] cave exhausted; no poll-fn detour");
    }
}
