# Real-PSP plugin loading (PRO CFW) — the debugging saga + findings

Status 2026-06-25: **the plugin-LOAD wall is solved; boot-SURVIVAL fix is in test.**
The framework always loaded on PPSSPP but failed on a friend's real PSP. Real hardware
here = **PRO CFW** (PRO-B/PRO-C, ~2012, firmware 6.61), **NOT ARK-4** — every earlier
ARK-4 assumption in the old docs is retracted. Reference good binaries: a working
psplink build at `~/Downloads/psplink/` (byte-diffing it against ours found the load
fix). Friend test package: `/tmp/brute_tigrex_psp_package/`.

## 1. THE LOAD FIX (SOLVED) — the missing mandatory syslib export

Symptom: `0x800200D9` ("game could not be started"), before any plugin code runs — at
*any* plugin size (a 1 KB probe failed identically), with ~24 MB free. PPSSPP loaded the
same PRX fine.

Root cause: when the framework was switched to **`build_prx.mak`** (for the
`module_start`/`-nostartfiles` conversion), it silently lost its **export table**.
- `build.mak` ALWAYS links an export object: your `PRX_EXPORTS` `.exp`, else the default
  `$(PSPSDK)/lib/prxexports.o`.
- `build_prx.mak` does **NOT** — it only has the `%.exp -> %.c` rule and never adds an
  export object to the link.

A PRX **must** export its module via the **syslib** entry (`module_start` / `module_info`
in `.lib.ent`) so the firmware loader can find the entry point. Ours shipped with an
**empty `.lib.ent`** → firmware rejects it. PPSSPP finds `module_info` by scanning
sections instead, so it loaded ours regardless — exactly why it always worked in the
emulator and never on hardware.

Proof: `psp-readelf -S` on the working `psplink_user.prx` → `.lib.ent` = 16 bytes; ours =
absent. psplink's `exports.exp` comment: *"These four lines are mandatory."*

**FIX:** add `$(PSPSDK)/lib/prxexports.o` to **LIBS** (first, so its `module_start`/
`module_info` refs resolve from our objects) in `framework/prx/Makefile.psp` and
`framework/prx/memprobe/Makefile`. (In LIBS, not OBJS — as an OBJS prerequisite, make
tries to "build" the SDK object → "No rule to make target".) After this our PRX has the
same 16 B `.lib.ent` and the plugin **loads + runs on real hardware** (wrote its file).

## 2. THE BOOT CRASH (fix in test) — module_start runs on the kernel loader thread

After the load fix the plugin loaded and ran, but the game still crashed:
```
Exception - Coprocessor unusable   Cause 0x9000002C (COP1, BD set)
Th Name SceKernelLoadExecThread    Mod sceLowIO_Driver   EPC 0x8808EAFC (jr $ra)
(garbage regs: s0=0x359B..., at=0xBD..., t6=0xBD...)
```
Mechanism (research-confirmed against MrColdbird/procfw + uofw):
- PRO loads `game.txt` plugins by calling `sctrlKernelLoadModule` **synchronously on
  `SceKernelLoadExecThread`** (the kernel boot/loader thread) at the `sceMediaSync`
  hook — **before the game's EBOOT runs**.
- That kernel thread has a **tiny stack** (~4-16 KB) and the **FPU disabled** (CU1=0).
- So our `module_start` runs *on that thread*. Doing `sceIoOpen` there descends
  `iofilemgr → sceLowIO_Driver`, eating the stack → overflow → corrupts the loader
  thread's saved regs → it later `jr $ra` with a clobbered `$ra` → **wild jump** lands on
  an FPU instr in `sceLowIO_Driver` → CU1=0 → **Coprocessor Unusable**. Stack depth varies
  per boot → **non-deterministic** (irrelevant code changes flip load-vs-crash).
- A background thread doing ms0 I/O that **races the loadexec's own EBOOT disc read** is
  the same class of failure (sceLowIO state corruption).
- `0x800200D9` is *also* an intermittent alloc failure at plugin load — a separate,
  occasional outcome of the same load path.

This is the real-hardware twin of the documented `brute-port-prx-stack-collision`.

**FIX (psplink's model — under test in the deferred-worker memprobe):** `module_start`
does the **bare minimum** on that tiny stack — `sceKernelCreateThread` +
`sceKernelStartThread` (worker, own user stack, `THREAD_ATTR_USER`, ~16 KB) + `return`.
**No I/O, no float, no deep calls in module_start.** The worker then **sleeps past the
boot/loadexec phase (~5 s)** before any ms0 I/O. (psplink_user's `module_start` literally
just registers a callback and returns — the work runs later, kernel-driven.)

**Framework TODO:** `bootstrap.cpp`'s `module_start` already spawns a thread, but
`framework_main` does heavy work *immediately* (logging, hooks, `mhfu_mods_init_all` →
Lua, inject file reads). All of that must move behind a "wait out boot, then go" gate
(a delay, or gate on the `screen_state` oracle `0x08A8CA48`). Threads that run float/Lua
also need `THREAD_ATTR_VFPU`.

## 3. Brute extra-RAM — ONE auto-detecting build (MHFU_REALHW split RETIRED)

`inject.cpp::mhfu_xram_platform_init` (called from `bootstrap.cpp`) picks the inject
scratch region once at bootstrap:
- **PPSSPP:** the raw flat `0x0B000000` window (`memory=64`). Gated by
  `sceKernelMaxFreeMemSize() > 32 MB` so the HW-unsafe probe-write never runs on a real
  24 MB partition.
- **Real PSP:** the **4 MB VOLATILE partition** (`sceKernelVolatileMemTryLock`) — user-
  accessible, no partition resize. The ~2.8 MB Brute (grown PAC + orig key) fits in 4 MB.
  Trade-off: while held, MHFU paths needing volatile RAM may stall.

**RETRACTED:** ARK-4 "Use Extra Memory: Forced". Confirmed on hardware to **crash MHFU at
the boot logo by itself** (a grown user partition isn't tolerated by this game's loader).
So we do NOT grow any partition. The old `MHFU_REALHW` compile-time split + the
`sceKernelAllocPartitionMemory` "grown user partition" path are gone.

## 4. build_prx.mak / module_start conversion (PPSSPP-parity, kept)

The framework was converted from the pspsdk `main()`/crt0 pattern to `build_prx.mak`
(`-nostartfiles`, no crt0) with a directly-defined `module_start` that spawns its own
thread (vs crt0 auto-creating a main thread + newlib init). This was a PPSSPP-parity win
but is NOT the real-HW load fix on its own (§1 is).

## 5. RED HERRINGS (ruled out, in order chased)

- **Plugin size / memory** — a 1 KB probe failed identically; ~24 MB free. Not memory.
- **`PSP_HEAP_SIZE_KB`** (default newlib heap grab) — not it.
- **Partition contiguity / load timing** — not it.
- **`.MIPS.abiflags` / `.reginfo` sections, EABI `e_flags`** — the working psplink carries
  ALL of these and loads fine. The `tools/patch_prx_eflags.py` (clears EF_MIPS_ABI) and
  `framework/prx/linkfile_nohw.lds` (discards the sections) are harmless but **unnecessary**;
  kept, not the fix.

## 6. The rejected alternative — FUComplete's EBOOT patch

The PROVEN method for MHFU on hardware (per the FUComplete source in
`tools/FUComplete-Patch/`): static-patch `EBOOT.BIN` in the ISO → in-process preloader →
hook the game's own file-load fn at `0x0884E730` (the project's file-replacer hook). Runs
inside the game process, never touches the CFW plugin loader → sidesteps all of §1–§2.
**User REJECTED this** (doesn't want a modified-ISO / modified game). We pursue the plugin
route instead.

## Key build/tooling facts

- Builds in Docker `pspdev/pspdev:latest`. `make` (framework) / `make` in `memprobe/`.
- `framework/prx/linkfile_nohw.lds` (NOT `.prx` — `make clean` does `rm -f *.prx` and would
  delete it, and `.prx` is gitignored). Referenced by `Makefile.psp` LDFLAGS override.
- `tools/patch_prx_eflags.py` runs host-side in the wrapper Makefile `all` (no python in
  the docker image).
- Inspect a PRX: `psp-readelf -h/-S/-l <file.prx>` inside the docker image.
- Reference working binaries: `~/Downloads/psplink/` (psplink_user.prx is the closest
  shape to our memprobe).
