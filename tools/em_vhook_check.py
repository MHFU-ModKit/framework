#!/usr/bin/env python3
"""Assemble em_vhook's stubs on the HOST and check them before they go near the
engine.

    python tools/em_vhook_check.py            # assert + summary
    python tools/em_vhook_check.py --dis      # ... and print both listings

The stub builders live in framework/prx/mods/em_vhook/stubs.h and depend on
nothing but <stdint.h> and mhfu/mips.h, so the host C compiler can run them
with a fake block address. The words are then read back through
tools/mips_dis.py — an independent decoder — and these properties asserted:

  - NO BRANCH in either stub (no beq/bne/b*/bgez...): every decision is a
    MOVN/MOVZ select, so the JIT sees one basic block per stub and armed and
    unarmed runs execute the same instruction stream;
  - the slot-29 stub never touches $sp (frame-free; its ra and the step's
    arguments go to config words) and ends in `j <original>`;
  - the slot-32 stub's only $sp traffic is the 16-byte frame v2 carried, it
    calls the original exactly once with `jal`, and ends in `jr ra`;
  - every jump/call target is one of: the original, the engine's enter-action
    dispatcher 0x09AC89F0, the in-block `jr ra` stub;
  - every jalr's delay slot is a nop, every config-block load/store offset is
    inside the block (stubs.h's CFG_SIZE);
  - both fit their slots (STUB_AI_INSNS / STUB_ACT_INSNS).

Why this exists: a stub bug is not a crash with a backtrace, it is PPSSPP
spinning at 131 % CPU ten minutes into a hunt. Everything checkable offline is
checked offline.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mips_dis import decode  # noqa: E402

PRX = ROOT / "framework" / "prx"
STUBS_H = PRX / "mods" / "em_vhook" / "stubs.h"

CFG = 0x08A5E000          # a fake block address (the debugger proofs used this one)
ORIG_AI = 0x09D3A000      # fake originals, inside the overlay band
ORIG_ACT = 0x09D3D608
RET = 0x08A5F000
ENTER_DISPATCH = 0x09AC89F0

HOST_C = r"""
#include <stdio.h>
#include <stdint.h>
#include "stubs.h"
int main(void) {
    static uint32_t ai[STUB_AI_INSNS + 64], act[STUB_ACT_INSNS + 64];
    int oa = 0, ob = 0;
    int na = emv_build_ai_stub(ai, STUB_AI_INSNS, %#xu, %#xu, %#xu, &oa);
    int nb = emv_build_act_stub(act, STUB_ACT_INSNS, %#xu, %#xu, &ob);
    printf("AI %%d %%d %%d\n", na, oa, STUB_AI_INSNS);
    for (int i = 0; i < na && i < STUB_AI_INSNS; i++) printf("%%08x\n", ai[i]);
    printf("ACT %%d %%d %%d\n", nb, ob, STUB_ACT_INSNS);
    for (int i = 0; i < nb && i < STUB_ACT_INSNS; i++) printf("%%08x\n", act[i]);
    printf("CFG_SIZE %%d\n", (int)CFG_SIZE);
    return 0;
}
"""

BRANCHES = {"beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl",
            "bltz", "bgez", "bltzal", "bgezal", "bltzl", "bgezl", "bc1f", "bc1t", "b"}


def build() -> dict:
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "check.c"
        src.write_text(HOST_C % (CFG, ORIG_AI, RET, CFG, ORIG_ACT))
        exe = Path(td) / "check"
        subprocess.run(["cc", "-std=c11", "-O0", "-I", str(PRX / "include"),
                        "-I", str(STUBS_H.parent), str(src), "-o", str(exe)],
                       check=True)
        out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    stubs, cur = {}, None
    for line in out.splitlines():
        parts = line.split()
        if parts[0] in ("AI", "ACT"):
            cur = parts[0]
            stubs[cur] = {"n": int(parts[1]), "overflow": int(parts[2]),
                          "cap": int(parts[3]), "words": []}
        elif parts[0] == "CFG_SIZE":
            stubs["cfg_size"] = int(parts[1])
        else:
            stubs[cur]["words"].append(int(parts[0], 16))
    return stubs


def listing(words: list[int], base: int) -> list:
    return [decode(w, base + 4 * i) for i, w in enumerate(words)]


_MEM = re.compile(r"(-?\d+)\((\w+)\)")


def check(stubs: dict, show: bool) -> int:
    fails = []
    cfg_size = stubs["cfg_size"]

    def mem_offsets(insns):
        for ins in insns:
            m = _MEM.search(str(ins))
            if m:
                yield ins, int(m.group(1)), m.group(2)

    for name, orig, tail in (("AI", ORIG_AI, "j"), ("ACT", ORIG_ACT, "jr")):
        st = stubs[name]
        insns = listing(st["words"], CFG)
        if show:
            print(f"--- {name} stub: {st['n']} insns (cap {st['cap']})")
            for ins in insns:
                print(f"  {ins.va:08X}  {ins.word:08X}  {str(ins)}")
        if st["overflow"] or st["n"] > st["cap"]:
            fails.append(f"{name}: {st['n']} insns does not fit {st['cap']}")
        ops = [i.op for i in insns]
        for ins in insns:
            if ins.op in BRANCHES:
                fails.append(f"{name}: branch at +{ins.va - CFG:#x}: {str(ins)}")
        # targets
        for ins in insns:
            if ins.op in ("j", "jal"):
                if ins.target not in (orig, ENTER_DISPATCH):
                    fails.append(f"{name}: {str(ins)} targets {ins.target:#x}")
        # jalr delay slots are nops, jal/j delay slots too
        for k, ins in enumerate(insns):
            if ins.op in ("jalr", "jal", "j", "jr") and k + 1 < len(insns):
                if insns[k + 1].word != 0:
                    fails.append(f"{name}: delay slot of {str(ins)} is {str(insns[k+1])}")
        # ends
        if name == "AI":
            if not (insns[-2].op == "j" and insns[-2].target == orig):
                fails.append("AI: does not end in j <original>")
            if any("sp" in str(i) for i in insns):
                fails.append("AI: touches $sp")
            if ops.count("jalr") != 1 + 4:
                fails.append(f"AI: expected 5 jalr (request + 4 rules), got {ops.count('jalr')}")
            # the selected targets: lui/ori pairs for RET and ENTER_DISPATCH
            txt = "\n".join(str(i) for i in insns)
            for addr in (RET, ENTER_DISPATCH):
                hi = f"0x{addr >> 16:04X}"
                if f"lui t9, {hi}" not in txt and f"lui t6, {hi}" not in txt:
                    fails.append(f"AI: never loads {addr:#x}")
        else:
            if not (insns[-2].op == "jr"):
                fails.append("ACT: does not end in jr ra")
            if ops.count("jal") != 1:
                fails.append(f"ACT: expected exactly one jal, got {ops.count('jal')}")
            sp_ops = [str(i) for i in insns if "sp" in str(i)]
            frame = [t for t in sp_ops if t.startswith("addiu sp")]
            if len(frame) != 2:
                fails.append(f"ACT: frame setup/teardown expected twice, saw {frame}")
        # config offsets in range (loads/stores off t7 or t3 = ring)
        for ins, off, base in mem_offsets(insns):
            if base == "t7" and not (0 <= off < cfg_size):
                fails.append(f"{name}: cfg access out of block: {str(ins)}")
    print(f"AI stub  {stubs['AI']['n']:3d} / {stubs['AI']['cap']} insns   "
          f"ACT stub {stubs['ACT']['n']:3d} / {stubs['ACT']['cap']} insns   "
          f"cfg {cfg_size:#x} bytes")
    if fails:
        for f in fails:
            print("FAIL", f)
        return 1
    print("OK: branchless, frame-free slot 29, single-call slot 32, targets and offsets in range")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dis", action="store_true", help="print both listings")
    args = ap.parse_args(argv)
    return check(build(), args.dis)


if __name__ == "__main__":
    sys.exit(main())
