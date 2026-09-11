/*
 * em_vhook — the two stubs, assembled word by word.
 *
 * Kept apart from mod.cpp so the SAME builders compile on the host
 * (tools/em_vhook_check.py builds them with the host cc, disassembles the words
 * with tools/mips_dis.py and asserts the properties below). Nothing here may
 * touch the PSP SDK: only <stdint.h> and mhfu/mips.h.
 *
 * Properties the check enforces, because they are what keeps the engine alive:
 *   - NO BRANCH in either stub (no beq/bne/b*): every decision is a MOVN/MOVZ
 *     select, so the JIT sees one basic block and armed/unarmed runs execute the
 *     same instruction stream;
 *   - the slot-29 stub never touches $sp: its return address and the step's
 *     arguments are spilled to config words for the one call it may make;
 *   - the slot-32 stub's only stack use is the 16-byte frame v2 already carried
 *     (block comment on build_act_stub in mod.cpp);
 *   - each fits its slot (STUB_AI_INSNS / STUB_ACT_INSNS).
 *
 * The config block layout below is FIXED: the stubs index it by byte offset,
 * and mod.cpp's em_vhook_cfg_t mirrors it field for field (static_asserts).
 */
#ifndef EM_VHOOK_STUBS_H
#define EM_VHOOK_STUBS_H

#include <stdint.h>
#include "mhfu/mips.h"

/* ---- engine anchors (MHFU EU) --------------------------------------------- */
#define EMV_ENTER_DISPATCH 0x09AC89F0u   /* game_task: enter_action(ent, main, id, mode) -> vt+0x88 */
#define EMV_PLAYER_XZ      0x090B3480u   /* player combat entity 0x090B3440 + 0x40: f32 x,y,z */
#define EMV_ENT_POS        0x200         /* entity f32 x,y,z */
#define EMV_ENT_MAIN       0x298
#define EMV_ENT_SUB        0x299

/* ---- config block layout (byte offsets from the block base) --------------- */
#define CFG_WANT_MAIN   0x00   /* u8   v2 budget override: match this main ... */
#define CFG_WANT_SUB    0x01   /* u8   ... and this sub (0xFE = any)            */
#define CFG_ARMED       0x02   /* u8                                             */
#define CFG_ARM29       0x03   /* u8   1 = the slot-29 one-shot owns +0x414 too */
#define CFG_PATCH_OFF   0x04   /* u32                                            */
#define CFG_PATCH_VAL   0x08   /* u32                                            */
#define CFG_SINK        0x0C   /* u32  harmless store target when unmatched     */
#define CFG_AI_TICKS    0x10   /* u32  slot-29 dispatches                        */
#define CFG_ACT_ENTER   0x14   /* u32  slot-32 dispatches                        */
#define CFG_LAST_PAIR   0x18   /* u32  (main<<8)|sub last seen at enter-action  */
#define CFG_CANARY      0x1C   /* u32                                            */
#define CFG_PREV        0x20   /* u32  pair at the previous slot-29 tick        */
#define CFG_PREV2       0x24   /* u32  ... and the one before                   */
/* v3 */
#define CFG_FRAMES      0x28   /* u32  ticks the live pair has stood (0 = first) */
#define CFG_RA_SPILL    0x2C   /* u32  slot-29 stub's ra across its call         */
#define CFG_A0_SPILL    0x30   /* u32  the step's a0..a3 across the call         */
#define CFG_A1_SPILL    0x34
#define CFG_A2_SPILL    0x38
#define CFG_A3_SPILL    0x3C
#define CFG_D2          0x40   /* f32 bits  player<->monster XZ distance^2       */
#define CFG_D2_PREV     0x44   /* f32 bits  ... at the previous tick            */
#define CFG_BRAIN_FIRES 0x48   /* u32  rule fires, total                        */
#define CFG_SCRATCH     0x4C   /* u32  the match bit across a call              */
/* request slot */
#define CFG_REQ_PENDING 0x50   /* u32  1 = issue on the next frame              */
#define CFG_REQ_MAIN    0x54   /* u8 */
#define CFG_REQ_SUB     0x55   /* u8 */
#define CFG_REQ_MODE    0x56   /* u8 */
#define CFG_REQ_DONE    0x58   /* u32  requests issued                          */
#define CFG_REQ_RESULT  0x5C   /* u32  (main<<8)|sub read back after the call   */
/* substitution table: EM_VHOOK_SUBS entries of 8 bytes */
#define CFG_SUB_BASE    0x60
#define SUB_STRIDE      0x08
#define SUB_MASK        0x00   /* u8   bit k = main k eligible; 0 = entry off  */
#define SUB_FROM_SUB    0x01   /* u8   exact id, or 0xFE any                    */
#define SUB_TO_MAIN     0x02   /* u8 */
#define SUB_TO_SUB      0x03   /* u8 */
#define SUB_LEFT        0x04   /* u32  substitutions left; 0 = off              */
#define CFG_SUB_HITS    0x80   /* u32  incoming calls rewritten                 */
#define CFG_SUB_LANDED  0x84   /* u32  ... whose cells read our pair afterwards */
#define CFG_SUB_LAST_IN 0x88   /* u32  (mode<<16)|(main<<8)|sub last rewritten  */
#define CFG_SUB_PENDING 0x8C   /* u32  pre part -> post part: did we rewrite    */
#define CFG_SUB_TO_PEND 0x90   /* u32  (to_main<<8)|to_sub of that rewrite      */
/* enter-action ring */
#define CFG_RING_IDX    0x94   /* u32 */
#define CFG_RING        0x98   /* u32[8]  (subst<<24)|(mode<<16)|(main<<8)|sub  */
/* rules: EM_VHOOK_RULES entries of 0x28 bytes */
#define CFG_RULE_BASE   0xB8
#define RULE_STRIDE     0x28
#define RULE_MASK       0x00   /* u8 */
#define RULE_FROM_SUB   0x01   /* u8 */
#define RULE_TO_MAIN    0x02   /* u8 */
#define RULE_TO_SUB     0x03   /* u8 */
#define RULE_MODE       0x04   /* u8 */
#define RULE_FLAGS      0x05   /* u8   1 receding, 2 closing                    */
#define RULE_MIN_FRAMES 0x08   /* u32 */
#define RULE_D2_LO      0x0C   /* f32 bits */
#define RULE_D2_HI      0x10   /* f32 bits */
#define RULE_LEFT       0x14   /* u32  0 = off */
#define RULE_FIRED      0x18   /* u32 */
#define RULE_LAST_FIRE  0x1C   /* u32  ai_ticks at the last fire */
#define RULE_COOLDOWN   0x20   /* u32  frames */
#define CFG_SIZE        (CFG_RULE_BASE + 4 * RULE_STRIDE)   /* 0x158 */

#define SUB_ANY         0xFEu
#define PATCH_OFF_MASK  0x7FCu   /* the entity is 0x800 bytes: bound + align every store */

/* slot sizes, in instructions. The check asserts the builders fit. */
#define STUB_AI_INSNS   480
#define STUB_ACT_INSNS  200

/* -------------------------------------------------------------------------- */
/* register aliases used below */
#define R_ZERO MIPS_REG_ZERO
#define R_A0 MIPS_REG_A0
#define R_A1 MIPS_REG_A1
#define R_A2 MIPS_REG_A2
#define R_A3 MIPS_REG_A3
#define R_T0 MIPS_REG_T0
#define R_T1 MIPS_REG_T1
#define R_T2 MIPS_REG_T2
#define R_T3 MIPS_REG_T3
#define R_T4 MIPS_REG_T4
#define R_T5 MIPS_REG_T5
#define R_T6 MIPS_REG_T6
#define R_T7 MIPS_REG_T7
#define R_T8 MIPS_REG_T8
#define R_T9 MIPS_REG_T9
#define R_SP MIPS_REG_SP
#define R_RA MIPS_REG_RA

typedef struct {
    uint32_t *s;
    int i, cap;
    int overflow;
} emv_asm_t;

static inline void emv_emit(emv_asm_t *a, uint32_t w)
{
    if (a->i < a->cap) a->s[a->i] = w;
    else a->overflow = 1;
    a->i++;
}
#define E(w) emv_emit(a, (w))

/* t7 = cfg (two instructions; needed again after every call, which clobbers it) */
static inline void emv_load_cfg(emv_asm_t *a, uint32_t cfg)
{
    E(mips_lui(R_T7, (uint16_t)(cfg >> 16)));
    E(mips_ori(R_T7, R_T7, (uint16_t)cfg));
}

/* t2 = 1 iff (mask >> main) & 1 and (from_sub == sub or from_sub == ANY),
 * with main in t0, sub in t1 and the entry's bytes at off_mask/off_sub off t7.
 * Clobbers t3, t4. */
static inline void emv_pair_match(emv_asm_t *a, int off_mask, int off_sub)
{
    E(mips_lbu(R_T2, (int16_t)off_mask, R_T7));
    E(mips_srlv(R_T2, R_T2, R_T0));
    E(mips_andi(R_T2, R_T2, 1));                       /* main eligible */
    E(mips_lbu(R_T3, (int16_t)off_sub, R_T7));
    E(mips_xor(R_T4, R_T3, R_T1));
    E(mips_sltiu(R_T4, R_T4, 1));                      /* 1 iff sub == from_sub */
    E(mips_xori(R_T3, R_T3, SUB_ANY));
    E(mips_sltiu(R_T3, R_T3, 1));                      /* 1 iff wildcard */
    E(mips_or(R_T4, R_T4, R_T3));
    E(mips_and(R_T2, R_T2, R_T4));
}

/* A CONDITIONAL CALL WITHOUT A BRANCH. t2 holds the 0/1 decision; a0 is the
 * entity, a1..a3 the enter-action's (main, id, mode). The callee is chosen with
 * MOVN between the engine's dispatcher and a two-instruction `jr ra` in our own
 * block, so the jalr always executes and the instruction stream never forks.
 * ra was spilled to CFG_RA_SPILL by the stub prologue. Afterwards t7 and a0 are
 * reloaded (the callee clobbers every caller-saved register) and t2 is restored
 * from CFG_SCRATCH so the caller can keep using the decision. */
static inline void emv_cond_call(emv_asm_t *a, uint32_t cfg, uint32_t ret_stub)
{
    E(mips_sw(R_T2, CFG_SCRATCH, R_T7));
    E(mips_lui(R_T9, (uint16_t)(ret_stub >> 16)));
    E(mips_ori(R_T9, R_T9, (uint16_t)ret_stub));
    E(mips_lui(R_T6, (uint16_t)(EMV_ENTER_DISPATCH >> 16)));
    E(mips_ori(R_T6, R_T6, (uint16_t)EMV_ENTER_DISPATCH));
    E(mips_movn(R_T9, R_T6, R_T2));
    E(mips_jalr(R_T9));
    E(MIPS_NOP);
    emv_load_cfg(a, cfg);
    E(mips_lw(R_A0, CFG_A0_SPILL, R_T7));
    E(mips_lw(R_T2, CFG_SCRATCH, R_T7));
    /* a fire this frame means the pair just changed: its dwell restarts */
    E(mips_lw(R_T5, CFG_FRAMES, R_T7));
    E(mips_movn(R_T5, R_ZERO, R_T2));
    E(mips_sw(R_T5, CFG_FRAMES, R_T7));
}

/* --- slot 29: the per-frame pre-hook. ------------------------------------- */
static inline int emv_build_ai_stub(uint32_t *out, int cap, uint32_t cfg,
                                    uint32_t original, uint32_t ret_stub,
                                    int *overflow)
{
    emv_asm_t A = { out, 0, cap, 0 };
    emv_asm_t *a = &A;

    emv_load_cfg(a, cfg);
    E(mips_lw(R_T0, CFG_AI_TICKS, R_T7));
    E(mips_addiu(R_T0, R_T0, 1));
    E(mips_sw(R_T0, CFG_AI_TICKS, R_T7));

    /* cur = (main<<8)|sub off the entity; a0 is the entity, nothing ran yet */
    E(mips_lbu(R_T0, EMV_ENT_MAIN, R_A0));
    E(mips_lbu(R_T1, EMV_ENT_SUB,  R_A0));
    E(mips_sll(R_T8, R_T0, 8));
    E(mips_or(R_T8, R_T8, R_T1));

    /* frames = (cur == prev) ? frames + 1 : 0 */
    E(mips_lw(R_T4, CFG_PREV, R_T7));
    E(mips_xor(R_T6, R_T4, R_T8));
    E(mips_sltiu(R_T6, R_T6, 1));                     /* 1 iff same pair */
    E(mips_lw(R_T5, CFG_FRAMES, R_T7));
    E(mips_addiu(R_T5, R_T5, 1));
    E(mips_movz(R_T5, R_ZERO, R_T6));                  /* changed -> 0 */
    E(mips_sw(R_T5, CFG_FRAMES, R_T7));

    /* d2 = (mx-px)^2 + (mz-pz)^2, kept as raw f32 bits: non-negative floats
     * order like unsigned ints, so the rules compare with SLTU. f0..f3 are
     * caller-saved and we are at the step's entry, so they are free. */
    E(mips_lui(R_T2, (uint16_t)(EMV_PLAYER_XZ >> 16)));
    E(mips_ori(R_T2, R_T2, (uint16_t)EMV_PLAYER_XZ));
    E(mips_lwc1(0, EMV_ENT_POS,     R_A0));
    E(mips_lwc1(1, EMV_ENT_POS + 8, R_A0));
    E(mips_lwc1(2, 0, R_T2));
    E(mips_lwc1(3, 8, R_T2));
    E(mips_sub_s(0, 0, 2));
    E(mips_sub_s(1, 1, 3));
    E(mips_mul_s(0, 0, 0));
    E(mips_mul_s(1, 1, 1));
    E(mips_add_s(0, 0, 1));
    E(mips_lw(R_T3, CFG_D2, R_T7));
    E(mips_sw(R_T3, CFG_D2_PREV, R_T7));
    E(mips_mfc1(R_T3, 0));
    E(mips_sw(R_T3, CFG_D2, R_T7));

    /* spill what the calls below may clobber: the step's args and our ra */
    E(mips_sw(R_A0, CFG_A0_SPILL, R_T7));
    E(mips_sw(R_A1, CFG_A1_SPILL, R_T7));
    E(mips_sw(R_A2, CFG_A2_SPILL, R_T7));
    E(mips_sw(R_A3, CFG_A3_SPILL, R_T7));
    E(mips_sw(R_RA, CFG_RA_SPILL, R_T7));

    /* ---- the REQUEST: a pair Lua asked for, entered on the game thread ---- */
    E(mips_lw(R_T2, CFG_REQ_PENDING, R_T7));
    E(mips_sltu(R_T2, R_ZERO, R_T2));                  /* 1 iff pending */
    E(mips_lw(R_T3, CFG_REQ_PENDING, R_T7));
    E(mips_subu(R_T3, R_T3, R_T2));                    /* consume it (a write that
                                                          races in from Lua after
                                                          our read survives) */
    E(mips_sw(R_T3, CFG_REQ_PENDING, R_T7));
    E(mips_lw(R_T3, CFG_REQ_DONE, R_T7));
    E(mips_addu(R_T3, R_T3, R_T2));
    E(mips_sw(R_T3, CFG_REQ_DONE, R_T7));
    E(mips_lbu(R_A1, CFG_REQ_MAIN, R_T7));
    E(mips_lbu(R_A2, CFG_REQ_SUB,  R_T7));
    E(mips_lbu(R_A3, CFG_REQ_MODE, R_T7));
    emv_cond_call(a, cfg, ret_stub);
    /* what the cells say now, for Lua to judge the landing */
    E(mips_lbu(R_T0, EMV_ENT_MAIN, R_A0));
    E(mips_lbu(R_T1, EMV_ENT_SUB,  R_A0));
    E(mips_sll(R_T3, R_T0, 8));
    E(mips_or(R_T3, R_T3, R_T1));
    E(mips_lw(R_T4, CFG_REQ_RESULT, R_T7));
    E(mips_movn(R_T4, R_T3, R_T2));
    E(mips_sw(R_T4, CFG_REQ_RESULT, R_T7));

    /* ---- the RULES: the native brain, one block each ---- */
    for (int r = 0; r < 4; r++) {
        const int B = CFG_RULE_BASE + r * RULE_STRIDE;
        /* live pair (a rule above may have changed it) */
        E(mips_lbu(R_T0, EMV_ENT_MAIN, R_A0));
        E(mips_lbu(R_T1, EMV_ENT_SUB,  R_A0));
        emv_pair_match(a, B + RULE_MASK, B + RULE_FROM_SUB);      /* t2 */
        /* frames >= min_frames */
        E(mips_lw(R_T3, B + RULE_MIN_FRAMES, R_T7));
        E(mips_lw(R_T5, CFG_FRAMES, R_T7));
        E(mips_sltu(R_T3, R_T5, R_T3));                /* 1 iff frames < min */
        E(mips_xori(R_T3, R_T3, 1));
        E(mips_and(R_T2, R_T2, R_T3));
        /* lo <= d2 < hi */
        E(mips_lw(R_T3, CFG_D2, R_T7));
        E(mips_lw(R_T4, B + RULE_D2_LO, R_T7));
        E(mips_sltu(R_T4, R_T3, R_T4));                /* 1 iff d2 < lo */
        E(mips_xori(R_T4, R_T4, 1));
        E(mips_and(R_T2, R_T2, R_T4));
        E(mips_lw(R_T4, B + RULE_D2_HI, R_T7));
        E(mips_sltu(R_T4, R_T3, R_T4));                /* 1 iff d2 < hi */
        E(mips_and(R_T2, R_T2, R_T4));
        /* receding / closing, each only if the flag asks for it */
        E(mips_lw(R_T4, CFG_D2_PREV, R_T7));
        E(mips_sltu(R_T6, R_T4, R_T3));                /* receding: prev < d2 */
        E(mips_sltu(R_T4, R_T3, R_T4));                /* closing:  d2 < prev */
        E(mips_lbu(R_T3, B + RULE_FLAGS, R_T7));
        E(mips_andi(R_T5, R_T3, 1));
        E(mips_sltiu(R_T5, R_T5, 1));                  /* 1 iff not required */
        E(mips_or(R_T5, R_T5, R_T6));
        E(mips_and(R_T2, R_T2, R_T5));
        E(mips_andi(R_T5, R_T3, 2));
        E(mips_sltiu(R_T5, R_T5, 1));
        E(mips_or(R_T5, R_T5, R_T4));
        E(mips_and(R_T2, R_T2, R_T5));
        /* fire budget left */
        E(mips_lw(R_T3, B + RULE_LEFT, R_T7));
        E(mips_sltu(R_T3, R_ZERO, R_T3));
        E(mips_and(R_T2, R_T2, R_T3));
        /* cooldown: ai_ticks - last_fire >= cooldown */
        E(mips_lw(R_T3, CFG_AI_TICKS, R_T7));
        E(mips_lw(R_T4, B + RULE_LAST_FIRE, R_T7));
        E(mips_subu(R_T3, R_T3, R_T4));
        E(mips_lw(R_T4, B + RULE_COOLDOWN, R_T7));
        E(mips_sltu(R_T3, R_T3, R_T4));                /* 1 iff still cooling */
        E(mips_xori(R_T3, R_T3, 1));
        E(mips_and(R_T2, R_T2, R_T3));
        /* bookkeeping, all unconditional: left -= m, fired += m, total += m,
         * last_fire = m ? ai_ticks : last_fire */
        E(mips_lw(R_T3, B + RULE_LEFT, R_T7));
        E(mips_subu(R_T3, R_T3, R_T2));
        E(mips_sw(R_T3, B + RULE_LEFT, R_T7));
        E(mips_lw(R_T3, B + RULE_FIRED, R_T7));
        E(mips_addu(R_T3, R_T3, R_T2));
        E(mips_sw(R_T3, B + RULE_FIRED, R_T7));
        E(mips_lw(R_T3, CFG_BRAIN_FIRES, R_T7));
        E(mips_addu(R_T3, R_T3, R_T2));
        E(mips_sw(R_T3, CFG_BRAIN_FIRES, R_T7));
        E(mips_lw(R_T3, CFG_AI_TICKS, R_T7));
        E(mips_lw(R_T4, B + RULE_LAST_FIRE, R_T7));
        E(mips_movn(R_T4, R_T3, R_T2));
        E(mips_sw(R_T4, B + RULE_LAST_FIRE, R_T7));
        /* the enter-action's arguments; a0 is still the entity */
        E(mips_lbu(R_A1, B + RULE_TO_MAIN, R_T7));
        E(mips_lbu(R_A2, B + RULE_TO_SUB,  R_T7));
        E(mips_lbu(R_A3, B + RULE_MODE,    R_T7));
        emv_cond_call(a, cfg, ret_stub);
    }

    /* ---- v2's one-shot +0x414 budget seam, on the pair as it stands now ---- */
    E(mips_lbu(R_T0, EMV_ENT_MAIN, R_A0));
    E(mips_lbu(R_T1, EMV_ENT_SUB,  R_A0));
    E(mips_sll(R_T8, R_T0, 8));
    E(mips_or(R_T8, R_T8, R_T1));                      /* t8 = cur */
    E(mips_lbu(R_T2, CFG_WANT_MAIN, R_T7));
    E(mips_lbu(R_T3, CFG_WANT_SUB,  R_T7));
    E(mips_xor(R_T0, R_T0, R_T2));
    E(mips_xor(R_T1, R_T1, R_T3));
    E(mips_xori(R_T3, R_T3, SUB_ANY));
    E(mips_sltiu(R_T3, R_T3, 1));
    E(mips_movn(R_T1, R_ZERO, R_T3));
    E(mips_or(R_T0, R_T0, R_T1));
    E(mips_sltiu(R_T2, R_T0, 1));                      /* 1 iff match */
    E(mips_lbu(R_T3, CFG_ARM29, R_T7));
    E(mips_sltiu(R_T3, R_T3, 1));
    E(mips_xori(R_T3, R_T3, 1));                       /* 1 iff seam on */
    E(mips_and(R_T2, R_T2, R_T3));
    /* second tick of this action: prev == cur && prev2 != cur */
    E(mips_lw(R_T4, CFG_PREV,  R_T7));
    E(mips_lw(R_T5, CFG_PREV2, R_T7));
    E(mips_xor(R_T6, R_T4, R_T8));
    E(mips_sltiu(R_T6, R_T6, 1));
    E(mips_and(R_T2, R_T2, R_T6));
    E(mips_xor(R_T6, R_T5, R_T8));
    E(mips_sltiu(R_T6, R_T6, 1));
    E(mips_xori(R_T6, R_T6, 1));
    E(mips_and(R_T2, R_T2, R_T6));
    /* shift the history every tick */
    E(mips_sw(R_T4, CFG_PREV2, R_T7));
    E(mips_sw(R_T8, CFG_PREV,  R_T7));
    /* the bounded, branchless store */
    E(mips_lw(R_T4, CFG_PATCH_OFF, R_T7));
    E(mips_lw(R_T5, CFG_PATCH_VAL, R_T7));
    E(mips_andi(R_T4, R_T4, PATCH_OFF_MASK));
    E(mips_addu(R_T4, R_A0, R_T4));
    E(mips_addiu(R_T6, R_T7, CFG_SINK));
    E(mips_movn(R_T6, R_T4, R_T2));
    E(mips_sw(R_T5, 0, R_T6));

    /* restore the step's arguments and our caller's ra, then tail-call */
    E(mips_lw(R_A1, CFG_A1_SPILL, R_T7));
    E(mips_lw(R_A2, CFG_A2_SPILL, R_T7));
    E(mips_lw(R_A3, CFG_A3_SPILL, R_T7));
    E(mips_lw(R_RA, CFG_RA_SPILL, R_T7));
    E(mips_j(original));
    E(MIPS_NOP);

    if (overflow) *overflow = A.overflow;
    return A.i;
}

/* --- slot 32: substitution PRE part, the original, the v2 POST part. ------ */
#define ACT_FRAME   0x10
#define ACT_SP_ENT  0x00
#define ACT_SP_MAIN 0x04
#define ACT_SP_SUB  0x08
#define ACT_SP_RA   0x0C

static inline int emv_build_act_stub(uint32_t *out, int cap, uint32_t cfg,
                                     uint32_t original, int *overflow)
{
    emv_asm_t A = { out, 0, cap, 0 };
    emv_asm_t *a = &A;

    E(mips_addiu(R_SP, R_SP, -ACT_FRAME));
    E(mips_sw(R_RA, ACT_SP_RA,  R_SP));
    E(mips_sw(R_A0, ACT_SP_ENT, R_SP));

    emv_load_cfg(a, cfg);
    E(mips_andi(R_T0, R_A1, 0xFF));                    /* main as passed */
    E(mips_andi(R_T1, R_A2, 0xFF));                    /* id as passed   */

    /* ring[++idx & 7] = (mode<<16)|(main<<8)|sub — the original arguments */
    E(mips_lw(R_T2, CFG_RING_IDX, R_T7));
    E(mips_addiu(R_T2, R_T2, 1));
    E(mips_andi(R_T2, R_T2, 7));
    E(mips_sw(R_T2, CFG_RING_IDX, R_T7));
    E(mips_sll(R_T3, R_T2, 2));
    E(mips_addu(R_T3, R_T3, R_T7));                    /* t3 = cfg + idx*4 */
    E(mips_andi(R_T4, R_A3, 0xFF));
    E(mips_sll(R_T4, R_T4, 16));
    E(mips_sll(R_T5, R_T0, 8));
    E(mips_or(R_T4, R_T4, R_T5));
    E(mips_or(R_T4, R_T4, R_T1));                      /* t4 = entry */

    /* the substitution table: first matching entry wins (t8 = taken) */
    E(mips_addu(R_T8, R_ZERO, R_ZERO));
    for (int e = 0; e < 4; e++) {
        const int B = CFG_SUB_BASE + e * SUB_STRIDE;
        E(mips_lbu(R_T5, B + SUB_MASK, R_T7));
        E(mips_srlv(R_T5, R_T5, R_T0));
        E(mips_andi(R_T5, R_T5, 1));                   /* main eligible */
        E(mips_lbu(R_T6, B + SUB_FROM_SUB, R_T7));
        E(mips_xor(R_T9, R_T6, R_T1));
        E(mips_sltiu(R_T9, R_T9, 1));                  /* id == from_sub */
        E(mips_xori(R_T6, R_T6, SUB_ANY));
        E(mips_sltiu(R_T6, R_T6, 1));                  /* wildcard */
        E(mips_or(R_T9, R_T9, R_T6));
        E(mips_and(R_T5, R_T5, R_T9));
        E(mips_lw(R_T6, B + SUB_LEFT, R_T7));
        E(mips_sltu(R_T6, R_ZERO, R_T6));              /* left != 0 */
        E(mips_and(R_T5, R_T5, R_T6));
        E(mips_xori(R_T6, R_T8, 1));                   /* not taken yet */
        E(mips_and(R_T5, R_T5, R_T6));                 /* t5 = this entry fires */
        E(mips_or(R_T8, R_T8, R_T5));
        E(mips_lbu(R_T6, B + SUB_TO_MAIN, R_T7));
        E(mips_movn(R_A1, R_T6, R_T5));
        E(mips_lbu(R_T6, B + SUB_TO_SUB, R_T7));
        E(mips_movn(R_A2, R_T6, R_T5));
        E(mips_lw(R_T6, B + SUB_LEFT, R_T7));
        E(mips_subu(R_T6, R_T6, R_T5));
        E(mips_sw(R_T6, B + SUB_LEFT, R_T7));
    }
    /* bookkeeping on the whole table */
    E(mips_lw(R_T6, CFG_SUB_HITS, R_T7));
    E(mips_addu(R_T6, R_T6, R_T8));
    E(mips_sw(R_T6, CFG_SUB_HITS, R_T7));
    E(mips_lw(R_T6, CFG_SUB_LAST_IN, R_T7));
    E(mips_movn(R_T6, R_T4, R_T8));
    E(mips_sw(R_T6, CFG_SUB_LAST_IN, R_T7));
    E(mips_sll(R_T6, R_T8, 24));
    E(mips_or(R_T4, R_T4, R_T6));
    E(mips_sw(R_T4, CFG_RING, R_T3));                  /* ring entry, subst bit set */
    E(mips_sw(R_T8, CFG_SUB_PENDING, R_T7));
    E(mips_andi(R_T6, R_A1, 0xFF));
    E(mips_sll(R_T6, R_T6, 8));
    E(mips_andi(R_T9, R_A2, 0xFF));
    E(mips_or(R_T6, R_T6, R_T9));
    E(mips_sw(R_T6, CFG_SUB_TO_PEND, R_T7));
    /* the pair actually entered goes to the frame for the post part */
    E(mips_sw(R_A1, ACT_SP_MAIN, R_SP));
    E(mips_sw(R_A2, ACT_SP_SUB,  R_SP));

    /* the species' own enter-action, a0..a3 as (possibly) rewritten */
    E(mips_jal(original));
    E(MIPS_NOP);

    /* v2 post part: reload, tear the frame down; $v0/$v1 untouched */
    E(mips_lw(R_T8, ACT_SP_ENT,  R_SP));               /* entity */
    E(mips_lw(R_T0, ACT_SP_MAIN, R_SP));
    E(mips_lw(R_T1, ACT_SP_SUB,  R_SP));
    E(mips_lw(R_RA, ACT_SP_RA,   R_SP));
    E(mips_addiu(R_SP, R_SP, ACT_FRAME));
    emv_load_cfg(a, cfg);
    E(mips_andi(R_T0, R_T0, 0xFF));
    E(mips_andi(R_T1, R_T1, 0xFF));
    E(mips_lw(R_T6, CFG_ACT_ENTER, R_T7));
    E(mips_addiu(R_T6, R_T6, 1));
    E(mips_sw(R_T6, CFG_ACT_ENTER, R_T7));
    E(mips_sll(R_T6, R_T0, 8));
    E(mips_or(R_T6, R_T6, R_T1));
    E(mips_sw(R_T6, CFG_LAST_PAIR, R_T7));
    E(mips_lbu(R_T2, CFG_WANT_MAIN, R_T7));
    E(mips_lbu(R_T3, CFG_WANT_SUB,  R_T7));
    E(mips_xor(R_T0, R_T0, R_T2));
    E(mips_xor(R_T1, R_T1, R_T3));
    E(mips_xori(R_T3, R_T3, SUB_ANY));
    E(mips_sltiu(R_T3, R_T3, 1));
    E(mips_movn(R_T1, R_ZERO, R_T3));
    E(mips_or(R_T0, R_T0, R_T1));
    E(mips_sltiu(R_T2, R_T0, 1));                      /* 1 iff match */
    E(mips_lw(R_T4, CFG_PATCH_OFF, R_T7));
    E(mips_lw(R_T5, CFG_PATCH_VAL, R_T7));
    E(mips_andi(R_T4, R_T4, PATCH_OFF_MASK));
    E(mips_addu(R_T4, R_T8, R_T4));
    E(mips_addiu(R_T6, R_T7, CFG_SINK));
    E(mips_movn(R_T6, R_T4, R_T2));
    E(mips_sw(R_T5, 0, R_T6));

    /* landed? the cells after the call vs the pair we substituted in */
    E(mips_lbu(R_T2, EMV_ENT_MAIN, R_T8));
    E(mips_lbu(R_T3, EMV_ENT_SUB,  R_T8));
    E(mips_sll(R_T2, R_T2, 8));
    E(mips_or(R_T2, R_T2, R_T3));
    E(mips_lw(R_T4, CFG_SUB_TO_PEND, R_T7));
    E(mips_xor(R_T4, R_T4, R_T2));
    E(mips_sltiu(R_T4, R_T4, 1));
    E(mips_lw(R_T5, CFG_SUB_PENDING, R_T7));
    E(mips_and(R_T4, R_T4, R_T5));
    E(mips_lw(R_T6, CFG_SUB_LANDED, R_T7));
    E(mips_addu(R_T6, R_T6, R_T4));
    E(mips_sw(R_T6, CFG_SUB_LANDED, R_T7));
    E(mips_sw(R_ZERO, CFG_SUB_PENDING, R_T7));

    E(mips_jr(R_RA));
    E(MIPS_NOP);

    if (overflow) *overflow = A.overflow;
    return A.i;
}

/* the no-op callee the conditional call lands on when it does not fire */
static inline int emv_build_ret_stub(uint32_t *out)
{
    out[0] = mips_jr(R_RA);
    out[1] = MIPS_NOP;
    return 2;
}

#undef E
#endif /* EM_VHOOK_STUBS_H */
