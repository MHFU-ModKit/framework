#!/usr/bin/env python3
"""
Clear the MIPS ABI field from a PRX's ELF e_flags.

WHY: the modern pspdev toolchain (GCC for the mipsallegrexel-psp-elf target)
defaults to the EABI ABI, so compiled objects — and the final PRX, because
psp-prxgen copies e_flags verbatim — carry EF_MIPS_ABI_EABI32 (0x00003000) in
the e_flags ABI field (bits 12-15, mask 0x0000F000). Old PSP firmware module
loaders (retail 6.x, and CFW like PRO whose loadcore predates the modern
toolchain) treat an EABI ABI flag as unknown/unsupported and REJECT the module
at load — surfaced by the CFW as "could not be started 0x800200D9", before any
code runs. PPSSPP is lenient and ignores the ABI field, which is why such PRXes
load in the emulator but not on hardware. Old-toolchain prebuilt plugins (the
user's working psplink/meminfo) have a zero/o32 ABI field, so they load.

The code itself is self-consistent EABI internally; the firmware only validates
the e_flags METADATA. Clearing the ABI field (back to "unspecified", matching
old homebrew) makes the loader accept it without changing behaviour.

This is a 4-byte edit of the ELF header — non-destructive (does NOT touch the
program headers / PT_LOAD, unlike re-running psp-prxgen or psp-objcopy on a .prx).

Usage: patch_prx_eflags.py <file.prx>
"""
import struct
import sys

E_FLAGS_OFF = 0x24          # e_flags in a 32-bit ELF header
MIPS_ABI_MASK = 0x0000F000  # EF_MIPS_ABI field (EABI32 = 0x3000 lives here)


def main(path):
    with open(path, "r+b") as f:
        f.seek(E_FLAGS_OFF)
        (flags,) = struct.unpack("<I", f.read(4))
        new = flags & ~MIPS_ABI_MASK
        if new == flags:
            print(f"{path}: e_flags=0x{flags:08X} already has no ABI bits — no change")
            return 0
        f.seek(E_FLAGS_OFF)
        f.write(struct.pack("<I", new))
        print(f"{path}: e_flags 0x{flags:08X} -> 0x{new:08X} (cleared EF_MIPS_ABI)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: patch_prx_eflags.py <file.prx>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
