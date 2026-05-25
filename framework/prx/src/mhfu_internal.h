/*
 * Internal contract between the wrapper trampolines and the
 * per-event dispatcher functions.
 *
 * Not part of the public mod API (mhfu_framework.h) — mod authors
 * should never touch mhfu_anchor_regs_t directly. The framework's
 * dispatcher converts it into the public mhfu_event_ctx_t before
 * invoking user callbacks.
 */

#ifndef MHFU_INTERNAL_H
#define MHFU_INTERNAL_H

#include <stdint.h>

typedef struct mhfu_anchor_regs {
    uint32_t a0, a1, a2, a3;
    uint32_t v0, v1;
    uint32_t ra;     /* original $ra at anchor */
    uint32_t sp;     /* original $sp at anchor (pre-wrapper) */
    uint32_t pc;     /* anchor PC (the patched call-site address) */
} mhfu_anchor_regs_t;

/* sizeof(mhfu_anchor_regs_t) MUST equal 36 (9 u32s) — the trampoline
 * builds this layout literally on the stack, offset by offset. */

#endif /* MHFU_INTERNAL_H */
