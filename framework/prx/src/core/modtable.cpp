/*
 * Static mod table.
 *
 * Every MHFU_MOD() descriptor lands in the 'mhfu_mods' linker section;
 * GNU ld bookends it with __start_mhfu_mods / __stop_mhfu_mods. We walk
 * that table, resolve `needs` / `conflicts`, and call each init() in
 * dependency order. On shutdown we run shutdown() in reverse and restore
 * the mod's hooks centrally.
 */
#include <string.h>

#include "mhfu/mod.h"
#include "mhfu/hooks.h"
#include "mhfu/log.h"
#include "internal.h"

extern "C" {
extern const mhfu_mod_t __start_mhfu_mods[];
extern const mhfu_mod_t __stop_mhfu_mods[];
}

#define MAX_MODS 32

enum mod_state { MS_PENDING = 0, MS_ACTIVE, MS_REFUSED };

static const mhfu_mod_t *g_mods[MAX_MODS];
static uint8_t           g_state[MAX_MODS];
static int               g_n_mods = 0;
static const mhfu_mod_t *g_init_order[MAX_MODS];
static int               g_n_init = 0;

/* True if `id` appears as a whitespace-separated token in `list`. */
static int list_has(const char *list, const char *id)
{
    if (!list || !id) return 0;
    size_t idlen = strlen(id);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t toklen = (size_t)(p - start);
        if (toklen == idlen && strncmp(start, id, idlen) == 0) return 1;
    }
    return 0;
}

static int find_mod(const char *id)
{
    for (int i = 0; i < g_n_mods; i++)
        if (g_mods[i]->id && strcmp(g_mods[i]->id, id) == 0) return i;
    return -1;
}

/* Does any already-active mod declare `id` as a conflict, or does `id`
 * conflict an active mod? (symmetric check) */
static int conflicts_active(const mhfu_mod_t *m)
{
    for (int i = 0; i < g_n_mods; i++) {
        if (g_state[i] != MS_ACTIVE) continue;
        const mhfu_mod_t *o = g_mods[i];
        if (list_has(m->conflicts, o->id)) return 1;
        if (list_has(o->conflicts, m->id)) return 1;
    }
    return 0;
}

/* All `needs` satisfied (each named mod is active)? Returns 0 if a needed
 * mod is REFUSED/absent (unsatisfiable), 1 if all active, -1 if still
 * pending (a needed mod hasn't been tried yet). */
static int needs_ready(const mhfu_mod_t *m)
{
    if (!m->needs || !*m->needs) return 1;
    const char *p = m->needs;
    int all = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (len == 0) break;
        char id[64];
        if (len >= sizeof(id)) len = sizeof(id) - 1;
        memcpy(id, start, len); id[len] = 0;
        int idx = find_mod(id);
        if (idx < 0 || g_state[idx] == MS_REFUSED) return 0;  /* unsatisfiable */
        if (g_state[idx] != MS_ACTIVE) all = -1;              /* not yet */
    }
    return all;
}

extern "C" void mhfu_mods_init_all(void)
{
    g_n_mods = 0;
    for (const mhfu_mod_t *m = __start_mhfu_mods;
         m < __stop_mhfu_mods && g_n_mods < MAX_MODS; m++) {
        if (!m->id) continue;
        g_mods[g_n_mods] = m;
        g_state[g_n_mods] = MS_PENDING;
        g_n_mods++;
    }
    mhfu_log("[mods] %d descriptor(s) found", g_n_mods);

    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < g_n_mods; i++) {
            if (g_state[i] != MS_PENDING) continue;
            const mhfu_mod_t *m = g_mods[i];
            if (conflicts_active(m)) {
                g_state[i] = MS_REFUSED;
                mhfu_log("[mods] '%s' REFUSED (conflict)", m->id);
                progress = 1;
                continue;
            }
            int nr = needs_ready(m);
            if (nr == 0) {
                g_state[i] = MS_REFUSED;
                mhfu_log("[mods] '%s' REFUSED (unmet dependency)", m->id);
                progress = 1;
                continue;
            }
            if (nr < 0) continue;   /* deps pending — retry next pass */
            int rc = m->init ? m->init() : 0;
            if (rc == 0) {
                g_state[i] = MS_ACTIVE;
                g_init_order[g_n_init++] = m;
                mhfu_log("[mods] '%s' v%s active", m->id,
                         m->version ? m->version : "?");
            } else {
                g_state[i] = MS_REFUSED;
                mhfu_log("[mods] '%s' init failed rc=%d", m->id, rc);
            }
            progress = 1;
        }
    }
    for (int i = 0; i < g_n_mods; i++)
        if (g_state[i] == MS_PENDING)
            mhfu_log("[mods] '%s' SKIPPED (dependency cycle / never ready)",
                     g_mods[i]->id);
}

extern "C" void mhfu_mods_shutdown_all(void)
{
    for (int i = g_n_init - 1; i >= 0; i--) {
        const mhfu_mod_t *m = g_init_order[i];
        if (m->shutdown) m->shutdown();
        mhfu_unhook_owner(m->id);
    }
    g_n_init = 0;
}
