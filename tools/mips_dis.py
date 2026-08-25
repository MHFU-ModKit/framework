"""A small MIPS32/Allegrex disassembler — enough to read MHFU's overlays.

Why this exists: the PSP overlays are ~300 KB of compiled MIPS each and nothing
in the repo could read them offline. PPSSPP's `memory.disasm` can, but only for
code that is currently loaded, which costs a cold boot per question and returns
JIT-polluted bytes for anything already translated (`ppsspp-debugging`). This
module reads the file on disk instead, so overlay questions are answerable for
free and reproducibly.

Coverage is deliberately partial: the integer ISA, the FPU (cop1) forms the
game actually uses, and enough shape to recognise jump tables and hi/lo address
pairs. VFPU instructions decode as `vfpu.<op>` with raw bits — they appear in
maths helpers, not in the AI logic we care about, and pretending to decode them
would be worse than admitting we don't.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

_SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall",
    0x0D: "break", 0x0F: "sync", 0x10: "mfhi", 0x11: "mthi", 0x12: "mflo",
    0x13: "mtlo", 0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and",
    0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
    0x2C: "max", 0x2D: "min",                      # Allegrex
    0x34: "teq",
}
_REGIMM = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal",
           0x02: "bltzl", 0x03: "bgezl"}
_OP = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
    0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
    0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
    0x26: "lwr", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x2F: "cache", 0x30: "ll", 0x31: "lwc1", 0x35: "lvs", 0x38: "sc",
    0x39: "swc1", 0x3D: "svs",
}
_LOADSTORE = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "sb", "sh", "swl",
              "sw", "swr", "ll", "sc", "lwc1", "swc1"}
_FPU_FMT = {16: "s", 17: "d", 20: "w"}
_FPU_FN = {0x00: "add", 0x01: "sub", 0x02: "mul", 0x03: "div", 0x04: "sqrt",
           0x05: "abs", 0x06: "mov", 0x07: "neg", 0x0C: "round.w",
           0x0D: "trunc.w", 0x0E: "ceil.w", 0x0F: "floor.w", 0x20: "cvt.s",
           0x21: "cvt.d", 0x24: "cvt.w"}
_FPU_CMP = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule",
            "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]

BRANCH_OPS = {"beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl",
              "bltz", "bgez", "bltzal", "bgezal", "bltzl", "bgezl",
              "bc1t", "bc1f", "bc1tl", "bc1fl"}
JUMP_OPS = {"j", "jal"}


@dataclass
class Insn:
    va: int
    word: int
    op: str
    args: str = ""
    target: int | None = None       # branch/jump destination VA
    imm: int | None = None          # signed immediate, when there is one

    @property
    def is_branch(self) -> bool:
        return self.op in BRANCH_OPS

    @property
    def is_jump(self) -> bool:
        return self.op in JUMP_OPS

    def __str__(self) -> str:
        return f"{self.op} {self.args}".rstrip()


def _s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def decode(word: int, va: int) -> Insn:
    op = word >> 26
    rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
    sa, fn = (word >> 6) & 31, word & 63
    imm = word & 0xFFFF
    s = _s16(imm)

    if word == 0:
        return Insn(va, word, "nop")

    if op == 0:
        name = _SPECIAL.get(fn)
        if name is None:
            return Insn(va, word, f".word 0x{word:08X}")
        if name in ("sll", "srl", "sra"):
            return Insn(va, word, name, f"{REG[rd]}, {REG[rt]}, {sa}")
        if name in ("sllv", "srlv", "srav"):
            return Insn(va, word, name, f"{REG[rd]}, {REG[rt]}, {REG[rs]}")
        if name == "jr":
            return Insn(va, word, name, REG[rs])
        if name == "jalr":
            return Insn(va, word, name, f"{REG[rd]}, {REG[rs]}")
        if name in ("mfhi", "mflo"):
            return Insn(va, word, name, REG[rd])
        if name in ("mthi", "mtlo"):
            return Insn(va, word, name, REG[rs])
        if name in ("mult", "multu", "div", "divu", "teq"):
            return Insn(va, word, name, f"{REG[rs]}, {REG[rt]}")
        if name in ("syscall", "break", "sync"):
            return Insn(va, word, name)
        return Insn(va, word, name, f"{REG[rd]}, {REG[rs]}, {REG[rt]}")

    if op == 1:
        name = _REGIMM.get(rt)
        if name is None:
            return Insn(va, word, f".word 0x{word:08X}")
        tgt = va + 4 + s * 4
        return Insn(va, word, name, f"{REG[rs]}, 0x{tgt:08X}", target=tgt)

    if op == 0x11:                                  # cop1
        if rs == 8:                                 # bc1t / bc1f
            name = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(rt & 3, "bc1?")
            tgt = va + 4 + s * 4
            return Insn(va, word, name, f"0x{tgt:08X}", target=tgt)
        if rs in (0, 4):                            # mfc1 / mtc1
            name = "mfc1" if rs == 0 else "mtc1"
            return Insn(va, word, name, f"{REG[rt]}, f{rd}")
        fmt = _FPU_FMT.get(rs)
        if fmt:
            if (fn & 0x30) == 0x30:                 # c.cond.fmt
                return Insn(va, word, f"c.{_FPU_CMP[fn & 15]}.{fmt}",
                            f"f{(word >> 11) & 31}, f{(word >> 16) & 31}")
            name = _FPU_FN.get(fn)
            if name:
                fd, fs, ft = (word >> 6) & 31, (word >> 11) & 31, (word >> 16) & 31
                if fn in (0x00, 0x01, 0x02, 0x03):
                    return Insn(va, word, f"{name}.{fmt}", f"f{fd}, f{fs}, f{ft}")
                return Insn(va, word, f"{name}.{fmt}", f"f{fd}, f{fs}")
        return Insn(va, word, f"cop1.0x{fn:02X}", f".word 0x{word:08X}")

    if op in (0x12, 0x13, 0x18, 0x1B, 0x1C, 0x1F, 0x34, 0x37, 0x3C, 0x3F):
        return Insn(va, word, f"vfpu.0x{op:02X}", f".word 0x{word:08X}")

    name = _OP.get(op)
    if name is None:
        return Insn(va, word, f".word 0x{word:08X}")

    if name in ("j", "jal"):
        tgt = (va & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        return Insn(va, word, name, f"0x{tgt:08X}", target=tgt)
    if name in ("beq", "bne", "beql", "bnel"):
        tgt = va + 4 + s * 4
        if name in ("beq", "beql") and rs == 0 and rt == 0:
            return Insn(va, word, "b", f"0x{tgt:08X}", target=tgt)
        return Insn(va, word, name, f"{REG[rs]}, {REG[rt]}, 0x{tgt:08X}", target=tgt)
    if name in ("blez", "bgtz", "blezl", "bgtzl"):
        tgt = va + 4 + s * 4
        return Insn(va, word, name, f"{REG[rs]}, 0x{tgt:08X}", target=tgt)
    if name == "lui":
        return Insn(va, word, name, f"{REG[rt]}, 0x{imm:04X}", imm=imm)
    if name in _LOADSTORE:
        r = f"f{rt}" if name in ("lwc1", "swc1") else REG[rt]
        return Insn(va, word, name, f"{r}, {s}({REG[rs]})", imm=s)
    if name in ("andi", "ori", "xori"):
        return Insn(va, word, name, f"{REG[rt]}, {REG[rs]}, 0x{imm:04X}", imm=imm)
    if name == "cache":
        return Insn(va, word, name, f"0x{rt:02X}, {s}({REG[rs]})", imm=s)
    return Insn(va, word, name, f"{REG[rt]}, {REG[rs]}, {s}", imm=s)


def disasm(data: bytes, base_va: int, off: int = 0, count: int = 32):
    out = []
    for i in range(count):
        o = off + i * 4
        if o + 4 > len(data):
            break
        out.append(decode(struct.unpack_from("<I", data, o)[0], base_va + o))
    return out
