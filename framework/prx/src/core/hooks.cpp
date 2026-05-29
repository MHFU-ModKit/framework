/*
 * Hook arbitration — the framework's "Harmony" layer (see hooks.h).
 *
 * Event hooks fan out (delegated to the registry). Exclusive patches
 * (function-entry redirect / vtable-slot swap / code-word) each claim ONE
 * address; a second claimant is refused with MHFU_HOOK_CONFLICT. The
 * owning mod id + original bytes are recorded so the framework restores
 * everything centrally on mod shutdown (mhfu_unhook_owner) — mods never
 * save/restore originals themselves.
 */
#include <pspsdk.h>
#include <psputils.h>
#include <string.h>

#include "mhfu/hooks.h"
#include "mhfu/mips.h"
#include "mhfu/log.h"
#include "internal.h"

/* registry.cpp */
extern "C" int mhfu_register_event_pri(mhfu_event_id_t, mhfu_event_cb_t, int);

#define MAX_HOOKS    32
#define OWNER_LEN    32

enum hook_kind { HK_FREE = 0, HK_FUNCTION, HK_VTABLE, HK_WORD };

typedef struct {
    uint8_t  kind;
    uint32_t addr;          /* patched address (function entry / vtable slot / word) */
    uint32_t orig;          /* original word/ptr, for restore */
    char     owner[OWNER_LEN];
} hook_record_t;

static hook_record_t g_hooks[MAX_HOOKS];

extern "C" void mhfu_hookmgr_init(void)
{
    memset(g_hooks, 0, sizeof(g_hooks));
}

static hook_record_t *find_by_addr(uint32_t addr)
{
    for (int i = 0; i < MAX_HOOKS; i++)
        if (g_hooks[i].kind != HK_FREE && g_hooks[i].addr == addr)
            return &g_hooks[i];
    return 0;
}

static hook_record_t *alloc_record(void)
{
    for (int i = 0; i < MAX_HOOKS; i++)
        if (g_hooks[i].kind == HK_FREE) return &g_hooks[i];
    return 0;
}

static mhfu_hook_rc_t claim(uint32_t addr, uint8_t kind, const char *owner,
                            hook_record_t **out)
{
    if (!owner) return MHFU_HOOK_BADARG;
    hook_record_t *ex = find_by_addr(addr);
    if (ex) {
        if (strcmp(ex->owner, owner) == 0) { *out = ex; return MHFU_HOOK_OK; }
        mhfu_log("[hook] CONFLICT @0x%08lx: '%s' wants it, owned by '%s'",
                 (unsigned long)addr, owner, ex->owner);
        return MHFU_HOOK_CONFLICT;
    }
    hook_record_t *r = alloc_record();
    if (!r) { mhfu_log("[hook] table full, '%s' refused @0x%08lx",
                       owner, (unsigned long)addr); return MHFU_HOOK_NOSPACE; }
    r->kind = kind;
    r->addr = addr;
    r->orig = *(volatile uint32_t *)addr;
    strncpy(r->owner, owner, OWNER_LEN - 1);
    r->owner[OWNER_LEN - 1] = 0;
    *out = r;
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_hook_event(mhfu_event_id_t id, mhfu_event_cb_t cb, int priority)
{
    return mhfu_register_event_pri(id, cb, priority) == 0
         ? MHFU_HOOK_OK : MHFU_HOOK_BADARG;
}

extern "C" mhfu_hook_rc_t mhfu_hook_function(uint32_t addr, uint32_t stub, const char *owner)
{
    hook_record_t *r;
    mhfu_hook_rc_t rc = claim(addr, HK_FUNCTION, owner, &r);
    if (rc != MHFU_HOOK_OK) return rc;
    mhfu_smc_patch_word(addr, mips_j(stub));
    mhfu_log("[hook] '%s' function @0x%08lx -> stub 0x%08lx",
             owner, (unsigned long)addr, (unsigned long)stub);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_hook_vtable(uint32_t slot_addr, uint32_t fn, const char *owner)
{
    hook_record_t *r;
    mhfu_hook_rc_t rc = claim(slot_addr, HK_VTABLE, owner, &r);
    if (rc != MHFU_HOOK_OK) return rc;
    *(volatile uint32_t *)slot_addr = fn;          /* data write — JIT-immune */
    sceKernelDcacheWritebackInvalidateAll();
    mhfu_log("[hook] '%s' vtable @0x%08lx -> 0x%08lx",
             owner, (unsigned long)slot_addr, (unsigned long)fn);
    return MHFU_HOOK_OK;
}

extern "C" mhfu_hook_rc_t mhfu_patch_word(uint32_t addr, uint32_t word, const char *owner)
{
    hook_record_t *r;
    mhfu_hook_rc_t rc = claim(addr, HK_WORD, owner, &r);
    if (rc != MHFU_HOOK_OK) return rc;
    mhfu_smc_patch_word(addr, word);
    mhfu_log("[hook] '%s' word @0x%08lx = 0x%08lx (was 0x%08lx)",
             owner, (unsigned long)addr, (unsigned long)word, (unsigned long)r->orig);
    return MHFU_HOOK_OK;
}

extern "C" const char *mhfu_hook_owner_of(uint32_t addr)
{
    hook_record_t *r = find_by_addr(addr);
    return r ? r->owner : 0;
}

extern "C" void mhfu_unhook_owner(const char *owner)
{
    if (!owner) return;
    for (int i = 0; i < MAX_HOOKS; i++) {
        hook_record_t *r = &g_hooks[i];
        if (r->kind == HK_FREE || strcmp(r->owner, owner) != 0) continue;
        if (r->kind == HK_VTABLE) {
            *(volatile uint32_t *)r->addr = r->orig;
            sceKernelDcacheWritebackInvalidateAll();
        } else {
            mhfu_smc_patch_word(r->addr, r->orig);   /* FUNCTION / WORD */
        }
        mhfu_log("[hook] restored '%s' @0x%08lx -> 0x%08lx",
                 owner, (unsigned long)r->addr, (unsigned long)r->orig);
        r->kind = HK_FREE;
    }
}
