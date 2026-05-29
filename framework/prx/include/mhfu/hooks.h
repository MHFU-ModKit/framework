/*
 * Hook arbitration — the framework's "Harmony" layer.
 *
 * Two kinds of hook:
 *   • Event hooks (fan-out): many mods may hook the same event; all run,
 *     ordered by priority (higher first, ties in registration order).
 *   • Exclusive patches: a function-entry redirect, a vtable-slot swap,
 *     or a single code-word patch claims ONE address/slot. A second mod
 *     claiming the same target gets MHFU_HOOK_CONFLICT and is refused —
 *     the framework never lets two mods silently fight over one patch.
 *
 * The framework records the owning mod id for every exclusive patch and
 * restores them centrally on mod shutdown (mhfu_unhook_owner). Mods
 * therefore do NOT save/restore original bytes themselves.
 *
 * Code-word / function patches route through a CWCheat-style ranged
 * cache invalidate (the only PPSSPP path that drops a stale JIT block);
 * to beat the JIT pre-cache, claim them while screen_state is TITLE/MENU
 * (see docs/FRAMEWORK_ARCHITECTURE.md, "JIT pre-cache bypass").
 */
#ifndef MHFU_HOOKS_H
#define MHFU_HOOKS_H

#include <stdint.h>
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MHFU_HOOK_OK       =  0,
    MHFU_HOOK_CONFLICT = -1,   /* target already owned by another mod */
    MHFU_HOOK_NOSPACE  = -2,   /* owner table full */
    MHFU_HOOK_BADARG   = -3,
} mhfu_hook_rc_t;

/* Event fan-out with explicit priority (higher runs first). The plain
 * mhfu_register_event / mhfu_on_* sugar in events.h is priority 0. */
mhfu_hook_rc_t mhfu_hook_event(mhfu_event_id_t id, mhfu_event_cb_t cb, int priority);

/* Exclusive: redirect a function entry to `stub` (the framework writes a
 * `J stub; NOP` and centralizes restore). `stub` must be branchless,
 * single-basic-block (see docs — PPSSPP JIT freezes on short branch
 * blocks). owner = your mod id string. */
mhfu_hook_rc_t mhfu_hook_function(uint32_t addr, uint32_t stub, const char *owner);

/* Exclusive: swap a vtable slot (a data write — JIT-immune). */
mhfu_hook_rc_t mhfu_hook_vtable(uint32_t slot_addr, uint32_t fn, const char *owner);

/* Exclusive: patch a single code word with ranged cache invalidate. */
mhfu_hook_rc_t mhfu_patch_word(uint32_t addr, uint32_t word, const char *owner);

/* Restore every exclusive patch owned by `owner` (called by the
 * framework on mod shutdown; mods rarely call it directly). */
void mhfu_unhook_owner(const char *owner);

/* Who owns this address/slot right now? Returns the owner id, or NULL. */
const char *mhfu_hook_owner_of(uint32_t addr);

/* Flush dcache (writeback+invalidate) then icache — call after writing a
 * stub buffer so the CPU executes the freshly written bytes as code.
 * mhfu_patch_word / mhfu_hook_* already invalidate the patched word; this
 * is for the mod-owned stub memory those patches jump to. */
void mhfu_flush_caches(void);

/* ---- ergonomic install helpers (no hand-written MIPS / no gate boilerplate) ---- */

/* Like mhfu_patch_word, but DEFERRED: the framework applies it only once
 * screen_state is JIT-quiet (TITLE/MENU) AND the word at addr still equals
 * `expect` — beating PPSSPP's JIT pre-cache (Section 26). Returns OK once
 * queued; the patch lands later. Restored on owner shutdown like any patch. */
mhfu_hook_rc_t mhfu_patch_word_when_quiet(uint32_t addr, uint32_t expect,
                                          uint32_t word, const char *owner);

#define MHFU_WRAP_PREFIX  0   /* helper runs BEFORE the original */
#define MHFU_WRAP_POSTFIX 1   /* helper runs AFTER the original */

/* Wrap a `jal orig_target` call site: the framework builds a wrapper stub
 * (in its code cave) that calls helper(<the call's $a0>) prefix/postfix
 * around orig_target, then deferred-patches the site to call the stub.
 * Replaces hand-encoded stubs + the screen gate. owner = your mod id. */
mhfu_hook_rc_t mhfu_install_call_wrapper(uint32_t call_site, uint32_t orig_target,
                                         void (*helper)(uint32_t), int mode,
                                         const char *owner);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MHFU_HOOKS_H */
