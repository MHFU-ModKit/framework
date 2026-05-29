/*
 * popo_growth — oscillates Popo size_scale 0.35x <-> 2.0x over 5 s while
 * the player is in snow-map section 1 (area_index 99).
 *
 * The engine re-syncs size from one of its mirrors every frame, so a
 * single write to +0x024 is clobbered within ms — mhfu_entity_set_size()
 * writes all five mirrors at ~30 ms cadence to win the race.
 */
#include <pspthreadman.h>
#include "mhfu/mhfu.h"

#define POPO_TYPE        0x46
#define TARGET_AREA      99
#define MAX_TRACKED      8
#define STEP_MS          500
#define TICK_MS          30

static const float SIZE_STEPS[10] = {
    0.35f, 0.85f, 1.35f, 1.85f, 2.00f, 1.85f, 1.35f, 0.85f, 0.35f, 0.35f,
};

typedef struct { uint32_t ent; float orig; } tracked_t;
static tracked_t       g_tracked[MAX_TRACKED];
static volatile int    g_active = 0;

static int slot_of(uint32_t ent)
{
    for (int i = 0; i < MAX_TRACKED; i++) if (g_tracked[i].ent == ent) return i;
    return -1;
}
static int free_slot(void)
{
    for (int i = 0; i < MAX_TRACKED; i++) if (g_tracked[i].ent == 0) return i;
    return -1;
}

static void restore_all(void)
{
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (g_tracked[i].ent && mhfu_entity_is_alive(g_tracked[i].ent))
            mhfu_entity_set_size(g_tracked[i].ent, g_tracked[i].orig);
        g_tracked[i].ent = 0; g_tracked[i].orig = 0.0f;
    }
}

static void on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now = (ctx->section_id == TARGET_AREA);
    int was = (ctx->prev_section_id == TARGET_AREA);
    if (now && !was) { g_active = 1; mhfu_log("[popo_growth] ACTIVATE"); }
    else if (was && !now) { g_active = 0; restore_all(); mhfu_log("[popo_growth] DEACTIVATE"); }
}

static void on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != POPO_TYPE) return;
    if (slot_of(ctx->entity_ptr) >= 0) return;
    int idx = free_slot();
    if (idx < 0) return;
    g_tracked[idx].ent  = ctx->entity_ptr;
    g_tracked[idx].orig = mhfu_entity_size(ctx->entity_ptr);
}

static int worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int step = 0, ticks = 0;
    const int ticks_per_step = STEP_MS / TICK_MS;
    for (;;) {
        sceKernelDelayThread(TICK_MS * 1000);
        if (!g_active) { step = 0; ticks = 0; continue; }
        /* prune dead */
        for (int i = 0; i < MAX_TRACKED; i++)
            if (g_tracked[i].ent && !mhfu_entity_is_alive(g_tracked[i].ent))
                g_tracked[i].ent = 0;
        float sz = SIZE_STEPS[step];
        for (int i = 0; i < MAX_TRACKED; i++)
            if (g_tracked[i].ent) mhfu_entity_set_size(g_tracked[i].ent, sz);
        if (++ticks >= ticks_per_step) {
            ticks = 0;
            step = (step + 1) % 10;
        }
    }
    return 0;
}

static int growth_init(void)
{
    mhfu_on_map_section_entered(on_section);
    mhfu_on_monster_spawned(on_monster);
    SceUID th = sceKernelCreateThread("popo_growth",
                                      (SceKernelThreadEntry)worker, 0x18, 0x1000, 0, NULL);
    if (th < 0) return -1;
    sceKernelStartThread(th, 0, NULL);
    return 0;
}

MHFU_MOD(.id = "popo_growth", .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = growth_init, .shutdown = 0);
