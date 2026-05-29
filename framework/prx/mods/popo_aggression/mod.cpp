/*
 * popo_aggression — chase-chain behaviour. On entering snow-map section 1
 * (area_index 99), each tracked popo is aimed at the next popo in slot
 * order (last wraps to first) by pinning heading + state/anim cells at
 * ~3 Hz (faster pin rates halt the AI; see docs/agent_session_log.md §20).
 *
 * HONEST STATUS (Section 23.12): runtime A/B showed the cell pins are
 * largely PLACEBO — popos roam near-identically with pins on or off. The
 * heading vec at +0x10 is engine OUTPUT, not input. Kept as a reference
 * for the event/track/worker scaffold; real motion control is unsolved.
 * Off by default.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"

#define POPO_TYPE     0x46
#define TARGET_AREA   99
#define MAX_TRACKED   8
#define TICK_MS       333
#define STATE_LOCOMOTION 5
#define ANIM_WALK_FWD    1011
#define OFF_PURSUE   0x1C8
#define OFF_ANIM     0x324

typedef struct { uint32_t ent; int reg_slot; } tracked_t;
static tracked_t    g_tracked[MAX_TRACKED];
static volatile int g_active = 0;

static int slot_of(uint32_t e){ for(int i=0;i<MAX_TRACKED;i++) if(g_tracked[i].ent==e) return i; return -1; }
static int free_slot(void){ for(int i=0;i<MAX_TRACKED;i++) if(g_tracked[i].ent==0) return i; return -1; }

static void prune_and_refresh(void)
{
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (!g_tracked[i].ent) continue;
        int s = mhfu_entity_slot_of(g_tracked[i].ent);
        if (s < 0) { g_tracked[i].ent = 0; g_tracked[i].reg_slot = 0; }
        else g_tracked[i].reg_slot = s;
    }
}

static void repopulate(void)
{
    for (int s = 1; s < MHFU_ENTITY_REGISTRY_SLOTS; s++) {
        uint32_t p = mhfu_entity_at(s);
        if (!p || slot_of(p) >= 0) continue;
        if (mhfu_entity_type(p) != POPO_TYPE) continue;
        int idx = free_slot();
        if (idx < 0) break;
        g_tracked[idx].ent = p; g_tracked[idx].reg_slot = s;
    }
}

static void sort_by_slot(uint32_t *out, int *out_n)
{
    int n = 0;
    for (int i = 0; i < MAX_TRACKED; i++) if (g_tracked[i].ent) out[n++] = g_tracked[i].ent;
    *out_n = n;
    for (int i = 1; i < n; i++) {
        uint32_t p = out[i]; int sp = mhfu_entity_slot_of(p); int j = i;
        while (j > 0 && mhfu_entity_slot_of(out[j-1]) > sp) { out[j] = out[j-1]; j--; }
        out[j] = p;
    }
}

static void aim_at(uint32_t shooter, uint32_t target)
{
    mhfu_vec3_t s = mhfu_entity_pos(shooter), t = mhfu_entity_pos(target);
    float dx = t.x - s.x, dz = t.z - s.z;
    float magsq = dx*dx + dz*dz;
    if (magsq >= 1.0f) {
        float inv = 1.0f / __builtin_sqrtf(magsq);
        mhfu_write_f32(shooter + MHFU_ENT_HEADING + 0, dx * inv);
        mhfu_write_f32(shooter + MHFU_ENT_HEADING + 4, 0.0f);
        mhfu_write_f32(shooter + MHFU_ENT_HEADING + 8, dz * inv);
    }
    if (mhfu_read_u16(shooter + MHFU_ENT_AI_STATE) != STATE_LOCOMOTION)
        mhfu_write_u16(shooter + MHFU_ENT_AI_STATE, STATE_LOCOMOTION);
    if (mhfu_read_u16(shooter + OFF_ANIM) != ANIM_WALK_FWD)
        mhfu_write_u16(shooter + OFF_ANIM, ANIM_WALK_FWD);
    if (mhfu_read_u32(shooter + OFF_PURSUE) != 0)
        mhfu_write_u32(shooter + OFF_PURSUE, 0);
}

static void on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now = (ctx->section_id == TARGET_AREA);
    int was = (ctx->prev_section_id == TARGET_AREA);
    if (now && !was) { g_active = 1; mhfu_log("[popo_agg] ACTIVATE"); }
    else if (was && !now) {
        g_active = 0;
        for (int i = 0; i < MAX_TRACKED; i++) { g_tracked[i].ent = 0; g_tracked[i].reg_slot = 0; }
        mhfu_log("[popo_agg] DEACTIVATE");
    }
}

static void on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != POPO_TYPE) return;
    if (slot_of(ctx->entity_ptr) >= 0) return;
    int idx = free_slot();
    if (idx >= 0) { g_tracked[idx].ent = ctx->entity_ptr; g_tracked[idx].reg_slot = ctx->slot; }
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(TICK_MS * 1000);
        /* poll-based activate (savestate-load path fires no section event) */
        int should = (mhfu_get_area_index() == TARGET_AREA);
        if (should && !g_active) g_active = 1;
        else if (!should && g_active) {
            g_active = 0;
            for (int i = 0; i < MAX_TRACKED; i++) g_tracked[i].ent = 0;
        }
        if (!g_active) continue;
        repopulate();
        prune_and_refresh();
        uint32_t ptrs[MAX_TRACKED]; int n = 0;
        sort_by_slot(ptrs, &n);
        if (n < 2) continue;
        for (int i = 0; i < n; i++) aim_at(ptrs[i], ptrs[(i + 1) % n]);
    }
    return 0;
}

static int agg_init(void)
{
    mhfu_on_map_section_entered(on_section);
    mhfu_on_monster_spawned(on_monster);
    SceUID th = sceKernelCreateThread("popo_aggression",
                                      (SceKernelThreadEntry)worker, 0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

MHFU_MOD(.id = "popo_aggression", .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = agg_init, .shutdown = 0);
