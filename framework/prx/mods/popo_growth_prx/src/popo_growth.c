/*
 * popo_growth.prx — first real runtime mod for MHFU.
 *
 * What it does:
 *   • Subscribes to mhfu_on_map_section_entered + mhfu_on_monster_spawned.
 *   • On entering section 1 of a snowy-mountains-style map, the mod
 *     activates a worker thread that oscillates the size_scale of
 *     every Popo currently in the entity registry.
 *   • On leaving section 1, the worker pauses and restores the
 *     original size_scale of each tracked Popo.
 *
 * The oscillation:
 *   Period 5 seconds. Size sequence (0.5 step) cycled at 0.5 s per
 *   step, bouncing 0.35 → 2.0 → 0.35 → ...
 *     0.35 → 0.85 → 1.35 → 1.85 → 2.00 → 1.85 → 1.35 → 0.85 → 0.35 …
 *   The endpoints are clamped to the user-specified [0.35, 2.0]
 *   range so the visual stays in a comically large/small band.
 *
 * The framework dispatches MHFU_EVENT_MAP_SECTION_ENTERED for every
 * area_index transition; this mod filters for section_id == 1 to
 * decide when to activate/deactivate.
 *
 * Tracked-Popo bookkeeping:
 *   When MHFU_EVENT_MONSTER_SPAWNED fires for a Popo (monster_type
 *   == 0x46 in MHFU EU), we record (entity_ptr, original size_scale)
 *   into a small fixed table. The growth worker iterates the table
 *   each tick and writes the new size_scale into entity_ptr+0x024.
 *   A Popo whose entity_ptr disappears from the registry is purged
 *   from the table on the next sweep.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mhfu_framework.h"

PSP_MODULE_INFO("popo_growth", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

#define POPO_MONSTER_TYPE      0x46

/* The user-facing "section 1" of the snowy-mountains map shows up as
 * area_index == 99 in the game's internal numbering (verified live on
 * the snow_quest_before_section1 savestate 2026-05-25: 98 → 99 on the
 * gate from base camp into the first Popo zone). Change this constant
 * if you fork the mod for a different map's first section. */
#define POPO_TARGET_AREA_INDEX 99
#define MAX_TRACKED_POPOS      8

#define ENTITY_OFF_SIZE_SCALE  0x024
#define ENTITY_REGISTRY_ADDR   0x09C1213Cu
#define ENTITY_REGISTRY_SLOTS  21

/* Oscillation pattern. 10 steps × 500 ms = 5 s total period. */
static const float g_size_steps[10] = {
    0.35f, 0.85f, 1.35f, 1.85f, 2.00f,
    1.85f, 1.35f, 0.85f, 0.35f, 0.35f,
};
#define POPO_STEP_COUNT       (sizeof(g_size_steps) / sizeof(g_size_steps[0]))
#define POPO_STEP_INTERVAL_MS 500

typedef struct {
    uint32_t entity_ptr;        /* 0 = slot empty */
    float    original_size;
} tracked_popo_t;

static tracked_popo_t g_popos[MAX_TRACKED_POPOS];
static volatile int   g_active = 0;      /* set by section events */
static volatile int   g_worker_running = 0;
static SceUID         g_worker_uid = -1;

/* ------------------------------------------------------------------------ *
 * Tracking helpers.
 * ------------------------------------------------------------------------ */

static int find_popo_slot(uint32_t entity_ptr)
{
    for (int i = 0; i < MAX_TRACKED_POPOS; i++)
        if (g_popos[i].entity_ptr == entity_ptr) return i;
    return -1;
}

static int find_free_popo_slot(void)
{
    for (int i = 0; i < MAX_TRACKED_POPOS; i++)
        if (g_popos[i].entity_ptr == 0) return i;
    return -1;
}

static void track_popo(uint32_t entity_ptr)
{
    if (find_popo_slot(entity_ptr) >= 0) return;
    int idx = find_free_popo_slot();
    if (idx < 0) return;
    union { uint32_t u; float f; } cvt;
    cvt.u = *(volatile uint32_t *)(entity_ptr + ENTITY_OFF_SIZE_SCALE);
    g_popos[idx].entity_ptr    = entity_ptr;
    g_popos[idx].original_size = cvt.f;
    mhfu_log("[popo_growth] tracking Popo @ 0x%08lx orig_size_raw=0x%08lx",
             (unsigned long)entity_ptr, (unsigned long)cvt.u);
}

/* Is the pointer still in the live entity registry? Slot 0 (player)
 * is intentionally skipped. */
static int is_entity_still_alive(uint32_t entity_ptr)
{
    for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
        if (p == entity_ptr) return 1;
    }
    return 0;
}

static void restore_and_purge(int reset_originals)
{
    for (int i = 0; i < MAX_TRACKED_POPOS; i++) {
        if (g_popos[i].entity_ptr == 0) continue;
        if (reset_originals && is_entity_still_alive(g_popos[i].entity_ptr)) {
            union { uint32_t u; float f; } cvt;
            cvt.f = g_popos[i].original_size;
            *(volatile uint32_t *)(g_popos[i].entity_ptr +
                                   ENTITY_OFF_SIZE_SCALE) = cvt.u;
        }
        g_popos[i].entity_ptr    = 0;
        g_popos[i].original_size = 0.0f;
    }
}

static void prune_dead_popos(void)
{
    for (int i = 0; i < MAX_TRACKED_POPOS; i++) {
        if (g_popos[i].entity_ptr == 0) continue;
        if (!is_entity_still_alive(g_popos[i].entity_ptr)) {
            g_popos[i].entity_ptr    = 0;
            g_popos[i].original_size = 0.0f;
        }
    }
}

/* ------------------------------------------------------------------------ *
 * Growth worker — drives the size_scale oscillation.
 *
 * Runs for the lifetime of the PRX. Only writes when g_active != 0,
 * so leaving the target section pauses the visual effect.
 * ------------------------------------------------------------------------ */

static int growth_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_worker_running = 1;
    int step = 0;

    for (;;) {
        sceKernelDelayThread(POPO_STEP_INTERVAL_MS * 1000);
        if (!g_active) continue;

        prune_dead_popos();
        union { uint32_t u; float f; } cvt;
        cvt.f = g_size_steps[step];
        for (int i = 0; i < MAX_TRACKED_POPOS; i++) {
            uint32_t p = g_popos[i].entity_ptr;
            if (p == 0) continue;
            *(volatile uint32_t *)(p + ENTITY_OFF_SIZE_SCALE) = cvt.u;
        }
        step = (step + 1) % POPO_STEP_COUNT;
    }
    return 0;
}

/* ------------------------------------------------------------------------ *
 * Event callbacks.
 * ------------------------------------------------------------------------ */

static void on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now_target = (ctx->section_id == POPO_TARGET_AREA_INDEX);
    int was_target = (ctx->prev_section_id == POPO_TARGET_AREA_INDEX);

    if (now_target && !was_target) {
        g_active = 1;
        mhfu_log("[popo_growth] ACTIVATE — entered section %u (from %u)",
                 (unsigned)ctx->section_id, (unsigned)ctx->prev_section_id);
    } else if (was_target && !now_target) {
        g_active = 0;
        restore_and_purge(1);
        mhfu_log("[popo_growth] DEACTIVATE — left section %u (now %u)",
                 (unsigned)ctx->prev_section_id, (unsigned)ctx->section_id);
    }
}

static void on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != POPO_MONSTER_TYPE) return;
    track_popo(ctx->entity_ptr);
}

/* ------------------------------------------------------------------------ *
 * Module entry.
 * ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Defer registration until the framework has finished its own
     * module_start path — give it a beat so its mhfu_log fd is open
     * and its event table is fully constructed.  Earlier ordering
     * caused the framework's "ready" + trampoline-install lines to
     * never reach the log (the framework main thread was apparently
     * starved before it could call install_worker). */
    sceKernelDelayThread(2 * 1000 * 1000);

    mhfu_on_map_section_entered(on_section);
    mhfu_on_monster_spawned(on_monster);
    mhfu_log("[popo_growth] registered: section + monster-spawn callbacks");

#ifndef POPO_GROWTH_NO_WORKER
    g_worker_uid = sceKernelCreateThread(
        "popo_growth_worker", growth_worker,
        0x18, 0x1000, 0, NULL);
    if (g_worker_uid >= 0) {
        sceKernelStartThread(g_worker_uid, 0, NULL);
        mhfu_log("[popo_growth] worker thread started (uid=0x%08lx)",
                 (unsigned long)g_worker_uid);
    } else {
        mhfu_log("[popo_growth] worker thread create failed: %d",
                 g_worker_uid);
    }
#else
    mhfu_log("[popo_growth] worker thread DISABLED (diag)");
#endif

    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_active = 0;
    restore_and_purge(1);
    mhfu_unregister_event(MHFU_EVENT_MAP_SECTION_ENTERED,
                          (mhfu_event_cb_t)(void *)on_section);
    mhfu_unregister_event(MHFU_EVENT_MONSTER_SPAWNED,
                          (mhfu_event_cb_t)(void *)on_monster);
    return 0;
}
