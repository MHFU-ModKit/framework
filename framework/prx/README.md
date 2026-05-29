# MHFU PRX SDK

C/C++ SDK for shipping MHFU mods at native game speed as a single
`mhfu_framework.prx`. Mods are small descriptor modules statically
composed into that one PRX (PPSSPP wedges with two plugin PRXes
co-loaded — see "Why one PRX" below).

## Prerequisites

- **Docker** — the pspdev toolchain ships as `pspdev/pspdev:latest`.
- A PPSSPP memstick dir (macOS: `~/Documents/PPSSPP/PSP/`, Linux:
  `~/.config/ppsspp/PSP/`, Windows: `%USERPROFILE%/Documents/PSP/`).

## Build

```bash
cd framework/prx
make            # → mhfu_framework.prx (composes the mods in build/mods.manifest)
```

Which mods are baked in is controlled by **`build/mods.manifest`** (one
mod path per line, `#` disables). No Makefile edit needed — add a line,
rebuild. Override for a one-off build with `make host-make MODS="diag tigrex_inject"`.

## Layout

```
include/mhfu/          PUBLIC SDK — what a mod #includes
  mhfu.h               umbrella (pulls everything below)
  events.h             event ids, ctx structs, mhfu_on_* registration
  memory.h             read/write + named cell getters
  entity.h             typed entity accessors + registry walk
  hooks.h              hook arbitration: events + exclusive patches
  mod.h                mhfu_mod_t descriptor + MHFU_MOD() macro
  mips.h               Allegrex/MIPS encoder (for mods that build stubs)
  addresses.h          per-region address table
src/core/              framework internals (one job per TU)
  registry.cpp         event registration + dispatch + spawn poll
  trampoline.cpp       event trampolines + code cave + SMC helpers
  hooks.cpp            HookManager: ownership table + conflict detection
  modtable.cpp         walks the MHFU_MOD descriptors, resolves deps
  memory.cpp entity.cpp region.cpp log.cpp bootstrap.cpp
  internal.h           cross-TU contract (NOT public)
mods/<id>/mod.cpp      one descriptor mod each
mods/experimental/     research hooks (off by default; see status notes)
build/mods.manifest    which mods compose into the PRX
framework_exports.exp  symbols exported to a future separate mod PRX
```

The core compiles as light C++ (`-fno-exceptions -fno-rtti
-fno-threadsafe-statics`, no STL, **no global constructors** — verified
empty `.ctors`) so it links under psp-gcc with no libstdc++ runtime and
dodges PSP's unspecified static-init order. All public API is `extern "C"`.

## Authoring a mod

A mod is one TU exposing a descriptor. Minimal:

```c
#include "mhfu/mhfu.h"

static void on_spawn(const mhfu_monster_spawn_ctx_t *c) {
    if (c->monster_type == 0x46) mhfu_entity_set_size(c->entity_ptr, 2.0f);
}
static int  my_init(void)     { mhfu_on_monster_spawned(on_spawn); return 0; }
static void my_shutdown(void) {}

MHFU_MOD(.id = "my_mod", .version = "1.0",
         .needs = 0, .conflicts = 0,
         .init = my_init, .shutdown = my_shutdown);
```

Then add `my_mod` to `build/mods.manifest` and `make`. `init()` returns
0 to load, negative to refuse. Register events, claim hooks, and start
threads from `init()`.

### Events vs exclusive hooks

- **Events** (`mhfu_on_*` / `mhfu_hook_event(id, cb, priority)`) fan out
  — many mods may subscribe; all run, highest priority first.
- **Exclusive patches** claim one address and the framework records the
  owner + original bytes:
  - `mhfu_hook_function(addr, stub, "my_mod")` — redirect a function entry.
  - `mhfu_hook_vtable(slot_addr, fn, "my_mod")` — swap a vtable slot (JIT-immune).
  - `mhfu_patch_word(addr, word, "my_mod")` — patch one code word (ranged invalidate).

  A second mod claiming the same address gets `MHFU_HOOK_CONFLICT` and is
  refused. Declare known clashes up front with `.conflicts = "other_mod"`
  (symmetric) and ordering with `.needs = "dep_mod"`. The framework
  restores all of a mod's patches on shutdown — mods never save/restore
  original bytes themselves. Build stubs with `mhfu/mips.h`; after writing
  a stub buffer call `mhfu_flush_caches()` so the CPU runs it as code.

### Beating the PPSSPP JIT

For a code patch on a function PPSSPP may have already JIT-translated,
claim it while `mhfu_get_screen_state()` is TITLE (0x04) or MENU (0x01),
before the target block first executes. `tigrex_inject` does this; see
docs/FRAMEWORK_ARCHITECTURE.md and the CLAUDE.md JIT notes.

## Why one PRX (and the path to many)

PPSSPP's plugin host wedges MHFU at boot when two plugin PRXes are
co-loaded (verified; black screen). So mods are **statically composed**
into `mhfu_framework.prx` via the mod table — one PRX, many mods, with
conflict arbitration. This is "approach A".

"Approach B" (true drop-in: the framework itself `sceKernelLoadModule`s
mod PRXes from a folder) is a stretch goal — the wedge *may* not recur
when the framework loads the module instead of the plugin host, but that
needs a spike (`load_mods()` is the seam). The mod-author API is
identical for both, so nothing here is wasted toward B.

## Install on PPSSPP

```
<memstick>/PSP/PLUGINS/mhfu_framework/
    mhfu_framework.prx
    plugin.ini
    framework.log        (runtime)
```

`plugin.ini` (note: NOT flat key=value — PPSSPP wants these sections):
```
[games]
ULES01213 = true
ULUS10391 = true
ULJM05500 = true
[options]
type = prx
filename = mhfu_framework.prx
name = MHFU Framework
version = 1
```

## Gotchas (all hit the hard way)

1. **plugin.ini needs `[games]` + `[options]` sections** — a flat
   key=value file is silently ignored.
2. **Module attr = 0 (user mode).** PPSSPP's plugin host is user-mode
   only; `0x10xx` (kernel) breaks startup.
3. **crt0_prx provides `module_start`; you provide `main()`.** Defining
   your own `module_start` is a multiple-definition error.
4. **Don't return from `main()`** — crt0 then unloads the PRX, killing
   spawned threads. Spin (`for(;;) sceKernelDelayThread(...)`).
5. **EBOOT loads in waves.** The install worker waits until both anchor
   PCs show the original Allegrex `sv.q` (op 0x3E), then patches, then
   re-installs if a savestate/later load reverts them.
6. **Savestates created without the plugin don't load it.** Boot fresh,
   let it install, then save a new state.
7. **C++ atan2f etc. need `extern "C"`** (or `<math.h>`) or the linker
   looks for the mangled name.

## Limits today

- **Region detection stubbed** → defaults to EU (ULES01213). Fill NA/JP
  in `include/mhfu/addresses.h`.
- **Approach B (`load_mods()`) not wired** — static composition only.
- **No hot reload** — ship and restart (the Python live framework has
  hot reload for interactive iteration).
