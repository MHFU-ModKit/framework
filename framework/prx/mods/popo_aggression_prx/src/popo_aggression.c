/*
 * popo_aggression.prx — heading-vec-driven AI control mod.
 *
 * Sister mod to popo_growth (which writes size mirrors). This one writes
 * the per-entity heading vec at +0x010..+0x018 and forces state/anim to
 * the "walk-forward" combo so the engine's motion pipeline picks the
 * direction we choose.
 *
 * Behavior:
 *   • On entering snow-map section 1 (area_index 99), every tracked
 *     popo is told to walk toward the NEXT popo in slot order; the last
 *     wraps to the first. Forms a visible chase chain.
 *   • Worker thread runs at 30 ms cadence (matches game-logic tick).
 *
 * Discovery context (2026-05-26):
 *   The host-side debugger pin engine couldn't reliably steer popos —
 *   memory writes from the WebSocket debugger trigger AI halts (8-60 Hz
 *   rates produce STATIC popos) AND PPSSPP JIT-caches the popo overlay
 *   so patches to the heading-rotator functions don't invalidate the
 *   cache. PSP-side writes from a PRX thread are CPU-coherent and win
 *   the race naturally, same proven pattern popo_growth uses.
 *
 * IMPORTANT: PPSSPP's plugin host wedges MHFU at boot when two plugin
 * PRXes co-load (verified 2026-05-25 — framework alone boots; framework
 * + popo_growth wedges at black screen). The shipping build embeds this
 * mod directly into mhfu_framework.prx via MHFU_EMBED_POPO_AGGRESSION.
 * This file remains the canonical reference for the future stable-stub
 * path when PPSSPP's plugin-co-load bug is resolved.
 *
 * Cell offsets are entity-relative — verified live in slot 9
 * (snow_quest_before_section1) for MHFU EU (ULES01213).
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mhfu_framework.h"

PSP_MODULE_INFO("popo_aggression", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

#define POPO_MONSTER_TYPE       0x46
#define POPO_TARGET_AREA_INDEX  99
#define MAX_TRACKED_POPOS       8
/* 333 ms (~3 Hz). Tested 2026-05-26: at 30 ms writes per-frame race the
 * engine's wander cycle and halt popos. At 333 ms the AI gets ~10 frames
 * to integrate root-motion between our overrides. Host-side sweep showed
 * tracking score +0.86 at this rate. */
#define TICK_MS                 333

/* Entity-relative offsets. The "alarmed-state" cells (busy_bits, +0xC0/
 * +0xC4, +0x298, +0x460, +0x63C) from an earlier hit-diff turned out to
 * encode a popo's death state (the diff window contained the popo dying
 * from two SnS swings), and writing them made popos invisible. Removed.
 * The minimal working set is heading + state/anim/pursue change-only. */
#define ENTITY_OFF_HEADING       0x010
#define ENTITY_OFF_POSITION      0x200
#define ENTITY_OFF_PURSUE_TARGET 0x1C8
#define ENTITY_OFF_ANIM_324      0x324
#define ENTITY_OFF_STATE_334     0x334

#define STATE_LOCOMOTION         5
#define ANIM_WALK_FORWARD        1011

#define ENTITY_REGISTRY_ADDR    0x09C1213Cu
#define ENTITY_REGISTRY_SLOTS   21

typedef struct {
    uint32_t entity_ptr;    /* 0 = empty */
    int      reg_slot;      /* most recently observed registry slot */
} tracked_popo_t;

static tracked_popo_t g_popos[MAX_TRACKED_POPOS];
static volatile int   g_active = 0;
static SceUID         g_worker_uid = -1;

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

static int entity_still_alive(uint32_t entity_ptr, int *out_slot)
{
    for (int slot = 1; slot < ENTITY_REGISTRY_SLOTS; slot++) {
        uint32_t p = *(volatile uint32_t *)(ENTITY_REGISTRY_ADDR + slot * 4);
        if (p == entity_ptr) {
            if (out_slot) *out_slot = slot;
            return 1;
        }
    }
    return 0;
}

static void prune_dead_popos(void)
{
    for (int i = 0; i < MAX_TRACKED_POPOS; i++) {
        if (g_popos[i].entity_ptr == 0) continue;
        int slot;
        if (!entity_still_alive(g_popos[i].entity_ptr, &slot)) {
            g_popos[i].entity_ptr = 0;
            g_popos[i].reg_slot   = 0;
        } else {
            g_popos[i].reg_slot = slot;
        }
    }
}

/* Aim shooter toward target via heading + alarmed-state cells. See the
 * embedded mod in framework.c for the design rationale; this is the
 * canonical-reference standalone copy of the same logic. */
static void aim_at(uint32_t shooter_ptr, uint32_t target_ptr)
{
    union { uint32_t u; float f; } sx, sz, tx, tz, ux, uy, uz;
    sx.u = *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_POSITION + 0);
    sz.u = *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_POSITION + 8);
    tx.u = *(volatile uint32_t *)(target_ptr  + ENTITY_OFF_POSITION + 0);
    tz.u = *(volatile uint32_t *)(target_ptr  + ENTITY_OFF_POSITION + 8);
    float dx = tx.f - sx.f;
    float dz = tz.f - sz.f;
    float magsq = dx * dx + dz * dz;
    if (magsq >= 1.0f) {
        float invmag = 1.0f / __builtin_sqrtf(magsq);
        ux.f = dx * invmag;
        uy.f = 0.0f;
        uz.f = dz * invmag;
        *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_HEADING + 0) = ux.u;
        *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_HEADING + 4) = uy.u;
        *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_HEADING + 8) = uz.u;
    }
    uint16_t cur_state = *(volatile uint16_t *)(shooter_ptr + ENTITY_OFF_STATE_334);
    if (cur_state != STATE_LOCOMOTION)
        *(volatile uint16_t *)(shooter_ptr + ENTITY_OFF_STATE_334) = STATE_LOCOMOTION;
    uint16_t cur_anim = *(volatile uint16_t *)(shooter_ptr + ENTITY_OFF_ANIM_324);
    if (cur_anim != ANIM_WALK_FORWARD)
        *(volatile uint16_t *)(shooter_ptr + ENTITY_OFF_ANIM_324) = ANIM_WALK_FORWARD;
    uint32_t cur_pursue = *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_PURSUE_TARGET);
    if (cur_pursue != 0)
        *(volatile uint32_t *)(shooter_ptr + ENTITY_OFF_PURSUE_TARGET) = 0;
    (void)target_ptr;
}

static void sort_by_slot(uint32_t *out_ptrs, int *out_n)
{
    int n = 0;
    for (int i = 0; i < MAX_TRACKED_POPOS; i++)
        if (g_popos[i].entity_ptr != 0) out_ptrs[n++] = g_popos[i].entity_ptr;
    *out_n = n;
    for (int i = 1; i < n; i++) {
        uint32_t p = out_ptrs[i];
        int slot_p = 0;
        entity_still_alive(p, &slot_p);
        int j = i;
        while (j > 0) {
            int slot_prev = 0;
            entity_still_alive(out_ptrs[j - 1], &slot_prev);
            if (slot_prev <= slot_p) break;
            out_ptrs[j] = out_ptrs[j - 1];
            j--;
        }
        out_ptrs[j] = p;
    }
}

static int aggression_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(TICK_MS * 1000);
        if (!g_active) continue;
        prune_dead_popos();
        uint32_t ptrs[MAX_TRACKED_POPOS];
        int n = 0;
        sort_by_slot(ptrs, &n);
        if (n < 2) continue;
        for (int i = 0; i < n; i++) {
            aim_at(ptrs[i], ptrs[(i + 1) % n]);
        }
    }
    return 0;
}

static void on_section(const mhfu_map_section_ctx_t *ctx)
{
    int now_target = (ctx->section_id      == POPO_TARGET_AREA_INDEX);
    int was_target = (ctx->prev_section_id == POPO_TARGET_AREA_INDEX);
    if (now_target && !was_target) {
        g_active = 1;
        mhfu_log("[popo_agg] ACTIVATE  section=%u (prev=%u)",
                 (unsigned)ctx->section_id, (unsigned)ctx->prev_section_id);
    } else if (was_target && !now_target) {
        g_active = 0;
        for (int i = 0; i < MAX_TRACKED_POPOS; i++) {
            g_popos[i].entity_ptr = 0;
            g_popos[i].reg_slot   = 0;
        }
        mhfu_log("[popo_agg] DEACTIVATE  left=%u now=%u",
                 (unsigned)ctx->prev_section_id, (unsigned)ctx->section_id);
    }
}

static void on_monster(const mhfu_monster_spawn_ctx_t *ctx)
{
    if (ctx->monster_type != POPO_MONSTER_TYPE) return;
    if (find_popo_slot(ctx->entity_ptr) >= 0) return;
    int idx = find_free_popo_slot();
    if (idx < 0) return;
    g_popos[idx].entity_ptr = ctx->entity_ptr;
    g_popos[idx].reg_slot   = ctx->slot;
    mhfu_log("[popo_agg] tracking popo @ 0x%08lx (reg slot %d)",
             (unsigned long)ctx->entity_ptr, ctx->slot);
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    sceKernelDelayThread(2 * 1000 * 1000);   /* let framework settle */
    mhfu_on_map_section_entered(on_section);
    mhfu_on_monster_spawned(on_monster);
    mhfu_log("[popo_agg] registered: section + monster-spawn callbacks");

#ifndef POPO_AGGRESSION_NO_WORKER
    g_worker_uid = sceKernelCreateThread(
        "popo_aggression_worker", aggression_worker,
        0x18, 0x1000, 0, NULL);
    if (g_worker_uid >= 0) {
        sceKernelStartThread(g_worker_uid, 0, NULL);
        mhfu_log("[popo_agg] worker thread started (uid=0x%08lx)",
                 (unsigned long)g_worker_uid);
    } else {
        mhfu_log("[popo_agg] worker thread create failed: %d", g_worker_uid);
    }
#endif
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    g_active = 0;
    mhfu_unregister_event(MHFU_EVENT_MAP_SECTION_ENTERED,
                          (mhfu_event_cb_t)(void *)on_section);
    mhfu_unregister_event(MHFU_EVENT_MONSTER_SPAWNED,
                          (mhfu_event_cb_t)(void *)on_monster);
    return 0;
}
