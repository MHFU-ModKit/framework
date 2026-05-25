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

## Limits today

- **First build not yet attempted** in the pspdev Docker container —
  the source is written; the next concrete validation is `make` +
  loading the PRX in PPSSPP with the demo savestate.
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
