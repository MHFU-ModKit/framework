# MHFU PRX SDK

C SDK for shipping MHFU mods as compiled `.prx` plugins.

## Prerequisites

- **Docker** (the pspdev toolchain ships as a Docker image; building
  outside it is possible but supported only as a fast path inside
  the container).
- Working memstick directory used by PPSSPP — usually
  `~/Documents/PPSSPP/` on macOS, `~/.config/ppsspp/PSP/` on Linux
  (and `%USERPROFILE%\Documents\PSP\` on Windows).

## Building the framework PRX

```bash
cd framework/prx
make
# → produces mhfu_framework.prx
```

The first build pulls `pspdev/pspdev:latest` if you don't have it.

## Building the sample mod

```bash
cd framework/prx/mods/hello_world_prx
make
# → produces hello_world.prx
```

## Installing on PPSSPP

```
<memstick>/PSP/PLUGINS/mhfu_framework/
    mhfu_framework.prx
    mhfu_framework.ini       (game-id allowlist)
    framework.log            (created at runtime)
    mods/
        hello_world.prx
        hello_world.ini
```

`mhfu_framework.ini`:
```
ULES01213 = 1
ULUS10391 = 1
ULJM05500 = 1
```

PPSSPP loads the PRX automatically when one of the listed game IDs is
detected. Both PRXes appear under **Settings → System → Plugins** (toggle
to enable/disable without removing files).

## How the SDK is laid out

```
include/mhfu_framework.h
    Mod-facing API: mhfu_register_event, mhfu_on_quest_beginning,
    state getters, logging. STABLE — don't break.

include/mhfu_framework_addresses.h
    Per-region anchor PCs and observable cells. Updated when new
    addresses are discovered via the live framework.

src/framework.c
    The runtime: callback registry, dispatcher functions, log file
    handling. Skeleton — MIPS trampoline installer is the next thing
    to land.

framework_exports.exp
    Symbol export list — controls what downstream mods can import.

mods/hello_world_prx/
    Sample mod that mirrors mods/hello_world/ (the Python live mod).
    Same event coverage; same intent; different language and runtime.
```

## Authoring a new mod

1. Copy `mods/hello_world_prx/` to a new directory.
2. Edit `PSP_MODULE_INFO`, the callback bodies, and the
   `mhfu_on_*` calls in `module_start`.
3. `make` — produces `<your_mod>.prx`.
4. Drop into `<memstick>/PSP/PLUGINS/mhfu_framework/mods/`.

The headers expose the same name surface as the Python live framework,
so a mod's callback bodies can usually be ported verbatim — only the
language changes.

## How the trampoline installer works

At `module_start`, for each known event the framework:

1. Snapshots the two 4-byte instructions at the anchor PC.
2. Picks a slot in the static code cave (`g_code_cave[]` — 64-byte
   aligned BSS array, 30 instructions per slot, 8 slots reserved).
3. Builds a wrapper into the slot using the local MIPS encoder
   (`src/mips_encoder.h`):
   - reserve a 0x40 scratch frame
   - spill `$a0..$a3`, `$v0`, `$v1`, `$ra`, original `$sp`, anchor PC
     into 9 contiguous u32 slots (the on-stack
     `mhfu_anchor_regs_t` layout)
   - `move $a0, $sp` then `jal mhfu_dispatch_<event>`
   - restore everything, tear down the frame
   - replay the two displaced instructions
   - `j anchor_pc + 8; nop` to resume the original code path
4. Patches the anchor PC with `j cave; nop`.
5. Flushes the dcache + icache via `sceKernelDcacheWritebackInvalidateAll`
   and `sceKernelIcacheInvalidateAll`. PPSSPP picks these up as the
   signal to retranslate; on real PSP they flush the real CPU caches.

`module_stop` runs the reverse: restore the two original instructions,
flush caches.

### Vetting an anchor PC

The wrapper copies the two displaced instructions verbatim. If anchor[0]
or anchor[1] is itself a control-transfer (j/jal/branch), the wrapper
breaks the original delay-slot relationship. Our two anchors are plain
`sw` instructions and are safe. Any new event anchor must be checked
before adding to the address table — disassemble the two words at the
anchor PC and confirm neither is a branch.

## Validation status (verified 2026-05-25)

What the first build + load-in-PPSSPP confirmed:

- **Build via Docker.** `make` from this directory produces a valid
  PRX in ~20s. The "stubs out of order" warning from `psp-fixup-imports`
  is cosmetic — the binary loads and runs.
- **PPSSPP loads the PRX.** With `plugin.ini` in the standard format
  (`[games]` + `[options]` sections — *not* a flat key=value INI as
  one might naively assume), PPSSPP's plugin host finds, parses, and
  loads the module. Log line: `Loaded plugin: ms0:/PSP/PLUGINS/mhfu_framework/mhfu_framework.prx`.
- **main() runs.** Sentinel writes at every stage of main confirmed
  via debugger memory read (`STAGE = 0xCAFE0004`).
- **Worker thread spawns and runs.** `sceKernelCreateThread` works
  in PPSSPP plugin context once the module is user-mode (PSP_MODULE_INFO
  attr = 0, not 0x1007), and once we avoid crt0_prx's default
  thread-creating bootstrap by defining
  `int sce_newlib_nocreate_thread_in_start = 1;`.
- **Trampolines install and persist.** Both anchor PCs read back as
  `J cave; NOP` after ~1s of game runtime, and remain that way for
  20+ seconds. A re-install monitor loop catches any case where the
  game's own EBOOT loading overwrites our patches and reapplies them.

What's NOT yet end-to-end verified:

- **The wrapper code actually executing.** To prove the full chain
  (game's CPU hits the anchor → J's into our cave → wrapper saves
  regs → JAL dispatcher → demo callback runs → wrapper restores →
  J back), the game's PC must actually reach 0x088655E4 / 0x0884CDFC.
  These addresses are only executed during quest start; from a clean
  boot, reaching them takes ~10 menu interactions. We've proven up to
  the trampoline-install step and that the game runs normally with
  trampolines in place (no crash), but the dispatcher fire is a
  manual-navigation follow-up.
- **Savestate compatibility.** PPSSPP does NOT load PRX plugins when
  the game state is restored from a `--state=` savestate (the savestate
  was created without the plugin present, so its restored module table
  doesn't include us). To use the PRX path with a quest-ready
  starting point: boot fresh, let the plugin install, then *save a new
  state* and use that one going forward.

## Known gotchas (write these down — we already hit them all)

1. **plugin.ini format.** PPSSPP parses `<plugin_dir>/plugin.ini`
   (literally that filename) with two sections — `[games]` listing
   `<DiscID> = true`, and `[options]` with `type = prx`,
   `filename = <name>.prx`, `name = <human>`, `version = 1`. A flat
   key=value file (the format you find on random forum posts) is
   silently ignored.

2. **Module attributes.** `PSP_MODULE_INFO(name, attr, ...)` — the
   `attr` second arg controls user/kernel mode. PPSSPP plugin host is
   user-mode-only. Use `0` (PSP_MODULE_USER), not `0x1007` or any
   `0x10xx` (kernel). Kernel-mode triggers
   `unsupported thread attributes 0x07` warnings and breaks startup.

3. **crt0_prx wants `main`, not `module_start`.** Modern pspsdk's
   `crt0_prx.o` already defines `module_start` (as an alias of `_start`).
   User code provides `main()`; crt0's path runs it. Trying to define
   your own `module_start` causes "multiple definition" link errors.

4. **Default crt0 spawns a thread that hangs under PPSSPP.** `_start`
   uses `sceKernelCreateThread` + `sceKernelStartThread` to run main()
   on a new thread. This hangs silently in PPSSPP plugin context.
   Define `int sce_newlib_nocreate_thread_in_start = 1;` to bypass
   the thread bootstrap and call `main()` directly.

5. **Don't return from main().** If `main()` returns, crt0's `_exit()`
   calls `sceKernelSelfStopUnloadModule` and our PRX gets unloaded —
   killing any background threads we spawned (and leaving anchor
   patches pointing into freed memory). Either spin in main() forever
   (`for (;;) sceKernelDelayThread(...);`) or call the install loop
   directly from main without returning.

6. **EBOOT loads in two waves.** On PPSSPP at least, the game's text
   sections at our two anchor PCs arrive in memory at different times.
   The first attempt to patch quest_beginning succeeded but the game
   then overwrote it with the original `sv.q` opcode about a second
   later, while quest_entered stayed patched. Solution: wait until
   BOTH anchors show the expected original opcode (Allegrex `sv.q`,
   op=0x3E), wait an additional 500ms, then re-check, then install.
   And keep a monitor loop running that re-installs if either anchor
   reverts.

7. **Anchors are Allegrex VFPU stores (`sv.q`), not standard `sw`.**
   Both `0x088655E4` and `0x0884CDFC` are `sv.q` (op 0x3E) — vector
   quad stores. These trigger our memory write BPs on the integer cell
   below them because `sv.q` writes 16 bytes. The trampoline copies
   the displaced instructions verbatim, so we don't need to special-
   case them, but anchor-PC vetting must confirm none of the two
   displaced words is a control transfer.

## Limits today (still)

- **Region detection stubbed**. The framework defaults to EU. For
  NA/JP, fill in the address tables in `mhfu_framework_addresses.h`
  via `scripts/re_quest_events.py` with a regional savestate.
- **On-disk mod enumeration not implemented.** Mod PRXes currently
  load via PPSSPP's normal plugin mechanism (each gets its own
  `<memstick>/PSP/PLUGINS/<id>/` folder + INI). The framework's
  `load_mods()` is a stub.
- **No hot-reload** — that's a Python-side capability the live
  framework can give you; for PRX you ship and restart.
- **Kernel APIs not available**. PPSSPP's plugin host is user-mode
  only; that's plenty for the events we care about but worth knowing
  if you start chasing low-level syscall hooks.
