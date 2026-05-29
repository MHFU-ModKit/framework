/*
 * Event registry + dispatch + monster-spawn polling.
 *
 * Callbacks fan out in priority order (higher first; ties keep
 * registration order). The quest/section dispatchers are invoked from
 * the trampoline wrappers (trampoline.cpp); monster-spawn has no anchor
 * instruction, so a poll thread watches the entity registry.
 */
#include <pspthreadman.h>

#include "mhfu/events.h"
#include "mhfu/memory.h"
#include "mhfu/entity.h"
#include "internal.h"

#define MAX_CBS_PER_EVENT 16

typedef struct {
    mhfu_event_cb_t cb;
    int             priority;
} cb_entry_t;

static cb_entry_t g_cbs[MHFU_EVENT_COUNT_][MAX_CBS_PER_EVENT];
static int        g_n_cbs[MHFU_EVENT_COUNT_];

/* --- registration ------------------------------------------------------- */

extern "C" int mhfu_register_event_pri(mhfu_event_id_t id, mhfu_event_cb_t cb, int priority)
{
    if (id < 0 || id >= MHFU_EVENT_COUNT_) return -1;
    if (!cb) return -2;
    int *n = &g_n_cbs[id];
    if (*n >= MAX_CBS_PER_EVENT) return -3;
    /* insert keeping priority descending, stable for equal priority */
    int pos = *n;
    while (pos > 0 && g_cbs[id][pos - 1].priority < priority) {
        g_cbs[id][pos] = g_cbs[id][pos - 1];
        pos--;
    }
    g_cbs[id][pos].cb = cb;
    g_cbs[id][pos].priority = priority;
    (*n)++;
    return 0;
}

extern "C" int mhfu_register_event(mhfu_event_id_t id, mhfu_event_cb_t cb)
{
    return mhfu_register_event_pri(id, cb, 0);
}

extern "C" int mhfu_unregister_event(mhfu_event_id_t id, mhfu_event_cb_t cb)
{
    if (id < 0 || id >= MHFU_EVENT_COUNT_) return -1;
    int *n = &g_n_cbs[id];
    for (int i = 0; i < *n; i++) {
        if (g_cbs[id][i].cb == cb) {
            for (int j = i; j < *n - 1; j++) g_cbs[id][j] = g_cbs[id][j + 1];
            (*n)--;
            return 0;
        }
    }
    return -4;
}

extern "C" int mhfu_registry_count(mhfu_event_id_t id)
{
    if (id < 0 || id >= MHFU_EVENT_COUNT_) return 0;
    return g_n_cbs[id];
}

extern "C" void mhfu_registry_fire(mhfu_event_id_t id, const void *ctx)
{
    if (id < 0 || id >= MHFU_EVENT_COUNT_) return;
    int n = g_n_cbs[id];
    for (int i = 0; i < n; i++) g_cbs[id][i].cb(ctx);
}

/* --- quest / section dispatch (called by the trampoline wrappers) ------- */

static uint32_t g_last_quest_timer = 0;
static uint16_t g_last_area_index  = 0;

static void fill_regs(mhfu_event_ctx_t *c, mhfu_event_id_t id,
                      uint32_t cell, const mhfu_anchor_regs_t *r)
{
    c->event_id = id;  c->cell_value = cell;
    c->a0 = r->a0; c->a1 = r->a1; c->a2 = r->a2; c->a3 = r->a3;
    c->v0 = r->v0; c->v1 = r->v1; c->ra = r->ra; c->sp = r->sp; c->pc = r->pc;
}

extern "C" void mhfu_dispatch_quest_beginning(const mhfu_anchor_regs_t *regs)
{
    uint32_t cur  = mhfu_get_quest_timer();
    uint32_t prev = g_last_quest_timer;
    g_last_quest_timer = cur;
    /* fire once on the 0 -> non-zero transition (quest commit) */
    if (prev != 0 || cur == 0) return;

    mhfu_event_ctx_t ctx;
    fill_regs(&ctx, MHFU_EVENT_QUEST_BEGINNING, cur, regs);
    mhfu_registry_fire(MHFU_EVENT_QUEST_BEGINNING, &ctx);
}

extern "C" void mhfu_dispatch_quest_entered(const mhfu_anchor_regs_t *regs)
{
    uint16_t cur  = mhfu_get_area_index();
    uint16_t prev = g_last_area_index;
    if (cur == prev) return;
    g_last_area_index = cur;

    /* map-section change fires on EVERY transition */
    {
        mhfu_map_section_ctx_t m;
        m.event_id         = MHFU_EVENT_MAP_SECTION_ENTERED;
        m.section_id       = cur;
        m.prev_section_id  = prev;
        m.quest_timer      = mhfu_get_quest_timer();
        m.screen_state     = mhfu_get_screen_state();
        m.is_in_quest_area = (m.screen_state == 17) ? 1 : 0;
        m._pad             = 0;
        mhfu_registry_fire(MHFU_EVENT_MAP_SECTION_ENTERED, &m);
    }

    /* quest-entered only when transitioning TO 98 (in-area) */
    if (cur != 98) return;
    mhfu_event_ctx_t ctx;
    fill_regs(&ctx, MHFU_EVENT_QUEST_ENTERED, cur, regs);
    mhfu_registry_fire(MHFU_EVENT_QUEST_ENTERED, &ctx);
}

/* --- monster-spawn poll ------------------------------------------------- */

static uint32_t g_last_entity_ptrs[MHFU_ENTITY_REGISTRY_SLOTS];

static void dispatch_monster_spawned(int slot, uint32_t ent)
{
    if (mhfu_registry_count(MHFU_EVENT_MONSTER_SPAWNED) == 0) return;
    mhfu_monster_spawn_ctx_t c;
    c.event_id     = MHFU_EVENT_MONSTER_SPAWNED;
    c.slot         = slot;
    c.entity_ptr   = ent;
    c.monster_type = mhfu_entity_type(ent);
    c.entity_id    = *(volatile uint8_t *)(ent + MHFU_ENT_ENTITY_ID);
    c.hp           = mhfu_entity_hp(ent);
    c.size_scale   = mhfu_entity_size(ent);
    mhfu_registry_fire(MHFU_EVENT_MONSTER_SPAWNED, &c);
}

extern "C" int mhfu_monster_spawn_poll_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        sceKernelDelayThread(200 * 1000);   /* 5 Hz */
        for (int slot = 1; slot < MHFU_ENTITY_REGISTRY_SLOTS; slot++) {
            uint32_t cur  = mhfu_entity_at(slot);
            uint32_t prev = g_last_entity_ptrs[slot];
            if (cur == 0) {
                if (prev != 0) g_last_entity_ptrs[slot] = 0;
                continue;
            }
            if (cur == prev) continue;
            if (cur < 0x08000000u || cur > 0x0A000000u) continue;
            g_last_entity_ptrs[slot] = cur;
            dispatch_monster_spawned(slot, cur);
        }
    }
    return 0;
}
