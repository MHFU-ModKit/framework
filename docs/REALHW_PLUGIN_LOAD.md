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

## 7. Extra RAM on real hardware — the exhaustive search (2026-06-26)

The Brute's full moveset (and any monster with no native analog) needs >1.2 MB of
*user-readable* scratch the engine can read its model from. We mapped the real PSP's RAM
and tried every supported door. **All HITL on the user's own PSP (Slim, model=1, PRO-C2
6.20).** Kernel query probe `framework/prx/memprobe/diag/v_partprobe.c`
(`sceKernelQueryMemoryPartitionInfo`, read-only — safe even for kernel partitions; an
earlier alloc-based v1 froze the kernel allocating from partition 5).

**The partition map (decisive):**

| Part | Start | Size | attr | Segment |
|------|-------|------|------|---------|
| 2 / 6 | `0x08800000` | 24 MB | `0x0F` user | Main user partition (game + XMB) |
| 5 | `0x08400000` | 4 MB | `0x0F` user | **Volatile partition** — the only spare *user-readable* RAM |
| 1 / 3 / 4 | `0x88000000` | 3+3+1 MB | `0x0C` kern | Kernel partitions |
| 9 | `0x8A000000` | 24 MB | `0x0C` kern | **Extra RAM (Slim) — kernel-only** |
| 8 / 12 / 11 | `0x8B800000`+ | 4 MB ea | `0x0C` kern | Extra RAM pieces — kernel-only |

`attr 0x0F` = user-accessible, `0x0C` = kernel-only; `0x08…` = user virtual segment,
`0x88…/0x8A…` = kernel segment. **The Slim's extra 32 MB exists but is entirely
kernel-mapped** (`0x8A000000`), so the user-mode game engine cannot read it as configured.
A partition-2 `PSP_SMEM_High` alloc lands at `0x09FA1C00` — top of the 24 MB, *not* the
extra RAM. Without a remap, the only spare user-readable RAM is the **4 MB volatile**.

**Every supported remap breaks MHFU EU:**

| Mechanism | Result (HITL) |
|-----------|---------------|
| Grow partition 2 (PRO Recovery "Remove RAM restrictions" / `ForceHighMemory` MAX) | MHFU **hard-crashes the PSP** at boot (powers off). MHFU can't run with a resized p2. |
| Size separate p8 (`sctrlHENSetMemory(24, 8)`, p2 stays 24) | returns rc=0, but **no live effect** (the partition map is byte-identical before/after — `sctrlHENSetMemory` is deferred-to-next-loadexec only); the deferred apply **freezes the next loadexec**; and **merely calling it poisons this session's exit** — the probe reset it to `(24,0)` and the HOME-exit *still* froze. |
| Raw memory-protection remap of `0x0A000000` | off-limits (security constraint) **and** would crash MHFU like the p2 grow. |

`ForceHighMemory` enum (`systemctrl_se.h`) = `{OFF, STABLE, MAX}`; PRO-C2 6.20 exposes it
as a single on/off ("Remove RAM restrictions") = the aggressive grow → crash. No gentle
STABLE on this CFW.

**THE TEARDOWN INSIGHT + the launcher test (current, UNTESTED):** every freeze was a
`loadexec` issued while **MHFU itself was tearing down** (self-reload from inside MHFU, the
HOME-exit) with p8 pending. MHFU's *teardown*-with-p8 freezes; a *fresh boot* with p8 (p2
untouched) might not. So the only untested configuration = apply p8 from a **clean launcher
app**, then loadexec MHFU so MHFU only ever fresh-boots with p8. Built:
`framework/prx/himem_launcher/` — a homebrew EBOOT (user-mode, `libpspsystemctrl_user`):
`sctrlHENSetMemory(24,8)` → `sctrlSESetUmdFile(<ms0:/ISO/*.iso>)` →
`sctrlKernelLoadExecVSHDisc("disc0:/PSP_GAME/SYSDIR/EBOOT.BIN", &param{key="game"})`. Run it
from the XMB; `game.txt` = `mhfu_partprobe.prx` reports MHFU's live map. If a partition
appears USER-mapped at `0x0A000000` → the extra-RAM path is open and the launcher is the
production boot path. If kernel-only / MHFU crashes → extra RAM is a dead end for MHFU EU.
`sctrlHENSetMemory` fails (-1) from vsh, so we MUST loadexec from the launcher (not route
through the XMB). Failure is recoverable by a plain **power-cycle** (runtime setting clears
on cold boot; no auto-loaded plugin → no Recovery needed).

**THE ZERO-RISK FALLBACK (works today):** the Brute fits the **native 1.2 MB slot** without
any extra RAM — swap the bloated anim sub[3] (his 77-clip moveset, the *only* reason the PAC
exceeds 1.2 MB) for the native Tigrex anim → `tmp/brute_tigrex_v59_nativeanim.bin` (1198288 B
≤ 1216512 native slot) → **same-size in-place inject** (`mhfu_inject_register`,
`USE_RELOCATE=false`). Renders + textured + animated (native Tigrex motion) + damages, on
every PSP model, no `sctrlHENSetMemory`, no XMB risk. The *general* real-HW technique for any
ported monster: splice the model+textures into a native slot, drive with the swapped native
monster's skeleton+anim. Only a *custom* moveset needs the extra RAM we can't get.

Test tools (all `framework/prx/memprobe/diag/`): `v_partprobe` (safe query map),
`v_p8test`/`v_p8test2` (self-reload / manual-relaunch p8 apply — both froze),
`v_p8live` (set + live re-query, proved deferred-only).

## 8. Character-select freeze — a self-inflicted regression, reverted (2026-06-27)

**Symptom.** While chasing the volatile "squat alongside the game" + ms0-safety work, real
HW began freezing on the **main-menu → Continue → character-select** transition (right as the
SAVEDATA "reading memory stick" utility inits), *before* the savestate slots appear. Boot log
showed `framework loaded — done` at the menu, then a freeze; `framework.log` empty (never
reached gameplay). Reproduced on every attempt, identical spot.

**Two leading theories — BOTH RULED OUT by HITL:**
- **Footprint / savedata RAM contention.** Split the framework init in two — phase 1 (menu):
  only the VM lock + a tiny waiter thread; phase 2 (first village, `screen_state==22`): the
  heavy Lua VM/slab/filebuf/threads + ms0 script reads. This removed ~240 KB of USER-partition
  allocation and all ms0 activity from the character-select window. **Still froze** ⇒ not
  footprint, not menu-time ms0 contention.
- **Bootstrap load-timing / partition-state-at-load.** Reverted the bootstrap to the
  known-good **first-flicker** gate (load at the first TITLE/MENU). Boot log confirmed it
  loaded at `screen_state=4` (TITLE, earliest possible). **Still froze** ⇒ not load timing.

**Conclusion.** The culprit is a **framework SOURCE change this session** — the
`mhfu_ms0_io_safe()` ms0-gating (added to `log.cpp`, `registry.cpp` spawn-poll,
`lua_host` worker, `inject.cpp` tick) is the prime suspect: it is the only change present in
*every* frozen build and the only one touching the menu/character-select window. The paradox:
static analysis shows it only *reduces* framework activity there (the working baseline did
*more* — ungated logging + ungated spawn-poll + ungated `sceIoGetstat` — and was fine), so the
mechanism is unresolved. Environmental causes (MS card, CFW state) are not fully excluded.

**Action taken.** Reverted the framework **and** bootstrap to commit `58737dd`, rebuilt,
redeployed — and HITL-confirmed the working baseline (boot → save-select → Giadrome quest →
native Tigrex + swap). The split-init *idea* (defer Lua VM/slab/threads + ms0 to the village,
keep only JIT-cold hook queuing at the menu; `mhfu_quest_init` must then queue the buildTargets
swap wrapper UNCONDITIONALLY since there's no menu between the village and the quest) is sound
and **saved** in `tmp/split-init-saved/` (force-tracked: the full diff patch + the two key
files + README). To root-cause: re-introduce the session's changes **one at a time on HW**,
testing character-select after each — start with the ms0-gating.

**BISECT (2026-06-27, in progress).** The session delta over `58737dd` splits into 6 groups:
**A** ms0-gating (`mhfu_ms0_io_safe` + its gates in log/registry/lua_host/inject), **B** volatile
recon (inject.cpp xram functions, `g_xram_lock_enabled=0`), **C** `install.cpp` village-gated
volatile poll, **D** `registry.cpp` quest-begin `arm_prelock`, **E** split-init (lua_host
phase1/phase2), **F** quest unconditional buildTargets. Re-applied on the clean baseline one
group at a time:
- **A alone → PASS (HITL).** Char-select survived, loaded into the Giadrome quest. **ms0-gating
  is EXONERATED** — overturns the §8 "prime suspect" verdict. (The paradox is resolved: A really
  is inert/safer, as static analysis said.)
- Next: **B+C** (volatile recon + village poll — the from-boot continuous additions, and the
  behavioral delta of the first frozen "recon" build over A). Then D, E, F.

## Key build/tooling facts

- Builds in Docker `pspdev/pspdev:latest`. `make` (framework) / `make` in `memprobe/`.
- `framework/prx/linkfile_nohw.lds` (NOT `.prx` — `make clean` does `rm -f *.prx` and would
  delete it, and `.prx` is gitignored). Referenced by `Makefile.psp` LDFLAGS override.
- `tools/patch_prx_eflags.py` runs host-side in the wrapper Makefile `all` (no python in
  the docker image).
- Inspect a PRX: `psp-readelf -h/-S/-l <file.prx>` inside the docker image.
- Reference working binaries: `~/Downloads/psplink/` (psplink_user.prx is the closest
  shape to our memprobe).
