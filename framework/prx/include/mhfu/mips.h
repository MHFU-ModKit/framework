/*
 * In-PRX MIPS R4000 / Allegrex instruction encoder.
 *
 * Mirrors src/mhfu_bot/mips/encoder.py exactly — same opcode subset,
 * same field encoding, same delay-slot semantics. We build trampolines
 * word-by-word from C so we don't need a separate .S file.
 *
 * All emitters return a single 32-bit little-endian word (the PSP CPU
 * is little-endian and so is host memory, so the value is also the
 * in-memory representation).
 *
 * Registers are integers 0..31. Use the REG_* macros to spell them.
 * Branch delay slots are NOT auto-emitted; the caller must place a
 * valid instruction (often NOP) after every J/JAL/JR/BEQ/BNE.
 */

#ifndef MHFU_MIPS_ENCODER_H
#define MHFU_MIPS_ENCODER_H

#include <stdint.h>

#define MIPS_REG_ZERO  0
#define MIPS_REG_AT    1
#define MIPS_REG_V0    2
#define MIPS_REG_V1    3
#define MIPS_REG_A0    4
#define MIPS_REG_A1    5
#define MIPS_REG_A2    6
#define MIPS_REG_A3    7
#define MIPS_REG_T0    8
#define MIPS_REG_T1    9
#define MIPS_REG_T2   10
#define MIPS_REG_T3   11
#define MIPS_REG_T4   12
#define MIPS_REG_T5   13
#define MIPS_REG_T6   14
#define MIPS_REG_T7   15
#define MIPS_REG_S0   16
#define MIPS_REG_S1   17
#define MIPS_REG_S2   18
#define MIPS_REG_S3   19
#define MIPS_REG_S4   20
#define MIPS_REG_S5   21
#define MIPS_REG_S6   22
#define MIPS_REG_S7   23
#define MIPS_REG_T8   24
#define MIPS_REG_T9   25
#define MIPS_REG_SP   29
#define MIPS_REG_RA   31

#define MIPS_NOP  0x00000000u

static inline uint32_t mips_jal(uint32_t target_addr) {
    /* opcode 0x03, top 4 bits of (PC+4) shared with target_addr */
    return (0x03u << 26) | ((target_addr >> 2) & 0x03FFFFFFu);
}

static inline uint32_t mips_j(uint32_t target_addr) {
    return (0x02u << 26) | ((target_addr >> 2) & 0x03FFFFFFu);
}

static inline uint32_t mips_jr(uint32_t rs) {
    /* R-type: op=0, rs, 0, 0, 0, funct=0x08 */
    return ((rs & 0x1Fu) << 21) | 0x08u;
}

static inline uint32_t mips_move(uint32_t rd, uint32_t rs) {
    /* addu rd, rs, $zero — op=0, rs, 0, rd, 0, funct=0x21 */
    return ((rs & 0x1Fu) << 21) | ((rd & 0x1Fu) << 11) | 0x21u;
}

static inline uint32_t mips_addiu(uint32_t rt, uint32_t rs, int16_t imm) {
    return (0x09u << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)imm);
}

static inline uint32_t mips_lui(uint32_t rt, uint16_t imm) {
    return (0x0Fu << 26) | ((rt & 0x1Fu) << 16) | imm;
}

static inline uint32_t mips_ori(uint32_t rt, uint32_t rs, uint16_t imm) {
    return (0x0Du << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16) | imm;
}

static inline uint32_t mips_lw(uint32_t rt, int16_t off, uint32_t base) {
    return (0x23u << 26) | ((base & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)off);
}

/* LBU rt, offset(base) — load byte unsigned. opcode 0x24. */
static inline uint32_t mips_lbu(uint32_t rt, int16_t off, uint32_t base) {
    return (0x24u << 26) | ((base & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)off);
}

static inline uint32_t mips_sw(uint32_t rt, int16_t off, uint32_t base) {
    return (0x2Bu << 26) | ((base & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)off);
}

static inline uint32_t mips_beq(uint32_t rs, uint32_t rt, int16_t off_insns) {
    /* Branch if equal. off_insns is signed PC-relative offset in INSTRUCTIONS
     * (PC+4-based). e.g., off_insns=2 lands at insn after delay slot + 1. */
    return (0x04u << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)off_insns);
}

static inline uint32_t mips_bne(uint32_t rs, uint32_t rt, int16_t off_insns) {
    return (0x05u << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((uint32_t)(uint16_t)off_insns);
}

static inline uint32_t mips_jalr(uint32_t rs) {
    /* R-type: op=0, rs, 0, rd=$ra(31), 0, funct=0x09 */
    return ((rs & 0x1Fu) << 21) | (31u << 11) | 0x09u;
}

/* R-type helper: op=0, rs, rt, rd, shamt=0, funct. */
static inline uint32_t mips_r3(uint32_t rd, uint32_t rs, uint32_t rt, uint32_t fn) {
    return ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((rd & 0x1Fu) << 11) | (fn & 0x3Fu);
}

static inline uint32_t mips_addu(uint32_t rd, uint32_t rs, uint32_t rt) {
    return mips_r3(rd, rs, rt, 0x21u);
}
static inline uint32_t mips_subu(uint32_t rd, uint32_t rs, uint32_t rt) {
    return mips_r3(rd, rs, rt, 0x23u);
}
static inline uint32_t mips_xor(uint32_t rd, uint32_t rs, uint32_t rt) {
    return mips_r3(rd, rs, rt, 0x26u);
}
static inline uint32_t mips_or(uint32_t rd, uint32_t rs, uint32_t rt) {
    return mips_r3(rd, rs, rt, 0x25u);
}

static inline uint32_t mips_andi(uint32_t rt, uint32_t rs, uint16_t imm) {
    return (0x0Cu << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16) | imm;
}
static inline uint32_t mips_xori(uint32_t rt, uint32_t rs, uint16_t imm) {
    return (0x0Eu << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16) | imm;
}
/* SLTIU rt, rs, imm -> rt = (unsigned)rs < imm. With imm=1 this is "rs == 0",
 * which is how a branchless equality test produces its 0/1 selector. */
static inline uint32_t mips_sltiu(uint32_t rt, uint32_t rs, uint16_t imm) {
    return (0x0Bu << 26) | ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16) | imm;
}

/* MOVN: if (rt != 0) rd = rs. Branchless conditional move. R-type funct=0x0B. */
static inline uint32_t mips_movn(uint32_t rd, uint32_t rs, uint32_t rt) {
    return ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((rd & 0x1Fu) << 11) | 0x0Bu;
}

/* MOVZ: if (rt == 0) rd = rs. R-type funct=0x0A. */
static inline uint32_t mips_movz(uint32_t rd, uint32_t rs, uint32_t rt) {
    return ((rs & 0x1Fu) << 21) | ((rt & 0x1Fu) << 16)
         | ((rd & 0x1Fu) << 11) | 0x0Au;
}

#endif /* MHFU_MIPS_ENCODER_H */
