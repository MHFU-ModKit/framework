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

### Typed helpers (read like intent, not pointer math)

Prefer these over raw `mhfu_read/write_*`. They wrap *stable, verified*
mechanics; bleeding-edge RE still uses the raw escape hatch.

- **Entity** (`entity.h`): `mhfu_entities_of_type(MON_TIGREX, out, 4)`,
  `mhfu_entity_pos/set_pos`, `set_size` (all 5 mirrors), `yaw`, `ai_state`,
  `engaged`, `mhfu_entity_calm()`.
- **Monster/species** (`monster.h`): `mhfu_species_set_detection(MON_TIGREX, 0)`.
- **Quest** (`quest.h`): `mhfu_quest_current()`, `mhfu_quest_has()`,
  `mhfu_quest_replace_monster(q, MON_GIADROME, MON_TIGREX)`. Edit the list
  from a `MHFU_EVENT_QUEST_TARGETS_BUILDING` callback — the framework owns
  the buildTargets hook + JIT-safe timing.
- **Install** (`hooks.h`): `mhfu_install_call_wrapper(site, target, helper,
  MHFU_WRAP_PREFIX, "mod")` builds the wrapper stub for you;
  `mhfu_patch_word_when_quiet(addr, expect, word, "mod")` defers a code
  patch to a TITLE/MENU screen to beat the JIT. (Quiet-gating suits EBOOT
  targets; overlay-resident hooks still install on their own section event.)
- **IDs** (`ids.h`): `MON_POPO/ANTEKA/TIGREX/GIADROME` (list incomplete).

`tigrex_inject/mod.cpp` is the worked example — quest injection + tame in
~15 lines of logic over these helpers.

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

### AI override events (Section 32d)

Three new events let a mod intercept the generic monster AI engine. They
hook the per-frame per-entity tick `z_un_08865648` and its sub-calls
(`docs/AI_SCRIPTING_ENGINE.md`):

- `mhfu_on_bigmonster_action_decided(cb, priority)` — fires after vt[8]
  picks an outcome pointer from the species probability table at
  `entity+0x1AC`. Sync override: `cb(ctx, engine_value) -> new_value`.
  Priority-chain (higher first); each handler sees the previous handler's
  return as its input. Return `engine_value` unchanged to abstain.
  JIT-immune (vt[8] swap on shared `0x08865254`).
- `mhfu_on_bigmonster_ai_step(cb, priority)` — observe-only, per-frame
  per-entity. Entry detour on `z_un_08865648`; quiet-gated at TITLE/MENU
  to dodge PPSSPP's JIT pre-cache (Section 26 + 32h).
- `mhfu_on_bigmonster_spawn(cb, priority)` — observe; fires when a big
  monster appears in the entity registry.
- `mhfu_on_bigmonster_death(cb, priority)` — observe; HP > 0 → 0 edge,
  one-shot per slot.
- `mhfu_on_bigmonster_action(cb, priority)` — **the ANIMATION-force seam**
  (Section 32k). Entry detour on the big-mon executor `0x09AC5228`; rewrite the
  `a1` action id and the engine fans it to every body-part slot itself —
  coherent, no desync/crash (verified Tigrex spin-lock). Prefer this over poking
  per-slot inputs. See `docs/AI_SCRIPTING_ENGINE.md` §32k.

  🔴 **It changes the CLIP, not the MOVE.** A big monster runs on two channels;
  hitboxes, effects and damage belong to the behaviour channel
  (`act_set` → `entity+0x298`/`+0x299`), which this hook does not touch. With `a1`
  pinned, the behaviour channel was measured moving through 8 states untouched. To
  drive an actual attack, write the `(main, sub)` pair — a Lua-forced spin dealt
  −33/−48 and killed the hunter. There is no native-call binding yet; the minimal
  Lua `act_set` and the per-species `(main,sub)` tables
  (`tools/em_moveset.py <ovl> --states`) are in `AI_SCRIPTING_ENGINE.md` §33–34.

Big-monster filtering uses an allowlist on `entity+0x1E8` (Tigrex 0x4B,
Giadrome 0x4D). Will switch to `quest.targets[2]` enumeration once
exposed via `quest.h`. See `mods/experimental/ai_demo/mod.cpp` for the
reference shape.

Companion header `mhfu/ai_actions.h` defines `<SPECIES>_ACTION_0x????`
constants per species, plus `mhfu_action_is_valid(monster_type, id)` (which
walks the species table at runtime — no static table to keep in sync).
Generated by `tools/dump_species_actions.py` against the species data table
at `0x09BB87C0`. Coverage: popo 13, anteka 40, giadrome 34.

**Tigrex is in a different AI class.** Its species_entry+0x0C is hitzone
data, not an action list; vt[8] returns POINTERS into the probability
table at `entity+0x1AC` rather than u16 action IDs. So tigrex gets
`TIGREX_VT8_INPUT_0x????` macros (50 action inputs + 4 probe inputs,
captured live from 1860 vt[8] calls). Inputs are stable across runs;
pointers are NOT. To force a tigrex action, look it up by stable input:

```c
uint32_t p = mhfu_action_ptr_for(MON_TIGREX, TIGREX_VT8_INPUT_0x04B1);
if (p) return p;   // engine has picked this action at least once
```

The framework snoops every `(vt8_input, engine_id)` pair from
`action_decided` callbacks into a 96-entry-per-species cache (active
even with zero subscribers). `mhfu_action_cache_size(type)` +
`mhfu_action_cache_entry(type, i, …)` introspect what's been seen.

### Other SDK helpers

* `mhfu_monster_name(type)` (`mhfu/ids.h`) — returns `"POPO"` /
  `"ANTEKA"` / `"TIGREX"` / `"GIADROME"`, or `"0xNN"` fallback. For
  logging.

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

## Lua ported-monster runtime (v2) — `mhfu_port.lua`

The current surface for "load a ported MHP3rd monster and script its AI". A plain memstick mod,
so no rebuild; full guide in **`docs/MOD_PORTED_MONSTER.md`**.

It exists because of the two-channel finding (`docs/AI_SCRIPTING_ENGINE.md` §33–34): the v1
surface below moves the ANIMATION only, and a port's clips are filed into host slots **by
position, not by meaning**, so the host handler asks for clip *N* and gets whatever the packer
put there. v2 makes the mapping explicit —

```lua
moves = { charge = { main = 3, sub = 6, clip = "charge" } }
```

`port:play("charge")` writes the behaviour pair `entity+0x298/+0x299` (the physics, the hitbox,
the damage) **and** latches the port's own clip over the executor dispatch that follows. That is
what lets a port with no host analogue — a Zinogre — have moves at all: you pick a host behaviour
for its physics and put your own animation on it.

🔴 **ONE dispatch, not all of them.** A forced pair is not a single executor dispatch: filmed
live, one seven-tick `(2,1)` asked the executor for a1 **15, 11, 19 and 18** in turn — a handler
runs a *sequence* of sub-actions. Answering every dispatch restarts the port's clip from frame 0
several times inside one move, which on screen is "no animation ever plays to the end" — the same
symptom as forcing a pair the engine refuses, from the opposite cause. The latch covers the
dispatch that OPENS the move (`act_set` has just zeroed the phase cursor) and abstains after;
`latch = <n>` on a move buys more.

Worked example: `mods/lua_host/scripts/brute_showcase.lua`, flown by `tools/brute_showcase.py`.

⚠️ v1's `brute_tigrex.lua` predates this and forces `a1` alone. Everything it does to the clip is
still true; everything it implies about the MOVE is not.

## Lua AI scripting layer (v1) — ported monsters

Lets a Lua script define the behaviour of a ported monster that runs on a
resident MHFU species as its engine host.  Brute Tigrex (port target) runs
on the resident Tigrex rig; the engine handles rendering, physics, and slot
management; the Lua script controls action selection.

Reference: `mods/lua_host/scripts/brute_tigrex.lua`.
Header:    `include/mhfu/ai_script.h` (a1 constants, C helper inlines, docs).

### v1 event surface

| Event | API | Thread | Purpose |
|-------|-----|--------|---------|
| spawn | `mhfu.on_bigmonster_spawn(fn)` | poll (5 Hz) | initial setup, render fix |
| animation override | `mhfu.on_bigmonster_action(fn, prio)` | exec (marshalled) | force the CLIP (not the move — see §33–34) |
| tick | `function mhfu_tick()` | worker (2 Hz) | maintenance, state advance |
| death | `mhfu.on_bigmonster_death(fn)` | poll (5 Hz) | disarm AI |

`on_action_end` is **deferred to v2** — detecting a slot-timer expiry cheaply
requires per-slot polling that isn't yet wired.

### Action force seam (coherent, no desync)

From an `on_bigmonster_action` callback, return an `a1` constant and the engine
fans it to all 3 body-part slots itself (executor `0x09AC5228`, `docs/AI_SCRIPTING_ENGINE.md` §32k):

```lua
mhfu.on_bigmonster_action(function(ctx)
    if ctx.entity ~= my_ent then return ctx.action_id end  -- filter
    return 0x2B   -- MHFU_AI_A1_ANGRY_SPIN: Tigrex tail spin
end, 10)
```

Named `a1` constants (defined in `include/mhfu/ai_script.h`):

| Constant | a1 | Input | Animation |
|----------|----|-------|-----------|
| `MHFU_AI_A1_IDLE_WALK` | `0x03` | `0x057B` | Walk straight |
| `MHFU_AI_A1_IDLE_STAND` | `0x20` | `0x0598` | Stand idle |
| `MHFU_AI_A1_ANGRY_SPIN` | `0x2B` | `0x05A3` | Tail spin (AoE) |
| `MHFU_AI_A1_ANGRY_CHARGE` | `0x11` | `0x0589` | Charge lunge |
| `MHFU_AI_A1_ANGRY_BITE_FWD` | `0x29` | `0x05A1` | Forward bite |
| `MHFU_AI_A1_ANGRY_JUMP_FWD` | `0x2F` | `0x05A7` | Jump forward |
| `MHFU_AI_A1_ANGRY_THROW_ROCKS` | `0x2D` | `0x05A5` | Rock throw |
| `MHFU_AI_A1_IDLE_SUSPICIOUS` | `0x50` | `0x05C8` | Alert look-around |

Derivation: `a1 = vt8_input(slot2) - 0x578`.  To add a new action, observe the
`input` value in `on_bigmonster_action_decided` for the animation you want, then
compute `a1 = input - 0x578`.

### Required maintenance (always do these)

**Freeze gate** — the engine sets bits `0x100|0x10000` at `entity+0x4B8` on
forced repeat-fire and halts the AI tick.  Zero those bits every action tick
AND from the 2 Hz worker (the halted AI tick cannot self-clear):

```lua
local function clear_freeze_gate(ent)
    local v = mhfu.read_u32(ent + 0x4B8)
    if (v & 0x10100) ~= 0 then mhfu.write_u32(ent + 0x4B8, v & ~0x10100) end
end
```

**Render fix** — swap-spawned monsters are culled until the first natural roam
sets `entity+0x29A` (section tracker).  Write the player's area + set
`entity+0x638 bit 0x8000` while co-located:

```lua
mhfu.entity_make_visible(ent, mhfu.get_area_index())
-- or raw:
mhfu.write_u16(ent + 0x29A, mhfu.get_area_index())
mhfu.write_u32(ent + 0x638, mhfu.read_u32(ent + 0x638) | 0x8000)
```

### Deferred (v2)

- `on_action_end` — slot-timer expiry detection (needs per-slot `+0x10` polling).
- Damage-per-part, part-break, wall-hit events.
- Multi-species support (non-Tigrex host).

## Limits today

- **Region detection stubbed** → defaults to EU (ULES01213). Fill NA/JP
  in `include/mhfu/addresses.h`.
- **Approach B (`load_mods()`) not wired** — static composition only.
- **No hot reload** — ship and restart (the Python live framework has
  hot reload for interactive iteration).
